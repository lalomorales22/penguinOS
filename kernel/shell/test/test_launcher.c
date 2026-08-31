// Host test for the launcher model, plus the one integration question that
// matters more than any of it: does super+q still reach eos_wm_close().
//
// The owner reported that nothing happens when they press it. The model half
// of that path is entirely testable off target — the keymap, the dispatch and
// eos_wm_close() are all pure — so the last section here drives exactly the
// call eos_shell_input_pump() makes, with the same modifier byte the K809
// puts in byte 0 of its report, and counts the windows before and after. If
// those checks pass, a super+q that does nothing on the glass is a keystroke
// that never arrived, not a dispatch that dropped it.
//
//   cc -std=c99 -Wall -Wextra -O1 -Ikernel/wm/include -Ikernel/hal/include \
//      -Ikernel/shell/include -Iboards/generated \
//      kernel/wm/eos_wm.c kernel/shell/eos_keys.c kernel/shell/eos_bar.c \
//      kernel/shell/eos_launcher.c kernel/shell/test/test_launcher.c \
//      -o /tmp/test_launcher && /tmp/test_launcher

#include <stdio.h>
#include <string.h>

#include "eos_wm.h"
#include "eos_keys.h"
#include "eos_launcher.h"

static int checks = 0, fails = 0;
#define CK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("    FAIL: %s\n", msg); } } while (0)

static void EQ(long got, long want, const char *what)
{
    checks++;
    if (got != want) { fails++; printf("    FAIL: %s: got %ld want %ld\n", what, got, want); }
}

// The six the owner named. Names are short because a row draws the name in the
// accent colour first and then as much of the description as still fits.
static const char *const NAME[] = { "chat", "buddy", "settings", "files", "media", "party" };
static const char *const DESC[] = {
    "ask megabrain a question",
    "the voxel penguin, and his mood",
    "wifi, theme, brightness",
    "what is on the card",
    "play something",
    "everybody dance"
};
#define NAPPS ((int)(sizeof(NAME) / sizeof(NAME[0])))

static void fill(eos_launcher_t *l, int n)
{
    int i;
    eos_launcher_init(l);
    for (i = 0; i < n; i++)
        eos_launcher_add(l, NAME[i % NAPPS], DESC[i % NAPPS], (uint16_t)i);
}

// A geometry with a chosen number of visible rows, so scrolling can be tested
// without pretending the panel is a different size than it is.
static void rows_of(eos_launcher_t *l, int rows)
{
    eos_launcher_geom_t g;
    eos_launcher_layout(&g, 240, 240, 8);
    g.rows = (uint8_t)rows;
    eos_launcher_set_geom(l, &g);
}

static eos_launcher_res_t key(eos_launcher_t *l, uint16_t k)
{
    return eos_launcher_key(l, k, 0);
}

// ------------------------------------------------------------- open / close

static void t_openclose(void)
{
    eos_launcher_t l;
    eos_launcher_res_t r;

    printf("  open and close\n");
    fill(&l, NAPPS);
    rows_of(&l, 16);

    CK(!eos_launcher_is_open(&l), "starts closed");
    EQ(eos_launcher_count(&l), NAPPS, "count");

    // Closed, every key passes straight through. This is the check that says
    // the launcher cannot eat the desktop's keyboard while it is not up.
    r = key(&l, EOS_KEY_DOWN);
    EQ(r.act, EOS_LAUNCHER_PASS, "closed: down passes");
    r = key(&l, EOS_KEY_ENTER);
    EQ(r.act, EOS_LAUNCHER_PASS, "closed: enter passes");

    CK(eos_launcher_toggle(&l), "toggle opens");
    CK(eos_launcher_is_open(&l), "is open");
    EQ(eos_launcher_selected(&l), 0, "opens on the first item");

    eos_launcher_move(&l, 3);
    EQ(eos_launcher_selected(&l), 3, "moved to 3");

    CK(!eos_launcher_toggle(&l), "toggle closes");
    CK(!eos_launcher_is_open(&l), "is closed");

    // Reopening resets. super+space, down, down, enter must mean the same
    // thing every time it is typed.
    eos_launcher_open(&l);
    EQ(eos_launcher_selected(&l), 0, "reopen resets the selection");
    EQ(eos_launcher_top(&l), 0, "reopen resets the scroll");

    r = key(&l, EOS_KEY_ESC);
    EQ(r.act, EOS_LAUNCHER_CLOSE, "escape closes");
    CK(r.changed, "escape wants a redraw");
    CK(!eos_launcher_is_open(&l), "escape really closed it");

    // set_open is the mirror path: idempotent, and it does not stomp the
    // selection when the caller re-asserts a state that is already true.
    eos_launcher_open(&l);
    eos_launcher_move(&l, 2);
    eos_launcher_set_open(&l, true);
    EQ(eos_launcher_selected(&l), 2, "set_open(true) on an open launcher keeps the selection");
    eos_launcher_set_open(&l, false);
    CK(!eos_launcher_is_open(&l), "set_open(false) closes");
    eos_launcher_set_open(&l, true);
    EQ(eos_launcher_selected(&l), 0, "set_open(true) from closed resets");
}

// ---------------------------------------------------------------- wrapping

static void t_wrap(void)
{
    eos_launcher_t l;
    int i;

    printf("  selection wraps at both ends\n");
    fill(&l, NAPPS);
    rows_of(&l, 16);
    eos_launcher_open(&l);

    for (i = 0; i < NAPPS - 1; i++) key(&l, EOS_KEY_DOWN);
    EQ(eos_launcher_selected(&l), NAPPS - 1, "down to the last item");

    key(&l, EOS_KEY_DOWN);
    EQ(eos_launcher_selected(&l), 0, "down off the end wraps to the first");

    key(&l, EOS_KEY_UP);
    EQ(eos_launcher_selected(&l), NAPPS - 1, "up off the start wraps to the last");

    // j and k do the same thing as the arrows, and tab walks down.
    key(&l, EOS_KEY_J);
    EQ(eos_launcher_selected(&l), 0, "j wraps like down");
    key(&l, EOS_KEY_K);
    EQ(eos_launcher_selected(&l), NAPPS - 1, "k wraps like up");
    key(&l, EOS_KEY_TAB);
    EQ(eos_launcher_selected(&l), 0, "tab wraps like down");

    key(&l, EOS_KEY_END);
    EQ(eos_launcher_selected(&l), NAPPS - 1, "end jumps to the last");
    key(&l, EOS_KEY_HOME);
    EQ(eos_launcher_selected(&l), 0, "home jumps to the first");

    // A one-item list wraps onto itself rather than reporting movement.
    fill(&l, 1);
    rows_of(&l, 16);
    eos_launcher_open(&l);
    CK(!eos_launcher_move(&l, +1), "one item: down does not move");
    EQ(eos_launcher_selected(&l), 0, "one item: still selected");
    CK(!eos_launcher_move(&l, -1), "one item: up does not move");
}

// ------------------------------------------------------------- empty list

static void t_empty(void)
{
    eos_launcher_t l;
    eos_launcher_res_t r;

    printf("  an empty list\n");
    eos_launcher_init(&l);
    rows_of(&l, 16);
    eos_launcher_open(&l);

    EQ(eos_launcher_count(&l), 0, "count is zero");
    EQ(eos_launcher_selected(&l), EOS_LAUNCHER_NONE, "nothing is selected");
    CK(eos_launcher_is_open(&l), "an empty launcher still opens");

    r = key(&l, EOS_KEY_ENTER);
    EQ(r.act, EOS_LAUNCHER_EAT, "enter on an empty list is eaten, not launched");
    CK(!r.changed, "enter on an empty list changes nothing");
    CK(eos_launcher_is_open(&l), "enter on an empty list leaves it open");

    r = key(&l, EOS_KEY_DOWN);
    EQ(r.act, EOS_LAUNCHER_EAT, "down on an empty list is eaten");
    CK(!r.changed, "down on an empty list changes nothing");
    EQ(eos_launcher_top(&l), 0, "empty list does not scroll");

    r = key(&l, EOS_KEY_ESC);
    EQ(r.act, EOS_LAUNCHER_CLOSE, "escape still closes an empty launcher");

    // Refusals. A nameless row could not be identified once drawn.
    eos_launcher_init(&l);
    CK(!eos_launcher_add(&l, NULL, "x", 0), "add refuses a NULL name");
    CK(!eos_launcher_add(&l, "", "x", 0),   "add refuses an empty name");
    EQ(eos_launcher_count(&l), 0, "refused adds did not land");
    CK(eos_launcher_add(&l, "chat", NULL, 7), "add accepts a NULL description");
    EQ(eos_launcher_item(&l, 0)->desc == NULL, 1, "NULL description stays NULL");
    CK(eos_launcher_item(&l, 1) == NULL, "item past the end is NULL");
    CK(eos_launcher_item(&l, -1) == NULL, "item before the start is NULL");
}

// ----------------------------------------------------------------- launching

static void t_launch(void)
{
    eos_launcher_t l;
    eos_launcher_res_t r;

    printf("  enter launches\n");
    fill(&l, NAPPS);
    rows_of(&l, 16);
    eos_launcher_open(&l);

    key(&l, EOS_KEY_DOWN);
    key(&l, EOS_KEY_DOWN);
    EQ(eos_launcher_selected(&l), 2, "down down selects the third app");

    r = key(&l, EOS_KEY_ENTER);
    EQ(r.act, EOS_LAUNCHER_LAUNCH, "enter launches");
    EQ(r.app_id, 2, "it launched the selected app id");
    CK(r.changed, "a launch wants a redraw");
    CK(!eos_launcher_is_open(&l), "launching closes the launcher");

    // And once closed it is inert again.
    r = key(&l, EOS_KEY_ENTER);
    EQ(r.act, EOS_LAUNCHER_PASS, "enter after a launch passes through");
}

// ------------------------------------------------------------------ scroll

static void t_scroll(void)
{
    eos_launcher_t l;
    int i;

    printf("  a list longer than the screen scrolls\n");
    fill(&l, 20);
    rows_of(&l, 5);
    eos_launcher_open(&l);

    EQ(eos_launcher_count(&l), 20, "twenty apps");
    EQ(eos_launcher_rows(&l), 5, "five rows fit");
    EQ(eos_launcher_top(&l), 0, "starts at the top");

    // Walking down inside the window does not scroll.
    for (i = 0; i < 4; i++) key(&l, EOS_KEY_DOWN);
    EQ(eos_launcher_selected(&l), 4, "selected the last visible row");
    EQ(eos_launcher_top(&l), 0, "still no scroll");

    // The fifth step pushes the view by exactly one row.
    key(&l, EOS_KEY_DOWN);
    EQ(eos_launcher_selected(&l), 5, "selected row 5");
    EQ(eos_launcher_top(&l), 1, "the view scrolled by one");

    key(&l, EOS_KEY_END);
    EQ(eos_launcher_selected(&l), 19, "end selects the last app");
    EQ(eos_launcher_top(&l), 15, "the view is parked at the bottom");

    // Wrapping past the end must bring the view back with it.
    key(&l, EOS_KEY_DOWN);
    EQ(eos_launcher_selected(&l), 0, "wrapped to the first");
    EQ(eos_launcher_top(&l), 0, "the view came back to the top");

    key(&l, EOS_KEY_UP);
    EQ(eos_launcher_selected(&l), 19, "wrapped back to the last");
    EQ(eos_launcher_top(&l), 15, "and the view went with it");

    key(&l, EOS_KEY_HOME);
    EQ(eos_launcher_top(&l), 0, "home scrolls back to the top");

    // Pages move a screen minus one so a row of context survives.
    key(&l, EOS_KEY_PGDN);
    EQ(eos_launcher_selected(&l), 4, "page down moves rows-1");
    key(&l, EOS_KEY_PGUP);
    EQ(eos_launcher_selected(&l), 0, "page up comes back");

    // The selection is NEVER off screen. That is the whole scrolling contract.
    for (i = 0; i < 60; i++) {
        int sel, top;
        key(&l, (i % 7 == 0) ? EOS_KEY_PGDN : EOS_KEY_DOWN);
        sel = eos_launcher_selected(&l);
        top = eos_launcher_top(&l);
        checks++;
        if (sel < top || sel >= top + eos_launcher_rows(&l)) {
            fails++;
            printf("    FAIL: selection %d fell outside the view [%d,%d)\n",
                   sel, top, top + eos_launcher_rows(&l));
        }
    }

    // A list that fits never scrolls, whatever it is asked to do.
    fill(&l, 3);
    rows_of(&l, 5);
    eos_launcher_open(&l);
    key(&l, EOS_KEY_END);
    EQ(eos_launcher_top(&l), 0, "a list that fits stays at the top");
    key(&l, EOS_KEY_DOWN);
    EQ(eos_launcher_top(&l), 0, "and after wrapping too");

    // Shrinking the panel under a parked selection re-clamps the scroll.
    fill(&l, 20);
    rows_of(&l, 10);
    eos_launcher_open(&l);
    key(&l, EOS_KEY_END);
    EQ(eos_launcher_top(&l), 10, "parked at the bottom with ten rows");
    rows_of(&l, 4);
    EQ(eos_launcher_rows(&l), 4, "the panel shrank");
    EQ(eos_launcher_top(&l), 16, "the view re-clamped to keep the selection on screen");
    CK(eos_launcher_selected(&l) >= eos_launcher_top(&l), "selection still visible");

    // The table has a hard ceiling and refuses the overflow rather than
    // wrapping around and overwriting item 0.
    fill(&l, EOS_LAUNCHER_MAX);
    EQ(eos_launcher_count(&l), EOS_LAUNCHER_MAX, "filled to the ceiling");
    CK(!eos_launcher_add(&l, "extra", "one too many", 99), "add refuses past the ceiling");
    EQ(eos_launcher_count(&l), EOS_LAUNCHER_MAX, "and the count did not move");
}

// ----------------------------------------------------------------- passthru

static void t_passthrough(void)
{
    eos_launcher_t l;
    eos_launcher_res_t r;
    uint16_t k;
    int eaten = 0, passed = 0;

    printf("  keys that are not launcher keys are passed on\n");
    fill(&l, NAPPS);
    rows_of(&l, 16);
    eos_launcher_open(&l);

    // Every HID usage the model does not bind must come back PASS, so the
    // shell, a text field, or an app can still have it.
    for (k = 0; k < 256; k++) {
        r = eos_launcher_key(&l, k, 0);
        if (r.act == EOS_LAUNCHER_PASS) { passed++; continue; }
        eaten++;
        if (r.act == EOS_LAUNCHER_CLOSE || r.act == EOS_LAUNCHER_LAUNCH)
            eos_launcher_open(&l);   // put it back for the rest of the sweep
    }
    // up, down, k, j, tab, pgup, pgdn, home, end, enter, escape.
    EQ(eaten, 11, "exactly eleven usages are launcher keys");
    EQ(passed, 245, "every other usage passes through");

    // A plain letter belongs to whoever is listening, not to the list.
    eos_launcher_open(&l);
    r = eos_launcher_key(&l, EOS_KEY_A, 0);
    EQ(r.act, EOS_LAUNCHER_PASS, "'a' passes through");
    r = eos_launcher_key(&l, EOS_KEY_SPACE, 0);
    EQ(r.act, EOS_LAUNCHER_PASS, "plain space passes through");

    // Super chords are the shell's, always. This is what lets a second
    // super+space close the launcher through eos_keys, and super+q close a
    // window from inside the overlay.
    r = eos_launcher_key(&l, EOS_KEY_SPACE, EOS_MOD_LGUI);
    EQ(r.act, EOS_LAUNCHER_PASS, "super+space passes to the shell");
    r = eos_launcher_key(&l, EOS_KEY_Q, EOS_MOD_LGUI);
    EQ(r.act, EOS_LAUNCHER_PASS, "super+q passes to the shell");
    r = eos_launcher_key(&l, EOS_KEY_DOWN, EOS_MOD_RGUI);
    EQ(r.act, EOS_LAUNCHER_PASS, "super+down is focus-down, not a list move");
    CK(eos_launcher_is_open(&l), "none of that closed the launcher");

    // Shift and ctrl are not super, so they do not take the key away.
    r = eos_launcher_key(&l, EOS_KEY_DOWN, EOS_MOD_LSHIFT);
    EQ(r.act, EOS_LAUNCHER_EAT, "shift+down still moves the list");
}

// ------------------------------------------------------------------ pointer

static void t_pointer(void)
{
    eos_launcher_t l;
    eos_launcher_geom_t g;
    eos_launcher_res_t r;
    int16_t rowy;

    printf("  the pointer drives it too\n");
    eos_launcher_layout(&g, 240, 240, 8);
    printf("    240x240 font 8: panel %d,%d %dx%d  list_y=%d row_h=%d rows=%d\n",
           g.x, g.y, g.w, g.h, g.list_y, g.row_h, (int)g.rows);
    CK(g.w > 0 && g.h > 0, "the panel has a size");
    CK(g.x >= 0 && g.y >= 0, "the panel is on screen");
    CK(g.x + g.w <= 240 && g.y + g.h <= 240, "the panel fits the screen");
    CK(g.rows >= 6, "a 240x240 panel holds at least the six apps");
    CK(g.list_y > g.rule_y && g.rule_y > g.title_y, "heading, rule, then list");
    CK(g.list_y + (int16_t)g.rows * g.row_h <= g.y + g.h, "the rows fit inside the panel");

    // A 128x64 OLED still lays out, and still reports at least one row.
    eos_launcher_layout(&g, 128, 64, 8);
    printf("    128x64  font 8: panel %d,%d %dx%d  list_y=%d row_h=%d rows=%d\n",
           g.x, g.y, g.w, g.h, g.list_y, g.row_h, (int)g.rows);
    CK(g.rows >= 1, "even an OLED holds a row");
    CK(g.list_y + (int16_t)g.rows * g.row_h <= g.y + g.h, "OLED rows fit too");

    fill(&l, NAPPS);
    eos_launcher_layout(&g, 240, 240, 8);
    eos_launcher_set_geom(&l, &g);

    // Closed, the pointer cannot reach it.
    EQ(eos_launcher_hit(&l, (int16_t)(g.x + 10), (int16_t)(g.list_y + 2)),
       EOS_LAUNCHER_NONE, "closed: no row under the cursor");

    eos_launcher_open(&l);
    rowy = (int16_t)(g.list_y + 2 * g.row_h + 2);        // inside row 2
    EQ(eos_launcher_hit(&l, (int16_t)(g.x + 10), rowy), 2, "hit finds row 2");
    CK(eos_launcher_hover(&l, (int16_t)(g.x + 10), rowy), "hover selects it");
    EQ(eos_launcher_selected(&l), 2, "row 2 is selected");
    CK(!eos_launcher_hover(&l, (int16_t)(g.x + 30), (int16_t)(rowy + 1)),
       "hovering inside the same row does not repaint");

    // Above the list, on the heading: no row, and the highlight stays put.
    EQ(eos_launcher_hit(&l, (int16_t)(g.x + 10), (int16_t)(g.title_y + 1)),
       EOS_LAUNCHER_NONE, "the heading is not a row");
    CK(!eos_launcher_hover(&l, (int16_t)(g.x + 10), (int16_t)(g.title_y + 1)),
       "hovering the heading changes nothing");
    EQ(eos_launcher_selected(&l), 2, "and leaves the selection alone");

    // Past the last item, inside the panel: still not a row.
    EQ(eos_launcher_hit(&l, (int16_t)(g.x + 10),
                        (int16_t)(g.list_y + NAPPS * g.row_h + 2)),
       EOS_LAUNCHER_NONE, "past the last app is not a row");

    // Outside the panel horizontally.
    EQ(eos_launcher_hit(&l, (int16_t)(g.x - 2), rowy), EOS_LAUNCHER_NONE,
       "left of the panel is not a row");
    EQ(eos_launcher_hit(&l, (int16_t)(g.x + g.w + 2), rowy), EOS_LAUNCHER_NONE,
       "right of the panel is not a row");

    // Clicking a row launches it.
    r = eos_launcher_click(&l, (int16_t)(g.x + 10),
                           (int16_t)(g.list_y + 4 * g.row_h + 2));
    EQ(r.act, EOS_LAUNCHER_LAUNCH, "a click on a row launches");
    EQ(r.app_id, 4, "and it launched the row that was clicked, not the hover");
    CK(!eos_launcher_is_open(&l), "the click closed the launcher");

    // Clicking away closes without launching.
    eos_launcher_open(&l);
    r = eos_launcher_click(&l, 1, 1);
    EQ(r.act, EOS_LAUNCHER_CLOSE, "a click outside the panel closes it");
    CK(!eos_launcher_is_open(&l), "and it really closed");

    // Clicking empty space inside the panel is eaten and does nothing.
    eos_launcher_open(&l);
    r = eos_launcher_click(&l, (int16_t)(g.x + 10), (int16_t)(g.title_y + 1));
    EQ(r.act, EOS_LAUNCHER_EAT, "a click on the heading is eaten");
    CK(eos_launcher_is_open(&l), "and leaves it open");

    // Scrolled, the hit test follows the view.
    fill(&l, 20);
    eos_launcher_set_geom(&l, &g);
    eos_launcher_open(&l);
    key(&l, EOS_KEY_END);
    EQ(eos_launcher_hit(&l, (int16_t)(g.x + 10), (int16_t)(g.list_y + 2)),
       eos_launcher_top(&l), "the top row on screen is the top of the view");
}

// ----------------------------------------- the close path, end to end

// The question the owner asked. This drives the exact call
// eos_shell_input_pump() makes, with the exact modifier byte the K809 sends.
static void t_close_path(void)
{
    eos_wm_t          wm;
    eos_wm_cfg_t      cfg = { 40, 30, 2, 12, 12 };
    eos_keymap_t      km;
    eos_shell_state_t st;
    eos_rect_t        scr = { 0, 0, 240, 240 };
    eos_key_result_t  r;
    int i, alive;

    printf("  super+q reaches eos_wm_close\n");

    eos_wm_init(&wm, &cfg);
    eos_keys_defaults(&km);
    eos_shell_state_init(&st, 1);

    for (i = 0; i < 5; i++) eos_wm_open(&wm, (uint16_t)i, scr);
    alive = 0;
    for (i = 0; i < EOS_MAX_WINDOWS; i++) alive += wm.win[i].alive ? 1 : 0;
    EQ(alive, 5, "five windows open");
    CK(wm.focus != EOS_NONE, "something is focused");

    // 0x08 is HID left-GUI: byte 0 of the report when super is held.
    r = eos_keys_feed(&km, &wm, &st, scr, eos_keys_mods_from_hid(0x08), EOS_KEY_Q);
    CK(r.handled, "super+q is bound");
    EQ(r.action, EOS_ACT_CLOSE, "and it is bound to close");
    CK(r.changed, "the close moved something");
    alive = 0;
    for (i = 0; i < EOS_MAX_WINDOWS; i++) alive += wm.win[i].alive ? 1 : 0;
    EQ(alive, 4, "one window really closed");

    // 0x80 is right-GUI. The keyboard's other super key must work too.
    r = eos_keys_feed(&km, &wm, &st, scr, eos_keys_mods_from_hid(0x80), EOS_KEY_Q);
    CK(r.changed, "right super+q closes as well");
    alive = 0;
    for (i = 0; i < EOS_MAX_WINDOWS; i++) alive += wm.win[i].alive ? 1 : 0;
    EQ(alive, 3, "and it closed a second window");

    // With the launcher open. Super chords still dispatch, which is why the
    // model above hands them back rather than eating them.
    st.launcher_open = true;
    r = eos_keys_feed(&km, &wm, &st, scr, eos_keys_mods_from_hid(0x08), EOS_KEY_Q);
    CK(r.changed, "super+q works with the launcher open");
    st.launcher_open = false;

    // Down to nothing, and then past nothing without a crash or a phantom.
    for (i = 0; i < 6; i++)
        eos_keys_feed(&km, &wm, &st, scr, eos_keys_mods_from_hid(0x08), EOS_KEY_Q);
    alive = 0;
    for (i = 0; i < EOS_MAX_WINDOWS; i++) alive += wm.win[i].alive ? 1 : 0;
    EQ(alive, 0, "every window closed");
    EQ(wm.focus, EOS_NONE, "and nothing is focused");
    r = eos_keys_feed(&km, &wm, &st, scr, eos_keys_mods_from_hid(0x08), EOS_KEY_Q);
    CK(r.handled, "super+q on an empty desktop is still bound");
    CK(!r.changed, "but it changes nothing");

    // A bare q must NOT close a window. If it did, typing would be lethal.
    eos_wm_open(&wm, 0, scr);
    r = eos_keys_feed(&km, &wm, &st, scr, 0, EOS_KEY_Q);
    CK(!r.handled, "a bare q is not bound");
    alive = 0;
    for (i = 0; i < EOS_MAX_WINDOWS; i++) alive += wm.win[i].alive ? 1 : 0;
    EQ(alive, 1, "and it closed nothing");

    // super+space toggles the shell flag the launcher mirrors.
    r = eos_keys_feed(&km, &wm, &st, scr, eos_keys_mods_from_hid(0x08), EOS_KEY_SPACE);
    EQ(r.action, EOS_ACT_LAUNCHER, "super+space is the launcher");
    CK(st.launcher_open, "and it opened");
    r = eos_keys_feed(&km, &wm, &st, scr, eos_keys_mods_from_hid(0x08), EOS_KEY_SPACE);
    CK(!st.launcher_open, "a second super+space closed it");
}

// --------------------------------------------------- the two together

// The glue eos_shell_input.c runs, written out here so the handoff between
// eos_keys and the launcher is held to account and not merely described.
static void t_glue(void)
{
    eos_wm_t          wm;
    eos_wm_cfg_t      cfg = { 40, 30, 2, 12, 12 };
    eos_keymap_t      km;
    eos_shell_state_t st;
    eos_launcher_t    l;
    eos_rect_t        scr = { 0, 0, 240, 240 };
    eos_key_result_t  kr;
    eos_launcher_res_t lr;
    int i, alive;

    printf("  keymap and launcher share one keystream\n");

    eos_wm_init(&wm, &cfg);
    eos_keys_defaults(&km);
    eos_shell_state_init(&st, 1);
    fill(&l, NAPPS);
    rows_of(&l, 16);

    // super+space: the keymap owns it, and the launcher mirrors the flag.
    kr = eos_keys_feed(&km, &wm, &st, scr, eos_keys_mods_from_hid(0x08), EOS_KEY_SPACE);
    CK(kr.handled, "the keymap took super+space");
    eos_launcher_set_open(&l, st.launcher_open);
    CK(eos_launcher_is_open(&l), "the launcher opened with it");

    // Plain down: the keymap declines it, the launcher takes it.
    kr = eos_keys_feed(&km, &wm, &st, scr, 0, EOS_KEY_DOWN);
    CK(!kr.handled, "the keymap declines a bare down while the launcher is up");
    lr = eos_launcher_key(&l, EOS_KEY_DOWN, 0);
    EQ(lr.act, EOS_LAUNCHER_EAT, "the launcher took it");
    EQ(eos_launcher_selected(&l), 1, "and moved");

    // Enter: launched, and the caller opens the window.
    lr = eos_launcher_key(&l, EOS_KEY_ENTER, 0);
    EQ(lr.act, EOS_LAUNCHER_LAUNCH, "enter launched");
    st.launcher_open = eos_launcher_is_open(&l);
    CK(!st.launcher_open, "the shell flag followed the launcher closed");
    kr = eos_keys_apply(&wm, &st, scr, EOS_ACT_SPAWN, (int16_t)lr.app_id);
    CK(kr.changed, "a window opened");
    CK(kr.win != EOS_NONE, "and it has an id");
    alive = 0;
    for (i = 0; i < EOS_MAX_WINDOWS; i++) alive += wm.win[i].alive ? 1 : 0;
    EQ(alive, 1, "exactly one window");
    EQ(wm.win[kr.win].app_id, 1, "and it is the app the launcher named");

    // And super+q still closes it afterwards.
    kr = eos_keys_feed(&km, &wm, &st, scr, eos_keys_mods_from_hid(0x08), EOS_KEY_Q);
    CK(kr.changed, "super+q closed the window the launcher opened");
    alive = 0;
    for (i = 0; i < EOS_MAX_WINDOWS; i++) alive += wm.win[i].alive ? 1 : 0;
    EQ(alive, 0, "the desktop is empty again");
}

int main(void)
{
    printf("launcher\n");
    t_openclose();
    t_wrap();
    t_empty();
    t_launch();
    t_scroll();
    t_passthrough();
    t_pointer();
    t_close_path();
    t_glue();
    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
