// Host test for eos_font. Runs on the Mac, no hardware needed.
//
//   cc -std=c99 -Wall -Wextra -O1 -Ikernel/font/include -Ikernel/hal/include \
//      -Ikernel/wm/include kernel/font/eos_font.c kernel/font/test/test_font.c \
//      -o /tmp/tf && /tmp/tf
//   cc -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
//      -Ikernel/font/include -Ikernel/hal/include -Ikernel/wm/include \
//      kernel/font/eos_font.c kernel/font/test/test_font.c -o /tmp/tf && /tmp/tf
//
// Two halves. The first half is assertions: the four advances and heights are
// the contract eos_bar.c fits the status bar against, so they are checked
// literally against 4x6 / 6x8 / 8x13 / 12x20; then every codepoint in range
// resolves, every codepoint OUT of range folds onto the fallback without the
// pointer ever leaving the table, and measure() agrees with advance * length.
// The gap column of every cell is asserted empty, because that column is the
// inter-glyph spacing and a glyph that eats it silently breaks bar fitting.
//
// The second half prints. A font is data whose only real test is a human
// looking at it, so every glyph of every face is rendered as ASCII art, plus
// sample runs — a clock, a heap figure, pangrams — composed the way the shell
// will compose them. Digits get their own side-by-side strip because the
// status bar renders a clock and a heap figure in tabular columns and 8 that
// looks like B is a bug you only ever catch by eye.

#include <stdio.h>
#include <string.h>
#include "eos_font.h"

static int checks = 0, fails = 0;
#define CK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("    FAIL: %s\n", msg); } } while (0)

// The advance contract, restated here on purpose. If someone edits the art and
// widens a cell, this table is what stops it reaching a board.
static const struct { eos_font_id_t id; const char *name; int w, h; } CONTRACT[] = {
    { EOS_FONT_TINY,  "tiny",   4,  6 },
    { EOS_FONT_SMALL, "small",  6,  8 },
    { EOS_FONT_MED,   "med",    8, 13 },
    { EOS_FONT_BIG,   "big",   12, 20 },
};
#define NFACE ((int)(sizeof CONTRACT / sizeof CONTRACT[0]))

// Ink boxes: how much of the cell a glyph is allowed to touch. The remainder
// is the drawn-in inter-glyph gap and interline gap. Mirrors tools/art/*.txt.
static const struct { int w, h; } INK[NFACE] = {
    { 3, 6 }, { 5, 8 }, { 7, 12 }, { 10, 18 }
};

// ------------------------------------------------------------------ helpers

static int pixel(const eos_font_t *f, const uint8_t *g, int x, int y)
{
    int rb = eos_font_row_bytes(f);
    return (g[y * rb + (x >> 3)] >> (7 - (x & 7))) & 1;
}

static int glyph_blank(const eos_font_t *f, const uint8_t *g)
{
    int n = eos_font_glyph_bytes(f), i;
    for (i = 0; i < n; i++) if (g[i]) return 0;
    return 1;
}

// ------------------------------------------------------------- assertions

static void test_faces(void)
{
    int i;
    printf("faces\n");
    for (i = 0; i < NFACE; i++) {
        const eos_font_t *f = eos_font_get(CONTRACT[i].id);
        char m[128];
        snprintf(m, sizeof m, "%s: eos_font_get returned NULL", CONTRACT[i].name);
        CK(f != NULL, m);
        if (!f) continue;

        snprintf(m, sizeof m, "%s: cell_w is %d, contract says %d",
                 CONTRACT[i].name, f->cell_w, CONTRACT[i].w);
        CK((int)f->cell_w == CONTRACT[i].w, m);
        snprintf(m, sizeof m, "%s: h is %d, contract says %d",
                 CONTRACT[i].name, f->h, CONTRACT[i].h);
        CK((int)f->h == CONTRACT[i].h, m);

        // gap and leading must stay 0 or advance != cell_w and pitch != h.
        snprintf(m, sizeof m, "%s: gap must be 0", CONTRACT[i].name);
        CK(f->gap == 0, m);
        snprintf(m, sizeof m, "%s: leading must be 0", CONTRACT[i].name);
        CK(f->leading == 0, m);
        snprintf(m, sizeof m, "%s: line_h must equal h", CONTRACT[i].name);
        CK(eos_font_line_h(f) == (int)f->h, m);

        snprintf(m, sizeof m, "%s: range must be ASCII 32..126", CONTRACT[i].name);
        CK(f->first == EOS_FONT_ASCII_FIRST && f->last == EOS_FONT_ASCII_LAST, m);
        snprintf(m, sizeof m, "%s: fallback must be '?' and inside the range",
                 CONTRACT[i].name);
        CK(f->fallback == EOS_FONT_FALLBACK &&
           f->fallback >= f->first && f->fallback <= f->last, m);

        snprintf(m, sizeof m, "%s: must be a fixed cell (widths/offsets NULL)",
                 CONTRACT[i].name);
        CK(f->widths == NULL && f->offsets == NULL, m);
        snprintf(m, sizeof m, "%s: bits must not be NULL", CONTRACT[i].name);
        CK(f->bits != NULL, m);

        snprintf(m, sizeof m, "%s: row_bytes", CONTRACT[i].name);
        CK(eos_font_row_bytes(f) == (CONTRACT[i].w + 7) / 8, m);
        snprintf(m, sizeof m, "%s: glyph_bytes", CONTRACT[i].name);
        CK(eos_font_glyph_bytes(f) == ((CONTRACT[i].w + 7) / 8) * CONTRACT[i].h, m);

        snprintf(m, sizeof m, "%s: name round-trips", CONTRACT[i].name);
        CK(eos_font_name(CONTRACT[i].id) &&
           strcmp(eos_font_name(CONTRACT[i].id), CONTRACT[i].name) == 0 &&
           eos_font_id_from_name(CONTRACT[i].name) == CONTRACT[i].id, m);
    }

    CK(eos_font_get((eos_font_id_t)-1) == NULL, "id -1 must be NULL");
    CK(eos_font_get(EOS_FONT_COUNT) == NULL, "id COUNT must be NULL");
    CK(eos_font_get((eos_font_id_t)9999) == NULL, "id 9999 must be NULL");
    CK(eos_font_id_from_name(NULL) == EOS_FONT_SMALL, "NULL name -> small");
    CK(eos_font_id_from_name("mono10") == EOS_FONT_SMALL, "unknown name -> small");
    CK(eos_font_id_from_name("") == EOS_FONT_SMALL, "empty name -> small");
    CK(eos_font_name((eos_font_id_t)-1) == NULL, "bad id has no name");
}

static void test_glyphs(void)
{
    int i, c, x, y;
    printf("glyphs\n");
    for (i = 0; i < NFACE; i++) {
        const eos_font_t *f = eos_font_get(CONTRACT[i].id);
        const uint8_t *base, *g;
        int gb, n, resolved = 0, blank = 0, gapdirty = 0, inkdirty = 0, padbits = 0;
        char m[128];
        if (!f) continue;
        base = f->bits;
        gb   = eos_font_glyph_bytes(f);
        n    = f->last - f->first + 1;

        for (c = f->first; c <= f->last; c++) {
            g = eos_font_glyph_bits(f, (unsigned char)c);
            if (!g) continue;
            resolved++;
            if (g != base + (size_t)(c - f->first) * (size_t)gb) resolved--;
            if (c != ' ' && glyph_blank(f, g)) blank++;
            // the gap column(s): everything from the ink width to the cell
            // width must be clear, on every row.
            for (y = 0; y < (int)f->h; y++) {
                for (x = INK[i].w; x < (int)f->cell_w; x++)
                    if (pixel(f, g, x, y)) gapdirty++;
                for (x = 0; x < (int)f->cell_w; x++)
                    if (y >= INK[i].h && pixel(f, g, x, y)) inkdirty++;
            }
            // padding bits past cell_w in the last byte of each row
            for (y = 0; y < (int)f->h; y++)
                for (x = (int)f->cell_w; x < eos_font_row_bytes(f) * 8; x++)
                    if (pixel(f, g, x, y)) padbits++;
        }
        snprintf(m, sizeof m, "%s: all %d codepoints resolve to their own slot",
                 CONTRACT[i].name, n);
        CK(resolved == n, m);
        snprintf(m, sizeof m, "%s: %d glyphs other than space are blank",
                 CONTRACT[i].name, blank);
        CK(blank == 0, m);
        snprintf(m, sizeof m, "%s: space must be blank", CONTRACT[i].name);
        CK(glyph_blank(f, eos_font_glyph_bits(f, ' ')), m);
        snprintf(m, sizeof m, "%s: %d pixels in the inter-glyph gap column",
                 CONTRACT[i].name, gapdirty);
        CK(gapdirty == 0, m);
        snprintf(m, sizeof m, "%s: %d pixels below the ink box",
                 CONTRACT[i].name, inkdirty);
        CK(inkdirty == 0, m);
        snprintf(m, sizeof m, "%s: %d stray padding bits", CONTRACT[i].name, padbits);
        CK(padbits == 0, m);

        // Legibility floor: no two glyphs in a face may have identical bits.
        // This is what catches 8 drawn the same as B, or 0 the same as O.
        {
            int a, b, dup = 0;
            for (a = 0; a < n; a++)
                for (b = a + 1; b < n; b++)
                    if (memcmp(base + (size_t)a * gb,
                               base + (size_t)b * gb, (size_t)gb) == 0) {
                        dup++;
                        if (dup <= 4)
                            printf("      '%c' and '%c' are the same glyph in %s\n",
                                   f->first + a, f->first + b, CONTRACT[i].name);
                    }
            snprintf(m, sizeof m, "%s: %d pairs of identical glyphs",
                     CONTRACT[i].name, dup);
            CK(dup == 0, m);
        }
    }
}

// Every byte value, including the 161 with no glyph, must fold onto a pointer
// inside the table and must be fully readable. Under -fsanitize=address the
// read is the assertion; the arithmetic check below makes the plain build
// catch it too.
static void test_out_of_range(void)
{
    int i, c, j;
    printf("out of range\n");
    for (i = 0; i < NFACE; i++) {
        const eos_font_t *f = eos_font_get(CONTRACT[i].id);
        const uint8_t *lo, *hi, *g;
        int gb, bad = 0, notfb = 0;
        unsigned sink = 0;
        char m[128];
        if (!f) continue;
        gb = eos_font_glyph_bytes(f);
        lo = f->bits;
        hi = f->bits + (size_t)(f->last - f->first + 1) * (size_t)gb;

        for (c = 0; c < 256; c++) {
            g = eos_font_glyph_bits(f, (unsigned char)c);
            if (!g || g < lo || g + gb > hi) { bad++; continue; }
            for (j = 0; j < gb; j++) sink += g[j];   /* ASan reads them all */
            if (c < f->first || c > f->last) {
                if (memcmp(g, f->bits + (size_t)(f->fallback - f->first) * gb,
                           (size_t)gb) != 0) notfb++;
            }
            if (eos_font_glyph_w(f, (unsigned char)c) != (int)f->cell_w) bad++;
        }
        snprintf(m, sizeof m, "%s: %d of 256 byte values left the table",
                 CONTRACT[i].name, bad);
        CK(bad == 0, m);
        snprintf(m, sizeof m, "%s: %d out-of-range codes did not fold onto '?'",
                 CONTRACT[i].name, notfb);
        CK(notfb == 0, m);
        CK(sink != 0, "glyph bytes were actually read");
    }

    // Hostile descriptors that no table here produces but a caller could build.
    {
        eos_font_t bad;
        eos_bitmap_t bm;
        static const uint8_t one[8] = {0};
        static const uint8_t wid[1] = {1};
        static const uint32_t off[1] = {0};

        memset(&bad, 0, sizeof bad);
        CK(eos_font_glyph_bits(NULL, 'A') == NULL, "NULL font -> NULL bits");
        CK(eos_font_glyph_bits(&bad, 'A') == NULL, "empty font -> NULL bits");
        CK(eos_font_row_bytes(NULL) == 0, "NULL font row_bytes 0");
        CK(eos_font_glyph_bytes(NULL) == 0, "NULL font glyph_bytes 0");

        bad.first = 100; bad.last = 10; bad.fallback = 100;
        bad.cell_w = 8; bad.h = 8; bad.bits = one;
        CK(eos_font_glyph_bits(&bad, 'A') == NULL, "inverted range -> NULL bits");

        bad.first = 32; bad.last = 32; bad.fallback = 32;
        bad.widths = wid; bad.offsets = off;
        CK(eos_font_glyph_bits(&bad, 'A') == NULL, "proportional font -> NULL bits");
        bad.widths = NULL; bad.offsets = NULL;
        CK(eos_font_glyph_bits(&bad, 'A') == one, "one-glyph font folds onto it");
        CK(eos_font_glyph_bitmap(&bad, 'A', 1, 2, &bm), "bitmap of a valid font");
        CK(eos_font_glyph_bitmap(&bad, 'A', 1, 2, NULL) == false, "NULL out");
    }
}

static void test_bitmap(void)
{
    int i;
    printf("bitmap handoff\n");
    for (i = 0; i < NFACE; i++) {
        const eos_font_t *f = eos_font_get(CONTRACT[i].id);
        eos_bitmap_t bm;
        char m[128];
        if (!f) continue;
        memset(&bm, 0xAA, sizeof bm);
        CK(eos_font_glyph_bitmap(f, 'A', 7, EOS_COLOR_NONE, &bm), "bitmap ok");
        snprintf(m, sizeof m, "%s: bitmap w/h match the cell", CONTRACT[i].name);
        CK(bm.w == (int16_t)f->cell_w && bm.h == (int16_t)f->h, m);
        snprintf(m, sizeof m, "%s: bitmap is MONO1", CONTRACT[i].name);
        CK(bm.fmt == EOS_PIXFMT_MONO1, m);
        snprintf(m, sizeof m, "%s: bitmap points into the table", CONTRACT[i].name);
        CK(bm.pixels == (const void *)eos_font_glyph_bits(f, 'A'), m);
        snprintf(m, sizeof m, "%s: stride equals row_bytes", CONTRACT[i].name);
        CK(bm.stride == (int16_t)eos_font_row_bytes(f), m);
        // The display's own stride rule must agree, or a blit shears.
        snprintf(m, sizeof m, "%s: eos_bitmap_stride agrees", CONTRACT[i].name);
        CK(eos_bitmap_stride(&bm) == (int16_t)eos_font_row_bytes(f), m);
        CK(bm.tint == 7 && bm.bg == EOS_COLOR_NONE, "tint and bg carried through");
        CK(bm.key == EOS_COLOR_NONE, "key disabled for MONO1");
    }
}

static void test_measure(void)
{
    static const char *SAMPLE[] = {
        "", "x", "12:34", "heap 401408", "?", "\x01\x02\x7f",
        "The quick brown fox jumps over the lazy dog",
        "~esp-os~", "[  ]", "                                "
    };
    int i, s, n, h;
    printf("measure\n");
    for (i = 0; i < NFACE; i++) {
        const eos_font_t *f = eos_font_get(CONTRACT[i].id);
        char m[160];
        if (!f) continue;
        for (s = 0; s < (int)(sizeof SAMPLE / sizeof SAMPLE[0]); s++) {
            n = (int)strlen(SAMPLE[s]);
            h = -1;
            snprintf(m, sizeof m, "%s: measure(\"%s\") != %d * %d",
                     CONTRACT[i].name, SAMPLE[s], CONTRACT[i].w, n);
            CK(eos_font_measure(f, SAMPLE[s], -1, &h) == CONTRACT[i].w * n, m);
            snprintf(m, sizeof m, "%s: one line is %d tall", CONTRACT[i].name,
                     CONTRACT[i].h);
            CK(h == CONTRACT[i].h, m);
            snprintf(m, sizeof m, "%s: measure agrees with eos_text_width",
                     CONTRACT[i].name);
            CK(eos_font_measure(f, SAMPLE[s], n, NULL) ==
               eos_text_width(f, SAMPLE[s], n), m);
            snprintf(m, sizeof m, "%s: measure_cb agrees", CONTRACT[i].name);
            CK((int)eos_font_measure_cb(SAMPLE[s], (void *)f) ==
               CONTRACT[i].w * n, m);
            // eos_text_fit must hand back every character when the box is exact
            snprintf(m, sizeof m, "%s: text_fit is exact at the measured width",
                     CONTRACT[i].name);
            CK(eos_text_fit(f, SAMPLE[s], n, CONTRACT[i].w * n) == n, m);
            snprintf(m, sizeof m, "%s: text_fit drops one at width-1",
                     CONTRACT[i].name);
            CK(n == 0 || eos_text_fit(f, SAMPLE[s], n, CONTRACT[i].w * n - 1)
                         == n - 1, m);
        }
        // multi-line
        h = -1;
        CK(eos_font_measure(f, "ab\ncdef\ng", -1, &h) == CONTRACT[i].w * 4,
           "widest line wins");
        CK(h == 3 * CONTRACT[i].h, "three lines, no leading");
        h = -1;
        CK(eos_font_measure(f, "\n\n", -1, &h) == 0, "empty lines are 0 wide");
        CK(h == 3 * CONTRACT[i].h, "two newlines are three lines");
        CK(eos_font_measure(f, "abcdef", 3, NULL) == CONTRACT[i].w * 3,
           "explicit len is honoured");
        CK(eos_font_measure(f, NULL, -1, NULL) == 0, "NULL text measures 0");
        CK(eos_font_measure(NULL, "abc", -1, NULL) == 0, "NULL font measures 0");
        h = 99;
        CK(eos_font_measure(NULL, "abc", -1, &h) == 0 && h == 0,
           "NULL font zeroes the height too");
    }
}

static void test_flash(void)
{
    uint32_t total = 0;
    int i;
    printf("flash\n");
    for (i = 0; i < NFACE; i++) {
        uint32_t want = (uint32_t)(EOS_FONT_ASCII_LAST - EOS_FONT_ASCII_FIRST + 1) *
                        (uint32_t)(((CONTRACT[i].w + 7) / 8) * CONTRACT[i].h);
        char m[128];
        snprintf(m, sizeof m, "%s: face_bytes is %u, expected %u", CONTRACT[i].name,
                 (unsigned)eos_font_face_bytes(CONTRACT[i].id), (unsigned)want);
        CK(eos_font_face_bytes(CONTRACT[i].id) == want, m);
        total += want;
    }
    CK(eos_font_flash_bytes() == total, "flash_bytes is the sum of the faces");
    CK(eos_font_face_bytes((eos_font_id_t)-1) == 0, "bad id costs 0");
    printf("    all four faces: %u bytes of flash\n",
           (unsigned)eos_font_flash_bytes());
}

// ------------------------------------------------------------------ render

// Rasterise a run into a caller-owned char canvas and print it. This is the
// only honest test of a font, so it prints even when every assertion passes.
#define CANVAS_W 1200
static char canvas[24][CANVAS_W + 1];

static void draw(const eos_font_t *f, const char *s, char ink, char paper)
{
    int len = (int)strlen(s), i, x, y, w;
    w = len * (int)f->cell_w;
    if (w > CANVAS_W) w = CANVAS_W;
    for (y = 0; y < (int)f->h; y++) {
        for (x = 0; x < w; x++) canvas[y][x] = paper;
        canvas[y][w] = 0;
    }
    for (i = 0; i < len; i++) {
        const uint8_t *g = eos_font_glyph_bits(f, (unsigned char)s[i]);
        int ox = i * (int)f->cell_w;
        if (!g) continue;
        for (y = 0; y < (int)f->h; y++)
            for (x = 0; x < (int)f->cell_w; x++)
                if (ox + x < w && pixel(f, g, x, y))
                    canvas[y][ox + x] = ink;
    }
    for (y = 0; y < (int)f->h; y++) printf("    |%s|\n", canvas[y]);
}

static void render_face(int i)
{
    const eos_font_t *f = eos_font_get(CONTRACT[i].id);
    int c, row;
    char line[64];

    if (!f) return;
    printf("\n");
    printf("================================================================\n");
    printf(" %s  %dx%d  ASCII %d..%d  %u bytes\n", CONTRACT[i].name,
           (int)f->cell_w, (int)f->h, f->first, f->last,
           (unsigned)eos_font_face_bytes(CONTRACT[i].id));
    printf("================================================================\n");

    // The full range, 16 codepoints per row, so a human can scan all 95.
    for (c = f->first; c <= f->last; c += 16) {
        int n = 0, k;
        for (k = c; k <= f->last && k < c + 16; k++) line[n++] = (char)k;
        line[n] = 0;
        printf("  %d..%d\n", c, c + n - 1);
        draw(f, line, '#', '.');
    }

    // Composed runs, which is how the shell will actually use it.
    printf("  sample runs\n");
    draw(f, "12:34  heap 401408  wifi -42dBm", '#', ' ');
    printf("\n");
    draw(f, "The quick brown fox jumps over the lazy dog.", '#', ' ');
    printf("\n");
    draw(f, "ESP-OS 0.1 // esp32c6 // ST7789 240x240", '#', ' ');
    printf("\n");

    // Digits alone, spaced out, because the status bar renders them in
    // tabular columns and 0/O, 5/S, 8/B, 1/l/I are the pairs that fail.
    printf("  digit and lookalike audit\n");
    draw(f, "0123456789", '#', ' ');
    printf("\n");
    draw(f, "0O 1lI 5S 8B 2Z 6G", '#', ' ');
    printf("\n");

    // Rows of the whole range with no separators, to judge rhythm and the
    // inter-glyph gap at real density.
    printf("  density\n");
    for (row = 0; row < 3; row++) {
        int n = 0, k;
        for (k = f->first + row * 32; k <= f->last && n < 32; k++) line[n++] = (char)k;
        line[n] = 0;
        if (n) draw(f, line, '#', ' ');
    }
}

int main(void)
{
    int i;
    printf("test_font\n");
    test_faces();
    test_glyphs();
    test_out_of_range();
    test_bitmap();
    test_measure();
    test_flash();

    for (i = 0; i < NFACE; i++) render_face(i);

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
