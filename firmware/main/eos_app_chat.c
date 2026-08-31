// eos_app_chat — a megabrain conversation on the glass. The one window in
// penguinOS that is not a readout: you ask it something and the answer arrives
// a token at a time, wrapped to whatever tile it has been given.
//
// It talks to the SAME relay the web app does. eos_brain_bridge exposes the
// brain task through four function pointers on eos_httpd_ports_t — ask, read,
// cancel, status — and those four are the only lock-protected way in. Going
// round them with a second client would mean two consumers of a one-request
// state machine and a reply that could end up half in a browser and half on
// the panel. So this file borrows the server's port table and uses it exactly
// as an HTTP worker would.
//
// The non-obvious constraint that shapes the whole file: brain_read() DRAINS.
// Every byte it hands over is gone from the relay's ring, so this window must
// only call it while it owns the reply — that is, between its own successful
// ask and the END or FAIL that closes it. `owns` is that flag, and it is why
// asking while the web app is mid-reply is refused with "busy" rather than
// quietly stealing the browser's text.
//
// The second constraint is the one every window here lives under: the draw
// runs once per display band and must produce identical pixels each time. So
// the transcript is only ever appended to by the tick, the wrap is a pure
// function of the buffer and the rect, and the line index it fills is a
// deterministic scratch — recomputing it six times gives the same six answers.
//
// Nothing here allocates. The transcript, the typed line and the line index
// are fixed arrays and eos_app_chat_bytes() reports what they cost.

#include "eos_app_registry.h"
#include "eos_shell_draw.h"

#include <stdio.h>
#include <string.h>

#include "eos_font.h"
#include "eos_brain.h"

// A kilobyte of conversation. Two exchanges at the 80-word ceiling
// EOS_BRAIN_SYSTEM_TINY asks for, which is as much as anyone is going to read
// off a 240 px panel, and the oldest lines are dropped as it fills.
#ifndef EOS_CHAT_LOG_BYTES
#define EOS_CHAT_LOG_BYTES 1024
#endif

// The typed line. EOS_BRAIN_PROMPT_MAX is 384 and this is deliberately far
// below it: at eighteen columns a 96-character prompt is already five wrapped
// lines and the tile has eight.
#ifndef EOS_CHAT_INPUT_MAX
#define EOS_CHAT_INPUT_MAX 96
#endif

// Wrapped lines the index can hold. A full kilobyte at the narrowest tile —
// eighteen columns — is about fifty-seven lines, so eighty leaves headroom for
// a transcript that is mostly short lines. Past it the index keeps the LAST
// eighty, which are the only ones that can be on screen anyway.
#ifndef EOS_CHAT_MAX_LINES
#define EOS_CHAT_MAX_LINES 80
#endif

// What the arrow keys pick when there is no keyboard to type with — and they
// stay useful once there is one, because they are also the six things anybody
// tries first. Short: they are drawn on the prompt line of a 110 px tile.
static const char *const CANNED[] = {
    "who are you?",
    "tell me a penguin fact",
    "what is a tiling wm?",
    "one tip for the esp32-c6",
    "haiku about a tiny screen",
    "what is 2 to the 20th?",
};
#define CANNED_N ((int)(sizeof CANNED / sizeof CANNED[0]))

typedef enum {
    CHAT_IDLE = 0,
    CHAT_WAITING,   // asked, nothing decoded yet
    CHAT_STREAMING, // text is arriving
    CHAT_FAILED     // the last ask did not finish
} chat_st_t;

static struct {
    char     log[EOS_CHAT_LOG_BYTES];
    uint16_t len;

    char     in[EOS_CHAT_INPUT_MAX];
    uint8_t  in_len;

    int8_t   sel;        // which canned prompt is highlighted
    uint8_t  st;         // chat_st_t
    bool     owns;       // this window started the reply now in flight
    bool     dirty;
    uint16_t scroll;     // lines held back from the bottom; 0 is pinned
} C = { { 0 }, 0, { 0 }, 0, 0, CHAT_IDLE, false, false, 0 };

// The server, borrowed. NULL on a board whose httpd never started, in which
// case every ask is refused and the tile says so instead of crashing.
static eos_httpd_t *H;

// The wrap index. File-static rather than stacked because the scene already
// spends 832 bytes of the drawing task's stack on a tile array and eos_wm's
// layout recursion spends over a kilobyte more; 160 bytes of BSS is the
// cheaper end of that trade. It is written during the
// draw, which is legal precisely because it is a pure function of the
// transcript and the rect: six bands compute the same eighty numbers.
static uint16_t line_at[EOS_CHAT_MAX_LINES];

uint32_t eos_app_chat_bytes(void)
{
    return (uint32_t)(sizeof C + sizeof line_at + sizeof H);
}

void eos_app_chat_bind(eos_httpd_t *h) { H = h; }

// ------------------------------------------------------------- transcript

// How far past the byte cut a line boundary is worth looking for. A quarter of
// the buffer: far enough to find the newline at the end of a normal wrapped
// paragraph, near enough that not finding one costs a few characters off the
// top rather than the whole conversation.
#define LOG_SCAN_SPAN (EOS_CHAT_LOG_BYTES / 4)

// Drops whole lines off the front until `need` bytes are free, PREFERRING a
// line boundary but never insisting on one. Whole lines, because half a
// wrapped sentence at the top of the window reads as corruption rather than as
// scrollback that has aged out; never insisting, because the text this window
// is fed comes off a socket and is under no obligation to contain a newline at
// all.
static void log_make_room(int need)
{
    uint16_t cut, scan, stop;

    if ((int)C.len + need <= EOS_CHAT_LOG_BYTES) return;

    cut = (uint16_t)(C.len + need - EOS_CHAT_LOG_BYTES);
    if (cut < EOS_CHAT_LOG_BYTES / 4) cut = EOS_CHAT_LOG_BYTES / 4;
    if (cut > C.len) cut = C.len;

    // Round the cut UP to a line boundary, but only to one that is actually
    // within reach. The scan used to run to the end of the buffer and take
    // whatever it ended on, which is right for a transcript full of newlines
    // and catastrophic for the one input this window is fed: a megabrain reply
    // is frequently a single paragraph with no newline anywhere in it, and a
    // scan that found none left `cut` sitting at C.len — so the first time the
    // kilobyte filled, the ENTIRE transcript was thrown away and the window
    // blanked itself mid-answer. Now the scan is bounded and its result is
    // adopted only if it found something, so a paragraph with no line breaks
    // scrolls a quarter of a buffer at a time like everything else.
    stop = cut;
    if ((uint16_t)(C.len - stop) > LOG_SCAN_SPAN) stop = (uint16_t)(stop + LOG_SCAN_SPAN);
    else                                          stop = C.len;

    scan = cut;
    while (scan < stop && C.log[scan] != '\n') scan++;
    if (scan < C.len && C.log[scan] == '\n') cut = (uint16_t)(scan + 1);

    memmove(C.log, C.log + cut, (size_t)(C.len - cut));
    C.len = (uint16_t)(C.len - cut);
}

static void log_add(const char *s, int n)
{
    if (!s) return;
    if (n < 0) n = (int)strlen(s);
    if (n <= 0) return;
    if (n > EOS_CHAT_LOG_BYTES / 2) n = EOS_CHAT_LOG_BYTES / 2;

    log_make_room(n);
    memcpy(C.log + C.len, s, (size_t)n);
    C.len = (uint16_t)(C.len + n);
    C.dirty = true;
}

// ------------------------------------------------------------------- wrap
//
// Byte offsets of every wrapped line, kept as a ring so that a transcript with
// more lines than the index can hold keeps the LAST ones. Returns how many
// entries are valid and writes the index of the first of them; *total is the
// real line count, which is what the scroll position is measured against.

static int wrap(const eos_font_t *f, int16_t maxw, int *total, int *first)
{
    int i = 0, ls = 0, w = 0, brk = -1, n = (int)C.len;
    int count = 0;

    *total = 0; *first = 0;
    if (!f || maxw <= 0) return 0;

    for (;;) {
        unsigned char ch;

        if (i >= n) {
            line_at[count % EOS_CHAT_MAX_LINES] = (uint16_t)ls;
            count++;
            break;
        }
        ch = (unsigned char)C.log[i];

        if (ch == '\n') {
            line_at[count % EOS_CHAT_MAX_LINES] = (uint16_t)ls;
            count++;
            i++; ls = i; w = 0; brk = -1;
            continue;
        }

        {
            int gw = eos_font_glyph_w(f, ch) + (w ? (int)f->gap : 0);
            if (w + gw > (int)maxw && i > ls) {
                // Break at the last space when there is one on this line, and
                // mid-word only when a single word is wider than the tile.
                int cut = (brk > ls) ? brk : i;
                line_at[count % EOS_CHAT_MAX_LINES] = (uint16_t)ls;
                count++;
                ls = cut;
                while (ls < n && C.log[ls] == ' ') ls++;
                i = ls; w = 0; brk = -1;
                continue;
            }
            w += gw;
        }
        if (ch == ' ') brk = i;
        i++;
    }

    *total = count;
    if (count > EOS_CHAT_MAX_LINES) {
        *first = count - EOS_CHAT_MAX_LINES;
        return EOS_CHAT_MAX_LINES;
    }
    *first = 0;
    return count;
}

// ------------------------------------------------------------------- asks

static const char *status_text(void)
{
    switch (C.st) {
    case CHAT_WAITING:   return "thinking";
    case CHAT_STREAMING: return "answering";
    case CHAT_FAILED:    return "that one failed";
    default:             break;
    }
    if (!H || !H->ports.brain_ask) return "no brain on this board";
    return "enter asks, up/down picks";
}

static void chat_send(const char *q)
{
    eos_httpd_ask_t a;
    int rc;

    if (!q || !q[0]) return;
    if (!H || !H->ports.brain_ask) {
        log_add("\n[no megabrain client on this board]\n", -1);
        C.st = CHAT_FAILED;
        return;
    }
    if (C.owns) return;              // already ours and still running

    log_add("\n> ", -1);
    log_add(q, -1);
    log_add("\n", -1);

    memset(&a, 0, sizeof a);
    a.q          = q;
    a.model      = "";               // "" is the configured default, per eos_httpd.h
    a.system     = "";
    a.max_tokens = 0;

    rc = H->ports.brain_ask(H->ctx, &a);
    if (rc == 0) {
        C.owns = true;
        C.st   = CHAT_WAITING;
        C.scroll = 0;                // a new answer pins the view to the bottom
    } else if (rc == -8) {
        // EOS_ERR_BUSY. Somebody — almost certainly the web app — is mid-reply,
        // and stealing brain_read() from them would take their text away.
        log_add("[the brain is busy with another request]\n", -1);
        C.st = CHAT_FAILED;
    } else {
        log_add("[the ask was refused]\n", -1);
        C.st = CHAT_FAILED;
    }
    C.dirty = true;
}

// Sixteen reads of sixty-four bytes empties the relay's whole one-kilobyte
// ring in a single pass. That matters: the brain task stops pumping its socket
// when the ring is nearly full, so a slow drain here becomes TCP back-pressure
// on the mini rather than a dropped reply — correct, but it makes the answer
// arrive in visible steps.
#define CHAT_DRAIN_READS 16

void eos_app_chat_tick(uint32_t now_ms)
{
    char buf[64];
    int i;

    (void)now_ms;
    if (!C.owns || !H || !H->ports.brain_read) return;

    for (i = 0; i < CHAT_DRAIN_READS; i++) {
        int n = H->ports.brain_read(H->ctx, buf, (int)sizeof buf);

        if (n > 0) {
            C.st = CHAT_STREAMING;
            log_add(buf, n);
            continue;
        }
        if (n == EOS_HTTPD_STREAM_WAIT) break;

        // END or FAIL. Either way the channel is ours no longer.
        C.owns = false;
        if (n == EOS_HTTPD_STREAM_END) {
            log_add("\n", -1);
            C.st = CHAT_IDLE;
        } else {
            log_add("\n[the reply did not finish]\n", -1);
            C.st = CHAT_FAILED;
        }
        C.dirty = true;
        break;
    }
}

bool eos_app_chat_take_dirty(void)
{
    bool d = C.dirty;
    C.dirty = false;
    return d;
}

// ------------------------------------------------------------------- keys

bool eos_app_chat_key(const eos_event_t *e)
{
    if (!e) return false;

    // One printable character, layout and modifiers already applied by the
    // HAL. This is the whole of "make typed input work the moment the keyboard
    // does": nothing else in this file knows about scan codes.
    if (e->type == EOS_EV_TEXT) {
        if (e->ch < 0x20 || e->ch > 0x7E) return false;
        if (C.in_len + 1 >= EOS_CHAT_INPUT_MAX) return true;   // eaten, not stored
        C.in[C.in_len++] = (char)e->ch;
        C.in[C.in_len] = '\0';
        C.dirty = true;
        return true;
    }

    if (e->type != EOS_EV_KEY_DOWN && e->type != EOS_EV_KEY_REPEAT) return false;

    switch (e->key) {
    case EOS_KEY_UP:
        if (C.in_len) return false;              // typing owns the arrows
        if (C.sel > 0) C.sel--;
        C.dirty = true;
        return true;

    case EOS_KEY_DOWN:
        if (C.in_len) return false;
        if (C.sel < CANNED_N - 1) C.sel++;
        C.dirty = true;
        return true;

    case EOS_KEY_PGUP:
        C.scroll++;
        C.dirty = true;
        return true;

    case EOS_KEY_PGDN:
        if (C.scroll) C.scroll--;
        C.dirty = true;
        return true;

    case EOS_KEY_BKSP:
        if (C.in_len) { C.in[--C.in_len] = '\0'; C.dirty = true; }
        return true;

    case EOS_KEY_ENTER:
        if (C.in_len) {
            chat_send(C.in);
            C.in_len = 0;
            C.in[0] = '\0';
        } else if (C.sel >= 0 && C.sel < CANNED_N) {
            chat_send(CANNED[C.sel]);
        }
        return true;

    case EOS_KEY_ESC:
        // Escape means "stop what is happening", and what is happening is
        // either a reply or a half-typed line, in that order.
        if (C.owns && H && H->ports.brain_cancel) {
            H->ports.brain_cancel(H->ctx);
            return true;
        }
        if (C.in_len) { C.in_len = 0; C.in[0] = '\0'; C.dirty = true; }
        return true;

    default:
        break;
    }
    return false;
}

// ------------------------------------------------------------------- draw

void eos_app_draw_chat(const eos_app_ctx_t *c, eos_rect_t r)
{
    int16_t line_h, y, body_h;
    int total = 0, first = 0, rows, start, i;
    const char *prompt;
    char pbuf[EOS_CHAT_INPUT_MAX + 4];

    if (!c->ui || eos_rect_empty(r)) return;
    line_h = (int16_t)(c->ui->h + 1);

    // Two rows are reserved at the bottom for the prompt and the status. Below
    // four rows in total there is no conversation left to show, so the tile
    // says what it is instead of drawing one line of an answer.
    if (r.h < 4 * line_h) {
        eos_app_text(r.x, r.y, c->ui, c->muted, "chat", r.w);
        if (r.h >= 2 * line_h)
            eos_app_text(r.x, (int16_t)(r.y + line_h), c->ui, c->muted,
                         "too small", r.w);
        return;
    }
    body_h = (int16_t)(r.h - 2 * line_h);
    rows   = body_h / line_h;

    // The index is filled for its side effect; `first` is the only part of the
    // answer the drawing below needs, because a transcript longer than the
    // index can hold has already dropped the lines nobody can see.
    (void)wrap(c->ui, r.w, &total, &first);

    if (C.len == 0) {
        eos_app_text(r.x, r.y, c->ui, c->muted, "ask megabrain something.", r.w);
        eos_app_text(r.x, (int16_t)(r.y + line_h), c->ui, c->muted,
                     "up/down picks, enter asks.", r.w);
    } else {
        // The bottom `rows` lines, less however far the reader has scrolled
        // back. Clamped rather than wrapped: page-up at the top of a short
        // transcript should stick, not jump to the end.
        int max_scroll = total - rows;
        int scroll = (int)C.scroll;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll > max_scroll) scroll = max_scroll;

        start = total - rows - scroll;
        if (start < first) start = first;

        y = r.y;
        for (i = start; i < total && y + (int16_t)c->ui->h <= r.y + body_h; i++) {
            int off = (int)line_at[i % EOS_CHAT_MAX_LINES];
            int end = (i + 1 < total)
                        ? (int)line_at[(i + 1) % EOS_CHAT_MAX_LINES]
                        : (int)C.len;
            int len = end - off;

            if (len < 0 || off < 0 || off > (int)C.len) break;
            while (len > 0 && (C.log[off + len - 1] == '\n')) len--;
            if (len > 0) {
                // A line the wrap already fitted, so no truncation happens
                // here — but it goes through the same call as everything else
                // so that a proportional face cannot make the two disagree.
                int nfit = eos_text_fit(c->ui, C.log + off, len, (int)r.w);
                eos_display_text(r.x, y, c->ui,
                                 (C.log[off] == '>') ? c->accent : c->text,
                                 C.log + off, nfit);
            }
            y = (int16_t)(y + line_h);
        }
    }

    // The prompt line. What enter would send, which is the typed text when
    // there is any and the highlighted canned question when there is not.
    y = (int16_t)(r.y + r.h - 2 * line_h);
    if (C.in_len) {
        // The caret is drawn only on the focused tile. A blinking cursor in a
        // window the keyboard is not talking to is a lie about where the keys
        // are going, and this one does not blink for a second reason: the scene
        // is replayed once per band and anything derived from a phase counter
        // would tear.
        snprintf(pbuf, sizeof pbuf, "%s%s", C.in, c->focused ? "_" : "");
        prompt = pbuf;
    } else if (C.sel >= 0 && C.sel < CANNED_N) {
        snprintf(pbuf, sizeof pbuf, "%d/%d %s", (int)C.sel + 1, CANNED_N, CANNED[C.sel]);
        prompt = pbuf;
    } else {
        prompt = "";
    }
    eos_display_fill(eos_rect(r.x, y, r.w, line_h),
                     c->focused ? c->bunf : c->surface);
    eos_app_text(r.x, y, c->ui, c->focused ? c->text : c->muted, prompt, r.w);

    y = (int16_t)(y + line_h);
    eos_app_text(r.x, y, c->ui,
                 (C.st == CHAT_FAILED) ? c->warn
                   : (C.st == CHAT_IDLE) ? c->muted : c->accent,
                 status_text(), r.w);
}
