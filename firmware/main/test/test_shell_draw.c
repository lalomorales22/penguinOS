// Host checks for the desktop scene, and the tool that renders it to a PPM.
//
// Everything in eos_shell_draw.c above the SPI call is arithmetic — the tile
// bodies, the tab strip, the bar fitter's placement and, since this run, the
// avatar's offscreen render and blit — so all of it can be held to account off
// target. Nothing here checks the wiring; that is the probe's job and it is
// already done.
//
// The buddy is why this file exists. It is the one window in penguinOS whose
// correctness cannot be read off a number, and it is also the one that breaks
// the rule the rest of the scene is built on: eos_buddy_render() writes whole
// pixels and REORDERS the model in place, and the scene is replayed once per
// band. So the checks below are mostly about that: that the avatar is rendered
// exactly once per frame, that two identical frames come out identical, and
// that the window still draws when there is no model to draw.
//
// The one non-obvious thing is that this #includes eos_shell_draw.c rather
// than linking it. eos_display_host_band() only answers while a frame is open,
// so a snapshot has to happen INSIDE the band loop — and that loop is inside
// eos_shell_draw_frame(). Including the translation unit puts scene() in
// reach, so the frame can be re-run here with one line added, instead of a
// hook being cut into production code that only a test would ever use. Do not
// also pass eos_shell_draw.c to the linker.
//
//   cc -std=c99 -Wall -Wextra -Werror -O1 \
//      -Ikernel/hal/include -Ikernel/wm/include -Ikernel/theme/include \
//      -Ikernel/shell/include -Ikernel/font/include -Ikernel/avatar/include \
//      -Iboards/generated -Ifirmware/main \
//      firmware/main/test/test_shell_draw.c firmware/main/eos_buddy_model.c \
//      kernel/hal/backend/esp_lcd/eos_display_st7789.c kernel/wm/eos_wm.c \
//      kernel/theme/eos_theme.c kernel/shell/eos_bar.c kernel/shell/eos_keys.c \
//      kernel/font/eos_font.c kernel/avatar/eos_vox.c kernel/avatar/eos_buddy.c \
//      -lm -o /tmp/tdraw && /tmp/tdraw
//
// With a path argument it also writes that frame as a PPM, which is what
// eos_shell_draw.h has always promised and nothing had taken it up on:
//
//   /tmp/tdraw /tmp/desktop.ppm            # idle
//   /tmp/tdraw /tmp/thinking.ppm thinking  # any eos_buddy_state_t name

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "eos_buddy_model.h"
#include "waveshare-c6-lcd-13.h"

// Not a header. See the note above.
#include "eos_shell_draw.c"

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

// ------------------------------------------------------------- the canvas

static uint16_t shot[H][W];
static uint16_t prev[H][W];
static uint8_t  cover[H][W];

static void take_band(void)
{
    eos_rect_t b;
    const uint16_t *px = eos_display_host_band(&b);
    int x, y;

    if (!px) return;
    for (y = 0; y < b.h; y++)
        for (x = 0; x < b.w; x++) {
            shot[b.y + y][b.x + x] = px[y * b.w + x];
            cover[b.y + y][b.x + x]++;
        }
}

static eos_wm_t         wm;
static eos_theme_t      theme;
static eos_keymap_t     keys;
static eos_bar_status_t bar;
static eos_buddy_t      buddy;
static eos_shell_view_t view;

// eos_shell_draw_frame(), with one line added. Every other line is the
// production function verbatim, which is the point: the avatar is rendered
// ONCE, before the frame opens, and the scene that replays into the bands does
// nothing with it but blit a picture that is already finished.
//
// Returns the number of bands the frame produced.
static int frame(void)
{
    skin_t s;
    eos_rect_t band;
    int n = 0;

    memset(cover, 0, sizeof cover);
    skin_build(&s, &view);
    buddy_prepare(&view, &s);
    eos_display_frame_begin();
    while (eos_display_frame_band(&band)) { scene(&view, &s); take_band(); n++; }
    eos_display_frame_end();
    return n;
}

// The same frame, with the trackpad reporting BETWEEN bands. That is the real
// shape of the hazard: scene() is re-run once per band and reads the cursor
// every time, and the BLE host task that moves it preempts the task drawing.
// Without eos_pointer_latch() band 1 and band 2 paint the arrow in two
// different places and the first one's pixels are outside every rect the next
// frame repaints, so a fragment of it stays on the glass.
static int frame_jostled(eos_pointer_t *p)
{
    skin_t s;
    eos_rect_t band;
    int n = 0;

    memset(cover, 0, sizeof cover);
    skin_build(&s, &view);
    buddy_prepare(&view, &s);
    eos_display_frame_begin();
    while (eos_display_frame_band(&band)) {
        scene(&view, &s);
        take_band();
        n++;
        eos_pointer_feed(p, 9, 9, 0, view.pointer_ms);
    }
    eos_display_frame_end();
    return n;
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;

    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            // The backend keeps the panel's byte order; unswap, then unpack.
            uint16_t v = (uint16_t)((shot[y][x] >> 8) | (shot[y][x] << 8));
            unsigned r = (v >> 11) & 0x1Fu, g = (v >> 5) & 0x3Fu, b = v & 0x1Fu;
            unsigned char px[3];
            px[0] = (unsigned char)((r << 3) | (r >> 2));
            px[1] = (unsigned char)((g << 2) | (g >> 4));
            px[2] = (unsigned char)((b << 3) | (b >> 2));
            fwrite(px, 1, 3, f);
        }
    fclose(f);
    printf("    wrote %s\n", path);
}

// ------------------------------------------------------------------ setup

static void upload_theme(void)
{
    uint32_t rgb[32];
    int i, j;

    for (i = 0; i < EOS_PAL_SIZE; i += 32) {
        for (j = 0; j < 32; j++) {
            eos_rgb_t c = eos_theme_palette_rgb(&theme, (uint8_t)(i + j));
            rgb[j] = eos_rgb(c.r, c.g, c.b);
        }
        eos_display_palette(rgb, (uint16_t)i, 32);
    }
}

static void adopt_default_buddy(void)
{
    eos_buddy_cfg_t cfg;
    eos_buddy_model_default_cfg(&cfg);
    eos_shell_buddy_shade(eos_buddy_model_default_palette(), &cfg);
    eos_buddy_init(&buddy, eos_buddy_model_default(), &cfg);
}

// The tile the buddy is in, or an empty rect when it is not on screen.
static eos_rect_t buddy_rect(void)
{
    eos_tile_t t[EOS_MAX_WINDOWS * 2];
    int n = eos_wm_layout(&wm, eos_rect(0, 0, W, H), t, EOS_MAX_WINDOWS * 2);
    int i;
    for (i = 0; i < n; i++)
        if (t[i].visible && t[i].app_id == EOS_APP_BUDDY) return t[i].rect;
    return eos_rect(0, 0, 0, 0);
}

// How many pixels inside `r` are neither the surface colour nor the border's.
// This is "did the avatar actually draw", asked of the composited panel rather
// than of the renderer's return value, which is the only version of the
// question that catches a blit landing outside the clip.
static int ink_in(eos_rect_t r)
{
    uint16_t surf, foc;
    int x, y, n = 0;

    {
        eos_rgb_t c = eos_theme_role_rgb(&theme, EOS_ROLE_SURFACE);
        uint16_t v = eos_theme_rgb565(c);
        surf = (uint16_t)((v >> 8) | (v << 8));
        c = eos_theme_role_rgb(&theme, EOS_ROLE_BORDER_FOCUSED);
        v = eos_theme_rgb565(c);
        foc = (uint16_t)((v >> 8) | (v << 8));
    }
    for (y = r.y; y < r.y + r.h; y++)
        for (x = r.x; x < r.x + r.w; x++)
            if (shot[y][x] != surf && shot[y][x] != foc) n++;
    return n;
}

int main(int argc, char **argv)
{
    const eos_board_t *b = eos_board_get();
    const char *out  = argc > 1 ? argv[1] : NULL;
    const char *want = argc > 2 ? argv[2] : "idle";
    eos_wm_cfg_t cfg;
    eos_rect_t screen, br;
    int i, bands, uncovered, once, ink;

    if (eos_display_init() != EOS_OK) { printf("display init failed\n"); return 1; }

    eos_theme_default(&theme);
    upload_theme();

    memset(&cfg, 0, sizeof cfg);
    cfg.min_tile_w = b->render.min_tile_w;
    cfg.min_tile_h = b->render.min_tile_h;
    cfg.gap        = theme.m.gap;
    cfg.bar_h      = theme.m.bar_h;
    cfg.tab_h      = theme.m.tab_h;
    eos_wm_init(&wm, &cfg);

    // The same five, in the same order app_main opens them in. The buddy is
    // last, and that is what puts it on screen as the visible tab.
    screen = eos_board_screen(b);
    eos_wm_open(&wm, EOS_APP_CLOCK, screen);
    eos_wm_open(&wm, EOS_APP_BOARD, screen);
    eos_wm_open(&wm, EOS_APP_HEAP,  screen);
    eos_wm_open(&wm, EOS_APP_KEYS,  screen);
    eos_wm_open(&wm, EOS_APP_BUDDY, screen);

    eos_keys_defaults(&keys);
    eos_bar_status_init(&bar);
    bar.ws_occupied = 0x0003;
    bar.title       = "buddy";
    bar.wifi        = EOS_WIFI_UP;
    bar.wifi_rssi   = -58;
    bar.brain_up    = true;
    bar.brain_model = "qwen3.5:2b";
    bar.free_heap   = 156000;
    bar.heap_warn   = 65536;
    bar.hour = 14; bar.minute = 32; bar.clock_valid = true;

    memset(&view, 0, sizeof view);
    view.theme = &theme;
    view.wm    = &wm;
    view.bar   = &bar;
    view.keys  = &keys;
    view.buddy = &buddy;
    view.heap_free = 156000; view.heap_largest = 147456; view.uptime_ms = 52321000u;
    view.board_line[0] = "penguinos.local";
    view.board_line[1] = "192.168.0.160";
    view.board_line[2] = "WavvyWorld";
    view.board_line[3] = "ESP32-C6 -58dBm";

    // ------------------------------------------------- 1. the model itself

    printf("\n== the compiled-in buddy ==\n");
    eq(eos_buddy_model_default_count(), 572, "Pip is 572 shell voxels");
    ck(eos_buddy_model_default()->count < eos_buddy_model_default()->cap,
       "the shell still fits the pool with room to spare");
    {
        eos_vox_model_t *m = eos_buddy_model_default();
        int buried = 0, air = 0;
        for (i = 0; i < (int)m->count; i++) {
            if (m->v[i].faces == 0) buried++;
            if (m->v[i].ci == 0)    air++;
        }
        eq(buried, 0, "no stored voxel has all six faces buried");
        eq(air, 0, "no stored voxel is empty");
        ck(m->pal != NULL, "the model carries its palette");
        ck(eos_buddy_model_default() == m, "the model is built once and cached");
    }

    // The shade table must never resolve to the display HAL's transparency
    // sentinel: a face that shaded onto it would not draw, and the buddy would
    // come out with holes in exactly its brightest places. eos_buddy.h warns
    // about this; going through eos_display_match() is what makes it
    // impossible rather than merely avoided, and this is the check that says so.
    {
        eos_buddy_cfg_t c;
        int sentinel = 0;
        eos_buddy_model_default_cfg(&c);
        eos_shell_buddy_shade(eos_buddy_model_default_palette(), &c);
        ck(c.shade_lut != NULL, "shade() hands back a table");
        for (i = 0; i < 768; i++) if (c.shade_lut[i] == EOS_COLOR_NONE) sentinel++;
        eq(sentinel, 0, "no shade resolves to EOS_COLOR_NONE");
        ck(c.shade_lut[0 * 256 + 1] != c.shade_lut[2 * 256 + 1],
           "the top face and an x face of the body are different indices");
        eos_shell_buddy_shade(NULL, &c);
        ck(c.shade_lut == NULL, "a NULL palette clears the table rather than keeping a stale one");
    }

    // ------------------------------------------------------- 2. the frame

    printf("\n== the desktop ==\n");
    adopt_default_buddy();
    for (i = 0; i < EOS_BUDDY_STATE_COUNT; i++) {
        const char *n = eos_buddy_state_name((eos_buddy_state_t)i);
        int k;
        for (k = 0; want[k] && n[k]; k++) {
            char x = want[k], y = n[k];
            if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
            if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
            if (x != y) break;
        }
        if (!want[k] && !n[k]) eos_buddy_set_state(&buddy, (eos_buddy_state_t)i);
    }
    bar.mood = (eos_bar_mood_t)eos_buddy_state(&buddy);
    // Enough ticks to settle the yaw ease into the state's pose. The blink
    // schedule comes off a fixed seed, so this is repeatable.
    for (i = 0; i < 40; i++) eos_buddy_tick(&buddy, 50);

    br = buddy_rect();
    ck(!eos_rect_empty(br), "the buddy window is on screen");
    ck(eos_shell_app_visible(&view, EOS_APP_BUDDY), "and eos_shell_app_visible() agrees");

    eos_display_damage_all();
    bands = frame();
    ck(bands >= 2, "a full-screen frame comes back in bands, not one pass");

    uncovered = 0; once = 0;
    for (i = 0; i < H * W; i++) {
        uint8_t c = ((uint8_t *)cover)[i];
        if (c == 0) uncovered++;
        if (c == 1) once++;
    }
    eq(uncovered, 0, "every pixel of a full-screen frame was composited");
    eq(once, H * W, "and none of them twice");

    ink = ink_in(br);
    ck(ink > 300, "the avatar put real pixels in its tile");
    ck(buddy.faces_drawn > 100, "and the renderer says it drew a real number of faces");
    ck(buddy_ready, "the offscreen box was prepared");
    ck(buddy_bm.w <= EOS_SHELL_BUDDY_PX && buddy_bm.h <= EOS_SHELL_BUDDY_PX,
       "the render never exceeds the box it has BSS for");
    ck(buddy_at_x >= br.x && buddy_at_y >= br.y &&
       buddy_at_x + buddy_bm.w <= br.x + br.w &&
       buddy_at_y + buddy_bm.h <= br.y + br.h,
       "and it lands inside the tile it was measured for");

    // The close box, checked on the COMPOSITED PANEL rather than against the
    // renderer's arithmetic. eos_pointer_close_box() is what the dispatcher
    // tests a trackpad click against and it is what draw_tile() paints into;
    // this is the one check that can tell whether those two rectangles are the
    // same rectangle, because it reads the pixels back out of the frame.
    {
        eos_tile_t tl[EOS_MAX_WINDOWS * 2];
        eos_pointer_chrome_t ch;
        int nt, ti, boxed = 0;

        eos_shell_tile_chrome(&theme, &ch);
        nt = eos_wm_layout(&wm, eos_rect(0, 0, W, H), tl, EOS_MAX_WINDOWS * 2);
        for (ti = 0; ti < nt; ti++) {
            eos_rect_t box = eos_pointer_close_box(&ch, &tl[ti]);
            if (eos_rect_empty(box)) continue;
            boxed++;
            ck(ink_in(box) >= 5,
               "the tile's close box has a cross painted in it");
            ck(box.x + box.w <= tl[ti].rect.x + tl[ti].rect.w,
               "and the box the pixels went into is inside its tile");
        }
        ck(boxed >= 2, "at least two visible tiles carry a close box");
    }

    // The cursor against the banded backend, which is the constraint the whole
    // arrow was designed around. A trackpad reports about fifty times a second
    // and the panel has no framebuffer; if a moved cursor damaged the screen,
    // the board would be pushing 115 KB per report and could not keep up. So
    // the two rects it declares must come back as strictly fewer bands than a
    // full frame, and the pixels it moves must be a small fraction of 57,600.
    {
        eos_pointer_t cur;
        int full_bands = bands, moved_bands, i, moved_px = 0;

        // One count on each axis: inside the accel curve's unity zone, so the
        // arrow moves exactly one pixel off centre and is nowhere near an edge
        // the clamp could pin it against. A big first shove would park it in
        // the corner, where the second report moves it nowhere and the check
        // below would be measuring the clamp instead of the damage.
        eos_pointer_init(&cur, W, H);
        eos_pointer_feed(&cur, 1, 1, 0, 1000);
        view.pointer    = &cur;
        view.pointer_ms = 1000;

        eos_display_damage_all();
        frame();                       // the arrow is on the glass and committed
        eos_pointer_commit(&cur, 1000);
        memcpy(prev, shot, sizeof shot);

        eos_pointer_feed(&cur, 3, 2, 0, 1030);
        view.pointer_ms = 1030;
        ck(eos_shell_damage_pointer(&view),
           "a cursor that moved declares damage");
        moved_bands = frame();
        ck(moved_bands > 0, "and the frame that follows composites something");
        ck(moved_bands < full_bands,
           "in fewer bands than a full-screen frame: the arrow damages two rects");

        for (i = 0; i < H * W; i++)
            if (((uint16_t *)shot)[i] != ((uint16_t *)prev)[i]) moved_px++;
        ck(moved_px > 0, "the arrow actually moved on the glass");
        ck(moved_px < 400,
           "and fewer than four hundred pixels changed, not the panel");

        // A cursor that has not moved since the last committed frame declares
        // nothing at all, which is what lets an idle board stay idle.
        eos_pointer_commit(&cur, 1030);
        ck(!eos_shell_damage_pointer(&view),
           "a cursor that has not moved damages nothing");

        // And the latch, which is what makes any of the above true when the
        // trackpad is actually moving. Two frames drawn from the same latched
        // position: one undisturbed, one with a report landing between every
        // pair of bands. They must be the same picture, and the commit must
        // record the position that was PAINTED and not the one the pad had
        // reached by the time the last band went out.
        {
            eos_pointer_t quiet, jostled;
            int16_t painted_x, painted_y;

            eos_pointer_init(&quiet, W, H);
            eos_pointer_feed(&quiet, 12, 8, 0, 2000);
            painted_x = quiet.x;
            painted_y = quiet.y;
            view.pointer    = &quiet;
            view.pointer_ms = 2000;
            eos_pointer_latch(&quiet, 2000);
            eos_display_damage_all();
            frame();
            memcpy(prev, shot, sizeof shot);

            eos_pointer_init(&jostled, W, H);
            eos_pointer_feed(&jostled, 12, 8, 0, 2000);
            view.pointer = &jostled;
            eos_pointer_latch(&jostled, 2000);
            eos_display_damage_all();
            frame_jostled(&jostled);

            ck(jostled.x != painted_x,
               "the trackpad really did move while the frame was going out");
            ck(memcmp(prev, shot, sizeof shot) == 0,
               "and the frame painted the latched arrow, not a smear of two");

            eos_pointer_commit(&jostled, 2000);
            ck(eos_pointer_drawn_rect(&jostled).x == painted_x &&
               eos_pointer_drawn_rect(&jostled).y == painted_y,
               "the commit records where the arrow was painted, not where the pad got to");
            ck(eos_shell_damage_pointer(&view),
               "so the motion it missed is what the next frame repaints");
        }

        view.pointer = NULL;
        view.pointer_ms = 0;
        eos_display_damage_all();
        bands = frame();
    }

    // The re-runnability rule, checked where it can actually fail. The avatar
    // sorts its voxel array in place on every render, so a second frame drawn
    // from the identical state is the cheapest possible proof that the sort is
    // idempotent and that nothing in the scene latches per-band state.
    memcpy(prev, shot, sizeof shot);
    eos_display_damage_all();
    frame();
    ck(memcmp(prev, shot, sizeof shot) == 0,
       "two frames drawn from identical state are identical pixel for pixel");

    // A damage rect covering only the buddy tile must still produce the same
    // pixels there. This is the path the loop actually takes at 10 Hz.
    memcpy(prev, shot, sizeof shot);
    memset(shot, 0, sizeof shot);
    eos_display_damage(br);
    frame();
    {
        int x, y, diff = 0;
        for (y = br.y; y < br.y + br.h; y++)
            for (x = br.x; x < br.x + br.w; x++)
                if (shot[y][x] != prev[y][x]) diff++;
        eq(diff, 0, "a tile-only damage rect redraws the tile identically");
    }
    memcpy(shot, prev, sizeof shot);

    // ------------------------------------------- 3. the ways it can be empty

    printf("\n== degradation ==\n");
    view.buddy = NULL;
    eos_display_damage_all();
    frame();
    ck(!buddy_ready, "no buddy, no offscreen render");
    ck(ink_in(br) > 20, "and the tile says so in text rather than drawing a hole");
    view.buddy = &buddy;

    {
        // A model with no voxels is what a .vox that parsed to nothing leaves
        // behind. It must be refused the same way a NULL buddy is, because
        // eos_buddy_render() would otherwise be asked to fit a zero-extent
        // model into a box.
        eos_vox_model_t empty;
        eos_buddy_t hollow;
        eos_buddy_cfg_t c;
        eos_voxel_t none[1];
        eos_buddy_model_default_cfg(&c);
        eos_vox_model_init(&empty, none, 1, 4, 4, 4, eos_buddy_model_default_palette());
        eos_buddy_init(&hollow, &empty, &c);
        view.buddy = &hollow;
        eos_display_damage_all();
        frame();
        ck(!buddy_ready, "an empty model is refused before the renderer sees it");
        view.buddy = &buddy;
    }

    // Behind a tab. eos_wm collapses the third split into a tab group, so
    // focusing another member hides the buddy — and a hidden buddy must not be
    // rendered at all, because that is the whole reason the loop asks.
    eos_wm_focus_tab_next(&wm, screen);
    ck(!eos_shell_app_visible(&view, EOS_APP_BUDDY),
       "a tab away, the buddy is not visible");
    eos_display_damage_all();
    frame();
    ck(!buddy_ready, "and it is not rendered");
    ck(!eos_shell_damage_app(&view, EOS_APP_BUDDY),
       "damage_app() reports nothing to redraw");
    while (!eos_shell_app_visible(&view, EOS_APP_BUDDY))
        eos_wm_focus_tab_next(&wm, screen);

    // ------------------------------------------------------ 4. the moods

    printf("\n== the seven moods ==\n");
    for (i = 0; i < EOS_BUDDY_STATE_COUNT; i++) {
        int n;
        eos_buddy_set_state(&buddy, (eos_buddy_state_t)i);
        for (n = 0; n < 20; n++) eos_buddy_tick(&buddy, 50);
        eos_display_damage_all();
        frame();
        n = ink_in(br);
        printf("    %-10s %5u faces, %4d pixels of ink\n",
               eos_buddy_state_name((eos_buddy_state_t)i),
               (unsigned)buddy.faces_drawn, n);
        ck(n > 300, "every mood draws something");
        ck(buddy.faces_drawn > 100, "every mood draws faces");
    }

    // And back to the one that was asked for, so the PPM is the requested pose.
    for (i = 0; i < EOS_BUDDY_STATE_COUNT; i++) {
        const char *n = eos_buddy_state_name((eos_buddy_state_t)i);
        int k;
        for (k = 0; want[k] && n[k]; k++) {
            char x = want[k], y = n[k];
            if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
            if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
            if (x != y) break;
        }
        if (!want[k] && !n[k]) eos_buddy_set_state(&buddy, (eos_buddy_state_t)i);
    }
    // 480 ms, not the two seconds the mood sweep above uses. HAPPY holds for
    // 1400 ms and CONFUSED for 1900 before both lapse back to IDLE, so a
    // longer settle would write a PPM labelled with a mood the buddy had
    // already left.
    for (i = 0; i < 12; i++) eos_buddy_tick(&buddy, 40);
    eos_display_damage_all();
    frame();
    if (out) write_ppm(out);

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed != 0;
}
