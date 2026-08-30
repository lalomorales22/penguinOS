// Host checks for the ST7789 backend's compositor. Everything above the SPI
// call is pure arithmetic — banding, damage coalescing, clipping, the four draw
// calls, the palette — so all of it can be held to account off-target.
//
// The one thing this cannot check is the panel: on the host the band is
// composited and then dropped instead of being clocked out. So a green run here
// means the drawing is right, not that the wiring is. The wiring is the probe's
// job and it is already verified.

#include "eos_display.h"
#include "eos_theme.h"
#include "waveshare-c6-lcd-13.h"

#include <stdio.h>
#include <string.h>

const eos_board_t *eos_board_get(void) { return &EOS_BOARD; }

const uint16_t *eos_display_host_band(eos_rect_t *band);

static int checks = 0, failed = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failed++; printf("FAIL: %s\n", what); }
}

static void eq(long got, long want, const char *what)
{
    checks++;
    if (got != want) { failed++; printf("FAIL: %s: got %ld want %ld\n", what, got, want); }
}

// ------------------------------------------------------------------ helpers

#define W 240
#define H 240

static uint8_t  cover[H][W];
static uint16_t shadow[H][W];   // every pixel any band composited, in wire order

static void shadow_take(void)
{
    eos_rect_t b;
    const uint16_t *px = eos_display_host_band(&b);
    if (!px) return;
    for (int y = 0; y < b.h; y++) {
        for (int x = 0; x < b.w; x++) {
            cover[b.y + y][b.x + x]++;
            shadow[b.y + y][b.x + x] = px[y * b.w + x];
        }
    }
}

static uint16_t wire_of(eos_color_t c)
{
    eos_theme_t th;
    eos_theme_default(&th);
    uint16_t v = eos_theme_palette565(&th)[c];
    return (uint16_t)((v >> 8) | (v << 8));
}

// ------------------------------------------------------------------- fonts

// Packed the way kernel/font/tools/gen_font.py packs: rows MSB-first, each row
// padded to whole bytes, glyphs concatenated. 3x3 cell, one byte per row.
// Glyph 'A' is a ring, glyph 'B' is blank.
static const uint8_t FBITS[6] = { 0xE0, 0xA0, 0xE0,   0x00, 0x00, 0x00 };
static const eos_font_t FONT = {
    .first = 'A', .last = 'B', .fallback = 'A',
    .cell_w = 3, .h = 3, .gap = 1, .leading = 1,
    .bits = FBITS, .widths = NULL, .offsets = NULL,
};

// 12 wide, so two bytes per row. This is the face that catches a reader which
// strides by the glyph width instead of by the padded row pitch — the 12x20
// EOS_FONT_BIG has exactly this shape.
static const uint8_t WBITS[8] = { 0x80, 0x00,  0x00, 0x10,    // 'A'
                                  0xFF, 0xF0,  0x00, 0x00 };  // 'B'
static const eos_font_t WFONT = {
    .first = 'A', .last = 'B', .fallback = 'A',
    .cell_w = 12, .h = 2, .gap = 0, .leading = 0,
    .bits = WBITS, .widths = NULL, .offsets = NULL,
};

// -------------------------------------------------------------------- tests

static void t_info(void)
{
    eq(eos_display_init(), EOS_OK, "init");
    const eos_display_info_t *i = eos_display_info();
    eq(i->w, W, "info.w");
    eq(i->h, H, "info.h");
    eq(i->tier, EOS_TIER_LEAN, "info.tier");
    eq(i->fmt, EOS_PIXFMT_RGB565, "info.fmt");
    eq(i->palette_len, 255, "info.palette_len excludes the sentinel");
    eq(i->band_h, 40, "info.band_h from render.band_h");
    eq(i->max_bands, 6, "info.max_bands");
    ok((i->caps & EOS_CAP_RGB565) != 0, "caps RGB565");
    ok((i->caps & EOS_CAP_BLEND) != 0, "caps BLEND");
    ok((i->caps & EOS_CAP_PALETTE) != 0, "caps PALETTE");
    ok((i->caps & EOS_CAP_BACKLIGHT) != 0, "caps BACKLIGHT");
    ok((i->caps & EOS_CAP_DIM) != 0, "caps DIM, the pin is LEDC");
    ok((i->caps & EOS_CAP_RETAINED) == 0, "not retained: this backend is banded");
    ok((i->caps & EOS_CAP_ANIM) == 0, "no animation budget on tier LEAN");
    eq(eos_display_init(), EOS_ERR_STATE, "second init refused");
}

static void t_full_frame_coverage(void)
{
    memset(cover, 0, sizeof(cover));
    eos_display_damage_all();
    eos_display_frame_begin();
    int n = 0;
    eos_rect_t band;
    while (eos_display_frame_band(&band)) { shadow_take(); n++; }
    eos_display_frame_end();
    eq(n, 6, "a full-screen frame is six bands");

    int bad = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (cover[y][x] != 1) bad++;
    eq(bad, 0, "every pixel banded exactly once");
}

static void t_partial_damage(void)
{
    memset(cover, 0, sizeof(cover));
    eos_display_damage(eos_rect(10, 10, 20, 20));
    eos_display_damage(eos_rect(200, 200, 30, 30));
    eos_display_frame_begin();
    eos_rect_t band;
    while (eos_display_frame_band(&band)) shadow_take();
    eos_display_frame_end();

    int in = 0, out = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int want = (x >= 10 && x < 30 && y >= 10 && y < 30) ||
                       (x >= 200 && x < 230 && y >= 200 && y < 230);
            if (want && cover[y][x] == 0) in++;
            if (!want && cover[y][x] != 0) out++;
        }
    eq(in, 0, "two disjoint damage rects are fully banded");
    eq(out, 0, "and nothing else is touched");

    // A narrow rect gets more rows per band than the nominal 40, because the
    // strip is sized in pixels, not rows.
    memset(cover, 0, sizeof(cover));
    eos_display_damage(eos_rect(0, 0, 10, 240));
    eos_display_frame_begin();
    int n = 0;
    while (eos_display_frame_band(&band)) n++;
    eos_display_frame_end();
    eq(n, 1, "a 10px-wide full-height column is one band");
}

static void t_damage_overflow(void)
{
    for (int i = 0; i < 20; i++)
        eos_display_damage(eos_rect((int16_t)(i * 11), (int16_t)(i * 11), 4, 4));

    memset(cover, 0, sizeof(cover));
    eos_display_frame_begin();
    eos_rect_t band;
    while (eos_display_frame_band(&band)) shadow_take();
    eos_display_frame_end();

    int missed = 0;
    for (int i = 0; i < 20; i++) {
        int x = i * 11, y = i * 11;
        if (x + 4 > W || y + 4 > H) continue;
        if (!cover[y][x]) missed++;
    }
    eq(missed, 0, "past EOS_DAMAGE_MAX rects coalesce, never drop");
}

static void t_damage_ignored_in_frame(void)
{
    eos_display_damage(eos_rect(0, 0, 8, 8));
    eos_display_frame_begin();
    eos_display_damage(eos_rect(100, 100, 8, 8));   // must be ignored
    memset(cover, 0, sizeof(cover));
    eos_rect_t band;
    while (eos_display_frame_band(&band)) shadow_take();
    eos_display_frame_end();
    eq(cover[100][100], 0, "damage declared inside a frame does not open a band");
    eq(cover[0][0], 1, "the damage declared before it still does");
}

static void t_fill_and_clip(void)
{
    const eos_color_t C = 20, D = 21;
    memset(shadow, 0, sizeof(shadow));
    eos_display_damage_all();
    eos_display_frame_begin();
    eos_rect_t band;
    while (eos_display_frame_band(&band)) {
        eos_display_clear(C);
        eos_display_fill(eos_rect(50, 50, 10, 10), D);
        eos_display_fill(eos_rect(70, 70, 10, 10), EOS_COLOR_NONE);

        // The push only succeeds on the band that actually contains the rect —
        // an empty intersection is refused and must not be popped.
        eos_rect_t want = eos_rect(100, 100, 10, 10);
        bool hit = !eos_rect_empty(eos_rect_isect(want, band));
        if (eos_display_clip_push(want)) {
            ok(hit, "clip push succeeded only where the band overlaps");
            eos_display_fill(eos_rect(0, 0, 240, 240), D);   // clipped to 10x10
            eos_display_clip_pop();
        } else {
            ok(!hit, "clip push refused where the intersection is empty");
        }
        eos_display_clip_pop();                          // the band is the floor
        eq(eos_display_clip().w, band.w, "the band cannot be popped off");
        shadow_take();
    }
    eos_display_frame_end();

    eq(shadow[55][55], wire_of(D), "fill lands, in wire byte order");
    eq(shadow[10][10], wire_of(C), "clear paints the band");
    eq(shadow[75][75], wire_of(C), "EOS_COLOR_NONE fill is a no-op");
    eq(shadow[105][105], wire_of(D), "clipped fill lands inside the clip");
    eq(shadow[99][99], wire_of(C), "and nowhere outside it");
    eq(shadow[110][110], wire_of(C), "clip is exclusive on the far edge");
}

static void t_clip_depth(void)
{
    eos_display_damage_all();
    eos_display_frame_begin();
    eos_rect_t band;
    ok(eos_display_frame_band(&band), "first band");
    int pushed = 0;
    for (int i = 0; i < EOS_CLIP_DEPTH + 3; i++)
        if (eos_display_clip_push(eos_rect(0, 0, 240, 240))) pushed++;
    eq(pushed, EOS_CLIP_DEPTH, "the stack takes EOS_CLIP_DEPTH pushes above the band");
    ok(!eos_display_clip_push(eos_rect(1000, 1000, 4, 4)), "an empty push is refused");
    eos_display_frame_end();
}

static void t_text(void)
{
    const eos_color_t BGC = 20, INK = 21;
    eq(eos_text_width(&FONT, "AB", 2), 7, "text width, two 3px cells and one gap");

    memset(shadow, 0, sizeof(shadow));
    eos_display_damage_all();
    eos_display_frame_begin();
    eos_rect_t band;
    while (eos_display_frame_band(&band)) {
        eos_display_clear(BGC);
        int adv = eos_display_text(10, 10, &FONT, INK, "AB", 2);
        eq(adv, 7, "text returns the pen advance");
        shadow_take();
    }
    eos_display_frame_end();

    eq(shadow[10][10], wire_of(INK), "glyph row 0 col 0 set");
    eq(shadow[11][11], wire_of(BGC), "glyph row 1 col 1 clear, background untouched");
    eq(shadow[12][12], wire_of(INK), "glyph row 2 col 2 set");
    eq(shadow[10][13], wire_of(BGC), "the gap column is not painted");
    eq(shadow[10][14], wire_of(BGC), "the blank second glyph paints nothing");

    checks++;
    if (eos_display_text(0, 0, &FONT, INK, "AB", 2) != 7) {
        failed++; printf("FAIL: text outside a frame still measures\n");
    }

    // Two bytes per row: the row pitch is 16 bits, not 12.
    memset(shadow, 0, sizeof(shadow));
    eos_display_damage_all();
    eos_display_frame_begin();
    while (eos_display_frame_band(&band)) {
        eos_display_clear(BGC);
        eq(eos_display_text(100, 100, &WFONT, INK, "AB", 2), 24, "wide face advance");
        shadow_take();
    }
    eos_display_frame_end();

    eq(shadow[100][100], wire_of(INK), "wide glyph row 0, leftmost bit");
    eq(shadow[100][101], wire_of(BGC), "wide glyph row 0, second bit clear");
    eq(shadow[101][111], wire_of(INK), "wide glyph row 1, bit 11 in the second byte");
    eq(shadow[101][100], wire_of(BGC), "wide glyph row 1 starts at the padded pitch");
    eq(shadow[100][112], wire_of(INK), "second wide glyph indexes past the padding");
    eq(shadow[100][123], wire_of(INK), "and covers all twelve columns");
    eq(shadow[101][112], wire_of(BGC), "its second row is blank");
}

// Held against the real tables when kernel/font/ is on the include path. The
// packing contract crosses an agent boundary, so it is checked rather than
// assumed: a reader that strides by the glyph width instead of the padded row
// pitch reads glyph N's rows out of glyph N+1, and the blank space cell is the
// cheapest place to see that happen.
#if defined(__has_include)
#  if __has_include("eos_font_data.inc")
#    define HAVE_REAL_FONTS 1
#    include "eos_font_data.inc"
#  endif
#endif

static void t_real_fonts(void)
{
#ifdef HAVE_REAL_FONTS
    const eos_color_t BGC = 20, INK = 21;
    for (int fi = 0; fi < EOS_FONT_COUNT; fi++) {
        const eos_font_t *f = &eos_font_table[fi];
        int ink_in_space = 0, ink_past_cell = 0, ink_total = 0;

        memset(shadow, 0, sizeof(shadow));
        eos_display_damage_all();
        eos_display_frame_begin();
        eos_rect_t band;
        while (eos_display_frame_band(&band)) {
            eos_display_clear(BGC);
            eos_display_text(0, 0, f, INK, " ", 1);          // must paint nothing
            for (int c = '!'; c <= '~'; c++) {
                char one[2] = { (char)c, 0 };
                int col = (c - '!') % 16, rowi = (c - '!') / 16;
                eos_display_text((int16_t)(col * 14), (int16_t)(20 + rowi * 22),
                                 f, INK, one, 1);
            }
            shadow_take();
        }
        eos_display_frame_end();

        for (int y = 0; y < f->h; y++)
            for (int x = 0; x < f->cell_w; x++)
                if (shadow[y][x] != wire_of(BGC)) ink_in_space++;

        for (int c = '!'; c <= '~'; c++) {
            int col = (c - '!') % 16, rowi = (c - '!') / 16;
            int ox = col * 14, oy = 20 + rowi * 22;
            for (int y = 0; y < f->h; y++)
                for (int x = 0; x < f->cell_w; x++)
                    if (shadow[oy + y][ox + x] != wire_of(BGC)) {
                        ink_total++;
                        if (x >= f->cell_w) ink_past_cell++;
                    }
        }
        eq(ink_in_space, 0, "the real space glyph paints nothing");
        ok(ink_total > 50, "the real face paints ink");
        eq(ink_past_cell, 0, "no ink escapes the cell");
    }
#else
    checks++;
    failed++;
    printf("FAIL: kernel/font/eos_font_data.inc not on the include path; "
           "add -Ikernel/font\n");
#endif
}

static void t_blit(void)
{
    const eos_color_t BGC = 20, A = 21, B = 22;

    static const uint8_t i8[4]   = { 21, 22, 0xFF, 21 };   // 2x2, one keyed, one sentinel
    static const uint8_t mono[2] = { 0x80, 0x00 };         // 2x2, one bit set per row byte
    static const uint8_t a8[4]   = { 255, 0, 128, 255 };
    static const uint8_t rgb[4]  = { 0x1F, 0x00, 0x00, 0xF8 };  // LE 565: blue, then red

    memset(shadow, 0, sizeof(shadow));
    eos_display_damage_all();
    eos_display_frame_begin();
    eos_rect_t band;
    while (eos_display_frame_band(&band)) {
        eos_display_clear(BGC);

        eos_bitmap_t b = {0};
        b.pixels = i8; b.w = 2; b.h = 2; b.fmt = EOS_PIXFMT_I8;
        b.key = 22; b.tint = EOS_COLOR_NONE; b.bg = EOS_COLOR_NONE;
        eos_display_blit(20, 20, &b);

        b.pixels = mono; b.w = 2; b.h = 2; b.fmt = EOS_PIXFMT_MONO1; b.stride = 1;
        b.key = EOS_COLOR_NONE; b.tint = A; b.bg = B;
        eos_display_blit(40, 40, &b);

        b.pixels = a8; b.w = 2; b.h = 2; b.fmt = EOS_PIXFMT_A8; b.stride = 0;
        b.tint = A; b.bg = EOS_COLOR_NONE;
        eos_display_blit(60, 60, &b);

        b.pixels = rgb; b.w = 2; b.h = 1; b.fmt = EOS_PIXFMT_RGB565; b.stride = 0;
        eos_display_blit(80, 80, &b);

        shadow_take();
    }
    eos_display_frame_end();

    eq(shadow[20][20], wire_of(21), "I8 blit writes through the palette");
    eq(shadow[20][21], wire_of(BGC), "I8 key pixel skipped");
    eq(shadow[21][20], wire_of(BGC), "I8 sentinel pixel skipped");
    eq(shadow[21][21], wire_of(21), "I8 last pixel");

    eq(shadow[40][40], wire_of(A), "MONO1 set bit takes the tint");
    eq(shadow[40][41], wire_of(B), "MONO1 clear bit takes the paper");
    eq(shadow[41][40], wire_of(B), "MONO1 second row, from the stride");

    eq(shadow[60][60], wire_of(A), "A8 full coverage is the tint");
    eq(shadow[60][61], wire_of(BGC), "A8 zero coverage over a transparent bg is untouched");
    ok(shadow[61][60] != wire_of(A) && shadow[61][60] != wire_of(BGC),
       "A8 half coverage blends, it does not threshold");

    // The source halfwords are little-endian 565; the strip holds them swapped
    // because that is what the ST7789 clocks in.
    eq(shadow[80][80], 0x1F00, "RGB565 source is LE, the strip is BE");
    eq(shadow[80][81], 0x00F8, "RGB565 second pixel");
}

static void t_palette(void)
{
    uint32_t p[3] = { eos_rgb(255, 0, 0), eos_rgb(0, 255, 0), eos_rgb(0, 0, 255) };
    eq(eos_display_palette(p, 30, 3), EOS_OK, "palette upload");
    eq(eos_display_palette(NULL, 0, 1), EOS_ERR_ARG, "null palette refused");
    eq(eos_display_palette(p, 254, 3), EOS_ERR_ARG, "past the end refused");

    eq(eos_display_match(eos_rgb(255, 0, 0)), 30, "match finds the red we just loaded");
    eq(eos_display_match(eos_rgb(0, 0, 255)), 32, "match finds the blue");

    // Writing the sentinel is ignored, and match can never return it.
    uint32_t white = eos_rgb(255, 255, 255);
    eq(eos_display_palette(&white, 255, 1), EOS_OK, "writing the sentinel is accepted");
    checks++;
    if (eos_display_match(white) == EOS_COLOR_NONE) {
        failed++; printf("FAIL: match returned EOS_COLOR_NONE\n");
    }
}

static void t_backlight(void)
{
    eq(eos_display_backlight(0), EOS_OK, "backlight off");
    eq(eos_display_backlight(100), EOS_OK, "backlight full");
    eq(eos_display_backlight(101), EOS_ERR_ARG, "out of range refused");
}

static void t_no_frame(void)
{
    // Every draw call is a no-op outside an open frame, and none of them crash.
    eos_display_fill(eos_rect(0, 0, 10, 10), 20);
    eos_bitmap_t b = {0};
    b.pixels = FBITS; b.w = 1; b.h = 1; b.fmt = EOS_PIXFMT_I8;
    b.key = EOS_COLOR_NONE; b.tint = EOS_COLOR_NONE; b.bg = EOS_COLOR_NONE;
    eos_display_blit(0, 0, &b);
    ok(!eos_display_clip_push(eos_rect(0, 0, 4, 4)), "no clip stack outside a frame");
    eq(eos_display_clip().w, 0, "clip is empty outside a frame");
    ok(true, "no draw call outside a frame faults");

    eos_display_frame_end();          // safe with no frame open
    eos_rect_t band;
    ok(!eos_display_frame_band(&band), "frame_band without frame_begin returns false");
}

int main(void)
{
    t_info();
    t_full_frame_coverage();
    t_partial_damage();
    t_damage_overflow();
    t_damage_ignored_in_frame();
    t_fill_and_clip();
    t_clip_depth();
    t_text();
    t_real_fonts();
    t_blit();
    t_palette();
    t_backlight();
    t_no_frame();

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
