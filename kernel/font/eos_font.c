#include "eos_font.h"
#include <string.h>

// The glyph tables. Drawn as ASCII art in tools/art/*.txt and packed by
// tools/gen_font.py; the art is the source of truth and this file is the only
// place it is compiled in. Kept as an include rather than a second translation
// unit so a board component adds exactly one .c file to build text.
#include "eos_font_data.inc"

const eos_font_t *eos_font_get(eos_font_id_t id)
{
    if ((int)id < 0 || (int)id >= (int)EOS_FONT_COUNT) return NULL;
    return &eos_font_table[(int)id];
}

// True for the packing this component ships and eos_font.h documents: a fixed
// cell, byte-aligned rows, glyphs concatenated in codepoint order. A font with
// widths[] or offsets[] is a proportional font packed some other way, and the
// callers below refuse it rather than reading it as if it were this one.
static bool fixed_cell(const eos_font_t *f)
{
    return f && f->bits && f->widths == NULL && f->offsets == NULL &&
           f->cell_w != 0 && f->h != 0 && f->last >= f->first;
}

const uint8_t *eos_font_glyph_bits(const eos_font_t *f, unsigned char c)
{
    int g, stride;
    if (!fixed_cell(f)) return NULL;
    // eos_font_glyph() folds anything outside [first,last] onto the fallback
    // and the fallback onto `first`, so g is in [0, last-first] and the offset
    // below cannot leave the table. Every measure and draw path uses it.
    g = eos_font_glyph(f, c);
    stride = eos_font_glyph_bytes(f);
    return f->bits + (size_t)g * (size_t)stride;
}

bool eos_font_glyph_bitmap(const eos_font_t *f, unsigned char c,
                           eos_color_t tint, eos_color_t bg, eos_bitmap_t *out)
{
    const uint8_t *bits;
    if (!out) return false;
    bits = eos_font_glyph_bits(f, c);
    if (!bits) return false;
    memset(out, 0, sizeof(*out));
    out->pixels = bits;
    out->w      = (int16_t)f->cell_w;
    out->h      = (int16_t)f->h;
    out->stride = (int16_t)eos_font_row_bytes(f);
    out->fmt    = EOS_PIXFMT_MONO1;
    out->key    = EOS_COLOR_NONE;   // keying is an I8 idea; MONO1 uses bg
    out->tint   = tint;
    out->bg     = bg;
    return true;
}

int eos_font_measure(const eos_font_t *f, const char *s, int len, int *out_h)
{
    int i, line_w = 0, max_w = 0, lines = 1, run = 0;

    if (out_h) *out_h = 0;
    if (!f || !s) return 0;
    if (len < 0) { len = 0; while (s[len]) len++; }

    for (i = 0; i < len; i++) {
        if (s[i] == '\n') {
            if (line_w > max_w) max_w = line_w;
            line_w = 0;
            run    = 0;
            lines++;
            continue;
        }
        // gap goes BETWEEN glyphs, never before the first one on a line, which
        // is the same rule eos_text_width() applies. It is 0 on every shipped
        // face; this arithmetic exists so the two never drift.
        if (run) line_w += (int)f->gap;
        line_w += eos_font_glyph_w(f, (unsigned char)s[i]);
        run++;
    }
    if (line_w > max_w) max_w = line_w;

    if (out_h) *out_h = lines * (int)f->h + (lines - 1) * (int)f->leading;
    return max_w;
}

int16_t eos_font_measure_cb(const char *s, void *ud)
{
    return (int16_t)eos_font_measure((const eos_font_t *)ud, s, -1, NULL);
}

static const char *const FACE_NAME[EOS_FONT_COUNT] = {
    "tiny", "small", "med", "big"
};

const char *eos_font_name(eos_font_id_t id)
{
    if ((int)id < 0 || (int)id >= (int)EOS_FONT_COUNT) return NULL;
    return FACE_NAME[(int)id];
}

eos_font_id_t eos_font_id_from_name(const char *name)
{
    int i;
    if (name) {
        for (i = 0; i < (int)EOS_FONT_COUNT; i++)
            if (strcmp(name, FACE_NAME[i]) == 0) return (eos_font_id_t)i;
    }
    return EOS_FONT_SMALL;
}

uint32_t eos_font_face_bytes(eos_font_id_t id)
{
    const eos_font_t *f = eos_font_get(id);
    if (!fixed_cell(f)) return 0;
    return (uint32_t)(f->last - f->first + 1) *
           (uint32_t)eos_font_glyph_bytes(f);
}

uint32_t eos_font_flash_bytes(void)
{
    return (uint32_t)EOS_FONT_FLASH_BYTES;
}
