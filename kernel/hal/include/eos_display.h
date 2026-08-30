// eos_display — the one drawing interface, satisfiable by a software
// compositor on a 20KB heap and by LVGL 9 on a PSRAM board.
//
// It is deliberately not a widget API. It is begin a frame, walk the damaged
// bands, fill rects, blit caller-owned pixels, draw a text run, flush. Widgets
// belong to the shell, above this line, where they cost nothing per backend.
//
// The one non-obvious constraint, and the reason the frame loop looks the way
// it does: a backend is not guaranteed to own a full framebuffer. A 320x240
// indexed buffer is 76,800 bytes and fits on the CYD; a 320x480 one is 153,600
// and does not fit next to WiFi. So a frame is drawn ONCE PER BAND, and the
// scene-drawing code must be re-runnable. Damage is declared BEFORE the frame
// opens, never during it, because the backend has to know the bands up front.
//
// Colour is always a palette index. Tier SOFT writes the index straight into
// the framebuffer; tier LEAN and RICH resolve it through a 256-entry RGB565
// LUT; the SSD1306 resolves it to one bit. That is how one eos_color_t serves
// an indexed compositor, LVGL, and a mono OLED without any caller branching.
//
// Nothing here allocates. eos_display_init() is the single exception and it is
// described at its declaration. Only eos_display_frame_band() and
// eos_display_frame_end() may block.

#ifndef EOS_DISPLAY_H
#define EOS_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "eos_board.h"

// ------------------------------------------------------------------ colour

typedef uint8_t eos_color_t;

// Reserved. Never resolves to a pixel: filling with it is a no-op, text in it
// draws nothing, and it is the "no key / no background" value for blits. It
// costs the palette its last entry, which is worth it for not needing a
// separate transparency flag on every call.
//
// The theme knows. eos_theme.h lays out all 256 indices and its 6x8x4 RGB cube
// would otherwise own 64..255, so it gives this cell up: EOS_PAL_CUBE_NONE.
// Neither eos_theme_cube_index() nor eos_theme_index() can return 255, and pure
// white quantises to slot 251 instead, one green step down. A backend must
// still never render 255 — eos_display_caps_t.palette_len is 255 for a reason.
#define EOS_COLOR_NONE ((eos_color_t)0xFF)

#define EOS_PALETTE_MAX 256

// Pack a palette entry. Palettes are authored in 24-bit and converted by the
// backend when eos_display_palette() is called, not per pixel.
static inline uint32_t eos_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline uint16_t eos_rgb565(uint32_t rgb888)
{
    return (uint16_t)((((rgb888 >> 16) & 0xF8) << 8) |
                      (((rgb888 >>  8) & 0xFC) << 3) |
                      (((rgb888      ) & 0xF8) >> 3));
}

// Rec.601 luma, integer. The mono backend thresholds on this to decide whether
// a palette entry is ink or paper.
static inline uint8_t eos_luma(uint32_t rgb888)
{
    uint32_t r = (rgb888 >> 16) & 0xFF, g = (rgb888 >> 8) & 0xFF, b = rgb888 & 0xFF;
    return (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
}

// --------------------------------------------------------------- geometry

static inline eos_rect_t eos_rect(int16_t x, int16_t y, int16_t w, int16_t h)
{
    eos_rect_t r; r.x = x; r.y = y; r.w = w; r.h = h; return r;
}

static inline bool eos_rect_empty(eos_rect_t r) { return r.w <= 0 || r.h <= 0; }

static inline eos_rect_t eos_rect_isect(eos_rect_t a, eos_rect_t b)
{
    int16_t x0 = a.x > b.x ? a.x : b.x;
    int16_t y0 = a.y > b.y ? a.y : b.y;
    int16_t x1 = (int16_t)((a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w));
    int16_t y1 = (int16_t)((a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h));
    if (x1 <= x0 || y1 <= y0) return eos_rect(0, 0, 0, 0);
    return eos_rect(x0, y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0));
}

static inline eos_rect_t eos_rect_union(eos_rect_t a, eos_rect_t b)
{
    if (eos_rect_empty(a)) return b;
    if (eos_rect_empty(b)) return a;
    int16_t x0 = a.x < b.x ? a.x : b.x;
    int16_t y0 = a.y < b.y ? a.y : b.y;
    int16_t x1 = (int16_t)((a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w));
    int16_t y1 = (int16_t)((a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h));
    return eos_rect(x0, y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0));
}

static inline bool eos_rect_hit(eos_rect_t r, int16_t x, int16_t y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static inline eos_rect_t eos_rect_inset(eos_rect_t r, int16_t d)
{
    return eos_rect((int16_t)(r.x + d), (int16_t)(r.y + d),
                    (int16_t)(r.w - 2 * d), (int16_t)(r.h - 2 * d));
}

// True when the two rects touch at all, which is the test the damage list uses
// before deciding to coalesce.
static inline bool eos_rect_overlap(eos_rect_t a, eos_rect_t b)
{
    return !eos_rect_empty(eos_rect_isect(a, b));
}

// ---------------------------------------------------------------- bitmaps

typedef enum {
    EOS_PIXFMT_I8 = 0,   // one palette index per pixel
    EOS_PIXFMT_A8,       // 8-bit coverage, painted in .tint over .bg
    EOS_PIXFMT_MONO1,    // 1bpp, MSB first; set bits get .tint, clear bits get .bg
    EOS_PIXFMT_RGB565,   // native colour, little endian. Only where EOS_CAP_RGB565.
} eos_pixfmt_t;

// A caller-owned pixel span. The display never takes ownership, never keeps
// the pointer past the call, and never writes through it.
typedef struct {
    const void *pixels;
    int16_t     w, h;
    int16_t     stride;    // BYTES per row, all formats. 0 means "tightly packed".
    uint8_t     fmt;       // eos_pixfmt_t
    eos_color_t key;       // I8: source pixels equal to this are skipped.
                           // EOS_COLOR_NONE disables keying.
    eos_color_t tint;      // A8 / MONO1: the ink colour.
    eos_color_t bg;        // A8 / MONO1: the paper colour, or EOS_COLOR_NONE for
                           // transparent. Ignored for I8 and RGB565.
} eos_bitmap_t;

// Rows are packed to whole bytes when stride is 0.
static inline int16_t eos_bitmap_stride(const eos_bitmap_t *b)
{
    if (b->stride) return b->stride;
    if (b->fmt == EOS_PIXFMT_MONO1)  return (int16_t)((b->w + 7) / 8);
    if (b->fmt == EOS_PIXFMT_RGB565) return (int16_t)(b->w * 2);
    return b->w;
}

// ------------------------------------------------------------------ fonts
//
// One 1bpp bitmap font format for every tier. LVGL backends are expected to
// rasterise these bits themselves rather than substitute an LVGL font, so a
// screenshot from the CYD and one from a PSRAM board line up pixel for pixel
// and the shell's layout arithmetic stays correct everywhere.

typedef struct {
    uint8_t  first;          // lowest character code with a glyph
    uint8_t  last;           // highest
    uint8_t  fallback;       // drawn for anything outside [first,last]
    uint8_t  cell_w;         // fixed-width cell, or 0 when widths[] applies
    uint8_t  h;              // glyph height in pixels
    uint8_t  gap;            // pixels inserted between glyphs
    uint8_t  leading;        // extra pixels between lines

    const uint8_t  *bits;    // glyph rows, MSB first, glyphs concatenated
    const uint8_t  *widths;  // per-glyph pixel width, NULL when cell_w != 0
    const uint32_t *offsets; // bit offset of each glyph into bits[], NULL when fixed
} eos_font_t;

typedef enum {
    EOS_FONT_TINY = 0,   // 4x6   dense lists, the SSD1306 status line
    EOS_FONT_SMALL,      // 6x8   the workhorse, and what the terminal renders in
    EOS_FONT_MED,        // 8x13
    EOS_FONT_BIG,        // 12x20 clock, scores, big numbers
    EOS_FONT_COUNT
} eos_font_id_t;

// Returns NULL when that font was not linked into this image. Implemented by
// the font tables, not by a display backend.
const eos_font_t *eos_font_get(eos_font_id_t id);

static inline int eos_font_line_h(const eos_font_t *f)
{
    return f ? (int)f->h + (int)f->leading : 0;
}

// Maps a character onto a glyph index, folding out-of-range codes onto the
// fallback. Every measurement and draw path agrees because they all call this.
static inline int eos_font_glyph(const eos_font_t *f, unsigned char c)
{
    if (c < f->first || c > f->last) {
        c = f->fallback;
        if (c < f->first || c > f->last) c = f->first;
    }
    return (int)c - (int)f->first;
}

static inline int eos_font_glyph_w(const eos_font_t *f, unsigned char c)
{
    if (f->cell_w) return f->cell_w;
    return f->widths[eos_font_glyph(f, c)];
}

// Width of a run in pixels. len < 0 means NUL-terminated.
static inline int eos_text_width(const eos_font_t *f, const char *s, int len)
{
    if (!f || !s) return 0;
    if (len < 0) { len = 0; while (s[len]) len++; }
    int w = 0;
    for (int i = 0; i < len; i++) {
        w += eos_font_glyph_w(f, (unsigned char)s[i]);
        if (i + 1 < len) w += f->gap;
    }
    return w;
}

// How many leading characters of s fit in max_w pixels. The shell uses this to
// truncate tab labels, which is unavoidable on a 2.4 inch panel.
static inline int eos_text_fit(const eos_font_t *f, const char *s, int len, int max_w)
{
    if (!f || !s || max_w <= 0) return 0;
    if (len < 0) { len = 0; while (s[len]) len++; }
    int w = 0, n = 0;
    for (int i = 0; i < len; i++) {
        int gw = eos_font_glyph_w(f, (unsigned char)s[i]);
        int step = (n ? f->gap : 0) + gw;
        if (w + step > max_w) break;
        w += step;
        n++;
    }
    return n;
}

// ------------------------------------------------------------------- caps

#define EOS_CAP_RETAINED  0x0001  // framebuffer survives between frames; one band per frame
#define EOS_CAP_RGB565    0x0002  // blit accepts EOS_PIXFMT_RGB565 sources
#define EOS_CAP_BLEND     0x0004  // A8 coverage blends; without this it thresholds at 128
#define EOS_CAP_BACKLIGHT 0x0008  // there is a backlight pin at all
#define EOS_CAP_DIM       0x0010  // that backlight is PWM, not on/off
#define EOS_CAP_ANIM      0x0020  // the shell may run animations (tier RICH only)
#define EOS_CAP_PALETTE   0x0040  // eos_display_palette() changes anything

typedef struct {
    int16_t  w, h;          // after rotation. This is what eos_wm_layout() is given.
    uint8_t  tier;          // eos_tier_t, copied from the board
    uint8_t  fmt;           // eos_pixfmt_t of the backend's own framebuffer
    uint16_t caps;
    uint16_t palette_len;   // usable palette entries, EOS_COLOR_NONE excluded
    int16_t  band_h;        // rows per band; equals h when EOS_CAP_RETAINED
    int16_t  max_bands;     // bands a full-screen frame will need
} eos_display_info_t;

// There is exactly one display in a firmware image. The backend is chosen at
// link time by the board's tier, so this is a plain singleton with no vtable
// and no indirect call on the hot path.
//
// eos_display_init() is the only call in this header permitted to take memory,
// and it takes it once: the framebuffer, at boot, from the board's declared
// heap_budget, and never returns it. Every other call here draws into memory
// that already exists.
eos_err_t eos_display_init(void);

const eos_display_info_t *eos_display_info(void);

// Loads count entries starting at index first. Converted to the backend's
// native format here, once, not per pixel. Writing to EOS_COLOR_NONE is
// ignored. Never blocks — a palette change lands on the next frame.
eos_err_t eos_display_palette(const uint32_t *rgb888, uint16_t first, uint16_t count);

// Nearest entry in the loaded palette. For loading images and for the theme
// component, not for the draw path — cache what you get back.
eos_color_t eos_display_match(uint32_t rgb888);

// ------------------------------------------------------------------ damage

#define EOS_DAMAGE_MAX 8

// Declare what changed, BEFORE opening the frame. Rects past EOS_DAMAGE_MAX
// are coalesced into their nearest neighbour rather than dropped, so damage is
// always conservative: it may repaint more than needed, never less.
//
// Drawing does NOT extend damage. On a banded backend it could not — the bands
// are already fixed by the time you draw. Declare first, then draw.
void eos_display_damage(eos_rect_t r);
void eos_display_damage_all(void);

// ------------------------------------------------------------- frame loop
//
//     eos_display_damage(tile_rect);
//     eos_display_frame_begin();
//     while (eos_display_frame_band(&band))
//         draw_scene(band);          // must be re-runnable
//     eos_display_frame_end();
//
// frame_begin  coalesces the damage list, resets the clip stack, opens the
//              frame. Never blocks, never allocates.
// frame_band   pushes the band just drawn to the panel — this is where SPI or
//              I2C happens and where the call MAY BLOCK — then installs the
//              next band as the base clip rect and returns true. Returns false
//              once every band has been pushed.
// frame_end    waits for the last transfer to retire and clears the damage
//              list. MAY BLOCK. Safe to call with no bands drawn.
//
// On EOS_CAP_RETAINED the loop body runs exactly once with band = the union of
// the damage. Retention buys a cheaper flush, not a cheaper draw: the scene
// callback is written once and is correct on every tier.
void eos_display_frame_begin(void);
bool eos_display_frame_band(eos_rect_t *band);
void eos_display_frame_end(void);

// ------------------------------------------------------------- clip stack

#define EOS_CLIP_DEPTH 8

// Intersects r with the current clip and pushes it. Returns false when the
// stack is full or the result is empty — in both cases nothing is pushed, so
// do not pop. The band installed by frame_band() is the floor and cannot be
// popped off.
bool       eos_display_clip_push(eos_rect_t r);
void       eos_display_clip_pop(void);
eos_rect_t eos_display_clip(void);

// ---------------------------------------------------------------- drawing
//
// All four are clipped to the current clip rect. All four are no-ops outside
// an open frame, outside the current band, or with EOS_COLOR_NONE. None of
// them allocate and none of them block: they touch the framebuffer or queue an
// LVGL draw and return.

void eos_display_fill(eos_rect_t r, eos_color_t c);

void eos_display_blit(int16_t x, int16_t y, const eos_bitmap_t *b);

// Draws a run with its TOP-LEFT at (x,y) — not a baseline, because half the
// glyph sets here are 6x8 cells with no meaningful baseline. Background is the
// caller's job: fill first, then draw. Returns the pen advance in pixels,
// which equals eos_text_width() and is returned anyway so the common case does
// not measure twice. len < 0 means NUL-terminated.
int eos_display_text(int16_t x, int16_t y, const eos_font_t *f,
                     eos_color_t c, const char *s, int len);

// 0 = off, 100 = full. Backends without EOS_CAP_DIM snap at 50. Returns
// EOS_ERR_NODEV when the board declares no backlight pin (the SSD1306 has
// none — it is emissive). May block briefly on the LEDC peripheral.
eos_err_t eos_display_backlight(uint8_t percent);

// -------------------------------------------------- composed convenience
//
// Everything below is written in terms of eos_display_fill(). It lives here so
// that a backend implements four drawing calls, not fourteen, and so the
// shapes look identical on all three tiers.

static inline void eos_display_pixel(int16_t x, int16_t y, eos_color_t c)
{
    eos_display_fill(eos_rect(x, y, 1, 1), c);
}

static inline void eos_display_hline(int16_t x, int16_t y, int16_t w, eos_color_t c)
{
    eos_display_fill(eos_rect(x, y, w, 1), c);
}

static inline void eos_display_vline(int16_t x, int16_t y, int16_t h, eos_color_t c)
{
    eos_display_fill(eos_rect(x, y, 1, h), c);
}

// Outline of thickness t drawn INSIDE r, so a bordered tile still occupies
// exactly the rect the window manager handed out.
static inline void eos_display_border(eos_rect_t r, int16_t t, eos_color_t c)
{
    if (t <= 0 || eos_rect_empty(r)) return;
    if (t * 2 >= r.w || t * 2 >= r.h) { eos_display_fill(r, c); return; }
    eos_display_fill(eos_rect(r.x, r.y, r.w, t), c);
    eos_display_fill(eos_rect(r.x, (int16_t)(r.y + r.h - t), r.w, t), c);
    eos_display_fill(eos_rect(r.x, (int16_t)(r.y + t), t, (int16_t)(r.h - 2 * t)), c);
    eos_display_fill(eos_rect((int16_t)(r.x + r.w - t), (int16_t)(r.y + t), t,
                              (int16_t)(r.h - 2 * t)), c);
}

// Paints the whole current clip rect, which inside the frame loop is the band.
static inline void eos_display_clear(eos_color_t c)
{
    eos_display_fill(eos_display_clip(), c);
}

// Centres a run horizontally in r and returns where it landed, so a caller can
// underline or invert it without measuring again.
static inline int16_t eos_display_text_center(eos_rect_t r, int16_t y,
                                              const eos_font_t *f,
                                              eos_color_t c, const char *s)
{
    int n = eos_text_fit(f, s, -1, r.w);
    int w = eos_text_width(f, s, n);
    int16_t x = (int16_t)(r.x + (r.w - w) / 2);
    eos_display_text(x, y, f, c, s, n);
    return x;
}

#endif // EOS_DISPLAY_H
