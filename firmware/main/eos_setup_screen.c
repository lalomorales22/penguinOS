// eos_setup_screen — implementation. See the header for why the QR is not
// themed and why these scenes own the whole panel.
//
// Like eos_shell_draw.c, every scene here is replayed once per band with a
// different clip installed, so nothing may be computed from a counter and
// nothing may depend on what an earlier band did. The QR is encoded and
// rasterised ONCE, before the frame opens, into a static buffer that the band
// loop only blits: encoding a version-3 symbol costs about 1,600 GF(256)
// multiplies and doing that six times for six identical answers is the kind of
// waste that shows up as a visible tear on a 40 MHz SPI panel.

#include "eos_setup_screen.h"

#include <stdio.h>
#include <string.h>

#include "eos_display.h"
#include "eos_font.h"
#include "eos_qr.h"

// The largest module scale this file will rasterise, and therefore the size of
// the static pixel buffer: a version-4 symbol with its quiet zone at 5x is
// 205x205 and costs 5,330 bytes. Scale is chosen at runtime to fit the panel
// and is usually 4 on a 240x240; the cap is here so the buffer is a compile
// time constant on a board with no PSRAM.
#define QR_SCALE_MAX 5

// Below this a symbol is not worth drawing. Two pixels per module is already
// marginal; one is a smudge that no camera resolves, and a QR that does not
// scan is worse than an honest instruction to type the password in by hand.
#define QR_SCALE_MIN 2

static eos_qr_t s_qr;
static uint8_t  s_qr_px[EOS_QR_SCALED_BYTES(EOS_QR_MAX_SIZE, QR_SCALE_MAX, EOS_QR_QUIET)];
static char     s_qr_text[EOS_QR_MAX_BYTES + 1];
static int      s_qr_w, s_qr_h;
static bool     s_qr_ready;
static bool     s_qr_drawn;

// ------------------------------------------------------------------- faces

typedef struct {
    const eos_font_t *tiny, *small, *med, *big;
    eos_color_t bg, surface, text, muted, accent, warn, ok;
    eos_color_t paper, ink;      // the QR's own two colours, never the theme's
} skin_t;

static const eos_font_t *face_or(const eos_font_t *want, const eos_font_t *fb)
{
    return want ? want : fb;
}

static void skin_build(skin_t *s, const eos_theme_t *t)
{
    memset(s, 0, sizeof(*s));

    s->small = eos_font_get(EOS_FONT_SMALL);
    s->tiny  = face_or(eos_font_get(EOS_FONT_TINY), s->small);
    s->med   = face_or(eos_font_get(EOS_FONT_MED),  s->small);
    s->big   = face_or(eos_font_get(EOS_FONT_BIG),  s->med);
    if (!s->small) s->small = s->tiny;

    s->bg      = eos_theme_role_index(t, EOS_ROLE_BG);
    s->surface = eos_theme_role_index(t, EOS_ROLE_SURFACE);
    s->text    = eos_theme_role_index(t, EOS_ROLE_TEXT);
    s->muted   = eos_theme_role_index(t, EOS_ROLE_MUTED);
    s->accent  = eos_theme_role_index(t, EOS_ROLE_ACCENT);
    s->warn    = eos_theme_role_index(t, EOS_ROLE_WARN);
    s->ok      = eos_theme_role_index(t, EOS_ROLE_OK);

    // Asked of the palette, not of the theme. eos_display_match() returns the
    // closest entry the backend actually has, which on a 1bpp panel is the only
    // two it has and on RGB565 is exact.
    s->paper = eos_display_match(eos_rgb(255, 255, 255));
    s->ink   = eos_display_match(eos_rgb(0, 0, 0));
}

// --------------------------------------------------------------- text bits

static int16_t line_h(const eos_font_t *f) { return f ? (int16_t)f->h : 0; }

static void centre(int16_t y, const eos_font_t *f, eos_color_t c, const char *s)
{
    eos_rect_t clip;
    if (!f || !s || !*s) return;
    clip = eos_rect(0, y, eos_display_info()->w, (int16_t)f->h);
    eos_display_text_center(clip, y, f, c, s);
}

// One glyph blown up by an integer factor, drawn as filled runs. There is no
// scaling blit in eos_display.h and there should not be — this is the only
// place in the image that wants one, and it wants it for six digits on a screen
// that is redrawn when a human does something, not sixty times a second.
//
// Runs rather than pixels: a 12x20 glyph at 3x is 720 fills done naively and
// about 60 done this way, and eos_display_fill() clips per call.
static void glyph_scaled(int16_t x, int16_t y, const eos_font_t *f,
                         unsigned char ch, eos_color_t c, int scale)
{
    const uint8_t *bits = eos_font_glyph_bits(f, ch);
    int row_bytes = eos_font_row_bytes(f);
    int w = eos_font_glyph_w(f, ch);
    int r, i;

    if (!bits || row_bytes <= 0) return;

    for (r = 0; r < (int)f->h; r++) {
        const uint8_t *row = bits + (size_t)r * (size_t)row_bytes;
        int run = -1;

        for (i = 0; i <= w; i++) {
            bool on = (i < w) && ((row[i >> 3] >> (7 - (i & 7))) & 1u);
            if (on && run < 0) run = i;
            if (!on && run >= 0) {
                eos_display_fill(eos_rect((int16_t)(x + run * scale),
                                          (int16_t)(y + r * scale),
                                          (int16_t)((i - run) * scale),
                                          (int16_t)scale), c);
                run = -1;
            }
        }
    }
}

static void text_scaled(int16_t x, int16_t y, const eos_font_t *f,
                        eos_color_t c, const char *s, int scale)
{
    int i;
    if (!f || !s || scale < 1) return;
    for (i = 0; s[i]; i++) {
        glyph_scaled(x, y, f, (unsigned char)s[i], c, scale);
        x = (int16_t)(x + ((int)eos_font_glyph_w(f, (unsigned char)s[i]) + (int)f->gap) * scale);
    }
}

static void centre_scaled(int16_t y, const eos_font_t *f, eos_color_t c,
                          const char *s, int scale)
{
    int w;
    if (!f || !s) return;
    w = eos_text_width(f, s, -1) * scale;
    text_scaled((int16_t)((eos_display_info()->w - w) / 2), y, f, c, s, scale);
}

// Greedy word wrap. Pure — same string, same breaks, in every band. Returns the
// number of lines it drew, so a caller can lay out what comes after it.
static int wrap(int16_t x, int16_t y, int16_t w, const eos_font_t *f,
                eos_color_t c, const char *s, int max_lines)
{
    int line = 0;

    if (!f || !s || w <= 0) return 0;
    while (*s && line < max_lines) {
        int fit = eos_text_fit(f, s, -1, (int)w);
        int take = fit, i;

        if (fit <= 0) break;
        if (s[fit]) {                       // more to come: break on a space
            for (i = fit; i > 0; i--) {
                if (s[i] == ' ') { take = i; break; }
            }
        }
        eos_display_text(x, (int16_t)(y + line * (line_h(f) + 1)), f, c, s, take);
        line++;
        s += take;
        while (*s == ' ') s++;
    }
    return line;
}

// ------------------------------------------------------------------ the QR

// Encodes and rasterises, at the largest scale that fits the box it is given.
// Re-encoding the same payload is skipped: this runs on every frame of a screen
// that is redrawn whenever the join status changes.
static void qr_prepare(const char *payload, int16_t box_w, int16_t box_h)
{
    int scale, px;

    s_qr_ready = false;
    if (!payload || !*payload) return;

    if (strcmp(payload, s_qr_text) != 0) {
        s_qr_text[0] = '\0';
        if (eos_qr_encode(&s_qr, payload) != EOS_QR_OK) return;
        snprintf(s_qr_text, sizeof s_qr_text, "%s", payload);
    }
    if (!s_qr_text[0]) return;

    for (scale = QR_SCALE_MAX; scale >= QR_SCALE_MIN; scale--) {
        px = EOS_QR_SCALED_PX(s_qr.size, scale, EOS_QR_QUIET);
        if (px <= (int)box_w && px <= (int)box_h) break;
    }
    if (scale < QR_SCALE_MIN) return;

    if (!eos_qr_render(&s_qr, scale, EOS_QR_QUIET, s_qr_px, sizeof s_qr_px,
                       &s_qr_w, &s_qr_h))
        return;
    s_qr_ready = true;
}

static void qr_blit(int16_t x, int16_t y, const skin_t *s)
{
    eos_bitmap_t bm;

    memset(&bm, 0, sizeof bm);
    bm.pixels = s_qr_px;
    bm.w      = (int16_t)s_qr_w;
    bm.h      = (int16_t)s_qr_h;
    bm.stride = (int16_t)((s_qr_w + 7) / 8);
    bm.fmt    = EOS_PIXFMT_MONO1;
    bm.key    = EOS_COLOR_NONE;
    bm.tint   = s->ink;      // set bit = dark module
    bm.bg     = s->paper;    // clear bit = the quiet zone and the light modules
    eos_display_blit(x, y, &bm);
}

// --------------------------------------------------------------- the header

static int16_t header(const skin_t *s, const char *title)
{
    const eos_display_info_t *info = eos_display_info();
    int16_t h = (int16_t)(line_h(s->small) + 6);

    eos_display_fill(eos_rect(0, 0, info->w, h), s->accent);
    centre((int16_t)3, s->small, s->bg, title);
    return h;
}

// ==================================================== the setup screen

// The text-only layout, and not only a fallback: docs/provisioning.md puts the
// 128x64 OLED here by design. It picks the largest face whose four lines fit,
// so the same code gives a 240x240 panel 12x20 digits and the OLED 6x8 ones.
static void setup_text_only(const eos_setup_view_t *v, const skin_t *s,
                            int16_t top)
{
    const eos_display_info_t *info = eos_display_info();
    const eos_font_t *f = s->small;
    const eos_font_t *cand[4];
    int16_t y, pitch;
    int i;

    cand[0] = s->big; cand[1] = s->med; cand[2] = s->small; cand[3] = s->tiny;
    for (i = 0; i < 4; i++) {
        if (!cand[i]) continue;
        if (top + 4 * (line_h(cand[i]) + 2) <= info->h) { f = cand[i]; break; }
    }

    pitch = (int16_t)(line_h(f) + 2);
    y = (int16_t)(top + 2);
    centre(y, f, s->accent, v->ap_ssid ? v->ap_ssid : "");   y = (int16_t)(y + pitch);
    centre(y, f, s->text,   v->ap_psk  ? v->ap_psk  : "");   y = (int16_t)(y + pitch);
    centre(y, f, s->muted,  v->url     ? v->url     : "");   y = (int16_t)(y + pitch);
    if (v->status)
        centre(y, f, v->status_warn ? s->warn : s->ok, v->status);
}

static void setup_scene(const eos_setup_view_t *v, const skin_t *s,
                        int16_t qr_x, int16_t qr_y, int16_t foot_y)
{
    int16_t y = foot_y;

    eos_display_clear(s->bg);
    (void)header(s, "JOIN THIS BOARD");

    if (!s_qr_ready) {
        setup_text_only(v, s, (int16_t)(line_h(s->small) + 8));
        return;
    }

    qr_blit(qr_x, qr_y, s);

    centre(y, s->med, s->accent, v->ap_ssid ? v->ap_ssid : "");
    y = (int16_t)(y + line_h(s->med) + 3);
    centre(y, s->big, s->text, v->ap_psk ? v->ap_psk : "");
    y = (int16_t)(y + line_h(s->big) + 3);
    centre(y, s->small, s->muted, v->url ? v->url : "");
    y = (int16_t)(y + line_h(s->small) + 2);
    if (v->status)
        centre(y, s->small, v->status_warn ? s->warn : s->ok, v->status);
}

void eos_setup_screen_draw(const eos_setup_view_t *v)
{
    const eos_display_info_t *info;
    skin_t s;
    eos_rect_t band;
    int16_t head_h, foot_h, foot_y, box_h, qr_x, qr_y;

    if (!v || !v->theme) return;
    info = eos_display_info();
    skin_build(&s, v->theme);

    // The foot is measured before the QR is sized, because the text is the part
    // that must not be squeezed: a password that does not fit on the panel
    // cannot be typed, and a symbol one module smaller still scans.
    head_h = (int16_t)(line_h(s.small) + 6);
    foot_h = (int16_t)(line_h(s.med) + 3 + line_h(s.big) + 3 +
                       line_h(s.small) + 2 + line_h(s.small) + 6);
    box_h  = (int16_t)(info->h - head_h - foot_h - 6);

    qr_prepare(v->qr, (int16_t)(info->w - 8), box_h);
    s_qr_drawn = s_qr_ready;

    qr_x   = (int16_t)((info->w - s_qr_w) / 2);
    qr_y   = (int16_t)(head_h + 3 + (box_h - s_qr_h) / 2);
    foot_y = (int16_t)(info->h - foot_h + 3);

    eos_display_damage_all();
    eos_display_frame_begin();
    while (eos_display_frame_band(&band)) setup_scene(v, &s, qr_x, qr_y, foot_y);
    eos_display_frame_end();
}

bool eos_setup_screen_had_qr(void) { return s_qr_drawn; }

// ==================================================== the pairing screen

static void passkey_scene(const skin_t *s, const char *digits, const char *peer,
                          const char *warning, int scale, int16_t top)
{
    const eos_display_info_t *info = eos_display_info();
    int16_t y = top;

    eos_display_clear(s->bg);
    (void)header(s, "PAIR KEYBOARD");

    centre(y, s->small, s->muted, peer && *peer ? peer : "keyboard");
    y = (int16_t)(y + line_h(s->small) + 8);

    centre_scaled(y, s->big, s->accent, digits, scale);
    y = (int16_t)(y + line_h(s->big) * scale + 8);

    centre(y, s->small, s->text, "type it on the keyboard, then Enter");
    y = (int16_t)(y + line_h(s->small) + 6);
    if (warning) {
        // As many lines as are left. On a 128x64 the warning is what gets cut,
        // never the digits: the digits are the only thing on this screen that
        // cannot be recovered from anywhere else.
        int lines = (info->h - y) / (line_h(s->small) + 1);
        if (lines > 4) lines = 4;
        if (lines > 0) (void)wrap(4, y, (int16_t)(info->w - 8), s->small, s->warn,
                                  warning, lines);
    }
}

void eos_setup_screen_passkey(const eos_theme_t *t, uint32_t passkey,
                              const char *peer, const char *warning)
{
    const eos_display_info_t *info;
    skin_t s;
    eos_rect_t band;
    char digits[8];
    int scale, w;
    int16_t head_h, body_h, top;

    if (!t) return;
    info = eos_display_info();
    skin_build(&s, t);

    snprintf(digits, sizeof digits, "%06u", (unsigned)(passkey % 1000000u));

    // As large as the panel will take. Six digits of the 12x20 face are 72 px,
    // so a 240 px panel lands on 3x and a 128 px one on 1x, and neither needs
    // to be special-cased here.
    w = eos_text_width(s.big, digits, -1);
    for (scale = 4; scale > 1; scale--) {
        if (w * scale <= info->w - 8 && line_h(s.big) * scale <= info->h / 2) break;
    }

    // The block is centred under the header rather than pinned to it: the
    // passkey is the one thing on this screen and pinning leaves a third of the
    // panel empty under it, which reads as a screen that has not finished.
    head_h = (int16_t)(line_h(s.small) + 6);
    body_h = (int16_t)(line_h(s.small) + 8 + line_h(s.big) * scale + 8 +
                       line_h(s.small) + 6 + 4 * (line_h(s.small) + 1));
    top    = (int16_t)(head_h + (info->h - head_h - body_h) / 2);
    if (top < (int16_t)(head_h + 4)) top = (int16_t)(head_h + 4);

    eos_display_damage_all();
    eos_display_frame_begin();
    while (eos_display_frame_band(&band))
        passkey_scene(&s, digits, peer, warning, scale, top);
    eos_display_frame_end();
}

// ======================================================== the message screen

static void message_scene(const skin_t *s, const char *title, const char *line)
{
    const eos_display_info_t *info = eos_display_info();
    int16_t y;

    eos_display_clear(s->bg);
    y = (int16_t)(info->h / 2 - line_h(s->big) - 6);
    centre(y, s->big, s->accent, title ? title : "penguinOS");
    y = (int16_t)(y + line_h(s->big) + 8);
    if (line) (void)wrap(6, y, (int16_t)(info->w - 12), s->small, s->muted, line, 3);
}

void eos_setup_screen_message(const eos_theme_t *t, const char *title,
                              const char *line)
{
    skin_t s;
    eos_rect_t band;

    if (!t) return;
    skin_build(&s, t);

    eos_display_damage_all();
    eos_display_frame_begin();
    while (eos_display_frame_band(&band)) message_scene(&s, title, line);
    eos_display_frame_end();
}
