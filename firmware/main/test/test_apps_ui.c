// Host checks for the app registry and for every window in it.
//
// Two questions, and they are the two that a table of function pointers makes
// answerable at all. Is the table CONSISTENT — no duplicate ids, a draw
// function on every row, and /api/apps reporting exactly what the table says?
// And does every one of those draw functions stay INSIDE the rectangle it was
// handed, at every size from a rect too small to be useful up to the whole
// panel?
//
// The second one is the interesting one and it is deliberately harsh. The
// scene clips a body to its tile before calling it, so on real hardware an
// overrunning app would be caught by the clip and nobody would ever know. This
// file calls the bodies with NO clip beyond the display band, paints the whole
// screen a sentinel colour first, and then reads back every pixel outside the
// rect. Anything that moved is a bug that the clip was quietly hiding.
//
// It also checks the rule the whole scene is built on: a body is called once
// per display band and must produce identical pixels every time. Two frames
// with nothing changed in between are compared pixel for pixel, which is what
// catches a body that latched a counter or advanced a phase of its own.
//
//   cc -std=c99 -Wall -Wextra -Werror -O1 \
//      -Ikernel/hal/include -Ikernel/wm/include -Ikernel/theme/include \
//      -Ikernel/shell/include -Ikernel/font/include -Ikernel/avatar/include \
//      -Ikernel/svc/include -Iboards/generated -Ifirmware/main \
//      firmware/main/test/test_apps_ui.c firmware/main/eos_app_*.c \
//      firmware/main/eos_led.c firmware/main/eos_shell_draw.c \
//      firmware/main/eos_buddy_model.c ... -lm -o /tmp/tappsui && /tmp/tappsui

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eos_app_registry.h"
#include "eos_shell_draw.h"
#include "eos_led.h"
#include "eos_font.h"
#include "eos_apps.h"
#include "eos_storage.h"
#include "waveshare-c6-lcd-13.h"

const eos_board_t *eos_board_get(void) { return &EOS_BOARD; }

// The backend's host seam: the band it last composited, in wire order.
const uint16_t *eos_display_host_band(eos_rect_t *band);

#define W 240
#define H 240

static int checks = 0, failed = 0;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) { failed++; printf("    FAIL: %s\n", what); }
}

static void eq(long got, long want, const char *what)
{
    checks++;
    if (got != want) { failed++; printf("    FAIL: %s: got %ld want %ld\n", what, got, want); }
}

// --------------------------------------------------------------- the canvas

static uint16_t shot[H][W];
static uint16_t prev[H][W];

static eos_wm_t         wm;
static eos_theme_t      theme;
static eos_keymap_t     keys;
static eos_bar_status_t bar;
static eos_shell_view_t view;
static eos_app_ctx_t    ctx;
static eos_settings_t   settings;

static void take_band(void)
{
    eos_rect_t b;
    const uint16_t *px = eos_display_host_band(&b);
    int x, y;

    if (!px) return;
    for (y = 0; y < b.h; y++)
        for (x = 0; x < b.w; x++)
            shot[b.y + y][b.x + x] = px[y * b.w + x];
}

// A colour no theme role resolves to, so "this pixel did not change" is a
// question with an answer. 254 is the last usable palette slot — 255 is
// EOS_COLOR_NONE and never reaches a pixel.
#define SENTINEL ((eos_color_t)254)

// Paints the whole screen with the sentinel and then runs one app body over
// the top of it with NO clip but the band. Returns the number of pixels
// outside `r` that moved, which must be zero.
static int render_app(const eos_app_t *a, eos_rect_t r, bool focused)
{
    eos_rect_t band;
    uint16_t   ground;
    int x, y, out = 0;

    ctx.focused = focused;

    eos_display_damage_all();
    eos_display_frame_begin();
    while (eos_display_frame_band(&band)) {
        eos_display_fill(eos_display_clip(), SENTINEL);
        a->draw(&ctx, r);
        take_band();
    }
    eos_display_frame_end();

    // Whatever the sentinel index resolves to through the loaded CLUT. Read
    // off a corner rather than computed, so the test does not need to know how
    // the backend packs a pixel.
    ground = shot[0][0];
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) continue;
            if (shot[y][x] != ground) out++;
        }
    return out;
}

static int ink_in(eos_rect_t r)
{
    uint16_t ground = shot[0][0];
    int x, y, n = 0;

    for (y = r.y; y < r.y + r.h && y < H; y++)
        for (x = r.x; x < r.x + r.w && x < W; x++)
            if (y >= 0 && x >= 0 && shot[y][x] != ground) n++;
    return n;
}

// Every size a body has to survive. The middle four are the real tile bodies
// this panel produces — measured off eos_wm_layout(), not guessed — and the
// two at each end are the degenerate cases.
static const eos_rect_t RECTS[] = {
    {  10,  30,   0,   0 },   // empty
    {  10,  30,   1,   1 },   // one pixel
    {  10,  30,  20,  12 },   // narrower than one word
    {  10,  30,  40,  30 },   // too small for most of them to say anything
    { 124, 149, 110,  76 },   // one of five tiles, the buddy's body
    { 124,  20, 110,  91 },
    {   6,  20, 110, 203 },   // the tall left column
    {   6,  20, 228, 203 },   // the only window on the panel
};
#define NRECTS ((int)(sizeof RECTS / sizeof RECTS[0]))

// ============================================================== the table

static void test_table(void)
{
    int i, j, n = eos_app_count();

    printf("\n== the table ==\n");
    eq(n, EOS_APP_COUNT, "the count is the enum");
    ck(eos_app_table_ok(), "the table passes its own consistency check");
    ck(n <= EOS_APPS_CATALOG_MAX, "and fits the catalog /api/apps can report");

    for (i = 0; i < n; i++) {
        const eos_app_t *a = eos_app_at(i);
        ck(a != NULL, "every index resolves");
        if (!a) continue;
        ck(a->id && a->id[0], "every entry has an id");
        ck(a->name && a->name[0], "every entry has a name");
        ck(a->summary && a->summary[0], "every entry has a summary");
        ck(a->draw != NULL, "every entry has a draw function");

        // A tab cell in a group of eight is fourteen pixels on this panel.
        // Nothing here can be fully legible at that size, but a name over eight
        // characters is one nobody will recognise from its first two either.
        ck(strlen(a->name) <= 8, "the name is short enough to be a tab label");
        ck(strlen(a->summary) < 96, "the summary fits a picker row");

        eq(eos_app_index_of(a->id), i, "the id round-trips to its index");
        ck(eos_app_by_id(a->id) == a, "and to its entry");

        for (j = 0; j < i; j++)
            ck(strcmp(a->id, eos_app_at(j)->id) != 0, "no two entries share an id");
    }

    ck(eos_app_at(-1) == NULL, "a negative index is NULL");
    ck(eos_app_at(n) == NULL, "and so is one past the end");
    eq(eos_app_index_of("nope"), -1, "an unknown id is -1");
    eq(eos_app_index_of(""), -1, "and so is an empty one");
    eq(eos_app_index_of(NULL), -1, "and so is NULL");
    ck(eos_app_by_id("nope") == NULL, "which by_id agrees with");

    // The five that were verified on hardware keep their numbers, because
    // main.c indexes win_of[] by them and sys.autostart resolves through them.
    eq(eos_app_index_of("clock"), EOS_APP_CLOCK, "clock is still 0");
    eq(eos_app_index_of("board"), EOS_APP_BOARD, "board is still 1");
    eq(eos_app_index_of("heap"),  EOS_APP_HEAP,  "heap is still 2");
    eq(eos_app_index_of("keys"),  EOS_APP_KEYS,  "keys is still 3");
    eq(eos_app_index_of("buddy"), EOS_APP_BUDDY, "buddy is still 4");

    // And the six the owner named are all there, spelled the way they will be
    // typed into sys.autostart.
    ck(eos_app_by_id("chat")     != NULL, "chat is in the table");
    ck(eos_app_by_id("settings") != NULL, "settings is in the table");
    ck(eos_app_by_id("files")    != NULL, "files is in the table");
    ck(eos_app_by_id("media")    != NULL, "media is in the table");
    ck(eos_app_by_id("party")    != NULL, "party is in the table");

    // eos_shell_app_names() is the same list restacked, and the launcher, the
    // tab strip and the bar all read it. It used to be a second array.
    {
        const char *const *names = eos_shell_app_names();
        for (i = 0; i < n; i++)
            ck(strcmp(names[i], eos_app_at(i)->name) == 0,
               "the name array is the table");
    }
}

// ==================================================== /api/apps is the table

static eos_httpd_t     HH;
static eos_httpd_req_t RQ;
static eos_httpd_resp_t RS;
static eos_apps_app_t  CAT[EOS_APP_COUNT];
static char            BODY[4096];

static int api_apps(void)
{
    int status;

    memset(&RQ, 0, sizeof RQ);
    memset(&RS, 0, sizeof RS);
    RQ.method = "GET";
    RQ.uri    = "/api/apps";

    status = eos_httpd_dispatch(&HH, &RQ, &RS);
    BODY[0] = '\0';
    if (RS.body && RS.body_len > 0) {
        int n = RS.body_len < (int)sizeof BODY - 1 ? RS.body_len : (int)sizeof BODY - 1;
        memcpy(BODY, RS.body, (size_t)n);
        BODY[n] = '\0';
    }
    return status;
}

static void test_api(void)
{
    eos_httpd_cfg_t cfg;
    int i, n = eos_app_count(), listed = 0;
    const char *m;
    char want[64];

    printf("\n== /api/apps ==\n");

    eos_httpd_cfg_default(&cfg);
    cfg.mode = EOS_HTTPD_MODE_RUN;
    eos_httpd_init(&HH, NULL, NULL, &cfg);
    eos_apps_init(NULL, NULL);
    eos_httpd_set_api(eos_apps_dispatch);

    // Built exactly the way main.c's build_app_catalog() builds it, which is
    // the point of the check: if this loop and that one can be written the same
    // way, there is one list.
    for (i = 0; i < n; i++) {
        const eos_app_t *a = eos_app_at(i);
        CAT[i].id       = a->id;
        CAT[i].name     = a->name;
        CAT[i].summary  = a->summary;
        CAT[i].tier_min = a->tier_min;
    }
    eos_apps_set_apps(CAT, n);

    eq(api_apps(), 200, "the catalog answers");
    for (m = BODY; (m = strstr(m, "\"id\":")); m++) listed++;
    eq(listed, n, "every row in the table is reported and no more");

    for (i = 0; i < n; i++) {
        snprintf(want, sizeof want, "\"id\":\"%s\"", eos_app_at(i)->id);
        ck(strstr(BODY, want) != NULL, "each id from the table is in the JSON");
    }
    ck(strstr(BODY, "megabrain") != NULL, "and the summaries come with them");
    ck(strstr(BODY, "\"summary\":null") == NULL, "with no empty summary anywhere");
}

// ============================================================ the rendering

static void test_rendering(void)
{
    int i, k, n = eos_app_count();

    printf("\n== every app into every rect ==\n");

    for (i = 0; i < n; i++) {
        const eos_app_t *a = eos_app_at(i);
        int worst = 0, ink_big = 0;

        for (k = 0; k < NRECTS; k++) {
            int out = render_app(a, RECTS[k], k % 2 == 0);
            if (out > worst) worst = out;
            if (k == NRECTS - 1) ink_big = ink_in(RECTS[k]);

            // And again, unchanged, into the same rect: the body must be a
            // pure function of the state it is given.
            memcpy(prev, shot, sizeof prev);
            render_app(a, RECTS[k], k % 2 == 0);
            ck(memcmp(prev, shot, sizeof shot) == 0,
               "two identical frames come out identical");
        }

        eq(worst, 0, "nothing is drawn outside the rect it was given");

        // The full-screen rect. Every window has something to say at 228x203,
        // even the ones whose hardware is absent — "no LED on this board" is
        // ink too, and a window that drew nothing at that size would be a
        // window nobody could tell was open.
        ck(ink_big > 40, "and something is drawn at full screen");
        printf("    %-9s worst overrun %d px, %5d px of ink at 228x203\n",
               a->name, worst, ink_big);
    }
}

// The tiny rects deserve their own statement: an app that cannot be useful
// small must SAY so rather than draw a mess, and the way to tell those two
// apart is that saying so leaves ink and a mess leaves ink outside the box.
static void test_small(void)
{
    eos_rect_t tiny = RECTS[3];    // 40x30
    int i, n = eos_app_count();

    printf("\n== 40x30, the size nothing really fits ==\n");
    for (i = 0; i < n; i++) {
        const eos_app_t *a = eos_app_at(i);
        int out = render_app(a, tiny, true);
        int ink = ink_in(tiny);
        eq(out, 0, "still inside its rect at 40x30");
        ck(ink > 0, "and still says something rather than going blank");
    }
}

// ================================================================== the LED

static void test_led(void)
{
    eos_led_state_t st;
    uint8_t r1, g1, b1, r2, g2, b2;
    int i;

    printf("\n== the light ==\n");

    memset(&st, 0, sizeof st);
    st.fx = EOS_LED_FX_OFF; st.hue = 0; st.sat = 255; st.bright = 255;
    eos_led_color_at(&st, 1234, &r1, &g1, &b1);
    eq(r1 + g1 + b1, 0, "off is dark whatever the brightness says");

    st.fx = EOS_LED_FX_SOLID;
    eos_led_color_at(&st, 1234, &r1, &g1, &b1);
    eq(r1, 255, "solid at hue 0 is full red");
    ck(g1 < 8 && b1 < 8, "and nothing else");

    // The whole reason the colour function is pure: the panel draws the swatch
    // from it and the driver writes the LED from it, at different moments.
    for (i = 0; i < EOS_LED_FX_COUNT; i++) {
        st.fx = (uint8_t)i;
        eos_led_color_at(&st, 7777, &r1, &g1, &b1);
        eos_led_color_at(&st, 7777, &r2, &g2, &b2);
        ck(r1 == r2 && g1 == g2 && b1 == b2, "the same clock gives the same colour");
        ck(eos_led_fx_name(i)[0] != '?', "every effect has a name");
    }
    ck(eos_led_fx_name(-1)[0] == '?', "and an out-of-range one does not");
    ck(eos_led_fx_name(EOS_LED_FX_COUNT)[0] == '?', "at either end");

    ck(!eos_led_fx_animated(EOS_LED_FX_OFF),   "off does not animate");
    ck(!eos_led_fx_animated(EOS_LED_FX_SOLID), "and neither does solid");
    ck(eos_led_fx_animated(EOS_LED_FX_RAINBOW), "rainbow does");

    // Strobe is on for 60 ms of every 240 and dark for the rest, which is what
    // makes it read as a flash rather than as a flicker.
    st.fx = EOS_LED_FX_STROBE;
    eos_led_color_at(&st, 1000, &r1, &g1, &b1);   /* 1000 % 240 == 40  -> lit  */
    eos_led_color_at(&st, 1100, &r2, &g2, &b2);   /* 1100 % 240 == 140 -> dark */
    ck(r1 > 0, "the strobe is lit inside its window");
    eq(r2 + g2 + b2, 0, "and dark outside it");

    // Rainbow must actually go round rather than sitting on one colour.
    {
        int distinct = 0;
        uint8_t seen_r = 0;
        st.fx = EOS_LED_FX_RAINBOW;
        for (i = 0; i < 6; i++) {
            eos_led_color_at(&st, (uint32_t)i * 1000u, &r1, &g1, &b1);
            if (i == 0 || r1 != seen_r) distinct++;
            seen_r = r1;
        }
        ck(distinct >= 3, "rainbow moves through the wheel");
    }

    // Saturation 0 is white at every hue, which is the one HSV case an integer
    // implementation gets wrong by an off-by-one in the sector arithmetic.
    for (i = 0; i < 256; i += 17) {
        eos_led_hsv((uint8_t)i, 0, 200, &r1, &g1, &b1);
        ck(r1 == g1 && g1 == b1, "saturation zero is grey at every hue");
    }
}

// ================================================================= the chat

static void key_down(uint16_t k)
{
    eos_event_t e;
    memset(&e, 0, sizeof e);
    e.type = EOS_EV_KEY_DOWN;
    e.key  = (uint8_t)k;
    eos_app_key(EOS_APP_CHAT, &e);
}

static void key_text(char ch)
{
    eos_event_t e;
    memset(&e, 0, sizeof e);
    e.type = EOS_EV_TEXT;
    e.ch   = (uint16_t)(unsigned char)ch;
    eos_app_key(EOS_APP_CHAT, &e);
}

// A megabrain that answers in one unbroken paragraph and never sends a
// newline. It is the shape of reply this window is actually fed and the shape
// the transcript trimmer used to erase itself on.
static int FAKE_left;

static int fake_ask(void *ctx, const eos_httpd_ask_t *a)
{
    (void)ctx; (void)a;
    FAKE_left = 10240;
    return 0;
}

static int fake_read(void *ctx, char *buf, int cap)
{
    int n, i;

    (void)ctx;
    if (FAKE_left <= 0) return EOS_HTTPD_STREAM_END;
    n = (cap < 64) ? cap : 64;
    if (n > FAKE_left) n = FAKE_left;
    for (i = 0; i < n; i++) buf[i] = (char)('a' + ((FAKE_left + i) % 26));
    FAKE_left -= n;
    return n;
}

static void test_chat(void)
{
    const eos_app_t *a = eos_app_at(EOS_APP_CHAT);
    eos_rect_t big = RECTS[NRECTS - 1];
    int i, out;

    printf("\n== chat ==\n");

    // The arrow keys move the canned selection even with no keyboard ever
    // bonded and no megabrain on the LAN, which is the whole point of them.
    ck(eos_app_key(EOS_APP_CHAT, NULL) == false, "a NULL event is refused");
    key_down(EOS_KEY_DOWN);
    key_down(EOS_KEY_DOWN);
    render_app(a, big, true);
    ck(ink_in(big) > 40, "the picker draws with nothing else set up");

    // Enter with no brain bound is a refusal that says so in the transcript
    // rather than a crash or a silence.
    key_down(EOS_KEY_ENTER);
    out = render_app(a, big, true);
    eq(out, 0, "an ask with no brain stays inside the rect");
    ck(ink_in(big) > 100, "and puts something on the glass");

    // Typing. Every character goes in through EOS_EV_TEXT, which is what the
    // HAL emits once a keyboard is delivering reports.
    for (i = 0; i < 20; i++) key_text((char)('a' + (i % 26)));
    key_text(' ');
    for (i = 0; i < 20; i++) key_text((char)('A' + (i % 26)));
    out = render_app(a, big, true);
    eq(out, 0, "a typed line stays inside the rect");

    key_down(EOS_KEY_BKSP);
    key_down(EOS_KEY_ENTER);

    // Overfill the transcript several times over. The wrap index holds eighty
    // lines and the log a kilobyte, and neither may be walked off the end by a
    // conversation that outgrows them.
    for (i = 0; i < 60; i++) {
        int j;
        for (j = 0; j < 30; j++) key_text((char)('a' + (j % 26)));
        key_down(EOS_KEY_ENTER);
    }
    for (i = 0; i < NRECTS; i++) {
        out = render_app(a, RECTS[i], true);
        eq(out, 0, "an overfilled transcript still stays inside every rect");
    }

    // Scrolling back and forward past both ends is clamped, not wrapped.
    for (i = 0; i < 200; i++) key_down(EOS_KEY_PGUP);
    out = render_app(a, big, true);
    eq(out, 0, "scrolled to the top, still inside");
    ck(ink_in(big) > 100, "and still showing text");
    for (i = 0; i < 400; i++) key_down(EOS_KEY_PGDN);
    out = render_app(a, big, true);
    eq(out, 0, "scrolled to the bottom, still inside");

    // A word longer than the tile is broken mid-word rather than looping
    // forever looking for a space to break on.
    for (i = 0; i < 90; i++) key_text('W');
    out = render_app(a, RECTS[2], true);   /* 20x12, three characters wide */
    eq(out, 0, "a word wider than the tile still stays inside it");

    // A reply with NO NEWLINE IN IT, which is what a megabrain answer usually
    // is: eighty words of one paragraph. The transcript trimmer prefers a line
    // boundary, and the version of it that INSISTED on one scanned to the end
    // of the buffer, found nothing, and threw the whole conversation away the
    // first time the kilobyte filled - the window blanking itself mid-answer
    // and starting again, over and over, for the length of the reply.
    //
    // Ten kilobytes of unbroken text through the same door the socket uses.
    {
        int (*save_ask)(void *, const eos_httpd_ask_t *)  = HH.ports.brain_ask;
        int (*save_read)(void *, char *, int)             = HH.ports.brain_read;
        int worst = 0, best = 0, t;

        HH.ports.brain_ask  = fake_ask;
        HH.ports.brain_read = fake_read;
        eos_app_chat_bind(&HH);

        key_down(EOS_KEY_ENTER);          /* asks the fake brain */

        for (t = 0; t < 40; t++) {
            int ink;

            eos_app_chat_tick((uint32_t)(50000 + t * 100));
            out = render_app(a, big, true);
            eq(out, 0, "an unbroken reply stays inside the rect");
            ink = ink_in(big);
            if (ink > best) best = ink;
            /* Only judged once the kilobyte has actually filled and started
               scrolling, which is the first few ticks. */
            if (t >= 12 && (worst == 0 || ink < worst)) worst = ink;
        }

        ck(best > 400, "the unbroken reply put a screenful of text on the glass");
        ck(worst * 2 >= best,
           "and the transcript scrolled rather than blanking itself");

        HH.ports.brain_ask  = save_ask;
        HH.ports.brain_read = save_read;
        eos_app_chat_bind(&HH);
    }
}

// ================================================================ the files

static void test_files(void)
{
    const eos_app_t *a = eos_app_at(EOS_APP_FILES);
    eos_event_t e;
    int i, out;

    printf("\n== files ==\n");

    // A scan against the host mount, which the harness pointed at a real
    // directory below. Every rect, before and after walking into and out of
    // whatever is there.
    eos_app_files_tick(true, 1000);
    for (i = 0; i < NRECTS; i++) {
        out = render_app(a, RECTS[i], true);
        eq(out, 0, "the listing stays inside its rect");
    }

    memset(&e, 0, sizeof e);
    e.type = EOS_EV_KEY_DOWN;

    // Down past the end, up past the start, into a directory, and out of it
    // further than there is filesystem to leave.
    e.key = EOS_KEY_DOWN;  for (i = 0; i < 40; i++) eos_app_key(EOS_APP_FILES, &e);
    e.key = EOS_KEY_UP;    for (i = 0; i < 40; i++) eos_app_key(EOS_APP_FILES, &e);
    e.key = EOS_KEY_ENTER; eos_app_key(EOS_APP_FILES, &e);
    eos_app_files_tick(true, 9000);
    out = render_app(a, RECTS[NRECTS - 1], true);
    eq(out, 0, "and so does whatever enter opened");

    e.key = EOS_KEY_BKSP;  for (i = 0; i < 12; i++) {
        eos_app_key(EOS_APP_FILES, &e);
        eos_app_files_tick(true, (uint32_t)(20000 + i * 4000));
    }
    out = render_app(a, RECTS[NRECTS - 1], true);
    eq(out, 0, "backspacing past the root stays inside the rect");
    ck(ink_in(RECTS[NRECTS - 1]) > 20, "and still draws a path");

    e.key = EOS_KEY_ESC; eos_app_key(EOS_APP_FILES, &e);
    eos_app_files_tick(true, 90000);
    out = render_app(a, RECTS[NRECTS - 1], true);
    eq(out, 0, "and so does escaping back to the root");

    ck(eos_app_key(EOS_APP_FILES, NULL) == false, "a NULL event is refused");
}

// ================================================================ the party

static void test_party(void)
{
    const eos_app_t *a = eos_app_at(EOS_APP_PARTY);
    eos_led_state_t before, during, after;
    uint32_t t;
    int i, out;

    printf("\n== party ==\n");

    // What the Media window was holding must come back afterwards. A demo that
    // silently rewrote somebody's lighting would be a demo you only run once.
    before.fx = EOS_LED_FX_SOLID; before.hue = 40; before.sat = 200; before.bright = 90;
    eos_led_set(&before);

    eos_app_party_tick(true, 1000, NULL);
    ck(eos_app_party_active(), "focusing the tile starts the party");
    eos_led_get(&during);
    ck(during.fx != before.fx, "and takes the light");

    for (t = 1000; t < 12000; t += 100) {
        eos_app_party_tick(true, t, NULL);
        for (i = 0; i < NRECTS; i++) {
            out = render_app(a, RECTS[i], true);
            if (out != 0) break;
        }
        if (out != 0) break;
    }
    eq(out, 0, "eleven seconds of party stays inside every rect");

    eos_app_party_tick(false, 12100, NULL);
    ck(!eos_app_party_active(), "and it stops when the tile goes away");
    eos_led_get(&after);
    eq(after.fx, before.fx, "the light goes back to what it was");
    eq(after.hue, before.hue, "hue included");
    eq(after.bright, before.bright, "and brightness");

    ck(eos_app_party_key(NULL) == false, "the party takes no keys");
    eq((long)eos_app_party_phase(), 0, "and its phase resets when it closes");
}

// ================================================== the tick and the damage

// Everything the OS loop actually calls, driven against a real window tree.
// The point is the visibility gate: an app that is behind a tab must not be
// damaged, must not be scanned, and must not be holding the LED.
static void test_wiring(void)
{
    eos_rect_t screen = eos_board_screen(eos_board_get());
    int party_win;

    printf("\n== the tick ==\n");

    eos_wm_init(&wm, &wm.cfg);
    eos_wm_open(&wm, EOS_APP_CLOCK, screen);
    eos_wm_open(&wm, EOS_APP_CHAT,  screen);

    eos_app_tick(&view, 100000);
    ck(!eos_app_wants_fast(), "a desktop with no animation asks for no extra frames");
    ck(!eos_app_party_active(), "and the party is not running");

    // The party, focused. It should take the light, ask for the faster loop,
    // and declare damage on every pass.
    party_win = eos_wm_open(&wm, EOS_APP_PARTY, screen);
    ck(party_win >= 0, "the party window opens");
    eos_app_tick(&view, 100100);
    ck(eos_app_party_active(), "and starts as soon as it is visible");
    ck(eos_app_wants_fast(), "and asks the loop to run at the avatar's rate");
    ck(eos_app_damage(&view), "and declares damage every pass");

    // On to another workspace, where none of the three is visible.
    eos_wm_goto_workspace(&wm, 1);
    eos_app_tick(&view, 100200);
    ck(!eos_app_party_active(), "leaving the workspace stops the party");
    ck(!eos_app_wants_fast(), "and gives the idle rate back");
    eos_wm_goto_workspace(&wm, 0);

    // An app with no key function refuses everything, which is what lets the
    // key fall through to nothing rather than being eaten.
    {
        eos_event_t e;
        memset(&e, 0, sizeof e);
        e.type = EOS_EV_KEY_DOWN;
        e.key  = EOS_KEY_DOWN;
        ck(!eos_app_key(EOS_APP_BUDDY, &e), "a window with no keys takes none");
        ck(!eos_app_key((uint16_t)EOS_APP_COUNT, &e), "and neither does a bad id");
        ck(!eos_app_key(EOS_APP_CHAT, NULL), "and neither does a NULL event");
    }

    // A NULL view must not crash the tick: it is what the loop holds before the
    // desktop exists, and eos_app_tick() is called from the loop and not from
    // inside the desktop branch.
    eos_app_tick(NULL, 100300);
    ck(true, "a tick with no view is survivable");
}

// =================================================================== main

int main(void)
{
    eos_wm_cfg_t cfg;
    const eos_board_t *b = eos_board_get();

    // A real directory for the file browser to walk. It is this repo's own
    // firmware/main, which has subdirectories and files of several sizes and is
    // guaranteed to be there when the suite runs.
    setenv("EOS_STORAGE_HOST_ROOT", "firmware/main", 1);

    if (eos_display_init() != EOS_OK) { printf("display init failed\n"); return 1; }
    eos_theme_default(&theme);
    {
        // The CLUT, the same 32 entries at a time the boot glue uploads it in.
        // Without it every palette index resolves through whatever the backend
        // seeded itself with and the sentinel could collide with a role.
        enum { CHUNK = 32 };
        uint32_t rgb[CHUNK];
        int i, j;
        for (i = 0; i < EOS_PAL_SIZE; i += CHUNK) {
            for (j = 0; j < CHUNK; j++) {
                eos_rgb_t c = eos_theme_palette_rgb(&theme, (uint8_t)(i + j));
                rgb[j] = eos_rgb(c.r, c.g, c.b);
            }
            eos_display_palette(rgb, (uint16_t)i, (uint16_t)CHUNK);
        }
    }
    eos_storage_init();

    memset(&cfg, 0, sizeof cfg);
    cfg.min_tile_w = b->render.min_tile_w;
    cfg.min_tile_h = b->render.min_tile_h;
    cfg.gap        = theme.m.gap;
    cfg.bar_h      = theme.m.bar_h;
    cfg.tab_h      = theme.m.tab_h;
    eos_wm_init(&wm, &cfg);
    eos_keys_defaults(&keys);
    eos_bar_status_init(&bar);

    memset(&view, 0, sizeof view);
    view.theme        = &theme;
    view.wm           = &wm;
    view.bar          = &bar;
    view.keys         = &keys;
    view.buddy        = NULL;
    view.heap_free    = 114688;
    view.heap_largest = 94208;
    view.uptime_ms    = 3723456;
    view.board_line[0] = "waveshare c6";
    view.board_line[1] = "192.168.0.160";
    view.board_line[2] = "esp32-c6, 4 MB";
    view.board_line[3] = "st7789 240x240";

    // The ctx the scene would build. Resolved once here rather than per draw,
    // exactly as eos_shell_draw.c does it.
    memset(&ctx, 0, sizeof ctx);
    ctx.view   = &view;
    ctx.ui     = eos_font_get(eos_font_id_from_name(eos_theme_font(&theme)));
    ctx.tiny   = eos_font_get(EOS_FONT_TINY);
    ctx.med    = eos_font_get(EOS_FONT_MED);
    ctx.big    = eos_font_get(EOS_FONT_BIG);
    if (!ctx.tiny) ctx.tiny = ctx.ui;
    if (!ctx.med)  ctx.med  = ctx.ui;
    if (!ctx.big)  ctx.big  = ctx.ui;
    ctx.border   = theme.m.border > 0 ? theme.m.border : 1;
    ctx.bg       = eos_theme_role_index(&theme, EOS_ROLE_BG);
    ctx.surface  = eos_theme_role_index(&theme, EOS_ROLE_SURFACE);
    ctx.overlay  = eos_theme_role_index(&theme, EOS_ROLE_OVERLAY);
    ctx.text     = eos_theme_role_index(&theme, EOS_ROLE_TEXT);
    ctx.muted    = eos_theme_role_index(&theme, EOS_ROLE_MUTED);
    ctx.accent   = eos_theme_role_index(&theme, EOS_ROLE_ACCENT);
    ctx.bfoc     = eos_theme_role_index(&theme, EOS_ROLE_BORDER_FOCUSED);
    ctx.bunf     = eos_theme_role_index(&theme, EOS_ROLE_BORDER_UNFOCUSED);
    ctx.barbg    = eos_theme_role_index(&theme, EOS_ROLE_BAR_BG);
    ctx.barfg    = eos_theme_role_index(&theme, EOS_ROLE_BAR_FG);
    ctx.tabact   = eos_theme_role_index(&theme, EOS_ROLE_TAB_ACTIVE);
    ctx.tabinact = eos_theme_role_index(&theme, EOS_ROLE_TAB_INACTIVE);
    ctx.ok       = eos_theme_role_index(&theme, EOS_ROLE_OK);
    ctx.warn     = eos_theme_role_index(&theme, EOS_ROLE_WARN);
    ck(ctx.ui != NULL, "the theme's ui face is linked into this image");
    if (!ctx.ui) { printf("no font; nothing below can run\n"); return 1; }

    // Before the bind, Settings has nothing to show and has to say so rather
    // than draw a blank tile. It is the state a board with no settings file
    // and no store is in, and it is one line of code away from being a crash.
    {
        const eos_app_t *a = eos_app_at(EOS_APP_SETTINGS);
        eq(render_app(a, RECTS[NRECTS - 1], true), 0,
           "settings with no store stays inside its rect");
        ck(ink_in(RECTS[NRECTS - 1]) > 10, "and says so rather than going blank");
    }

    // And then wired the way app_main wires it. eos_app_bind() also claims the
    // LED, which on the host is a no-op sink that still records frames, so the
    // Media and Party windows below take the present-hardware path.
    memset(&settings, 0, sizeof settings);
    snprintf(settings.ui_theme,    sizeof settings.ui_theme,    "%s", "cyd-amber");
    snprintf(settings.net_host,    sizeof settings.net_host,    "%s", "penguinos");
    snprintf(settings.brain_host,  sizeof settings.brain_host,  "%s", "192.168.0.139");
    snprintf(settings.brain_model, sizeof settings.brain_model, "%s", "qwen3.5:2b");
    settings.brain_port = 80;
    settings.ui_bright  = 204;
    eos_app_bind(&HH, &settings, b, NULL);
    ck(eos_app_settings() == &settings, "the settings pointer is what was bound");
    ck(eos_app_board() == b, "and so is the board");
    ck(eos_led_present(), "the host LED sink is up, so the light paths are live");

    {
        const eos_app_t *a = eos_app_at(EOS_APP_SETTINGS);
        int before = ink_in(RECTS[NRECTS - 1]);
        eq(render_app(a, RECTS[NRECTS - 1], true), 0,
           "settings with a store stays inside its rect");
        ck(ink_in(RECTS[NRECTS - 1]) > before,
           "and draws more once there is something to draw");
    }

    test_table();
    test_api();
    test_rendering();
    test_small();
    test_led();
    test_chat();
    test_files();
    test_party();
    test_wiring();

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed != 0;
}
