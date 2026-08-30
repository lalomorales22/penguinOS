// test_integration — the checks that no single component can make about itself.
//
// Every component here has its own test suite and every one of them passes in
// isolation. That is not the same as the components agreeing with each other,
// and the failures that matter on a repo built in parallel are all of the
// second kind: two enums that were meant to be in lockstep drifting apart, a
// palette index claimed by two headers, a struct field that moved after the
// generator last read it. None of those show up in a component's own tests,
// because a component's own tests never include anybody else's header.
//
// So this file includes ALL of them, in one translation unit, and asserts the
// contracts that are written down in comments but enforced nowhere. If it does
// not compile, two components have collided. If it compiles and fails, two
// components disagree.
//
// The one non-obvious constraint: this test may only use what is checked in.
// The board registry's C headers are generated build output and are deliberately
// not in the tree, so the registry-to-eos_board_t half of the contract is not
// covered here. That half is a separate command, recorded in STATUS.md.
//
// Build:
//   cc -std=c99 -Wall -Wextra -O1 \
//      -Ikernel/wm/include -Ikernel/hal/include -Ikernel/theme/include \
//      -Ikernel/avatar/include -Ikernel/shell/include -Ikernel/svc/include \
//      kernel/test/test_integration.c kernel/wm/eos_wm.c kernel/theme/eos_theme.c \
//      kernel/shell/eos_bar.c -o t && ./t

#include <stdio.h>
#include <string.h>

// Include order is deliberately not dependency order. These headers must
// compose in any order and survive being included twice; a clashing include
// guard or a duplicated typedef fails the build right here.
#include "eos_brain.h"
#include "eos_bar.h"
#include "eos_keys.h"
#include "eos_buddy.h"
#include "eos_vox.h"
#include "eos_theme.h"
#include "eos_storage.h"
#include "eos_input.h"
#include "eos_display.h"
#include "eos_board.h"
#include "eos_wm.h"
#include "eos_wm.h"      /* twice, on purpose */
#include "eos_display.h" /* twice, on purpose */

static int checks = 0, failed = 0;

static void ck(const char *what, int ok)
{
    checks++;
    if (!ok) { failed++; printf("  FAIL  %s\n", what); }
}

// ---------------------------------------------------------------------------
// The bar renders the buddy's mood by casting one enum to the other rather than
// through a switch it would forget to update. eos_bar.h says so and cannot
// check it, because it deliberately does not include eos_buddy.h.
static void moods(void)
{
    printf("bar mood <-> buddy state\n");
    ck("IDLE",      (int)EOS_MOOD_IDLE      == (int)EOS_BUDDY_IDLE);
    ck("THINKING",  (int)EOS_MOOD_THINKING  == (int)EOS_BUDDY_THINKING);
    ck("TALKING",   (int)EOS_MOOD_TALKING   == (int)EOS_BUDDY_TALKING);
    ck("LISTENING", (int)EOS_MOOD_LISTENING == (int)EOS_BUDDY_LISTENING);
    ck("SLEEPING",  (int)EOS_MOOD_SLEEPING  == (int)EOS_BUDDY_SLEEPING);
    ck("HAPPY",     (int)EOS_MOOD_HAPPY     == (int)EOS_BUDDY_HAPPY);
    ck("CONFUSED",  (int)EOS_MOOD_CONFUSED  == (int)EOS_BUDDY_CONFUSED);
    ck("no eighth buddy state the bar cannot name",
       (int)EOS_BUDDY_STATE_COUNT == (int)EOS_MOOD_CONFUSED + 1);

    // Every mood must render as something. A NULL here is a blank status bar.
    {
        int i;
        for (i = 0; i < (int)EOS_BUDDY_STATE_COUNT; i++) {
            const char *g = eos_bar_mood_glyph((eos_bar_mood_t)i);
            ck("mood glyph exists", g != NULL && g[0] != 0);
        }
    }
}

// ---------------------------------------------------------------------------
// eos_bar.h keeps EOS_WORKSPACE_PIPS as a literal so the bar model builds with
// no window manager present. That is fine right up until somebody changes
// EOS_WORKSPACES and the bar starts drawing pips for workspaces that cannot
// exist, or stops drawing one that can.
static void workspaces(void)
{
    printf("workspace pips <-> window manager\n");
    ck("EOS_WORKSPACE_PIPS == EOS_WORKSPACES", EOS_WORKSPACE_PIPS == EOS_WORKSPACES);
}

// ---------------------------------------------------------------------------
// The transparency sentinel and the theme palette both want an index, and there
// are only 256. eos_display.h reserves 255; eos_theme.h must not hand it out.
// A colour that resolves to 255 silently does not draw, which is invisible in
// the theme's own tests and very visible on a panel.
static void palette_sentinel(void)
{
    eos_theme_t t;
    eos_rgb_t white = { 255, 255, 255 };
    int r, g, b;
    long cube_hits = 0, search_hits = 0;

    printf("palette sentinel\n");
    eos_theme_default(&t);

    ck("EOS_COLOR_NONE is the theme's reserved slot",
       (int)EOS_COLOR_NONE == EOS_PAL_CUBE_NONE);
    ck("the cube does not hand out the reserved slot for white",
       eos_theme_cube_index(white) != EOS_COLOR_NONE);
    ck("the nearest-match search does not either",
       eos_theme_index(&t, white) != EOS_COLOR_NONE);
    ck("palette_len leaves exactly one slot out",
       EOS_PALETTE_MAX - 1 == EOS_PAL_SIZE - 1);

    for (r = 0; r < 256; r += 3)
        for (g = 0; g < 256; g += 3)
            for (b = 0; b < 256; b += 3) {
                eos_rgb_t c;
                c.r = (uint8_t)r; c.g = (uint8_t)g; c.b = (uint8_t)b;
                if (eos_theme_cube_index(c) == EOS_COLOR_NONE) cube_hits++;
                if (eos_theme_index(&t, c) == EOS_COLOR_NONE) search_hits++;
            }
    printf("      swept %d colours: cube hit the sentinel %ld times, search %ld\n",
           86 * 86 * 86, cube_hits, search_hits);
    ck("no colour anywhere resolves to the sentinel", cube_hits == 0 && search_hits == 0);

    // White still has to be IN the palette, just not reachable as a draw index.
    ck("pal565[255] is still white", eos_theme_palette565(&t)[255] == 0xFFFF);
}

// ---------------------------------------------------------------------------
// eos_theme.h says gap/bar_h/tab_h are "copied straight into eos_wm_cfg_t", and
// that min_tile_w/min_tile_h are deliberately absent because they belong to the
// panel. That leaves the shell assembling one struct from two sources, so the
// types have to line up and the result has to lay out.
static void theme_drives_wm(void)
{
    eos_theme_t th;
    eos_wm_cfg_t cfg;
    eos_wm_t wm;
    eos_rect_t screen = { 0, 0, 320, 240 };
    eos_tile_t tiles[EOS_MAX_WINDOWS];
    int n, i;

    printf("theme metrics -> eos_wm_cfg_t\n");
    eos_theme_default(&th);

    // From the board. 80x40 is what boards/*.json carries for the CYD.
    cfg.min_tile_w = 80;
    cfg.min_tile_h = 40;
    // From the theme, verbatim. If these are ever not assignable, this breaks.
    cfg.gap   = th.m.gap;
    cfg.bar_h = th.m.bar_h;
    cfg.tab_h = th.m.tab_h;

    ck("gap survived the copy",   cfg.gap   == th.m.gap);
    ck("bar_h survived the copy", cfg.bar_h == th.m.bar_h);
    ck("tab_h survived the copy", cfg.tab_h == th.m.tab_h);

    eos_wm_init(&wm, &cfg);
    for (i = 0; i < 5; i++)
        ck("window opened", eos_wm_open(&wm, (uint16_t)i, screen) != EOS_NONE);

    n = eos_wm_layout(&wm, screen, tiles, EOS_MAX_WINDOWS);
    ck("all five laid out", n == 5);
    for (i = 0; i < n; i++) {
        if (!tiles[i].visible) continue;
        ck("tile is on screen",
           tiles[i].rect.x >= 0 && tiles[i].rect.y >= 0 &&
           tiles[i].rect.x + tiles[i].rect.w <= screen.w &&
           tiles[i].rect.y + tiles[i].rect.h <= screen.h);
        ck("tile clears the theme's status bar", tiles[i].rect.y >= th.m.bar_h);
        ck("tile is not a sliver",
           tiles[i].rect.w >= cfg.min_tile_w && tiles[i].rect.h >= cfg.min_tile_h);
    }

    // The theme's own clamps must stay inside what a 16-bit layout can take.
    ck("gap is sane",   th.m.gap   >= 0 && th.m.gap   <= 32);
    ck("border is sane",th.m.border>= 0 && th.m.border<= 8);
    ck("bar_h is sane", th.m.bar_h >= 0 && th.m.bar_h <= 64);
    ck("tab_h is sane", th.m.tab_h >= 0 && th.m.tab_h <= 64);
}

// ---------------------------------------------------------------------------
// eos_bar.h maps its five roles onto eos_role_t "itself", in a comment. The bar
// cannot check that; the renderer that does the mapping does not exist yet. The
// least this can do is prove every role the bar emits has somewhere to land.
static void bar_roles(void)
{
    static const eos_role_t MAP[] = {
        EOS_ROLE_BAR_FG, EOS_ROLE_MUTED, EOS_ROLE_ACCENT,
        EOS_ROLE_OK,     EOS_ROLE_WARN
    };
    eos_theme_t t;
    int i;

    printf("bar roles -> theme roles\n");
    eos_theme_default(&t);
    ck("the map covers every bar role",
       (int)(sizeof MAP / sizeof MAP[0]) == (int)EOS_BAR_ROLE_WARN + 1);
    for (i = 0; i < (int)(sizeof MAP / sizeof MAP[0]); i++) {
        ck("bar role names a real theme role", MAP[i] < EOS_ROLE_COUNT);
        ck("that role sits in the role block of the palette",
           eos_theme_role_index(&t, MAP[i]) == (uint8_t)(EOS_PAL_ROLE_BASE + MAP[i]));
        ck("that role is not the sentinel",
           eos_theme_role_index(&t, MAP[i]) != EOS_COLOR_NONE);
    }
}

// ---------------------------------------------------------------------------
// eos_brain's events are documented as mapping one-for-one onto the buddy's.
// Nothing wires them together yet, so the most this can assert is that the
// buddy has a distinct destination for each brain event that claims one, and
// that driving the buddy through a whole request lifecycle actually moves it.
static void brain_drives_buddy(void)
{
    eos_buddy_t b;
    eos_buddy_cfg_t cfg;

    printf("brain lifecycle -> buddy moods\n");
    eos_buddy_default_cfg(&cfg);
    eos_buddy_init(&b, NULL, &cfg);

    eos_buddy_event(&b, EOS_BUDDY_EV_USER_TYPING);
    ck("typing -> LISTENING", eos_buddy_state(&b) == EOS_BUDDY_LISTENING);

    eos_buddy_event(&b, EOS_BUDDY_EV_REQUEST_SENT);      /* EV_SUBMITTED   */
    ck("submitted -> THINKING", eos_buddy_state(&b) == EOS_BUDDY_THINKING);

    eos_buddy_event(&b, EOS_BUDDY_EV_STREAM_FIRST);      /* EV_FIRST_TOKEN */
    ck("first token -> TALKING", eos_buddy_state(&b) == EOS_BUDDY_TALKING);

    // The gap that lapses a reply back to IDLE must be longer than one frame,
    // or the buddy drops out of TALKING between chunks of a live stream.
    eos_buddy_tick(&b, 30);
    ck("a 30ms frame does not lapse the reply", eos_buddy_state(&b) == EOS_BUDDY_TALKING);

    eos_buddy_event(&b, EOS_BUDDY_EV_STREAM_DONE);       /* EV_DONE        */
    ck("done -> HAPPY", eos_buddy_state(&b) == EOS_BUDDY_HAPPY);

    eos_buddy_event(&b, EOS_BUDDY_EV_ERROR);             /* EV_FAILED      */
    ck("error -> CONFUSED", eos_buddy_state(&b) == EOS_BUDDY_CONFUSED);

    // The bar reads the same value the buddy is in.
    ck("mood cast is meaningful",
       (int)(eos_bar_mood_t)eos_buddy_state(&b) == (int)EOS_MOOD_CONFUSED);
}

// ---------------------------------------------------------------------------
// Sizes that the tier-0 heap budget depends on. Not a correctness check so much
// as a tripwire: these are the numbers the RAM arithmetic in the docs uses.
static void footprints(void)
{
    printf("footprints\n");
    printf("      eos_wm_t %zu  eos_theme_t %zu  eos_brain_t %zu  eos_buddy_t %zu\n",
           sizeof(eos_wm_t), sizeof(eos_theme_t), sizeof(eos_brain_t), sizeof(eos_buddy_t));
    printf("      eos_board_t %zu  eos_event_t %zu  eos_rect_t %zu\n",
           sizeof(eos_board_t), sizeof(eos_event_t), sizeof(eos_rect_t));

    ck("eos_event_t is still 16 bytes", sizeof(eos_event_t) == 16);
    ck("eos_rect_t is four int16s", sizeof(eos_rect_t) == 8);
    ck("eos_theme_t fits a tier-0 budget", sizeof(eos_theme_t) <= 1024);
    // eos_rect_t comes from eos_wm.h and is used by the HAL. If the HAL ever
    // declares its own, this stops being one type and the WM stops agreeing
    // with the display about where anything is.
    {
        eos_rect_t a = eos_rect(1, 2, 3, 4);
        eos_tile_t t;
        memset(&t, 0, sizeof t);
        t.rect = a;   /* will not compile if these are two different types */
        ck("the HAL and the WM share one rect type",
           t.rect.x == 1 && t.rect.y == 2 && t.rect.w == 3 && t.rect.h == 4);
    }
}

int main(void)
{
    printf("== esp-os cross-component integration ==\n\n");
    moods();
    workspaces();
    palette_sentinel();
    theme_drives_wm();
    bar_roles();
    brain_drives_buddy();
    footprints();
    printf("\n=== %d checks, %d failed ===\n", checks, failed);
    return failed ? 1 : 0;
}
