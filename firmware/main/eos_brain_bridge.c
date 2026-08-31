// eos_brain_bridge — see the header for what this is. This file is the rule
// that makes it safe.
//
// THE RULE: exactly one task ever calls an eos_brain_* function, and it is the
// task created here. Not the HTTP workers, not the frame loop, not a callback.
// eos_brain_t holds a socket, a parser and a request in one struct with no lock
// anywhere in it, and eos_brain_pump() blocks — three seconds on a connect to a
// mini that is switched off, two more on an mDNS query that nothing answers. A
// second caller would corrupt the state machine; a caller that is an HTTP
// worker would spend five seconds of a four-worker pool on a socket it does not
// own. So everybody else writes intent into the small struct below, under a
// mutex held for microseconds, and the task picks it up.
//
// The second non-obvious constraint is the ring. Decoded text lands in it from
// the task and leaves it through an HTTP worker, and the task REFUSES TO PUMP
// when fewer than PUMP_HEADROOM bytes are free rather than dropping what does
// not fit. One eos_brain_pump(b, 0) does at most one recv of EOS_BRAIN_RX_MAX
// and can emit at most that plus the parser's staged tail, so that headroom is
// a bound and not a guess. The effect is that a browser that stops reading
// becomes a full TCP window on the megabrain socket, which costs this board
// nothing at all — and no byte of a reply is ever lost, which a bigger ring
// would not have promised either.

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "eos_brain_bridge.h"

static const char *TAG = "eos_brain";

// The header restates eos_brain's four ceilings so that a host suite can link
// eos_httpd with no brain present. This is the only place both are visible, so
// it is the only place that can check they still agree.
_Static_assert(EOS_HTTPD_ASK_MAX    <= EOS_BRAIN_PROMPT_MAX, "ask ceiling above the prompt buffer");
_Static_assert(EOS_HTTPD_MODEL_MAX  <= EOS_BRAIN_MODEL_MAX,  "model ceiling above the model buffer");
_Static_assert(EOS_HTTPD_SYSTEM_MAX <= EOS_BRAIN_SYSTEM_MAX, "system ceiling above the system buffer");
_Static_assert(EOS_HTTPD_BHOST_MAX  <= EOS_BRAIN_HOST_MAX,   "host ceiling above the host buffer");

// 1 KB of decoded text in flight. The consumer drains at socket speed and the
// producer is bounded per pump, so this is slack rather than capacity: it only
// has to cover the gap between one HTTP send completing and the next.
#define RING_BYTES     1024
#define PUMP_HEADROOM  (EOS_BRAIN_RX_MAX + EOS_BRAIN_TEXT_MAX)

#define TASK_STACK     4096   // rx[192] and the lwip call frames, measured below
#define TASK_PRIO      4      // under the HTTP workers (5), over the frame loop (1)
#define TICK_BUSY_MS   5      // while a request is in flight
#define TICK_IDLE_MS   200

// How often the link is re-probed. This only feeds the status bar and
// /api/brain/status: an ask re-probes on its own when eos_brain's 15 s link ttl
// has lapsed, so probing at the ttl instead would buy one saved round trip per
// ask at the price of 7,200 connections a day to the mini. Thirty seconds is
// the staleness the glass is allowed, not a correctness bound. Down: rarer
// still, because a probe against a mini that is off walks all three candidates
// and spends eleven seconds doing it.
#define PROBE_UP_MS    30000u
#define PROBE_DOWN_MS  45000u

// How long a finished reply waits for its reader before the channel is taken
// back. See the sweeper in the task.
#define CH_ABANDON_MS  5000u

#define EVENTS         8      // avatar events queued between two frames

// What the mini is holding. megabrain publishes no model list, so this is the
// one fact in the whole file that is asserted rather than discovered — see the
// report. A wrong entry costs a 4xx from the mini on that model and nothing
// else; the web app's model box is a free-text field with these as suggestions.
static const char *const MODELS[] = {
    "qwen3.5:2b",           // fast default
    "gemma4:12b-it-qat",    // clean
    "ornith:9b"
};
#define MODEL_COUNT ((int)(sizeof MODELS / sizeof MODELS[0]))

// The channel: who owns the reply that is being produced right now.
typedef enum {
    CH_IDLE = 0,
    CH_RUN,        // a request is in flight; the ring is being filled
    CH_DONE,       // it finished cleanly; whatever is left in the ring is the tail
    CH_FAIL        // it did not; ERR says why
} ch_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t      task;

    // Owned by the task alone. Never touched under the lock, and never by
    // anything else, which is the rule at the top of this file.
    eos_brain_t brain;

    // --- the ring ---------------------------------------------------------
    char     ring[RING_BYTES];
    uint16_t head, tail;      // head == tail is empty; one slot is always spare

    ch_t     ch;
    uint32_t ch_settled_ms;   // when ch became DONE or FAIL; 0 while it has not
    char     err[72];         // the sentence behind a CH_FAIL

    // --- posted intent ----------------------------------------------------
    bool     want_ask, want_cancel;
    char     q[EOS_BRAIN_PROMPT_MAX];
    char     ask_model[EOS_BRAIN_MODEL_MAX];
    char     ask_system[EOS_BRAIN_SYSTEM_MAX];
    int      ask_max;

    // --- configuration ----------------------------------------------------
    // live is what the task is using; pending is what settings asked for. The
    // swap happens in the task while it is between requests, because
    // eos_brain_t holds a pointer INTO live.host and reads it during discovery.
    eos_brain_bridge_cfg_t live, pending;
    bool     cfg_dirty;
    bool     online;

    // --- published, for the bar -------------------------------------------
    bool     reachable;
    char     host[EOS_BRAIN_HOST_MAX];

    // --- avatar events ----------------------------------------------------
    uint8_t  ev[EVENTS];
    uint8_t  ev_head, ev_tail;

    bool     started;
} bridge_t;

static bridge_t B;

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void lock(void)   { if (B.lock) xSemaphoreTake(B.lock, portMAX_DELAY); }
static void unlock(void) { if (B.lock) xSemaphoreGive(B.lock); }

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (!cap) return;
    if (src) { while (src[n] && n + 1 < cap) { dst[n] = src[n]; n++; } }
    dst[n] = '\0';
}

// ==========================================================================
// The ring. Producer is the task, consumer is one HTTP worker, both under the
// lock — a memcpy of at most a few hundred bytes, so holding it costs less than
// the barrier a lock-free version would need to get right.
// ==========================================================================

static uint16_t ring_used(void)
{
    return (uint16_t)((B.head - B.tail) & (RING_BYTES - 1));
}

static uint16_t ring_free(void)
{
    return (uint16_t)(RING_BYTES - 1 - ring_used());
}

static void ring_reset(void) { B.head = B.tail = 0; }

// Writes what fits and returns what it wrote. The caller has already checked
// there is headroom; the truncating return exists so that a bug upstream loses
// text visibly in the log rather than walking off the end of the buffer.
static size_t ring_push(const char *s, size_t n)
{
    size_t room = ring_free(), i;
    if (n > room) n = room;
    for (i = 0; i < n; i++) {
        B.ring[B.head] = s[i];
        B.head = (uint16_t)((B.head + 1) & (RING_BYTES - 1));
    }
    return n;
}

static int ring_pop(char *out, int cap)
{
    int n = 0;
    while (n < cap && B.tail != B.head) {
        out[n++] = B.ring[B.tail];
        B.tail = (uint16_t)((B.tail + 1) & (RING_BYTES - 1));
    }
    return n;
}

// ==========================================================================
// Avatar events
// ==========================================================================

// Runs of STREAM_CHUNK are coalesced and nothing else is: a fast reply produces
// hundreds of chunks between two 250 ms frames and the buddy only needs to know
// that it is still talking, but losing the FIRST or the DONE would leave it
// thinking at an answer that already arrived.
static void ev_push(eos_buddy_event_t e)
{
    uint8_t next = (uint8_t)((B.ev_head + 1u) % EVENTS);
    uint8_t last;

    if (e == EOS_BUDDY_EV_STREAM_CHUNK && B.ev_head != B.ev_tail) {
        last = (uint8_t)((B.ev_head + EVENTS - 1u) % EVENTS);
        if (B.ev[last] == (uint8_t)EOS_BUDDY_EV_STREAM_CHUNK) return;
    }
    if (next == B.ev_tail) {   // full: drop the oldest, keep the newest truth
        B.ev_tail = (uint8_t)((B.ev_tail + 1u) % EVENTS);
    }
    B.ev[B.ev_head] = (uint8_t)e;
    B.ev_head = next;
}

int eos_brain_bridge_next_event(void)
{
    int e = -1;
    lock();
    if (B.ev_head != B.ev_tail) {
        e = (int)B.ev[B.ev_tail];
        B.ev_tail = (uint8_t)((B.ev_tail + 1u) % EVENTS);
    }
    unlock();
    return e;
}

// ==========================================================================
// eos_brain callbacks. All of these run on the task, inside eos_brain_pump,
// with the lock NOT held — so they take it themselves.
// ==========================================================================

static void on_text(void *user, const char *text, size_t len)
{
    size_t wrote;
    (void)user;
    if (!text || !len) return;

    lock();
    wrote = ring_push(text, len);
    unlock();

    if (wrote != len)
        ESP_LOGE(TAG, "ring overflowed by %u B - the headroom check is wrong",
                 (unsigned)(len - wrote));
}

// Called with the lock held. Stamps when the reply stopped, so the task can
// tell a reader that is a moment behind from one that is never coming back.
static void settle(ch_t st)
{
    B.ch = st;
    B.ch_settled_ms = ms_now();
    if (!B.ch_settled_ms) B.ch_settled_ms = 1;
}

static void on_done(void *user, eos_brain_err_t err)
{
    (void)user;
    lock();
    if (err == EOS_BRAIN_OK) {
        settle(CH_DONE);
        B.err[0] = '\0';
        ev_push(EOS_BUDDY_EV_STREAM_DONE);
    } else if (err == EOS_BRAIN_ERR_CANCELLED) {
        // Being told to stop is not a failure, and the reply so far is real
        // text the browser has already rendered. The avatar has seven states
        // and none of them is "told to shut up"; DONE is the least wrong of
        // them, and it is the one that ends the thinking animation.
        settle(CH_DONE);
        B.err[0] = '\0';
        ev_push(EOS_BUDDY_EV_STREAM_DONE);
    } else {
        settle(CH_FAIL);
        snprintf(B.err, sizeof B.err, "megabrain: %s", eos_brain_err_name(err));
        ev_push(EOS_BUDDY_EV_ERROR);
    }
    unlock();
}

static void on_event(void *user, const eos_brain_evt_t *e)
{
    (void)user;
    if (!e) return;
    lock();
    switch (e->kind) {
    case EOS_BRAIN_EV_SUBMITTED:    ev_push(EOS_BUDDY_EV_REQUEST_SENT); break;
    case EOS_BRAIN_EV_FIRST_TOKEN:  ev_push(EOS_BUDDY_EV_STREAM_FIRST); break;
    case EOS_BRAIN_EV_TOKEN:        ev_push(EOS_BUDDY_EV_STREAM_CHUNK); break;
    case EOS_BRAIN_EV_LINK:
        if (e->link == EOS_BRAIN_LINK_UP)
            ESP_LOGI(TAG, "link   megabrain answers at %s", e->host ? e->host : "?");
        else if (e->link == EOS_BRAIN_LINK_DOWN)
            ESP_LOGW(TAG, "link   no megabrain on this network");
        break;
    default: break;
    }
    unlock();
}

// ==========================================================================
// The httpd ports. These run on an HTTP worker. None of them calls eos_brain.
// ==========================================================================

static bool p_status(void *ctx, eos_httpd_brain_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);

    lock();
    copy_str(out->host, sizeof out->host, B.host[0] ? B.host : B.live.host);
    out->port      = B.live.port;
    copy_str(out->model, sizeof out->model, B.live.model);
    out->reachable = B.reachable;
    out->busy      = (B.ch == CH_RUN);
    // Borrowed for the length of the call and pointing at rodata, so there is
    // nothing here for the caller to outlive.
    out->models      = MODELS;
    out->model_count = MODEL_COUNT;
    out->last_error  = B.err[0] ? B.err : NULL;
    unlock();
    return true;
}

static int p_ask(void *ctx, const eos_httpd_ask_t *a)
{
    (void)ctx;
    if (!a || !a->q || !a->q[0]) return -1;   // EOS_ERR_ARG

    lock();
    if (B.ch != CH_IDLE || B.want_ask) { unlock(); return -8; }   // EOS_ERR_BUSY

    copy_str(B.q,          sizeof B.q,          a->q);
    copy_str(B.ask_model,  sizeof B.ask_model,  a->model);
    copy_str(B.ask_system, sizeof B.ask_system, a->system);
    B.ask_max = a->max_tokens;

    ring_reset();
    B.err[0]  = '\0';
    B.ch      = CH_RUN;
    B.want_ask = true;
    unlock();

    // The task is asleep for up to TICK_IDLE_MS. Wake it so the first token is
    // not 200 ms behind the request that asked for it.
    if (B.task) xTaskNotifyGive(B.task);
    return 0;
}

static int p_read(void *ctx, char *buf, int cap)
{
    int n;
    (void)ctx;
    if (!buf || cap <= 0) return EOS_HTTPD_STREAM_FAIL;

    lock();
    // Every call is proof the reader still exists, which is what the sweeper
    // below is really asking. Stamping on arrival rather than on completion is
    // the difference between "this reply is old" and "nobody is reading it" —
    // and only the second is a reason to throw a tail away.
    if (B.ch_settled_ms) { uint32_t t = ms_now(); B.ch_settled_ms = t ? t : 1; }
    n = ring_pop(buf, cap);
    if (n == 0) {
        if (B.ch == CH_DONE)      { B.ch = CH_IDLE; B.ch_settled_ms = 0; n = EOS_HTTPD_STREAM_END;  }
        else if (B.ch == CH_FAIL) { B.ch = CH_IDLE; B.ch_settled_ms = 0; n = EOS_HTTPD_STREAM_FAIL; }
        else                      { n = EOS_HTTPD_STREAM_WAIT; }
    }
    unlock();
    return n;
}

static bool p_cancel(void *ctx)
{
    bool had;
    (void)ctx;

    lock();
    had = (B.ch == CH_RUN);
    if (had) {
        B.want_cancel = true;
    } else if (B.ch == CH_DONE || B.ch == CH_FAIL) {
        // A finished reply whose reader walked away. Reclaim the channel here
        // or the next ask is refused as busy forever.
        B.ch = CH_IDLE;
        B.ch_settled_ms = 0;
        ring_reset();
    }
    unlock();

    if (had && B.task) xTaskNotifyGive(B.task);
    return had;
}

void eos_brain_bridge_bind(eos_httpd_t *h)
{
    if (!h) return;
    h->ports.brain_status = p_status;
    h->ports.brain_ask    = p_ask;
    h->ports.brain_read   = p_read;
    h->ports.brain_cancel = p_cancel;
}

// ==========================================================================
// The task
// ==========================================================================

// Copies pending config over live and re-inits eos_brain against it. Only ever
// called with nothing in flight: eos_brain_t keeps the fallback_host POINTER,
// not a copy, and rewriting the bytes it points at during a discovery walk is
// exactly the kind of thing that works on the bench and not in the field.
static void apply_cfg(void)
{
    eos_brain_cfg_t bc;

    lock();
    B.live = B.pending;
    B.cfg_dirty = false;
    unlock();

    memset(&bc, 0, sizeof bc);
    bc.fallback_host = B.live.host;
    bc.port          = B.live.port;
    // Everything else is eos_brain's own default: 3 s connect, 20 s idle,
    // 60 s total, 15 s link ttl. They were chosen against this same mini.

    eos_brain_init(&B.brain, eos_brain_lwip_transport(), &bc, eos_brain_idf_hooks());
    eos_brain_set_event_cb(&B.brain, on_event, NULL);

    ESP_LOGI(TAG, "cfg    host %s:%u model %s max %d",
             B.live.host, (unsigned)B.live.port, B.live.model, B.live.max_tokens);
}

static void do_submit(void)
{
    eos_brain_req_t r;
    int32_t rc;

    memset(&r, 0, sizeof r);
    r.prompt     = B.q;
    r.system     = B.ask_system[0] ? B.ask_system : B.live.system;
    r.model      = B.ask_model[0]  ? B.ask_model  : B.live.model;
    r.max_tokens = B.ask_max       ? B.ask_max    : B.live.max_tokens;
    r.method     = EOS_BRAIN_METHOD_AUTO;
    r.on_token   = on_text;
    r.on_done    = on_done;

    rc = eos_brain_submit(&B.brain, &r);
    if (rc > 0) return;

    // It never started, so on_done will never fire and nothing else will free
    // the channel. Fail it here with the same vocabulary the stream uses.
    lock();
    settle(CH_FAIL);
    snprintf(B.err, sizeof B.err, "megabrain: %s",
             eos_brain_err_name((eos_brain_err_t)(-rc)));
    ev_push(EOS_BUDDY_EV_ERROR);
    unlock();
    ESP_LOGW(TAG, "submit refused: %s", B.err);
}

static void brain_task(void *arg)
{
    uint32_t last_probe = 0;
    (void)arg;

    for (;;) {
        bool submit = false, cancel = false, online;
        uint32_t now = ms_now();

        // --- collect what the workers posted -----------------------------
        lock();
        online = B.online;
        if (B.want_cancel) {
            B.want_cancel = false;
            cancel = true;
            // A cancel that arrives before the ask it names has been submitted
            // cancels the ASK. p_cancel only raises this flag while the channel
            // is CH_RUN, which is the state p_ask itself set, so the two always
            // name the same request. Without this the two flags are collected
            // in the same pass, the cancel runs against a request that has not
            // started yet and does nothing, and do_submit() then sends it - a
            // whole megabrain round trip spent on a browser that has already
            // gone, during which the one HTTP task is parked on the reply.
            if (B.want_ask) {
                B.want_ask      = false;
                B.ch            = CH_IDLE;
                B.ch_settled_ms = 0;
                ring_reset();
            }
        }
        if (B.want_ask && !eos_brain_busy(&B.brain)) { B.want_ask = false; submit = true; }
        unlock();

        // A pending ask that arrived while a health probe was walking the
        // candidate list. Drop the probe rather than refuse the person.
        if (!submit && eos_brain_busy(&B.brain)) {
            bool waiting;
            lock(); waiting = B.want_ask; unlock();
            if (waiting) eos_brain_cancel(&B.brain);
        }

        if (cancel) eos_brain_cancel(&B.brain);

        if (!eos_brain_busy(&B.brain)) {
            bool dirty;
            lock(); dirty = B.cfg_dirty; unlock();
            if (dirty) apply_cfg();
        }

        if (submit) do_submit();

        // --- drive it ----------------------------------------------------
        //
        // budget 0 is one step per call, which is what makes the headroom test
        // below a bound: one step is at most one recv of EOS_BRAIN_RX_MAX.
        {
            int steps = 0;
            while (eos_brain_busy(&B.brain) && steps++ < 64) {
                uint16_t room;
                lock(); room = ring_free(); unlock();
                if (room < PUMP_HEADROOM) break;      // back-pressure, see the header
                if (!eos_brain_pump(&B.brain, 0)) break;
            }
        }

        // --- the health probe --------------------------------------------
        if (online && !eos_brain_busy(&B.brain)) {
            bool up = (eos_brain_link(&B.brain) == EOS_BRAIN_LINK_UP);
            uint32_t every = up ? PROBE_UP_MS : PROBE_DOWN_MS;
            bool idle;
            lock(); idle = (B.ch == CH_IDLE); unlock();
            if (idle && (last_probe == 0 || (now - last_probe) >= every)) {
                last_probe = now ? now : 1;
                eos_brain_probe(&B.brain);
            }
        }

        // --- the abandoned-reply sweeper ---------------------------------
        //
        // A finished reply is only released when its reader drains the last of
        // it, and a reader can stop existing: a worker that hit its own
        // deadline, a socket that died between the last chunk and the next
        // read. Without this the channel stays claimed and every ask after it
        // is 409 for the life of the boot.
        //
        // The clock is time since the last READ, not since the reply ended, so
        // a reader crawling through a tail against a stalled TCP window is
        // never robbed of it. Five seconds is far longer than the worker's own
        // poll period and short enough that a person pressing send again does
        // not wait on it.
        {
            bool sweep = false;
            lock();
            if ((B.ch == CH_DONE || B.ch == CH_FAIL) && B.ch_settled_ms &&
                (now - B.ch_settled_ms) >= CH_ABANDON_MS) {
                B.ch = CH_IDLE;
                B.ch_settled_ms = 0;
                ring_reset();
                sweep = true;
            }
            unlock();
            if (sweep) ESP_LOGW(TAG, "reply  nobody drained the tail; channel reclaimed");
        }

        // --- publish ------------------------------------------------------
        {
            bool up = (eos_brain_link(&B.brain) == EOS_BRAIN_LINK_UP);
            const char *hp = eos_brain_host(&B.brain);
            lock();
            B.reachable = up;
            if (hp && hp[0]) copy_str(B.host, sizeof B.host, hp);
            unlock();
        }

        // Sleep, unless a worker rings the bell first. Short while a reply is
        // streaming so the panel and the browser both see it as it lands;
        // long the rest of the time, because an idle brain costs nothing.
        ulTaskNotifyTake(pdTRUE,
            pdMS_TO_TICKS(eos_brain_busy(&B.brain) ? TICK_BUSY_MS : TICK_IDLE_MS));
    }
}

// ==========================================================================
// Lifecycle
// ==========================================================================

void eos_brain_bridge_defaults(eos_brain_bridge_cfg_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    copy_str(cfg->host,   sizeof cfg->host,   EOS_BRAIN_FALLBACK_HOST);
    cfg->port = 80;
    copy_str(cfg->model,  sizeof cfg->model,  EOS_BRAIN_DEFAULT_MODEL);
    copy_str(cfg->system, sizeof cfg->system, EOS_BRAIN_SYSTEM_TINY);
    cfg->max_tokens = 256;
}

void eos_brain_bridge_configure(const eos_brain_bridge_cfg_t *cfg)
{
    if (!cfg) return;
    lock();
    B.pending = *cfg;
    if (!B.pending.host[0])   copy_str(B.pending.host,   sizeof B.pending.host,   EOS_BRAIN_FALLBACK_HOST);
    if (!B.pending.port)      B.pending.port = 80;
    if (!B.pending.model[0])  copy_str(B.pending.model,  sizeof B.pending.model,  EOS_BRAIN_DEFAULT_MODEL);
    if (!B.pending.system[0]) copy_str(B.pending.system, sizeof B.pending.system, EOS_BRAIN_SYSTEM_TINY);
    if (B.pending.max_tokens <= 0) B.pending.max_tokens = 256;
    B.cfg_dirty = true;
    unlock();
    if (B.task) xTaskNotifyGive(B.task);
}

#ifdef EOS_BRAIN_BRIDGE_HAS_SETTINGS
void eos_brain_bridge_from_settings(const eos_settings_t *s)
{
    eos_brain_bridge_cfg_t c;

    if (!s) return;
    // Start from the defaults rather than from zeros, so a store that has never
    // been written — which is every board on its first boot — configures the
    // compiled-in mini rather than an empty host and no model.
    eos_brain_bridge_defaults(&c);
    if (s->brain_host[0])   copy_str(c.host,   sizeof c.host,   s->brain_host);
    if (s->brain_model[0])  copy_str(c.model,  sizeof c.model,  s->brain_model);
    if (s->brain_system[0]) copy_str(c.system, sizeof c.system, s->brain_system);
    if (s->brain_port)      c.port       = s->brain_port;
    if (s->brain_max)       c.max_tokens = (int)s->brain_max;
    eos_brain_bridge_configure(&c);
}
#endif

void eos_brain_bridge_set_online(bool online)
{
    lock();
    B.online = online;
    unlock();
}

bool eos_brain_bridge_reachable(void)
{
    bool r;
    lock(); r = B.reachable; unlock();
    return r;
}

void eos_brain_bridge_model(char *out, size_t cap)
{
    if (!out || !cap) return;
    lock();
    copy_str(out, cap, B.live.model);
    unlock();
}

int eos_brain_bridge_start(void)
{
    if (B.started) return 0;

    memset(&B, 0, sizeof B);
    B.lock = xSemaphoreCreateMutex();
    if (!B.lock) return -4;                       // EOS_ERR_POOL

    eos_brain_bridge_defaults(&B.pending);
    apply_cfg();

    if (xTaskCreate(brain_task, "eos_brain", TASK_STACK, NULL,
                    TASK_PRIO, &B.task) != pdPASS) {
        vSemaphoreDelete(B.lock);
        B.lock = NULL;
        return -4;
    }

    B.started = true;
    ESP_LOGI(TAG, "task   up, %d B stack, prio %d, %d B of ring",
             TASK_STACK, TASK_PRIO, RING_BYTES);
    return 0;
}
