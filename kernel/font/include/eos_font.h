// eos_font — the four bitmap faces, and the only reason anything in ESP-OS can
// draw a character. eos_display.h declares eos_font_t, eos_font_id_t and
// eos_font_get(); this component is where the glyphs actually live and where
// that getter is implemented. Nothing here allocates and nothing here is
// writable: every byte is static const, so all four faces sit in flash.
//
// Flash cost, since a 12x20 face is not free on a 4MB part:
//
//     tiny   4x6   95 glyphs      570 bytes
//     small  6x8   95 glyphs      760 bytes
//     med    8x13  95 glyphs    1,235 bytes
//     big   12x20  95 glyphs    3,800 bytes
//     ------------------------------------
//     total                     6,365 bytes
//
// The one non-obvious constraint: THE CELL WIDTH IS THE PEN ADVANCE. gap is 0
// on every face, so a run of n characters is exactly n * cell_w pixels wide
// and one line is exactly h pixels tall. That is not a stylistic choice — it
// is the contract eos_bar.c relies on when it fits the status bar by
// multiplying char_w by strlen, and eos_font_id_t already publishes the four
// numbers (4x6, 6x8, 8x13, 12x20) to the themes and to the shell. So the
// inter-glyph and interline whitespace is DRAWN INTO THE CELL: every glyph
// leaves the rightmost column (two columns on big) and the bottom row empty
// rather than relying on gap or leading. A glyph that fills its cell edge to
// edge would be legal C and a layout bug on every board.
//
// Bit order and packing, stated unambiguously because the compositor blits
// these directly:
//
//   * Rows run top to bottom. Row 0 is the top of the cell.
//   * Within a row, bit 7 of the first byte is the LEFTMOST pixel, bit 6 the
//     next, and so on. MSB first. A set bit is ink.
//   * Each row is padded to a whole number of bytes:
//     row_bytes = (cell_w + 7) / 8 — 1 for tiny/small/med, 2 for big. The
//     padding bits at the right of the last byte are always clear.
//   * A glyph is row_bytes * h contiguous bytes. Glyphs are concatenated in
//     codepoint order starting at `first`, with no header and no padding
//     between them.
//
// That is byte for byte EOS_PIXFMT_MONO1 with stride = row_bytes, which is
// why eos_font_bitmap() can hand the display an eos_bitmap_t pointing straight
// into flash with no copy and no repacking.
//
// Coverage is printable ASCII 32..126. Anything outside that range renders as
// '?' — see eos_font_glyph() in eos_display.h, which every path here and in
// the compositor must go through so measuring and drawing never disagree.

#ifndef EOS_FONT_H
#define EOS_FONT_H

#include <stdint.h>
#include <stdbool.h>
#include "eos_display.h"

#define EOS_FONT_ASCII_FIRST 32
#define EOS_FONT_ASCII_LAST  126
#define EOS_FONT_FALLBACK    '?'

// Bytes per glyph row, and per whole glyph. Both are 0 for a NULL font and for
// a proportional font, which this component does not ship and does not pack.
static inline int eos_font_row_bytes(const eos_font_t *f)
{
    return (f && f->cell_w) ? ((int)f->cell_w + 7) / 8 : 0;
}

static inline int eos_font_glyph_bytes(const eos_font_t *f)
{
    return eos_font_row_bytes(f) * (f ? (int)f->h : 0);
}

// The four faces, by id. Returns NULL for an id outside eos_font_id_t.
// (Declared in eos_display.h; repeated here so this header stands alone.)
const eos_font_t *eos_font_get(eos_font_id_t id);

// First byte of the glyph for `c`, i.e. its top row. Out-of-range codepoints
// fold onto the fallback glyph, so this NEVER indexes past the table. Returns
// NULL only for a NULL font, a font with no bits, an inverted range, or a
// proportional font (widths/offsets non-NULL), which is not this packing.
const uint8_t *eos_font_glyph_bits(const eos_font_t *f, unsigned char c);

// Points `out` at that glyph as a 1bpp source the display can blit as-is.
// `bg` may be EOS_COLOR_NONE for transparent text, which is the normal case.
// Returns false and leaves `out` untouched when the glyph is unavailable.
bool eos_font_glyph_bitmap(const eos_font_t *f, unsigned char c,
                           eos_color_t tint, eos_color_t bg, eos_bitmap_t *out);

// Pixel size of a run. Returns the width of the widest line and, when out_h is
// non-NULL, writes the total height. len < 0 means NUL-terminated. '\n' starts
// a new line and is the only control character with a meaning here; every
// other byte outside [first,last] draws as the fallback and takes its width.
//
// For a run with no newline this returns exactly eos_text_width(), which is
// exactly cell_w * len on the shipped faces. eos_text_width() is the
// newline-blind primitive in eos_display.h; use this one when the text may be
// multi-line, and use that one on the hot path.
int eos_font_measure(const eos_font_t *f, const char *s, int len, int *out_h);

// eos_bar_metrics_t.measure adapter: pass this as `measure` and the font as
// `ud` and the status bar measures with the real face instead of char_w. With
// gap 0 the two agree exactly, so this exists for the day a proportional
// backend font shows up, not because the bar is wrong today.
int16_t eos_font_measure_cb(const char *s, void *ud);

// Lowercased face name — "tiny", "small", "med", "big" — matching the strings
// the themes carry in eos_theme_metrics_t.font. NULL for a bad id.
const char *eos_font_name(eos_font_id_t id);

// The inverse. An unknown or NULL name resolves to EOS_FONT_SMALL, the
// workhorse, which is also what eos_theme.h documents as its default.
eos_font_id_t eos_font_id_from_name(const char *name);

// Flash occupied by one face, and by all four. Constant-folded from the table.
uint32_t eos_font_face_bytes(eos_font_id_t id);
uint32_t eos_font_flash_bytes(void);

#endif // EOS_FONT_H
