// eos_input — the implementation behind kernel/hal/include/eos_input.h.
//
// One ring, one held-key table, one place a HID report is turned into events.
// Until this file existed eos_input_poll() was declared and implemented
// nowhere, so ESP-OS drew a window manager that nothing could drive: the BLE
// callback had no queue to push into and the shell's dispatch drained a queue
// that did not exist. Everything here is the join between those two.
//
// The one non-obvious constraint: a HID keyboard reports STATE, not events. An
// 8-byte report says "these six usages and these modifier bits are down right
// now", and the keyboard never says a key was released — it just stops
// mentioning it. So the events the rest of the OS wants are a DIFF against the
// previous report, and that diff is done exactly once, here, which is why no
// driver above the HAL is allowed to invent its own. It also means a dropped
// report leaves a key latched down until the next one arrives, and a keyboard
// that vanishes mid-chord leaves it latched forever, which is why a
// disconnect clears every held key belonging to that source.
//
// Reports arrive from an untrusted peripheral over the air. Everything in
// eos_input_hid_report() is written to be safe for any length from 0 to 255
// and any byte values, including the 0x01 ErrorRollOver flood a cheap keyboard
// emits when you mash it, which is a report that must NOT be read as six keys
// going down.
//
// Locking: two spinlocks, never nested in the other order. s_smux guards the
// held table and the modifier state, s_qmux guards the ring. eos_input_push()
// is callable from a BLE host callback or a GPIO ISR and takes only s_qmux.
//
// Nothing here allocates. On target the BLE HID host is brought up from
// eos_input_init(), which is a HAL calling a service and therefore upside
// down; it is done that way because eos_input.h promises that init brings up
// the sources the board declares, and inverting one include is cheaper than
// giving every board layer a second thing to remember.

#include "eos_input.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "eos_ble.h"

static portMUX_TYPE s_qmux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_smux = portMUX_INITIALIZER_UNLOCKED;
#define QLOCK()   portENTER_CRITICAL_SAFE(&s_qmux)
#define QUNLOCK() portEXIT_CRITICAL_SAFE(&s_qmux)
#define SLOCK()   portENTER_CRITICAL_SAFE(&s_smux)
#define SUNLOCK() portEXIT_CRITICAL_SAFE(&s_smux)
#else
#define QLOCK()   ((void)0)
#define QUNLOCK() ((void)0)
#define SLOCK()   ((void)0)
#define SUNLOCK() ((void)0)
#endif

// Six HID slots plus up to eight modifier keys is fourteen; sixteen is the
// next round number and costs 256 bytes of BSS. A seventeenth simultaneous key
// is refused rather than evicting one, because evicting one loses its key-up.
#define HELD_MAX   16
#define HID_SLOTS   6

// Milliseconds a GPIO button must read the same way before it counts. 20 ms is
// what the arcade build settled on for the BOOT button, which is a bare tact
// switch with no hardware debounce anywhere on any of these boards.
#define BTN_DEBOUNCE_MS 20

typedef struct {
    uint8_t  key;
    uint8_t  src;
    uint32_t down_ms;
    uint32_t next_rep_ms;   // 0 when this key does not repeat
    uint32_t expire_ms;     // 0 when the hold never expires on its own
} held_t;

static struct {
    bool            ready;
    eos_input_cfg_t cfg;

    eos_event_t q[EOS_INPUT_QUEUE];
    uint8_t     head;
    uint8_t     count;
    uint32_t    dropped;

    held_t   held[HELD_MAX];
    uint8_t  nheld;
    uint8_t  mods;
    uint8_t  prev[HID_SLOTS];   // last report's usage slots, for the diff

    uint8_t      nbtn;
    eos_button_t btn[EOS_MAX_BUTTONS];
    bool         btn_down[EOS_MAX_BUTTONS];    // debounced
    bool         btn_raw[EOS_MAX_BUTTONS];     // last sample
    uint32_t     btn_since[EOS_MAX_BUTTONS];   // when btn_raw last changed
} S;

// A signed difference so the comparison survives the 49-day wrap of a
// millisecond counter. now - deadline >= 0 means the deadline has passed.
static bool due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

// ------------------------------------------------------------------- queue

bool eos_input_push(const eos_event_t *e)
{
    bool ok;

    if (!e) return false;

    QLOCK();
    if (S.count >= EOS_INPUT_QUEUE) {
        S.dropped++;
        ok = false;
    } else {
        S.q[(uint8_t)((S.head + S.count) % EOS_INPUT_QUEUE)] = *e;
        S.count++;
        ok = true;
    }
    QUNLOCK();
    return ok;
}

bool eos_input_poll(eos_event_t *out)
{
    bool ok;

    if (!out) return false;

    QLOCK();
    if (S.count == 0) {
        ok = false;
    } else {
        *out = S.q[S.head];
        S.head = (uint8_t)((S.head + 1u) % EOS_INPUT_QUEUE);
        S.count--;
        ok = true;
    }
    QUNLOCK();
    return ok;
}

bool eos_input_peek(eos_event_t *out)
{
    bool ok;

    if (!out) return false;

    QLOCK();
    if (S.count == 0) {
        ok = false;
    } else {
        *out = S.q[S.head];
        ok = true;
    }
    QUNLOCK();
    return ok;
}

void eos_input_flush(void)
{
    QLOCK();
    S.head  = 0;
    S.count = 0;
    QUNLOCK();
}

uint32_t eos_input_dropped(void)
{
    uint32_t n;

    QLOCK();
    n = S.dropped;
    QUNLOCK();
    return n;
}

// ------------------------------------------------------------------- emit

static void emit(uint8_t type, uint8_t src, uint8_t key, uint8_t mods,
                 uint16_t ch, uint32_t ms)
{
    eos_event_t e;

    memset(&e, 0, sizeof e);
    e.type = type;
    e.src  = src;
    e.key  = key;
    e.mods = mods;
    e.ch   = ch;
    e.ms   = ms;
    eos_input_push(&e);
}

// A press is a key event plus, when the usage has a character behind it, one
// EOS_EV_TEXT. Splitting them is what lets the shell bind super+return without
// also having to know that return means '\n'.
static void emit_press(uint8_t type, uint8_t src, uint8_t key, uint8_t mods,
                       uint32_t ms)
{
    uint16_t ch;

    emit(type, src, key, mods, 0, ms);
    ch = eos_input_char(key, mods);
    if (ch) emit(EOS_EV_TEXT, src, 0, mods, ch, ms);
}

// -------------------------------------------------------------- held table
//
// All of these run with s_smux held.

static int held_find(uint8_t key)
{
    int i;
    for (i = 0; i < (int)S.nheld; i++) if (S.held[i].key == key) return i;
    return -1;
}

// Compacts by moving the last entry down. Order in the table is not meaningful
// and a memmove of sixteen structs from a BLE callback is not free.
static void held_del(int i)
{
    S.nheld--;
    if (i != (int)S.nheld) S.held[i] = S.held[S.nheld];
}

static bool held_add(uint8_t key, uint8_t src, uint32_t now, uint32_t expire)
{
    held_t *h;
    bool repeats;

    if (S.nheld >= HELD_MAX) return false;

    h = &S.held[S.nheld++];
    h->key       = key;
    h->src       = src;
    h->down_ms   = now;
    h->expire_ms = expire;

    repeats = S.cfg.repeat_delay_ms != 0;
    if (eos_key_is_mod(key) && !S.cfg.repeat_mods) repeats = false;
    h->next_rep_ms = repeats ? (now + S.cfg.repeat_delay_ms) : 0;
    return true;
}

// The modifier state is not a separate variable that could disagree with the
// held table: it IS the held table, recomputed after every change. That is why
// a stuck modifier cannot outlive the key it came from.
static void recalc_mods(void)
{
    uint8_t m = 0;
    int i;

    for (i = 0; i < (int)S.nheld; i++) m |= eos_mod_bit(S.held[i].key);
    S.mods = m;
}

// -------------------------------------------------------------- HID report

void eos_input_hid_report(const uint8_t *report, uint8_t len, uint32_t now_ms)
{
    uint8_t cur[HID_SLOTS];
    uint8_t ncur = 0;
    uint8_t newmods;
    bool rollover = false;
    unsigned i, j, b;

    // A report with no modifier byte is not a keyboard boot report at all.
    if (!report || len < 1) return;
    newmods = report[0];

    // report[1] is reserved and is skipped by every keyboard ever shipped.
    // Slots start at byte 2; a report shorter than that carries no keys, which
    // is a legitimate way to say "everything is up".
    for (i = 2; i < (unsigned)len; i++) {
        uint8_t k = report[i];

        if (k == 0) continue;
        // 0x01 ErrorRollOver, 0x02 POSTFail, 0x03 ErrorUndefined. A keyboard
        // that cannot resolve the matrix fills every slot with 0x01. Reading
        // that as six keys going down is how a mashed keyboard turns into six
        // spurious binds, so the whole key array is discarded and the previous
        // held set is left alone until the keyboard can speak again.
        if (k <= 0x03) { rollover = true; continue; }
        for (j = 0; j < ncur; j++) if (cur[j] == k) break;
        if (j < ncur) continue;                 // the same usage twice
        if (ncur < HID_SLOTS) cur[ncur++] = k;  // a seventh slot is ignored
    }

    SLOCK();

    // Modifier presses first, so the key-down that follows in the same report
    // carries them. super+return arrives as one report with both set, and a
    // dispatch that saw the return before the super would open nothing.
    for (b = 0; b < 8; b++) {
        uint8_t mk = (uint8_t)(EOS_KEY_LCTRL + b);
        if (!(newmods & (1u << b))) continue;
        if (held_find(mk) >= 0) continue;
        if (!held_add(mk, EOS_SRC_KEYBOARD, now_ms, 0)) continue;
        recalc_mods();
        emit(EOS_EV_KEY_DOWN, EOS_SRC_KEYBOARD, mk, S.mods, 0, now_ms);
    }

    if (!rollover) {
        // Releases: in the previous report, not in this one.
        for (i = 0; i < HID_SLOTS; i++) {
            uint8_t k = S.prev[i];
            int idx;

            if (k == 0) continue;
            for (j = 0; j < ncur; j++) if (cur[j] == k) break;
            if (j < ncur) continue;

            idx = held_find(k);
            if (idx >= 0) { held_del(idx); recalc_mods(); }
            emit(EOS_EV_KEY_UP, EOS_SRC_KEYBOARD, k, S.mods, 0, now_ms);
        }

        // Presses: in this report, not in the previous one.
        for (i = 0; i < ncur; i++) {
            uint8_t k = cur[i];

            for (j = 0; j < HID_SLOTS; j++) if (S.prev[j] == k) break;
            if (j < HID_SLOTS) continue;
            if (held_find(k) >= 0) continue;
            if (!held_add(k, EOS_SRC_KEYBOARD, now_ms, 0)) continue;
            recalc_mods();
            emit_press(EOS_EV_KEY_DOWN, EOS_SRC_KEYBOARD, k, S.mods, now_ms);
        }
    }

    // Modifier releases last, for the mirror of the reason above: the key-up
    // in the same report should still read as super+something.
    for (b = 0; b < 8; b++) {
        uint8_t mk = (uint8_t)(EOS_KEY_LCTRL + b);
        int idx;

        if (newmods & (1u << b)) continue;
        idx = held_find(mk);
        if (idx < 0) continue;
        if (S.held[idx].src != EOS_SRC_KEYBOARD) continue;   // an injected hold is not ours to drop
        held_del(idx);
        recalc_mods();
        emit(EOS_EV_KEY_UP, EOS_SRC_KEYBOARD, mk, S.mods, 0, now_ms);
    }

    if (!rollover) {
        memset(S.prev, 0, sizeof S.prev);
        for (i = 0; i < ncur; i++) S.prev[i] = cur[i];
    }

    SUNLOCK();
}

// --------------------------------------------------------------- injection

void eos_input_inject_key(uint8_t key, bool down, uint8_t mods,
                          uint8_t src, uint32_t now_ms)
{
    int idx;
    uint8_t ev_mods;

    if (key == EOS_KEY_NONE) return;

    SLOCK();
    idx = held_find(key);

    if (down) {
        // The phone and the serial console cannot be trusted to send a
        // release: the page navigates away mid-hold and the direction stays
        // latched. Every injected hold therefore carries an expiry that
        // eos_input_tick() honours, and a repeated press refreshes it rather
        // than emitting a second key-down.
        uint32_t expire = 0;
        if (src == EOS_SRC_WEB || src == EOS_SRC_SERIAL)
            expire = now_ms + (S.cfg.web_hold_ms ? S.cfg.web_hold_ms : 1u);

        if (idx >= 0) {
            S.held[idx].expire_ms = expire;
            SUNLOCK();
            return;
        }
        if (!held_add(key, src, now_ms, expire)) { SUNLOCK(); return; }
        recalc_mods();
        ev_mods = (uint8_t)(S.mods | mods);
        emit_press(EOS_EV_KEY_DOWN, src, key, ev_mods, now_ms);
    } else {
        if (idx >= 0) { held_del(idx); recalc_mods(); }
        ev_mods = (uint8_t)(S.mods | mods);
        emit(EOS_EV_KEY_UP, src, key, ev_mods, 0, now_ms);
    }
    SUNLOCK();
}

void eos_input_inject_text(uint16_t ch, uint8_t src, uint32_t now_ms)
{
    if (ch == 0) return;
    emit(EOS_EV_TEXT, src, 0, eos_input_mods(), ch, now_ms);
}

void eos_input_inject_touch(uint8_t type, int16_t x, int16_t y,
                            uint8_t src, uint32_t now_ms)
{
    eos_event_t e;

    if (type != EOS_EV_TOUCH_DOWN && type != EOS_EV_TOUCH_MOVE &&
        type != EOS_EV_TOUCH_UP) return;

    memset(&e, 0, sizeof e);
    e.type = type;
    e.src  = src;
    e.mods = eos_input_mods();
    e.x    = x;
    e.y    = y;
    e.ms   = now_ms;
    eos_input_push(&e);
}

void eos_input_inject_conn(uint8_t src, bool up, uint32_t now_ms)
{
    SLOCK();
    if (!up) {
        int i = 0;
        while (i < (int)S.nheld) {
            if (S.held[i].src == src) held_del(i);
            else i++;
        }
        // The diff state belongs to the keyboard that just left. Keeping it
        // would make the next keyboard's first report look like six releases.
        if (src == EOS_SRC_KEYBOARD) memset(S.prev, 0, sizeof S.prev);
        recalc_mods();
    }
    emit(up ? EOS_EV_CONNECT : EOS_EV_DISCONNECT, src, 0, S.mods, 0, now_ms);
    SUNLOCK();
}

// -------------------------------------------------------------- held state

bool eos_input_held(uint8_t key)
{
    bool h;

    SLOCK();
    h = held_find(key) >= 0;
    SUNLOCK();
    return h;
}

uint8_t eos_input_mods(void)
{
    uint8_t m;

    SLOCK();
    m = S.mods;
    SUNLOCK();
    return m;
}

uint32_t eos_input_held_ms(uint8_t key, uint32_t now_ms)
{
    uint32_t ms = 0;
    int i;

    SLOCK();
    i = held_find(key);
    if (i >= 0) {
        int32_t d = (int32_t)(now_ms - S.held[i].down_ms);
        ms = d > 0 ? (uint32_t)d : 1u;   // held, so never zero: zero means "up"
    }
    SUNLOCK();
    return ms;
}

bool eos_input_any_held(void)
{
    bool any;

    SLOCK();
    any = S.nheld > 0;
    SUNLOCK();
    return any;
}

void eos_input_clear_held(void)
{
    SLOCK();
    S.nheld = 0;
    S.mods  = 0;
    memset(S.prev, 0, sizeof S.prev);
    SUNLOCK();
}

// ----------------------------------------------------------------- buttons

#ifdef ESP_PLATFORM
static void buttons_configure(void)
{
    gpio_config_t io;
    int i;

    for (i = 0; i < (int)S.nbtn; i++) {
        const eos_button_t *b = &S.btn[i];
        if (!eos_pin_ok(b->pin)) continue;

        memset(&io, 0, sizeof io);
        io.pin_bit_mask = 1ULL << (unsigned)b->pin;
        io.mode         = GPIO_MODE_INPUT;
        io.pull_up_en   = (b->pull_up && b->active_low)  ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE;
        io.pull_down_en = (b->pull_up && !b->active_low) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
        io.intr_type    = GPIO_INTR_DISABLE;
        gpio_config(&io);
    }
}

static bool button_read(const eos_button_t *b)
{
    int lvl = gpio_get_level((gpio_num_t)b->pin);
    return b->active_low ? (lvl == 0) : (lvl != 0);
}
#endif

// Polled, not interrupt driven. A tact switch bounces for a few milliseconds
// and an ISR per edge would deliver that bounce as keystrokes; the frame loop
// runs often enough that sampling is both simpler and correct.
static void buttons_poll(uint32_t now_ms)
{
#ifdef ESP_PLATFORM
    int i;

    for (i = 0; i < (int)S.nbtn; i++) {
        const eos_button_t *b = &S.btn[i];
        bool raw;

        if (!eos_pin_ok(b->pin) || b->key == EOS_KEY_NONE) continue;

        raw = button_read(b);
        if (raw != S.btn_raw[i]) {
            S.btn_raw[i]   = raw;
            S.btn_since[i] = now_ms;
            continue;
        }
        if (raw == S.btn_down[i]) continue;
        if (!due(now_ms, S.btn_since[i] + BTN_DEBOUNCE_MS)) continue;

        S.btn_down[i] = raw;
        eos_input_inject_key(b->key, raw, 0, EOS_SRC_BUTTON, now_ms);
    }
#else
    (void)now_ms;   // no GPIO on the host; the injection path is still live
#endif
}

// -------------------------------------------------------------------- tick

void eos_input_tick(uint32_t now_ms)
{
    int i;

    if (!S.ready) return;   // no cfg yet: repeat and expiry timings are unset

    buttons_poll(now_ms);

    SLOCK();
    i = 0;
    while (i < (int)S.nheld) {
        held_t *h = &S.held[i];

        if (h->expire_ms && due(now_ms, h->expire_ms)) {
            uint8_t key = h->key, src = h->src;
            held_del(i);
            recalc_mods();
            emit(EOS_EV_KEY_UP, src, key, S.mods, 0, now_ms);
            continue;   // held_del moved a different entry into slot i
        }
        if (h->next_rep_ms && due(now_ms, h->next_rep_ms)) {
            h->next_rep_ms = now_ms + (S.cfg.repeat_rate_ms ? S.cfg.repeat_rate_ms : 1u);
            emit_press(EOS_EV_KEY_REPEAT, h->src, h->key, S.mods, now_ms);
        }
        i++;
    }
    SUNLOCK();

#ifdef ESP_PLATFORM
    eos_ble_tick(now_ms);
#endif
}

// -------------------------------------------------------------------- init

eos_err_t eos_input_init(const eos_input_cfg_t *cfg)
{
    eos_input_cfg_t c = cfg ? *cfg : eos_input_defaults();

    memset(&S, 0, sizeof S);
    S.cfg   = c;
    S.ready = true;

#ifdef ESP_PLATFORM
    {
        const eos_board_t *b = eos_board_get();
        int i;

        if (b) {
            S.nbtn = b->input.button_count;
            if (S.nbtn > EOS_MAX_BUTTONS) S.nbtn = EOS_MAX_BUTTONS;
            for (i = 0; i < (int)S.nbtn; i++) S.btn[i] = b->input.buttons[i];
            buttons_configure();
            for (i = 0; i < (int)S.nbtn; i++) {
                S.btn_raw[i]  = eos_pin_ok(S.btn[i].pin) ? button_read(&S.btn[i]) : false;
                S.btn_down[i] = S.btn_raw[i];
            }
        }

        // The radio comes up last and its failure is not fatal: a board with no
        // keyboard in range still has its buttons and its web page.
        if (b && b->input.ble_keyboard) (void)eos_ble_init(NULL);
    }
#endif

    return EOS_OK;
}
