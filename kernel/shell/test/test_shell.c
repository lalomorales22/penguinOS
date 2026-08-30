// Host test for the shell. Two halves.
//
// First it drives a real eos_wm through scripted keypresses - the same chords
// the owner will actually type - and asserts the window tree lands where it
// should: opening, forced splits, focus, moving a window, workspaces, tab
// groups, resize, close.
//
// Then it builds the status bar at 480, 320, 172, 128, 96 and 64 pixels and
// prints what survives, because the only way to know whether a 128px bar is
// still readable is to look at it. The asserts guard the two invariants that
// the eye cannot check: the bar never runs past its width, and it never keeps
// a segment while dropping a higher-priority one.

#include <stdio.h>
#include <string.h>
#include "eos_wm.h"
#include "eos_keys.h"
#include "eos_bar.h"

static int checks = 0, fails = 0;
#define CK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("    FAIL: %s\n", msg); } } while (0)

static const char *const APPNAME[] = { "term", "buddy", "files", "settings", "arcade", "cpm" };
#define NAPPS ((int)(sizeof(APPNAME) / sizeof(APPNAME[0])))

static eos_keymap_t     KM;
static eos_wm_t         WM;
static eos_shell_state_t ST;
static eos_rect_t       SCR;

// ------------------------------------------------------------------ helpers
//
// cc -std=c99 -Wall -Wextra -O1 -Ikernel/wm/include -Ikernel/hal/include \
//    -Ikernel/shell/include kernel/wm/eos_wm.c kernel/shell/eos_keys.c \
//    kernel/shell/eos_bar.c kernel/shell/test/test_shell.c -o /tmp/test_shell

static const eos_wm_cfg_t CFG_CYD  = { 80, 40, 2, 12, 10 };   // 320x240 ILI9341
static const eos_wm_cfg_t CFG_OLED = { 60, 20, 1,  8,  8 };   // 128x64 SSD1306

static void session(const eos_wm_cfg_t *cfg, eos_rect_t screen)
{
    eos_keys_defaults(&KM);
    eos_wm_init(&WM, cfg);
    eos_shell_state_init(&ST, 3);
    SCR = screen;
}

static eos_key_result_t press(const char *chord)
{
    eos_key_result_t r = { false, false, EOS_ACT_NONE, 0, EOS_NONE };
    uint8_t  mods;
    uint16_t key;
    if (!eos_keys_parse_chord(chord, &mods, &key)) {
        checks++; fails++;
        printf("    FAIL: test used an unparseable chord \"%s\"\n", chord);
        return r;
    }
    r = eos_keys_feed(&KM, &WM, &ST, SCR, mods, key);
    printf("    %-18s %-18s %-8s focus=%-3d\n", chord,
           eos_keys_action_name(r.action),
           r.handled ? (r.changed ? "changed" : "no-op") : "unbound",
           (int)WM.focus);
    return r;
}

static int tiles(eos_tile_t *out) { return eos_wm_layout(&WM, SCR, out, EOS_MAX_WINDOWS * 2); }

// The layout entry for one window, or a zeroed tile with win = EOS_NONE.
static eos_tile_t tile_of(int win)
{
    eos_tile_t t[EOS_MAX_WINDOWS * 2];
    eos_tile_t none;
    int n = tiles(t);
    memset(&none, 0, sizeof(none));
    none.win = EOS_NONE;
    for (int i = 0; i < n; i++) if (t[i].win == win) return t[i];
    return none;
}

static int live_windows(void)
{
    int c = 0;
    for (int i = 0; i < EOS_MAX_WINDOWS; i++) if (WM.win[i].alive) c++;
    return c;
}

static int windows_on_ws(void)
{
    eos_tile_t t[EOS_MAX_WINDOWS * 2];
    return tiles(t);
}

// ------------------------------------------------------------- the key table

static void show_table(void)
{
    eos_keymap_t km;
    char chord[32];
    eos_keys_defaults(&km);

    printf("\n=== default keybinds (%d of %d slots) ===\n", (int)km.count, EOS_MAX_BINDS);
    for (int i = 0; i < km.count; i++) {
        uint8_t  m2;
        uint16_t k2;
        eos_keys_format(&km.binds[i], chord, (int)sizeof(chord));
        printf("    %-20s %-20s", chord, eos_keys_action_name((eos_action_t)km.binds[i].action));
        if (km.binds[i].arg) printf(" %d", (int)km.binds[i].arg);
        printf("\n");
        CK(eos_keys_parse_chord(chord, &m2, &k2) &&
           m2 == km.binds[i].mods && k2 == km.binds[i].key,
           "formatted chord parses back to the same bind");
    }
    CK(eos_keys_lookup(&km, EOS_MOD_SUPER, EOS_KEY_ENTER) != NULL, "super+return is bound");
    CK(eos_keys_lookup(&km, 0, EOS_KEY_A) == NULL, "plain letters are not bound");

    // The HAL delivers the raw HID modifier byte, one bit per physical key.
    CK(eos_keys_lookup(&km, EOS_MOD_LGUI, EOS_KEY_ENTER) != NULL, "left super matches");
    CK(eos_keys_lookup(&km, EOS_MOD_RGUI, EOS_KEY_ENTER) != NULL, "right super matches");
    CK(eos_keys_lookup(&km, (uint8_t)(EOS_MOD_LGUI | EOS_MOD_RSHIFT), EOS_KEY_1) ==
       eos_keys_lookup(&km, (uint8_t)(EOS_MOD_SUPER | EOS_MOD_SHIFT), EOS_KEY_1),
       "either shift key reaches the same bind");
    {
        const eos_keybind_t *plain = eos_keys_lookup(&km, EOS_MOD_SUPER, EOS_KEY_1);
        const eos_keybind_t *sh    = eos_keys_lookup(&km,
                                       (uint8_t)(EOS_MOD_SUPER | EOS_MOD_SHIFT), EOS_KEY_1);
        CK(plain && sh && plain->action != sh->action,
           "matching is exact: super+shift+1 does not also fire super+1");
        CK(eos_keys_lookup(&km, (uint8_t)(EOS_MOD_SUPER | EOS_MOD_ALT), EOS_KEY_1) == NULL,
           "an extra modifier does not fall through to the plainer bind");
    }
}

// ---------------------------------------------------------- scripted session

static void scripted(void)
{
    eos_tile_t t[EOS_MAX_WINDOWS * 2];

    printf("\n=== scripted session: 320x240 CYD ===\n");
    session(&CFG_CYD, (eos_rect_t){ 0, 0, 320, 240 });

    press("super+return");
    press("super+return");
    press("super+return");
    CK(live_windows() == 3, "three super+return opened three windows");
    CK(WM.focus == 2, "focus follows the newest window");

    // 316x224 of usable space: w0 takes the left column, w1/w2 stack on the right.
    CK(tile_of(0).rect.w == 157 && tile_of(0).rect.h == 224, "first window keeps a full-height column");
    CK(tile_of(1).rect.x == tile_of(2).rect.x, "second split stacked, not side by side");
    CK(tile_of(1).rect.y < tile_of(2).rect.y, "the newer window went below");

    printf("  focus movement\n");
    press("super+k");  CK(WM.focus == 1, "super+k moves focus up");
    press("super+j");  CK(WM.focus == 2, "super+j moves focus down");
    press("super+h");  CK(WM.focus == 0, "super+h moves focus into the left column");
    press("super+left");
    CK(WM.focus == 0, "no tile further left, focus stays put");
    // Both right-hand tiles sit the same distance away horizontally, so the
    // straight-ahead bias in eos_wm_focus_dir breaks the tie by row: w2's
    // centre is one pixel closer to w0's than w1's is.
    press("super+l");  CK(WM.focus == 2, "super+l crosses back to the right column");

    printf("  moving a window\n");
    press("super+shift+h");
    CK(windows_on_ws() == 3, "moving a window does not change the tile count");
    CK(tile_of(2).rect.x == 2 && tile_of(2).rect.w == 157,
       "the moved window now owns the left column");
    CK(tile_of(0).rect.x == 161 && tile_of(0).rect.y == 127,
       "the window it displaced took its old slot");
    CK(WM.focus == 2, "focus follows the window that moved, not the slot");
    CK(tile_of(2).app_id == 0 && tile_of(0).app_id == 0, "app ids travel with the window");

    printf("  workspaces\n");
    press("super+2");
    CK(WM.ws == 1, "super+2 switches to workspace 2");
    CK(windows_on_ws() == 0, "the new workspace is empty");
    CK(WM.focus == EOS_NONE, "an empty workspace has no focus");
    eos_key_result_t r = eos_keys_apply(&WM, &ST, SCR, EOS_ACT_SPAWN, 4);   // arcade
    CK(r.win == 3, "spawning by action returns the new window");
    CK(tile_of(3).app_id == 4, "spawn carries its app id");
    press("super+shift+1");
    CK(WM.win[3].ws == 0, "super+shift+1 moved the window to workspace 1");
    CK(windows_on_ws() == 0, "it left the workspace it was on");
    press("super+1");
    CK(WM.ws == 0 && windows_on_ws() == 4, "back on workspace 1 with four windows");

    printf("  closing\n");
    int before = live_windows();
    press("super+q");
    CK(live_windows() == before - 1, "super+q closed the focused window");
    CK(WM.focus != EOS_NONE, "focus fell back to a surviving window");

    printf("\n=== forced split direction ===\n");
    session(&CFG_CYD, (eos_rect_t){ 0, 0, 320, 240 });
    press("super+return");
    press("super+ctrl+v");
    press("super+return");
    CK(tiles(t) == 2, "two tiles");
    CK(t[0].rect.x == t[1].rect.x && t[0].rect.y != t[1].rect.y,
       "super+ctrl+v forced the next open to stack");
    session(&CFG_CYD, (eos_rect_t){ 0, 0, 320, 240 });
    press("super+return");
    press("super+ctrl+h");
    press("super+return");
    CK(tiles(t) == 2, "two tiles");
    CK(t[0].rect.y == t[1].rect.y && t[0].rect.x != t[1].rect.x,
       "super+ctrl+h forced the next open side by side");

    printf("\n=== resize ===\n");
    session(&CFG_CYD, (eos_rect_t){ 0, 0, 320, 240 });
    press("super+return");
    press("super+return");
    int w_before = tile_of(1).rect.w;
    press("super+equal");
    CK(tile_of(1).rect.w > w_before, "super+equal grew the focused tile");
    press("super+minus");
    CK(tile_of(1).rect.w == w_before, "super+minus put it back");

    printf("\n=== tab groups: 128x64 OLED ===\n");
    session(&CFG_OLED, (eos_rect_t){ 0, 0, 128, 64 });
    press("super+return");
    press("super+return");
    press("super+return");
    CK(tile_of(2).tab_count == 2, "the third window collapsed a split into a two-tab group");
    CK(tile_of(2).visible && !tile_of(1).visible, "the newest tab is the visible one");
    press("super+tab");
    CK(tile_of(1).visible && !tile_of(2).visible, "super+tab shows the other tab");
    press("super+return");
    CK(tile_of(1).tab_count == 3, "a fourth window joins the same group");
    CK(tile_of(0).tab_group == EOS_NONE, "the untabbed window is still freely tiled");
    {
        int vis = 0;
        int n = tiles(t);
        for (int i = 0; i < n; i++) if (t[i].visible) vis++;
        CK(n == 4 && vis == 2, "four windows, two of them on screen");
    }

    printf("\n=== shell toggles ===\n");
    session(&CFG_CYD, (eos_rect_t){ 0, 0, 320, 240 });
    press("super+return");
    CK(ST.bar_visible, "the bar starts visible");
    press("super+b");  CK(!ST.bar_visible, "super+b hid the bar");
    press("super+b");  CK(ST.bar_visible, "super+b brought it back");
    press("super+t");  CK(ST.theme == 1, "super+t cycled the theme");
    press("super+t");  press("super+t");
    CK(ST.theme == 0, "the theme cycle wraps");

    // A bind with no modifier is the case the launcher has to protect.
    eos_keys_bind(&KM, 0, EOS_KEY_A, EOS_ACT_TOGGLE_BAR, 0);
    CK(press("a").handled, "a plain bind works while the launcher is closed");
    press("a");
    press("super+space");
    CK(ST.launcher_open, "super+space opened the launcher");
    CK(!press("a").handled, "with the launcher up, plain keys go to its text field");
    CK(press("super+t").handled, "super chords still reach the shell");
    press("super+space");
    CK(!ST.launcher_open, "super+space closed it again");

    printf("  lock\n");
    int live = live_windows();
    press("super+escape");
    CK(ST.locked, "super+escape locked the shell");
    eos_key_result_t q = press("super+q");
    CK(q.handled && !q.changed, "locked keys are swallowed, not passed through");
    CK(live_windows() == live, "nothing closed while locked");
    press("super+escape");
    CK(!ST.locked, "super+escape unlocked it");

    CK(!press("super+ctrl+z").handled, "an unbound chord is reported unhandled");
}

// ----------------------------------------------------------------- keys.json

static void json(void)
{
    eos_keymap_t km;
    eos_keys_load_t r;
    const eos_keybind_t *b;

    printf("\n=== keys.json ===\n");

    eos_keys_defaults(&km);
    r = eos_keys_load_json(&km,
        "{ \"comment\": \"owner overrides\",\n"
        "  \"binds\": [\n"
        "    { \"keys\": \"super+q\", \"action\": \"spawn\", \"arg\": 7 },\n"
        "    { \"keys\": \"SUPER+F\", \"action\": \"toggle_bar\" },\n"
        "    { \"keys\": \"super+b\", \"action\": \"none\" }\n"
        "  ] }");
    printf("    overrides: %s, %d applied\n", eos_keys_err_str(r.err), r.applied);
    CK(r.err == EOS_KEYS_OK && r.applied == 3, "override file parses");
    b = eos_keys_lookup(&km, EOS_MOD_SUPER, EOS_KEY_Q);
    CK(b && b->action == EOS_ACT_SPAWN && b->arg == 7, "super+q was rebound in place");
    b = eos_keys_lookup(&km, EOS_MOD_SUPER, EOS_KEY_F);
    CK(b && b->action == EOS_ACT_TOGGLE_BAR, "super+f was added, case insensitively");
    CK(eos_keys_lookup(&km, EOS_MOD_SUPER, EOS_KEY_B) == NULL, "\"none\" unbinds");
    CK(eos_keys_lookup(&km, EOS_MOD_SUPER, EOS_KEY_ENTER) != NULL,
       "everything not mentioned is left alone");

    eos_keys_defaults(&km);
    r = eos_keys_load_json(&km, "[ { \"keys\": \"alt+f4\", \"action\": \"close\" } ]");
    CK(r.err == EOS_KEYS_OK && r.applied == 1, "a bare array works too");
    CK(eos_keys_lookup(&km, EOS_MOD_ALT, 0x3D) != NULL, "alt+f4 bound");

    struct { const char *json; eos_keys_err_t want; const char *why; } bad[] = {
        { "{ \"binds\": [ { \"keys\": \"super+q\" ",      EOS_KEYS_ERR_SYNTAX,  "truncated file" },
        { "[ { \"keys\": \"super+nope\", \"action\": \"close\" } ]", EOS_KEYS_ERR_KEYNAME, "unknown key" },
        { "[ { \"keys\": \"super+z\", \"action\": \"frob\" } ]",     EOS_KEYS_ERR_ACTION,  "unknown action" },
        { "[ { \"action\": \"close\" } ]",                EOS_KEYS_ERR_SYNTAX,  "no keys field" },
        { "not json at all",                              EOS_KEYS_ERR_SYNTAX,  "not json" }
    };
    for (int i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++) {
        eos_keymap_t before;
        eos_keys_defaults(&km);
        before = km;
        r = eos_keys_load_json(&km, bad[i].json);
        printf("    %-16s -> %s at byte %d\n", bad[i].why, eos_keys_err_str(r.err), r.offset);
        CK(r.err == bad[i].want, "the right error is reported");
        CK(memcmp(&before, &km, sizeof(km)) == 0, "a bad file leaves the table untouched");
    }

    // The table is a fixed pool; running it dry must be refused, not wrapped.
    eos_keys_clear(&km);
    for (int i = 0; i < EOS_MAX_BINDS; i++)
        CK(eos_keys_bind(&km, EOS_MOD_SUPER, (uint16_t)(EOS_KEY_A + i), EOS_ACT_CLOSE, 0),
           "bind fits");
    CK(km.count == EOS_MAX_BINDS, "table filled to exactly EOS_MAX_BINDS");
    CK(!eos_keys_bind(&km, EOS_MOD_ALT, EOS_KEY_F12, EOS_ACT_CLOSE, 0),
       "one more bind is refused");

    printf("  from a file\n");
    {
        const char *path = "eos_shell_test_keys.json";
        char scratch[512];
        FILE *f = fopen(path, "wb");
        CK(f != NULL, "test can write a scratch keys.json");
        if (f) {
            fprintf(f, "{\"binds\":[{\"keys\":\"super+grave\",\"action\":\"launcher\"}]}");
            fclose(f);
        }
        eos_keys_defaults(&km);
        r = eos_keys_load_file(&km, path, scratch, (int)sizeof(scratch));
        CK(r.err == EOS_KEYS_OK && r.applied == 1, "keys.json loads from disk");
        CK(eos_keys_lookup(&km, EOS_MOD_SUPER, EOS_KEY_GRAVE) != NULL, "its bind took effect");

        r = eos_keys_load_file(&km, path, scratch, 8);
        CK(r.err == EOS_KEYS_ERR_TOO_BIG, "a file bigger than the scratch buffer is refused");

        remove(path);
        r = eos_keys_load_file(&km, path, scratch, (int)sizeof(scratch));
        CK(r.err == EOS_KEYS_ERR_IO, "a missing keys.json is an error, not a crash");
    }
}

// --------------------------------------------------------------- status bar

static void print_bar(const eos_bar_status_t *st, const eos_bar_metrics_t *m, int16_t width)
{
    eos_bar_seg_t seg[EOS_BAR_SEGS];
    char strip[256];
    int  cells = width / m->char_w;
    int  n = eos_bar_build(st, m, width, seg, EOS_BAR_SEGS);

    if (cells > (int)sizeof(strip) - 1) cells = (int)sizeof(strip) - 1;
    memset(strip, ' ', (size_t)cells);
    strip[cells] = 0;
    for (int i = 0; i < n; i++) {
        int at = seg[i].x / m->char_w;
        for (int k = 0; seg[i].text[k] && at + k < cells; k++) strip[at + k] = seg[i].text[k];
    }
    printf("    %3dpx |%s|\n", (int)width, strip);
    printf("          ");
    for (int i = 0; i < n; i++) printf("%s ", eos_bar_seg_name(seg[i].id));
    printf("\n");
}

// A crude proportional font. The thin glyphs really are narrower in every
// LVGL font, and that is exactly the case char_w * strlen gets wrong.
static int16_t prop_measure(const char *s, void *ud)
{
    int16_t w = 0;
    (void)ud;
    for (; *s; s++) {
        switch (*s) {
        case 'i': case 'l': case 'j': case '.': case ':': case '!':
        case '[': case ']': case '1': case ' ':          w = (int16_t)(w + 3); break;
        case 'm': case 'w': case 'M': case 'W': case '#': w = (int16_t)(w + 9); break;
        default:                                          w = (int16_t)(w + 7); break;
        }
    }
    return w;
}

// Proportional widths do not land on a character grid, so print the numbers.
static void print_bar_list(const eos_bar_status_t *st, const eos_bar_metrics_t *m, int16_t width)
{
    eos_bar_seg_t seg[EOS_BAR_SEGS];
    int n = eos_bar_build(st, m, width, seg, EOS_BAR_SEGS);
    printf("    %3dpx ", (int)width);
    for (int i = 0; i < n; i++)
        printf("%s@%d+%d \"%s\"  ", eos_bar_seg_name(seg[i].id),
               (int)seg[i].x, (int)seg[i].w, seg[i].text);
    printf("\n");
}

// Everything the fitter must never do, checked against the segment set the
// same status produces when width is not a constraint.
static void verify_bar(const eos_bar_status_t *st, const eos_bar_metrics_t *m, int16_t width)
{
    eos_bar_seg_t seg[EOS_BAR_SEGS], all[EOS_BAR_SEGS];
    int n   = eos_bar_build(st, m, width, seg, EOS_BAR_SEGS);
    int nal = eos_bar_build(st, m, 4096,  all, EOS_BAR_SEGS);
    uint8_t lowest = 255;

    CK(n <= nal, "a narrow bar never shows more segments than a wide one");
    for (int i = 0; i < n; i++) {
        CK(seg[i].x >= 0, "segment starts inside the bar");
        CK(seg[i].x + seg[i].w <= width, "segment ends inside the bar");
        CK(seg[i].w > 0 && seg[i].text[0] != 0, "segment has width and text");
        if (i > 0) CK(seg[i - 1].x + seg[i - 1].w <= seg[i].x, "segments do not overlap");
        if (seg[i].priority < lowest) lowest = seg[i].priority;
    }
    // Nothing above the weakest survivor may have been dropped.
    for (int i = 0; i < nal; i++) {
        if (all[i].priority < lowest) continue;
        int found = 0;
        for (int j = 0; j < n; j++) if (seg[j].id == all[i].id) found = 1;
        CK(found, "no higher-priority segment was dropped in favour of a lower one");
    }
}

static void bar(void)
{
    static const int16_t WIDTHS[] = { 480, 320, 172, 128, 96, 64 };
    eos_bar_metrics_t m;
    eos_bar_status_t  st;
    eos_bar_seg_t     seg[EOS_BAR_SEGS];

    eos_bar_metrics_init(&m, 6, 6);   // 6px cell, one blank cell between segments

    printf("\n=== status bar, everything nominal ===\n");
    eos_bar_status_init(&st);
    st.ws_occupied = 0x0017;              // workspaces 1, 2, 3 and 5 hold windows
    st.ws_active   = 2;
    st.title       = "megabrain";
    st.wifi        = EOS_WIFI_UP;
    st.wifi_rssi   = -58;
    st.brain_up    = true;
    st.brain_model = "qwen3.5:2b";
    st.free_heap   = 21504;
    st.mood        = EOS_MOOD_HAPPY;
    st.hour = 14; st.minute = 32; st.clock_valid = true;
    for (int i = 0; i < (int)(sizeof(WIDTHS) / sizeof(WIDTHS[0])); i++) {
        print_bar(&st, &m, WIDTHS[i]);
        verify_bar(&st, &m, WIDTHS[i]);
    }

    printf("\n=== status bar, long title and a long model name ===\n");
    st.title       = "cpm a> zork1.com running";
    st.brain_model = "gemma4:12b-it-qat";
    for (int i = 0; i < (int)(sizeof(WIDTHS) / sizeof(WIDTHS[0])); i++) {
        print_bar(&st, &m, WIDTHS[i]);
        verify_bar(&st, &m, WIDTHS[i]);
    }

    printf("\n=== status bar, everything unhappy ===\n");
    st.wifi        = EOS_WIFI_DOWN;
    st.brain_up    = false;
    st.free_heap   = 9 * 1024;
    st.mood        = EOS_MOOD_CONFUSED;
    st.clock_valid = false;
    st.ws_occupied = 0x01ff;
    st.ws_active   = 8;
    for (int i = 0; i < (int)(sizeof(WIDTHS) / sizeof(WIDTHS[0])); i++) {
        print_bar(&st, &m, WIDTHS[i]);
        verify_bar(&st, &m, WIDTHS[i]);
    }
    {
        int n = eos_bar_build(&st, &m, 480, seg, EOS_BAR_SEGS);
        int warn = 0;
        for (int i = 0; i < n; i++) {
            if (seg[i].id == EOS_SEG_HEAP  && seg[i].role == EOS_BAR_ROLE_WARN) warn++;
            if (seg[i].id == EOS_SEG_WIFI  && seg[i].role == EOS_BAR_ROLE_WARN) warn++;
            if (seg[i].id == EOS_SEG_BRAIN && seg[i].role == EOS_BAR_ROLE_WARN) warn++;
        }
        CK(warn == 3, "heap, wifi and brain all report WARN when they should");
    }

    printf("\n=== status bar, no focused window and nothing occupied ===\n");
    eos_bar_status_init(&st);
    st.title = NULL;
    st.hour = 9; st.minute = 5; st.clock_valid = true;
    for (int i = 0; i < (int)(sizeof(WIDTHS) / sizeof(WIDTHS[0])); i++) {
        print_bar(&st, &m, WIDTHS[i]);
        verify_bar(&st, &m, WIDTHS[i]);
    }
    {
        int n = eos_bar_build(&st, &m, 480, seg, EOS_BAR_SEGS);
        int has_title = 0;
        for (int i = 0; i < n; i++) if (seg[i].id == EOS_SEG_TITLE) has_title = 1;
        CK(!has_title, "no focused window means no title segment at all");
    }

    printf("\n=== status bar, absurdly narrow ===\n");
    for (int16_t w = 40; w >= 4; w = (int16_t)(w - 6)) {
        print_bar(&st, &m, w);
        verify_bar(&st, &m, w);
    }
    CK(eos_bar_build(&st, &m, 1, seg, EOS_BAR_SEGS) == 0, "one pixel of bar shows nothing");
    CK(eos_bar_build(&st, &m, 0, seg, EOS_BAR_SEGS) == 0, "a zero-width bar shows nothing");

    // The proportional-font path: a measurer that is not char_w * strlen.
    printf("\n=== status bar, proportional metrics (LVGL-style font) ===\n");
    m.measure = prop_measure;
    eos_bar_status_init(&st);
    st.ws_occupied = 0x0003;
    st.ws_active   = 1;
    st.title       = "settings";
    st.wifi        = EOS_WIFI_UP;
    st.wifi_rssi   = -71;
    st.brain_up    = true;
    st.brain_model = "ornith:9b";
    st.free_heap   = 4u * 1024u * 1024u;
    st.hour = 23; st.minute = 59; st.clock_valid = true;
    for (int i = 0; i < 4; i++) { print_bar_list(&st, &m, WIDTHS[i]); verify_bar(&st, &m, WIDTHS[i]); }
    {
        eos_bar_seg_t s2[EOS_BAR_SEGS];
        int n = eos_bar_build(&st, &m, 480, s2, EOS_BAR_SEGS);
        int measured = 1;
        for (int i = 0; i < n; i++)
            if (s2[i].w != prop_measure(s2[i].text, NULL)) measured = 0;
        CK(n > 0 && measured, "every segment width came from the supplied measurer");
    }
}

// ----------------------------------------------------------- wm -> bar glue

static void glue(void)
{
    eos_bar_status_t st;
    eos_bar_metrics_t m;
    eos_bar_seg_t seg[EOS_BAR_SEGS];

    printf("\n=== bar fed from a live window manager ===\n");
    eos_bar_metrics_init(&m, 6, 6);   // 6px cell, one blank cell between segments
    eos_bar_status_init(&st);

    session(&CFG_CYD, (eos_rect_t){ 0, 0, 320, 240 });
    press("super+return");
    press("super+3");
    eos_keys_apply(&WM, &ST, SCR, EOS_ACT_SPAWN, 5);     // cpm on workspace 3
    press("super+7");
    eos_keys_apply(&WM, &ST, SCR, EOS_ACT_SPAWN, 2);     // files on workspace 7

    eos_shell_status_sync(&WM, &st, APPNAME, NAPPS);
    CK(st.ws_occupied == 0x0045, "occupied workspaces are 1, 3 and 7");
    CK(st.ws_active == 6, "workspace 7 is current");
    CK(st.title && strcmp(st.title, "files") == 0, "the title is the focused window's app");

    st.clock_valid = true; st.hour = 7; st.minute = 4;
    print_bar(&st, &m, 320);
    print_bar(&st, &m, 128);

    press("super+q");
    eos_shell_status_sync(&WM, &st, APPNAME, NAPPS);
    CK(st.title == NULL, "closing the last window on a workspace clears the title");
    CK((st.ws_occupied & 0x0040) == 0, "workspace 7 is no longer occupied");
    CK(eos_bar_build(&st, &m, 320, seg, EOS_BAR_SEGS) > 0, "the bar still builds with no windows");
}

// ---------------------------------------------------------------- regressions
//
// Three things that were wrong once and must not come back. All three are
// cases a caller reaches, not internal detail: a small out array, a status
// field set to a workspace that does not exist, and a short chord buffer.

static void regressions(void)
{
    eos_bar_status_t st;
    eos_bar_metrics_t m;

    eos_bar_status_init(&st);
    st.ws_occupied = 0x0f; st.ws_active = 2; st.title = "megabrain";
    st.wifi = EOS_WIFI_UP; st.wifi_rssi = -58;
    st.brain_up = true; st.brain_model = "qwen3.5:2b";
    st.free_heap = 21u * 1024u; st.mood = EOS_MOOD_HAPPY;
    st.hour = 14; st.minute = 32; st.clock_valid = true;
    eos_bar_metrics_init(&m, 6, 6);

    // A caller with room for fewer segments than the bar could fill must lose
    // the LOWEST priority ones. Truncating the emission in display order
    // instead kept the buddy and dropped the clock.
    printf("  small out array, plenty of width\n");
    for (int max = 1; max <= EOS_BAR_SEGS; max++) {
        eos_bar_seg_t seg[EOS_BAR_SEGS];
        int n = eos_bar_build(&st, &m, 480, seg, max);
        uint8_t lowest = 255;
        CK(n <= max, "eos_bar_build honours max");
        printf("    max=%d ->", max);
        for (int i = 0; i < n; i++) {
            printf(" %s", eos_bar_seg_name(seg[i].id));
            if (seg[i].priority < lowest) lowest = seg[i].priority;
        }
        printf("\n");
        for (int id = 0; id < EOS_BAR_SEGS; id++) {
            int kept = 0;
            for (int i = 0; i < n; i++) if ((int)seg[i].id == id) kept = 1;
            if (kept) continue;
            CK(eos_bar_priority((eos_bar_seg_id_t)id) <= lowest,
               "a capped bar drops the lowest priority segments, not the last ones");
        }
    }

    // ws_active is caller-set. Out of range must not make the SHORTEST form
    // name a workspace the longest form never lists - which is why this walks
    // narrow widths too: at 40px the fitter upgrades straight past the short
    // form, and the bug hid there.
    printf("  ws_active out of range\n");
    for (int a = 8; a <= 12; a++) {
        st.ws_active = (uint8_t)a;
        for (int16_t width = 14; width <= 60; width = (int16_t)(width + 2)) {
            eos_bar_seg_t seg[EOS_BAR_SEGS];
            int n = eos_bar_build(&st, &m, width, seg, EOS_BAR_SEGS);
            if (n == 0) continue;
            CK(seg[0].id == EOS_SEG_PIPS, "pips outrank everything");
            if (width == 20) printf("    ws_active=%2d -> \"%s\"\n", a, seg[0].text);
            if (a >= EOS_WORKSPACE_PIPS)
                CK(strchr(seg[0].text, '[') == NULL,
                   "no workspace is marked live when ws_active does not exist");
            // The widest form lists exactly the workspaces that exist, so no
            // narrower form may introduce a digit it does not contain. The
            // short form naming workspace 10 is what this catches.
            eos_bar_seg_t full[EOS_BAR_SEGS];
            int nf = eos_bar_build(&st, &m, 4096, full, EOS_BAR_SEGS);
            CK(nf > 0 && full[0].id == EOS_SEG_PIPS, "the widest bar shows pips");
            for (const char *p = seg[0].text; *p && nf; p++) {
                if (*p < '0' || *p > '9') continue;
                CK(strchr(full[0].text, *p) != NULL,
                   "a narrow pips form never names a workspace the widest one omits");
            }
        }
    }
    st.ws_active = 2;

    // eos_keys_format returns the length WRITTEN, not the length wanted.
    printf("  eos_keys_format into a short buffer\n");
    {
        eos_keymap_t km;
        eos_keys_defaults(&km);
        const eos_keybind_t *b = eos_keys_lookup(&km, EOS_MOD_SUPER | EOS_MOD_SHIFT,
                                                 EOS_KEY_LEFT);
        CK(b != NULL, "super+shift+left is bound");
        for (int n = 1; n <= 24 && b; n++) {
            char buf[32];
            memset(buf, 0x7f, sizeof(buf));
            int r = eos_keys_format(b, buf, n);
            CK(r == (int)strlen(buf), "format returns the bytes it actually wrote");
            CK(r < n, "format's return leaves room for the NUL");
            for (int j = n; j < (int)sizeof(buf); j++)
                CK(buf[j] == 0x7f, "format writes nothing past the buffer");
        }
    }
}

int main(void)
{
    show_table();
    scripted();
    json();
    bar();
    glue();
    printf("\n=== regressions ===\n");
    regressions();
    printf("\n=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
