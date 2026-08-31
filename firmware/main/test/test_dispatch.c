// Host checks for the ONE input pipeline: eos_shell_input_pump().
//
// Every key, character and trackpad click on this board leaves kernel/hal's
// 32-event ring inside that one function, and what it does with each of them
// is a ladder of six rungs written down at the top of eos_shell_input.h. This
// file drives that ladder through the real ring, the real keymap, the real
// window manager, the real launcher and the real app registry — nothing is
// mocked, because the bugs this suite exists to catch are all bugs of
// AGREEMENT between two of those, and a mock agrees with whatever you tell it.
//
// The observable for "did the focused window get this key" is the chat
// window's own dirty flag. Chat is the app in the table that takes the most
// keys, eos_app_chat_take_dirty() clears as it reports, and EOS_KEY_PGUP is a
// key it always consumes and no default bind claims — so a pgup that arrives
// makes chat dirty and a pgup that was eaten further up the ladder does not.
// That one flag is how every "the app must not see this" check below is
// phrased.
//
//   cc -std=c99 -Wall -Wextra -Werror -O1 \
//      -Ikernel/hal/include -Ikernel/wm/include -Ikernel/theme/include \
//      -Ikernel/shell/include -Ikernel/font/include -Ikernel/avatar/include \
//      -Ikernel/svc/include -Iboards/generated -Ifirmware/main \
//      firmware/main/test/test_dispatch.c firmware/main/eos_app_*.c ... -lm

#include <stdio.h>
#include <string.h>

#include "eos_shell_input.h"
#include "eos_shell_draw.h"
#include "eos_app_registry.h"
#include "eos_theme.h"
#include "eos_font.h"
#include "eos_led.h"
#include "waveshare-c6-lcd-13.h"

const eos_board_t *eos_board_get(void) { return &EOS_BOARD; }

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

// ------------------------------------------------------------------- rig

static eos_wm_t            wm;
static eos_theme_t         theme;
static eos_keymap_t        keys;
static eos_shell_state_t   shell;
static eos_shell_input_t   in;
static eos_launcher_t      launcher;
static eos_pointer_chrome_t chrome;
static eos_rect_t          screen;

static int win_clock, win_chat, win_board;

static uint32_t clk = 1000;
static uint32_t tick(void) { clk += 10; return clk; }

// Empties the ring and both dirty flags, so each check starts from a board
// that is not carrying the previous check's leftovers.
static void quiesce(void)
{
    eos_event_t e;
    while (eos_input_poll(&e)) { }
    (void)eos_app_chat_take_dirty();
}

// One press. The HAL's inject door takes the collapsed modifier byte the same
// way the BLE report path does, so a chord here is the same chord a keyboard
// would produce.
static void key(uint16_t k, uint8_t mods)
{
    eos_input_inject_key((uint8_t)k, true, mods, EOS_SRC_KEYBOARD, tick());
    eos_input_inject_key((uint8_t)k, false, mods, EOS_SRC_KEYBOARD, tick());
}

static void text(uint16_t ch)
{
    eos_input_inject_text(ch, EOS_SRC_WEB, tick());
}

static void click_at(int16_t x, int16_t y)
{
    eos_input_inject_pointer(EOS_EV_CLICK, x, y, EOS_BTN_LEFT, EOS_SRC_MOUSE, tick());
}

static void move_to(int16_t x, int16_t y)
{
    eos_input_inject_pointer(EOS_EV_POINTER_MOVE, x, y, 0, EOS_SRC_MOUSE, tick());
}

static int live_windows(void)
{
    int i, n = 0;
    for (i = 0; i < EOS_MAX_WINDOWS; i++) if (wm.win[i].alive) n++;
    return n;
}

// The tile currently showing `win`, or a zero rect. The same eos_wm_layout()
// the renderer runs, which is the point: a hit test checked against anything
// else is a hit test checked against a guess.
static eos_tile_t tile_of(int win)
{
    eos_tile_t t[EOS_MAX_WINDOWS * 2], z;
    int n, i;

    memset(&z, 0, sizeof z);
    n = eos_wm_layout(&wm, screen, t, EOS_MAX_WINDOWS * 2);
    for (i = 0; i < n; i++) if (t[i].win == win) return t[i];
    return z;
}

static void rig(void)
{
    eos_wm_cfg_t cfg;
    eos_launcher_geom_t lg;
    int i;

    screen = eos_rect(0, 0, W, H);

    memset(&cfg, 0, sizeof cfg);
    // The same split main.c makes: the minimum tile is the PANEL's, the gap
    // and the two strip heights are the THEME's.
    cfg.min_tile_w = EOS_BOARD.render.min_tile_w;
    cfg.min_tile_h = EOS_BOARD.render.min_tile_h;
    cfg.gap        = theme.m.gap;
    cfg.bar_h      = theme.m.bar_h;
    cfg.tab_h      = theme.m.tab_h;
    eos_wm_init(&wm, &cfg);

    eos_keys_defaults(&keys);
    eos_shell_state_init(&shell, 1);
    eos_shell_input_init(&in, &wm, &shell, &keys, screen, theme.m.bar_h);

    eos_launcher_init(&launcher);
    for (i = 0; i < eos_app_count(); i++) {
        const eos_app_t *a = eos_app_at(i);
        if (a) eos_launcher_add(&launcher, a->name, a->summary, (uint16_t)i);
    }
    eos_shell_launcher_geom(&theme, &lg);
    eos_launcher_set_geom(&launcher, &lg);
    eos_shell_input_launcher(&in, &launcher);

    eos_shell_tile_chrome(&theme, &chrome);
    eos_shell_input_chrome(&in, &chrome);

    win_clock = eos_wm_open(&wm, EOS_APP_CLOCK, screen);
    win_board = eos_wm_open(&wm, EOS_APP_BOARD, screen);
    win_chat  = eos_wm_open(&wm, EOS_APP_CHAT,  screen);
    eos_wm_focus_win(&wm, win_chat);

    eos_shell_input_set_active(&in, true);
    quiesce();
}

// --------------------------------------------------- rung 1: not the desktop

static void test_inactive(void)
{
    printf("  inactive\n");
    quiesce();

    eos_shell_input_set_active(&in, false);
    key(EOS_KEY_Q, EOS_MOD_SUPER);       // would close a window
    key(EOS_KEY_PGUP, 0);                // would reach chat
    text('a');                           // would reach chat
    click_at(120, 120);                  // would move focus

    ck(!eos_shell_input_pump(&in), "an inactive pump moves nothing");
    eq(live_windows(), 3, "super+q behind the setup screen closes no window");
    ck(!eos_app_chat_take_dirty(), "and no key reaches the focused window");

    {
        eos_event_t e;
        ck(!eos_input_poll(&e),
           "the ring is EMPTY afterwards: drained, not left for the desktop");
    }

    // And the pass that arrives at the desktop starts from nothing, which is
    // the whole reason the drain lives inside the dispatcher now.
    eos_shell_input_set_active(&in, true);
    ck(!eos_shell_input_pump(&in), "the first desktop pass has no backlog to replay");
    eq(live_windows(), 3, "and still no window has closed");
    quiesce();
}

// --------------------------------------------------------- rung 2: locked

static void test_locked(void)
{
    eos_tile_t t;

    printf("  locked\n");
    quiesce();

    shell.locked = true;

    key(EOS_KEY_PGUP, 0);
    text('a');
    (void)eos_shell_input_pump(&in);
    ck(!eos_app_chat_take_dirty(), "a locked board sends no key to the focused window");

    // The pointer is the half that used to get through: it never went near
    // eos_keys_feed(), which is where the lock was enforced.
    t = tile_of(win_clock);
    ck(t.rect.w > 0, "the clock has a tile to aim at");
    click_at((int16_t)(t.rect.x + t.rect.w / 2), (int16_t)(t.rect.y + t.rect.h / 2));
    (void)eos_shell_input_pump(&in);
    eq(wm.focus, win_chat, "a click on a locked board moves no focus");

    // And the close box, which is the click that would have been irreversible.
    {
        eos_rect_t box = eos_pointer_close_box(&chrome, &t);
        ck(box.w > 0, "the clock's tile has a close box");
        click_at((int16_t)(box.x + box.w / 2), (int16_t)(box.y + box.h / 2));
        (void)eos_shell_input_pump(&in);
        eq(live_windows(), 3, "and closes nothing either");
    }

    shell.locked = false;
    quiesce();
}

// ------------------------------------------- rung 4 beats rung 5: the chords

static void test_global_beats_app(void)
{
    printf("  the keymap beats the focused window\n");
    quiesce();

    // The one the owner asked for. Chat has the focus and takes q as a
    // character all day; super+q must still close its window.
    key(EOS_KEY_Q, EOS_MOD_SUPER);
    ck(eos_shell_input_pump(&in), "super+q moves the layout");
    eq(live_windows(), 2, "super+q closes the focused window");
    ck(!eos_app_chat_take_dirty(), "and the app never saw the q");

    win_chat = eos_wm_open(&wm, EOS_APP_CHAT, screen);
    eos_wm_focus_win(&wm, win_chat);
    quiesce();

    // HANDLED but not CHANGED. This is the regression the ladder's rung 4 was
    // written for: the pump used to test `changed` here, so a bind that fired
    // and found nothing to do handed its key on to the focused window. Bound
    // to pgup on purpose, because pgup is a key chat always consumes and the
    // default keymap never claims — so if it leaks, this check sees it.
    ck(eos_keys_bind(&keys, EOS_MOD_SUPER, EOS_KEY_PGUP, EOS_ACT_FOCUS_UP, 0),
       "super+pgup binds to focus-up for the length of this check");
    {
        eos_key_result_t r = eos_keys_apply(&wm, &shell, screen, EOS_ACT_FOCUS_UP, 0);
        (void)r;
    }
    // Drive it from the top of the layout, where focus-up has nowhere to go.
    while (eos_keys_apply(&wm, &shell, screen, EOS_ACT_FOCUS_UP, 0).changed) { }
    quiesce();

    key(EOS_KEY_PGUP, EOS_MOD_SUPER);
    (void)eos_shell_input_pump(&in);
    ck(!eos_app_chat_take_dirty(),
       "a chord a bind CLAIMED is eaten even when it moved nothing");

    ck(eos_keys_bind(&keys, EOS_MOD_SUPER, EOS_KEY_PGUP, EOS_ACT_NONE, 0),
       "and the bind comes back out again");
    quiesce();
}

// ------------------------------------------------ rung 5: the focused window

static void test_app_gets_the_rest(void)
{
    printf("  the focused window\n");
    eos_wm_focus_win(&wm, win_chat);
    quiesce();

    key(EOS_KEY_PGUP, 0);
    ck(!eos_shell_input_pump(&in), "a key only an app wanted is not a layout move");
    ck(eos_app_chat_take_dirty(), "an unbound key reaches the focused window");

    text('h');
    ck(!eos_shell_input_pump(&in), "and neither is a character");
    ck(eos_app_chat_take_dirty(), "a printable character reaches it too");

    // Focus somewhere else and the same key goes nowhere: the clock takes no
    // keys, and "the focused window" means the focused one.
    eos_wm_focus_win(&wm, win_clock);
    quiesce();
    key(EOS_KEY_PGUP, 0);
    (void)eos_shell_input_pump(&in);
    ck(!eos_app_chat_take_dirty(), "a window that is not focused gets nothing");

    eos_wm_focus_win(&wm, win_chat);
    quiesce();
}

// ----------------------------------------------------- rung 3: the launcher

static void test_launcher(void)
{
    int sel;

    printf("  the launcher\n");
    quiesce();

    key(EOS_KEY_SPACE, EOS_MOD_SUPER);
    (void)eos_shell_input_pump(&in);
    ck(shell.launcher_open, "super+space opens the launcher");
    ck(eos_launcher_is_open(&launcher), "and the model and the flag agree");

    sel = eos_launcher_selected(&launcher);
    key(EOS_KEY_DOWN, 0);
    ck(eos_shell_input_pump(&in), "down moves the highlight");
    eq(eos_launcher_selected(&launcher), sel + 1, "by exactly one row");

    // A key the launcher does not bind must not fall through to the window
    // underneath while an overlay is up.
    quiesce();
    key(EOS_KEY_PGUP, 0);
    (void)eos_shell_input_pump(&in);
    ck(!eos_app_chat_take_dirty(),
       "a key the launcher declined does not reach the window behind it");

    // Nor does a character.
    text('z');
    (void)eos_shell_input_pump(&in);
    ck(!eos_app_chat_take_dirty(), "and neither does a character");

    // But a SUPER chord is handed straight back, which is what keeps the
    // desktop reachable from inside the list.
    eq(live_windows(), 3, "three windows before");
    key(EOS_KEY_Q, EOS_MOD_SUPER);
    (void)eos_shell_input_pump(&in);
    eq(live_windows(), 2, "super+q still closes a window from inside the launcher");
    win_chat = eos_wm_open(&wm, EOS_APP_CHAT, screen);
    eos_wm_focus_win(&wm, win_chat);

    ck(eos_launcher_is_open(&launcher), "and the launcher is still up");

    key(EOS_KEY_ESC, 0);
    (void)eos_shell_input_pump(&in);
    ck(!shell.launcher_open, "escape closes it");
    ck(!eos_launcher_is_open(&launcher), "and the model closed with it");
    quiesce();
}

static void test_launcher_pointer(void)
{
    eos_launcher_geom_t g;
    eos_tile_t t;
    int16_t rx, ry;

    printf("  the launcher owns the cursor while it is up\n");
    quiesce();
    eos_shell_launcher_geom(&theme, &g);

    key(EOS_KEY_SPACE, EOS_MOD_SUPER);
    (void)eos_shell_input_pump(&in);
    ck(shell.launcher_open, "the launcher is up");

    // A hover inside the panel picks the row under the cursor.
    rx = (int16_t)(g.x + g.w / 2);
    ry = (int16_t)(g.list_y + g.row_h * 2 + g.row_h / 2);
    move_to(rx, ry);
    (void)eos_shell_input_pump(&in);
    eq(eos_launcher_selected(&launcher), eos_launcher_top(&launcher) + 2,
       "hovering the third visible row selects it");

    // A click on a TILE while the overlay is up reaches the overlay, not the
    // tile. Aim at the close box, which is the click that would be worst to
    // let through.
    t = tile_of(win_clock);
    ck(t.rect.w > 0, "the clock still has a tile");
    {
        eos_rect_t box = eos_pointer_close_box(&chrome, &t);
        ck(box.w > 0, "with a close box on it");
        click_at((int16_t)(box.x + box.w / 2), (int16_t)(box.y + box.h / 2));
        (void)eos_shell_input_pump(&in);
        eq(live_windows(), 3,
           "a click through the overlay onto a close box closes nothing");
    }

    // And a click OUTSIDE the panel closes the list, which is the launcher's
    // own rule and not the pump's.
    click_at(1, (int16_t)(H - 2));
    (void)eos_shell_input_pump(&in);
    ck(!shell.launcher_open, "a click off the panel closes the launcher");
    quiesce();
}

// ------------------------------------------------ the pointer on the desktop

static void test_pointer_desktop(void)
{
    eos_tile_t t;

    printf("  the cursor on the desktop\n");
    eos_wm_focus_win(&wm, win_chat);
    quiesce();

    t = tile_of(win_clock);
    ck(t.rect.w > 0 && t.visible, "the clock is on the glass");

    // Focus. Aim at the middle of the body, well clear of the header.
    click_at((int16_t)(t.rect.x + t.rect.w / 2),
             (int16_t)(t.rect.y + t.rect.h - 4));
    ck(eos_shell_input_pump(&in), "a click on a tile is a layout move");
    eq(wm.focus, win_clock, "and it focuses that tile");

    // Clicking the tile that already has focus is not a move, so the frame
    // loop is not asked to repaint the screen for nothing.
    click_at((int16_t)(t.rect.x + t.rect.w / 2),
             (int16_t)(t.rect.y + t.rect.h - 4));
    ck(!eos_shell_input_pump(&in), "clicking the focused tile changes nothing");

    // The close box. This is the owner's ask, from the trackpad half.
    {
        eos_rect_t box;

        t = tile_of(win_board);
        box = eos_pointer_close_box(&chrome, &t);
        ck(box.w > 0 && box.h > 0, "the board window has a close box");
        ck(box.x + box.w <= t.rect.x + t.rect.w, "inside its tile on the right");
        ck(box.y >= t.rect.y, "and starting at the tile's own top edge");

        click_at((int16_t)(box.x + box.w / 2), (int16_t)(box.y + box.h / 2));
        ck(eos_shell_input_pump(&in), "clicking it moves the layout");
        eq(live_windows(), 2, "because it closed the window");
        ck(!wm.win[win_board].alive, "that window and not another one");
    }

    win_board = eos_wm_open(&wm, EOS_APP_BOARD, screen);
    quiesce();

    // Motion alone is not a click, and the bar is not clickable.
    move_to(4, 4);
    ck(!eos_shell_input_pump(&in), "motion on the desktop moves no layout");
    click_at((int16_t)(W / 2), 1);
    ck(!eos_shell_input_pump(&in), "and the status bar is not a target");
    eq(live_windows(), 3, "nothing closed while poking at the bar");
    quiesce();
}

// ------------------------------------------------------------ arrival order

static void test_order(void)
{
    printf("  arrival order\n");
    eos_wm_focus_win(&wm, win_chat);
    quiesce();

    // One pass, three events, and the LAST one has to win: super+q closes the
    // focused window, so the focus change queued ahead of it decides which
    // window that is. Anything that dispatched clicks before keys, or keys
    // before clicks, would close the other one.
    {
        eos_tile_t t = tile_of(win_clock);
        click_at((int16_t)(t.rect.x + t.rect.w / 2),
                 (int16_t)(t.rect.y + t.rect.h - 4));
        key(EOS_KEY_Q, EOS_MOD_SUPER);

        ck(eos_shell_input_pump(&in), "the pass moved something");
        ck(!wm.win[win_clock].alive,
           "the click was dispatched before the chord that acted on it");
        ck(wm.win[win_chat].alive, "and the window that had the focus survived");
    }

    win_clock = eos_wm_open(&wm, EOS_APP_CLOCK, screen);
    quiesce();
}

// ------------------------------------------------------ the chrome is shared

static void test_chrome_agrees(void)
{
    eos_pointer_chrome_t a, b;

    printf("  one set of chrome numbers\n");

    eos_shell_tile_chrome(&theme, &a);
    ck(a.close_w > 0, "this theme has a close box");
    ck(a.hdr_h > 0, "and a header to put it in");
    eq(a.border, theme.m.border > 0 ? theme.m.border : 1,
       "the border is the theme's");

    // Pure: same theme, same answer, every time. The scene calls it once per
    // frame and the dispatcher holds a copy taken at boot; if it were not
    // pure those two would drift.
    eos_shell_tile_chrome(&theme, &b);
    eq(memcmp(&a, &b, sizeof a), 0, "and the answer does not move under it");

    // A tile that is not visible has no box, so a click on the rect a hidden
    // tab-group member still reports cannot close anything.
    {
        eos_tile_t t;
        memset(&t, 0, sizeof t);
        t.rect = eos_rect(0, 0, 120, 90);
        t.visible = false;
        ck(eos_pointer_close_box(&a, &t).w == 0, "a hidden tile has no close box");
        t.visible = true;
        ck(eos_pointer_close_box(&a, &t).w > 0, "and a visible one does");

        // Too narrow to spare close_w and keep half its header for the name.
        // The box is its full width or it is not there: a shrunken one would
        // be a rectangle that closes a window with no x drawn on it.
        t.rect = eos_rect(0, 0, (int16_t)(a.close_w * 2 + 2 * (a.border + 1)), 90);
        ck(eos_pointer_close_box(&a, &t).w == a.close_w,
           "a tile exactly wide enough gets the full box");
        t.rect.w--;
        ck(eos_pointer_close_box(&a, &t).w == 0,
           "and one pixel narrower gets none at all, not a smaller one");
    }

    // And no chrome at all means no box, which is what every caller written
    // before there was one gets.
    {
        eos_tile_t t;
        memset(&t, 0, sizeof t);
        t.rect = eos_rect(0, 0, 120, 90);
        t.visible = true;
        ck(eos_pointer_close_box(NULL, &t).w == 0, "a null chrome disables it");
    }
}

// ------------------------------------------------------------------- main

int main(void)
{
    const eos_board_t *b = eos_board_get();

    printf("dispatch\n");

    if (eos_display_init() != EOS_OK) { printf("display init failed\n"); return 1; }
    eos_theme_default(&theme);
    eos_input_init(NULL);
    eos_app_bind(NULL, NULL, b, NULL);

    rig();

    ck(eos_app_table_ok(), "the app table is consistent before anything is driven");
    eq(eos_app_index_of("chat"), (long)EOS_APP_CHAT,
       "sys.autostart resolves \"chat\" through the registry's id column");
    eq(eos_app_index_of("nosuchapp"), -1, "and an unknown id resolves to nothing");

    test_chrome_agrees();
    test_inactive();
    test_locked();
    test_global_beats_app();
    test_app_gets_the_rest();
    test_launcher();
    test_launcher_pointer();
    test_pointer_desktop();
    test_order();

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed != 0;
}
