// Host test for eos_wm. Builds layouts at the real pixel sizes of the panels
// in this repo and renders them as ASCII so the tiling is visible, then checks
// the invariants that matter: no overlap, nothing off-screen, nothing below
// the minimum tile size, every window accounted for, one visible tab per group.

#include <stdio.h>
#include <string.h>
#include "eos_wm.h"

static int checks = 0, fails = 0;
#define CK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("    FAIL: %s\n", msg); } } while (0)

static const char *APPNAME[] = { "term", "buddy", "files", "settings", "arcade", "cpm" };

static int overlaps(eos_rect_t a, eos_rect_t b)
{
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
}

static void render(const eos_wm_t *wm, eos_rect_t screen, int gw, int gh)
{
    eos_tile_t t[EOS_MAX_WINDOWS * 2];
    int n = eos_wm_layout(wm, screen, t, EOS_MAX_WINDOWS * 2);

    printf("    +");
    for (int i = 0; i < gw; i++) printf("-");
    printf("+\n");

    for (int gy = 0; gy < gh; gy++) {
        printf("    |");
        for (int gx = 0; gx < gw; gx++) {
            int px = screen.x + (int)((long)gx * screen.w / gw);
            int py = screen.y + (int)((long)gy * screen.h / gh);
            char c = '.';
            if (py < screen.y + wm->cfg.bar_h) c = '#';           // status bar
            for (int i = 0; i < n && c == '.'; i++) {
                eos_rect_t r = t[i].tab_rect;
                if (r.w > 0 && px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h)
                    c = '=';                                       // tab strip
            }
            for (int i = 0; i < n && (c == '.' || c == '='); i++) {
                if (!t[i].visible) continue;
                eos_rect_t r = t[i].rect;
                if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h)
                    c = (char)((t[i].focused ? 'A' : 'a') + t[i].win);
            }
            printf("%c", c);
        }
        printf("|\n");
    }
    printf("    +");
    for (int i = 0; i < gw; i++) printf("-");
    printf("+\n");

    // textual summary
    for (int i = 0; i < n; i++) {
        if (t[i].tab_group != EOS_NONE && t[i].tab_index == 0) {
            printf("    tab group: ");
            for (int j = 0; j < n; j++)
                if (t[j].tab_group == t[i].tab_group)
                    printf("%s[%c:%s]", t[j].visible ? ">" : " ",
                           (char)('a' + t[j].win),
                           t[j].app_id < 6 ? APPNAME[t[j].app_id] : "?");
            printf("\n");
        }
    }
}

static void verify(const eos_wm_t *wm, eos_rect_t screen, int expect_windows)
{
    eos_tile_t t[EOS_MAX_WINDOWS * 2];
    int n = eos_wm_layout(wm, screen, t, EOS_MAX_WINDOWS * 2);

    int seen[EOS_MAX_WINDOWS] = {0};
    int visible = 0;
    for (int i = 0; i < n; i++) {
        if (t[i].win >= 0) seen[t[i].win]++;
        if (!t[i].visible) continue;
        visible++;
        eos_rect_t r = t[i].rect;
        CK(r.x >= screen.x && r.y >= screen.y &&
           r.x + r.w <= screen.x + screen.w &&
           r.y + r.h <= screen.y + screen.h, "tile inside screen");
        CK(r.w > 0 && r.h > 0, "tile non-empty");
        for (int j = i + 1; j < n; j++)
            if (t[j].visible) CK(!overlaps(r, t[j].rect), "tiles do not overlap");
    }

    int counted = 0;
    for (int i = 0; i < EOS_MAX_WINDOWS; i++) {
        if (wm->win[i].alive && wm->win[i].ws == wm->ws) {
            counted++;
            CK(seen[i] == 1, "each window reported exactly once");
        } else {
            CK(seen[i] == 0, "no stale windows reported");
        }
    }
    CK(counted == expect_windows, "workspace holds the expected window count");

    // exactly one visible member per tab group
    for (int i = 0; i < n; i++) {
        if (t[i].tab_group == EOS_NONE || t[i].tab_index != 0) continue;
        int vis = 0;
        for (int j = 0; j < n; j++)
            if (t[j].tab_group == t[i].tab_group && t[j].visible) vis++;
        CK(vis == 1, "exactly one visible tab per group");
    }

    // minimum tile size honoured whenever the screen could host it at all
    for (int i = 0; i < n; i++) {
        if (!t[i].visible) continue;
        if (screen.w >= 2 * wm->cfg.min_tile_w && screen.h >= 2 * wm->cfg.min_tile_h)
            CK(t[i].rect.w >= wm->cfg.min_tile_w && t[i].rect.h >= wm->cfg.min_tile_h,
               "visible tile meets minimum size");
    }
    (void)visible;
}

static void scenario(const char *name, eos_rect_t screen, eos_wm_cfg_t cfg,
                     int nwin, int gw, int gh)
{
    printf("\n  %s  (%dx%d, min tile %dx%d)\n", name, screen.w, screen.h,
           cfg.min_tile_w, cfg.min_tile_h);
    eos_wm_t wm;
    eos_wm_init(&wm, &cfg);
    for (int i = 0; i < nwin; i++) eos_wm_open(&wm, (uint16_t)i, screen);
    render(&wm, screen, gw, gh);
    verify(&wm, screen, nwin);
}

int main(void)
{
    eos_wm_cfg_t big   = { 110, 70, 3, 12, 11 };
    eos_wm_cfg_t small = { 96,  60, 2, 11, 10 };
    eos_wm_cfg_t tiny  = { 60,  24, 1,  8,  8 };

    printf("=== eos_wm layout across the panels in this repo ===\n");
    printf("    A/a = window (uppercase = focused), # = status bar, = = tab strip\n");

    scenario("ILI9488 4in landscape  (tft-videos)",
             (eos_rect_t){0,0,480,320}, big,   4, 72, 24);
    scenario("ILI9341 2.4in CYD      (ESP-OS-CY24)",
             (eos_rect_t){0,0,320,240}, small, 4, 56, 21);
    scenario("ST7789 1.47in C5       (esp32-c5)",
             (eos_rect_t){0,0,320,172}, small, 3, 56, 15);
    scenario("SSD1306 0.96in OLED    (the-displays)",
             (eos_rect_t){0,0,128,64},  tiny,  4, 44, 14);

    // ---- behaviour tests on the CYD geometry -------------------------------
    eos_rect_t scr = {0,0,320,240};
    eos_wm_t wm;
    eos_wm_init(&wm, &small);

    printf("\n  behaviour\n");
    int a = eos_wm_open(&wm, 0, scr);
    int b = eos_wm_open(&wm, 1, scr);
    int c = eos_wm_open(&wm, 2, scr);
    CK(a == 0 && b == 1 && c == 2, "window ids allocate in order");
    CK(wm.focus == c, "newest window takes focus");

    CK(eos_wm_focus_dir(&wm, EOS_DIR_LEFT, scr), "can move focus left");
    int after_left = wm.focus;
    CK(after_left != c, "focus actually moved");
    eos_wm_focus_win(&wm, c);

    eos_wm_close(&wm, b);
    CK(!wm.win[b].alive, "closed window is dead");
    verify(&wm, scr, 2);
    CK(wm.focus == c, "focus survives closing an unfocused window");

    eos_wm_close(&wm, c);
    CK(wm.focus == a, "focus falls back after closing the focused window");
    verify(&wm, scr, 1);
    eos_wm_close(&wm, a);
    CK(wm.focus == EOS_NONE, "empty workspace has no focus");
    CK(wm.root[0] == EOS_NONE, "empty workspace has no root");

    // node pool must be fully reclaimed
    int used = 0;
    for (int i = 0; i < EOS_MAX_NODES; i++) if (wm.node_used[i]) used++;
    CK(used == 0, "node pool fully reclaimed after closing everything");

    // workspaces
    eos_wm_init(&wm, &small);
    eos_wm_open(&wm, 0, scr);
    eos_wm_open(&wm, 1, scr);
    eos_wm_goto_workspace(&wm, 1);
    CK(wm.focus == EOS_NONE, "switching to an empty workspace clears focus");
    int d = eos_wm_open(&wm, 2, scr);
    verify(&wm, scr, 1);
    CK(eos_wm_move_to_workspace(&wm, d, 0), "window moves to workspace 1");
    CK(wm.win[d].ws == 0, "moved window records its new workspace");
    verify(&wm, scr, 0);
    eos_wm_goto_workspace(&wm, 0);
    verify(&wm, scr, 3);

    // forced splits and resize
    eos_wm_init(&wm, &small);
    eos_wm_open(&wm, 0, scr);
    eos_wm_set_split(&wm, EOS_SPLIT_ROWS);
    eos_wm_open(&wm, 1, scr);
    eos_tile_t t[8];
    int n = eos_wm_layout(&wm, scr, t, 8);
    CK(n == 2 && t[0].rect.y != t[1].rect.y && t[0].rect.x == t[1].rect.x,
       "forced ROWS split stacks vertically");
    int h_before = t[1].rect.h;
    CK(eos_wm_resize(&wm, 150), "resize accepted");
    eos_wm_layout(&wm, scr, t, 8);
    CK(t[1].rect.h > h_before, "focused tile grew");

    // pool exhaustion is graceful
    eos_wm_init(&wm, &small);
    int opened = 0;
    for (int i = 0; i < EOS_MAX_WINDOWS + 4; i++)
        if (eos_wm_open(&wm, 0, scr) != EOS_NONE) opened++;
    CK(opened == EOS_MAX_WINDOWS, "opens exactly EOS_MAX_WINDOWS then refuses");
    verify(&wm, scr, EOS_MAX_WINDOWS);

    printf("\n=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
