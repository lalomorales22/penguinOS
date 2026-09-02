// Host test for eos_brain_bridge — the half of the brain path that had NO
// coverage at all, and hung the board while 34,220 other checks passed.
//
// WHAT WENT WRONG, and why nothing caught it. The bridge task samples the
// clock once per pass (`uint32_t now = ms_now()`), and settle() stamps
// ch_settled_ms from a LATER ms_now() during that same pass. The channel
// sweeper then compared them unsigned:
//
//     (now - B.ch_settled_ms) >= CH_ABANDON_MS
//
// With the stamp newer than `now` that underflows to about 4.29 billion, so
// the sweeper reclaimed the channel in the very pass the reply completed.
// p_read() only reports EOS_HTTPD_STREAM_END from CH_DONE, so the reader
// could never be told the reply had finished: it polled until the relay's
// idle deadline fired and printed "the brain went quiet" — AFTER the answer
// text had already arrived, which is what made an arithmetic bug look like a
// network fault.
//
// WHY IT NEEDS A SIMULATION RATHER THAN A UNIT CHECK. The bug only appears
// when three things are true together: a clock that advances while the code
// runs, a reader on a higher-priority task draining between passes, and a
// server whose last text chunk and terminating zero chunk land in separate
// recv() calls. So this file supplies all three - a virtual clock that only
// moves when the code blocks or works, a modelled send_stream() polling every
// 10 ms, and a scripted socket carrying exactly the bytes
// tools/ollama-bridge.py puts on the wire - and then runs the REAL
// eos_brain_bridge.c and the REAL eos_brain.c against them.
//
// The load-bearing case is the sweep across the server's token-to-done gap.
// Before the fix 71 of those 81 latencies ended in the bang and 5 lost the
// answer text outright; a single hand-picked delay would have passed.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>

// The shim floor first: this file DEFINES the FreeRTOS entry points that
// eos_brain_bridge.c calls, so their declarations have to be in scope before
// the definitions below, not just wherever the bridge happens to include them.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#include "eos_brain.h"
#include "eos_httpd.h"

static int checks = 0, fails = 0;

#define CK(cond, msg) do { \
    checks++; \
    if (!(cond)) { fails++; printf("    FAIL: %s\n", msg); } \
} while (0)

#define CKS(got, want, msg) do { \
    checks++; \
    if (strcmp((got), (want)) != 0) { \
        fails++; \
        printf("    FAIL: %s\n      got  [%s]\n      want [%s]\n", msg, got, want); \
    } \
} while (0)

// ============================================================ the shim floor

static int64_t g_us;
static int64_t g_next_poll_us;
static int     g_sweeps;
static int     g_verbose;

static void worker_poll(void);

// The clock only moves here, and every mover is something the real code does:
// a syscall, a log write, a blocking wait. The modelled HTTP worker runs
// whenever its 10 ms poll falls inside the interval being skipped, which is
// what makes it a higher-priority reader rather than a coroutine.
static void advance_us(int64_t d)
{
    int64_t target = g_us + d;
    while (g_next_poll_us <= target) {
        g_us = g_next_poll_us;
        g_next_poll_us += 10000;            // EOS_HTTPD_STREAM_POLL_MS
        worker_poll();
    }
    g_us = target;
}

int64_t esp_timer_get_time(void) { return g_us; }

void eos_bridge_shim_note_warning(const char *tag, const char *fmt)
{
    (void)tag;
    // The sweeper is the only warning this suite cares about, and it is
    // matched on its real text rather than on a character trick.
    if (strstr(fmt, "channel reclaimed")) g_sweeps++;
}
int eos_bridge_shim_sweeps(void)  { return g_sweeps; }
int eos_bridge_shim_verbose(void) { return g_verbose; }

static jmp_buf g_esc;
static int     g_passes;
static int     g_stop;
static int     g_abandon;          // the reader walks away after its first read
static char    g_semmem;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &g_semmem; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
void       vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }

static void *g_task = (void *)1;

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t prio, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio;
    if (out) *out = g_task;
    return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t h) { (void)h; }

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait_ms)
{
    (void)clear;
    g_passes++;
    advance_us((int64_t)wait_ms * 1000);
    if (g_abandon && g_sweeps)  longjmp(g_esc, 1);
    if (g_stop || g_passes > 20000) longjmp(g_esc, 1);
    return 0;
}

// ======================================================= the scripted socket
//
// Byte-for-byte what tools/ollama-bridge.py emits: Python's http.server
// preamble, chunked with no Content-Length, Connection: close, and a FIN
// after the terminating zero chunk.

static const char SEG_HDR[] =
    "HTTP/1.0 200 OK\r\n"
    "Server: BaseHTTP/0.6 Python/3.11\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Cache-Control: no-cache\r\n"
    "X-Accel-Buffering: no\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Connection: close\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n";
static const char SEG_TXT[] = "4\r\nBlue\r\n";
static const char SEG_END[] = "0\r\n\r\n";

static const char HEALTH_OK[] =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
    "Content-Length: 22\r\n\r\n{\"ok\":true,\"up\":12345}";

static int     f_open;
static int64_t f_open_us;
static int     f_attempt;          // 0 = the health probe, 1 = the ask
static int     f_seg;
static size_t  f_pos;
static int     g_delay_ms = 30;    // server gap between last token and done

static int f_sock_open(void *c, const char *h, uint16_t p, uint32_t t)
{
    (void)c; (void)h; (void)p; (void)t;
    f_open = 1; f_seg = 0; f_pos = 0; f_open_us = g_us;
    advance_us(300);
    return 0;
}
static int f_sock_send(void *c, const uint8_t *d, size_t n)
{ (void)c; (void)d; advance_us(200); return (int)n; }

static void f_sock_close(void *c)
{ (void)c; if (f_open) { f_open = 0; f_attempt++; } advance_us(300); }

static int f_sock_recv(void *c, uint8_t *buf, size_t cap)
{
    const char *segs[3]; size_t lens[3]; int64_t ready[3]; int nseg;
    size_t left;
    (void)c;
    if (!f_open) return EOS_BRAIN_EOF;

    if (f_attempt == 0) {
        segs[0] = HEALTH_OK; lens[0] = sizeof HEALTH_OK - 1;
        ready[0] = f_open_us + 2000;
        nseg = 1;
    } else {
        segs[0] = SEG_HDR; lens[0] = sizeof SEG_HDR - 1; ready[0] = f_open_us + 40000;
        segs[1] = SEG_TXT; lens[1] = sizeof SEG_TXT - 1; ready[1] = f_open_us + 900000;
        segs[2] = SEG_END; lens[2] = sizeof SEG_END - 1;
        ready[2] = ready[1] + (int64_t)g_delay_ms * 1000;
        nseg = 3;
    }

    if (f_seg >= nseg)          { advance_us(50); return EOS_BRAIN_EOF; }  // FIN
    if (g_us < ready[f_seg])    { advance_us(50); return 0; }              // EWOULDBLOCK

    advance_us(400);                       // the syscall, and the log it carries

    left = lens[f_seg] - f_pos;
    if (left > cap) left = cap;
    memcpy(buf, segs[f_seg] + f_pos, left);
    f_pos += left;
    if (f_pos >= lens[f_seg]) { f_seg++; f_pos = 0; }
    return (int)left;
}

static uint32_t f_sock_now(void *c) { (void)c; return (uint32_t)(g_us / 1000); }

static const eos_brain_transport_t F_TP = {
    NULL, f_sock_open, f_sock_send, f_sock_recv, f_sock_close, f_sock_now
};
static const eos_brain_hooks_t F_HK = { NULL, NULL, NULL, NULL };

// These live behind #ifdef ESP_PLATFORM in eos_brain.c, so the host build has
// to supply them.
const eos_brain_transport_t *eos_brain_lwip_transport(void) { return &F_TP; }
const eos_brain_hooks_t     *eos_brain_idf_hooks(void)      { return &F_HK; }

// ===================================================== the code under test
//
// Included, not linked: the sweeper and the channel state it guards are
// static, and this suite exists to test exactly those.

#include "eos_brain_bridge.c"

// ================================================= send_stream(), modelled
//
// The same shape as the relay in kernel/svc/eos_httpd.c: poll p_read, treat a
// positive return as data, END as clean completion, and fall back on an idle
// deadline that bangs.

static bool     w_active, w_sent_any, w_clean, w_bang;
static char     w_out[4096];
static int      w_len;
static uint32_t w_t_last;
static const char *w_why;

static void worker_poll(void)
{
    char buf[256];
    int  n;
    if (!w_active) return;

    n = p_read(NULL, buf, (int)sizeof buf);
    if (n > 0) {
        w_t_last = ms_now();
        if (w_len + n < (int)sizeof w_out) { memcpy(w_out + w_len, buf, n); w_len += n; }
        w_sent_any = true;
        if (g_abandon) w_active = false;
        return;
    }
    if (n == EOS_HTTPD_STREAM_END)  { w_clean = true; w_active = false; g_stop = 1; return; }
    if (n == EOS_HTTPD_STREAM_FAIL) { w_bang = true; w_why = "stream FAIL";
                                      w_active = false; g_stop = 1; return; }
    if ((ms_now() - w_t_last) >= EOS_HTTPD_STREAM_IDLE_MS) {
        p_cancel(NULL);
        w_bang = true; w_why = "the brain went quiet";
        w_active = false; g_stop = 1;
    }
}

// ================================================================= one run

typedef struct {
    int      delivered;
    char     text[4096];
    bool     clean_end;
    bool     bang;
    const char *why;
    int      sweeps;
    uint32_t ended_ms;
} run_t;

static void run_once(int delay_ms, int abandon, run_t *out)
{
    eos_httpd_ask_t a;
    eos_brain_bridge_cfg_t c;

    // Every static the run touches, back to zero.
    g_us = 0; g_next_poll_us = 0; g_sweeps = 0;
    g_passes = 0; g_stop = 0; g_abandon = abandon;
    f_open = 0; f_attempt = 0; f_seg = 0; f_pos = 0; f_open_us = 0;
    g_delay_ms = delay_ms;
    w_active = false; w_sent_any = false; w_clean = false; w_bang = false;
    w_len = 0; w_out[0] = 0; w_why = NULL;
    memset(&B, 0, sizeof B);

    eos_brain_bridge_start();
    eos_brain_bridge_defaults(&c);
    snprintf(c.host, sizeof c.host, "10.0.0.9");
    c.port = 80;
    eos_brain_bridge_configure(&c);
    eos_brain_bridge_set_online(true);

    memset(&a, 0, sizeof a);
    a.q = "what colour is the sky";
    w_t_last = ms_now();
    w_active = true;
    if (p_ask(NULL, &a) != 0) { printf("    FAIL: p_ask refused\n"); fails++; checks++; return; }

    if (!setjmp(g_esc)) brain_task(NULL);

    w_out[w_len] = 0;
    out->delivered = w_len;
    snprintf(out->text, sizeof out->text, "%s", w_out);
    out->clean_end = w_clean;
    out->bang      = w_bang;
    out->why       = w_why;
    out->sweeps    = g_sweeps;
    out->ended_ms  = ms_now();
}

// ==================================================================== tests

// The regression. A reply that completes must reach the reader as a clean
// END at every plausible server latency - not at one convenient one.
static void test_reply_completes_at_every_latency(void)
{
    int d, bangs = 0, lost = 0, unclean = 0;
    run_t r;

    printf("\n== a completed reply reaches the reader ==\n");

    for (d = 0; d <= 80; d++) {
        run_once(d, 0, &r);
        if (r.bang)                       bangs++;
        if (r.delivered == 0)             lost++;
        if (!r.clean_end)                 unclean++;
    }

    // Stated as totals so a failure says how wide the breakage is. Before the
    // sweeper's clock was fixed these were 71, 5 and 71.
    CK(bangs == 0,   "no server latency makes a completed reply time out");
    CK(lost == 0,    "the answer text is never lost");
    CK(unclean == 0, "every completed reply ends with a clean STREAM_END");

    if (bangs || lost || unclean)
        printf("      %d/81 banged, %d/81 lost the text, %d/81 had no clean end\n",
               bangs, lost, unclean);
}

// The specific arithmetic, pinned. settle() stamps a clock newer than the
// pass's `now`, so an unsigned compare underflows and sweeps immediately.
static void test_sweeper_does_not_fire_on_a_fresh_reply(void)
{
    run_t r;
    printf("\n== the sweeper leaves a fresh reply alone ==\n");

    run_once(15, 0, &r);
    CK(r.sweeps == 0, "a reply that just settled is not swept in the same pass");
    CK(r.clean_end,   "and the reader is told the reply finished");
    CKS(r.text, "Blue", "with the answer intact");
}

// The sweeper still has to do its actual job, or the fix traded one bug for a
// channel that never gets reclaimed.
static void test_sweeper_still_reclaims_an_abandoned_reply(void)
{
    run_t r;
    printf("\n== but it still reclaims one nobody is reading ==\n");

    run_once(15, 1, &r);
    CK(r.sweeps == 1, "a reply whose reader walked away is still reclaimed");
    CK(r.ended_ms >= CH_ABANDON_MS,
       "and not before the abandon deadline has actually elapsed");
}

// The answer text and the terminator arriving in ONE recv is the case the
// current bridge produces (it writes the final flush and the zero chunk
// back-to-back), and it is where the answer used to be lost entirely.
static void test_text_and_terminator_in_one_recv(void)
{
    run_t r;
    printf("\n== text and terminator in a single read ==\n");

    run_once(0, 0, &r);
    CKS(r.text, "Blue", "a reply whose text and terminator share a read survives");
    CK(r.clean_end, "and still ends cleanly");
}

int main(int argc, char **argv)
{
    g_verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

    test_reply_completes_at_every_latency();
    test_sweeper_does_not_fire_on_a_fresh_reply();
    test_sweeper_still_reclaims_an_abandoned_reply();
    test_text_and_terminator_in_one_recv();

    printf("\n=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
