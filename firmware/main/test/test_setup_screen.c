// test_setup_screen — the acceptance test for the provisioning panel.
//
// A QR that does not scan fails silently, on someone else's phone, in a room
// you are not in. So this draws the real eos_setup_screen.c into a framebuffer
// and checks the symbol that came out module for module against
// eos_qr_module(), measures the quiet zone in pixels, and asserts that the AP
// name, the password and the URL landed under it and on the panel.
//
// The one non-obvious thing here: it links a minimal eos_display backend of its
// own rather than kernel/hal/backend/esp_lcd. That backend's host build
// composites forty rows at a time into BSS and eos_display_host_band() is only
// valid while the frame is open, so there is no way to read all six bands back
// from outside eos_setup_screen_draw()'s own band loop. This one keeps a whole
// 240x240 indexed page, which is what makes the symbol checkable.
//
// Panel size is a compile-time argument, because the text-only fallback is a
// property of small panels and docs/provisioning.md puts the 128x64 OLED there
// on purpose:
//
//   cc ... -DW=128 -DH=64 ...

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "eos_display.h"
#include "eos_font.h"
#include "eos_theme.h"
#include "eos_qr.h"
#include "eos_setup_screen.h"

#ifndef W
#define W 240
#endif
#ifndef H
#define H 240
#endif

static uint8_t  fb[H][W];
static uint32_t pal[256];
static eos_display_info_t info;
static eos_rect_t clipstack[8];
static int clipn;
static bool frame_open;
static int band_i;
#define BAND_H 40

eos_err_t eos_display_init(void)
{
    info.w = W; info.h = H; info.tier = 1; info.fmt = EOS_PIXFMT_I8;
    info.palette_len = 255; info.band_h = BAND_H;
    info.max_bands = (H + BAND_H - 1) / BAND_H;
    info.caps = EOS_CAP_PALETTE | EOS_CAP_BLEND;
    return EOS_OK;
}
const eos_display_info_t *eos_display_info(void) { return &info; }

eos_err_t eos_display_palette(const uint32_t *rgb, uint16_t first, uint16_t count)
{
    uint16_t i;
    for (i = 0; i < count && (uint32_t)first + i < 256u; i++) pal[first + i] = rgb[i];
    return EOS_OK;
}

eos_color_t eos_display_match(uint32_t rgb)
{
    long best = -1; int bi = 0, i;
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
    for (i = 0; i < 255; i++) {
        int pr = (int)((pal[i] >> 16) & 0xFF), pg = (int)((pal[i] >> 8) & 0xFF), pb = (int)(pal[i] & 0xFF);
        long d = (long)(pr-r)*(pr-r) + (long)(pg-g)*(pg-g) + (long)(pb-b)*(pb-b);
        if (best < 0 || d < best) { best = d; bi = i; }
    }
    return (eos_color_t)bi;
}

void eos_display_damage(eos_rect_t r) { (void)r; }
void eos_display_damage_all(void) { }

void eos_display_frame_begin(void) { frame_open = true; band_i = 0; }
bool eos_display_frame_band(eos_rect_t *band)
{
    if (!frame_open || band_i * BAND_H >= H) return false;
    clipn = 0;
    clipstack[clipn++] = eos_rect(0, (int16_t)(band_i * BAND_H), W,
                                  (int16_t)((band_i * BAND_H + BAND_H > H) ? (H - band_i * BAND_H) : BAND_H));
    if (band) *band = clipstack[0];
    band_i++;
    return true;
}
void eos_display_frame_end(void) { frame_open = false; clipn = 0; }

eos_rect_t eos_display_clip(void)
{
    return clipn ? clipstack[clipn - 1] : eos_rect(0, 0, 0, 0);
}
bool eos_display_clip_push(eos_rect_t r)
{
    eos_rect_t c;
    if (clipn >= 8) return false;
    c = eos_rect_isect(r, clipstack[clipn - 1]);
    if (eos_rect_empty(c)) return false;
    clipstack[clipn++] = c;
    return true;
}
void eos_display_clip_pop(void) { if (clipn > 1) clipn--; }

void eos_display_fill(eos_rect_t r, eos_color_t c)
{
    eos_rect_t a; int16_t x, y;
    if (!frame_open || c == EOS_COLOR_NONE || !clipn) return;
    a = eos_rect_isect(r, clipstack[clipn - 1]);
    for (y = a.y; y < a.y + a.h; y++)
        for (x = a.x; x < a.x + a.w; x++)
            if (x >= 0 && x < W && y >= 0 && y < H) fb[y][x] = c;
}

void eos_display_blit(int16_t x0, int16_t y0, const eos_bitmap_t *b)
{
    int16_t sx, sy, stride;
    const uint8_t *px = (const uint8_t *)b->pixels;
    if (!frame_open || !px) return;
    stride = eos_bitmap_stride(b);
    if (b->fmt != EOS_PIXFMT_MONO1) { fprintf(stderr, "harness: only MONO1\n"); exit(2); }
    for (sy = 0; sy < b->h; sy++) {
        const uint8_t *row = px + (size_t)sy * (size_t)stride;
        for (sx = 0; sx < b->w; sx++) {
            int on = (row[sx >> 3] >> (7 - (sx & 7))) & 1;
            eos_color_t c = on ? b->tint : b->bg;
            if (c == EOS_COLOR_NONE) continue;
            eos_display_fill(eos_rect((int16_t)(x0 + sx), (int16_t)(y0 + sy), 1, 1), c);
        }
    }
}

int eos_display_text(int16_t x, int16_t y, const eos_font_t *f, eos_color_t c,
                     const char *s, int len)
{
    int i, adv = 0;
    if (!f || !s) return 0;
    if (len < 0) { len = 0; while (s[len]) len++; }
    for (i = 0; i < len; i++) {
        const uint8_t *bits = eos_font_glyph_bits(f, (unsigned char)s[i]);
        int rb = eos_font_row_bytes(f), w = eos_font_glyph_w(f, (unsigned char)s[i]);
        int r, k;
        if (bits) for (r = 0; r < (int)f->h; r++)
            for (k = 0; k < w; k++)
                if ((bits[(size_t)r * (size_t)rb + (k >> 3)] >> (7 - (k & 7))) & 1)
                    eos_display_fill(eos_rect((int16_t)(x + adv + k), (int16_t)(y + r), 1, 1), c);
        adv += w + (i + 1 < len ? f->gap : 0);
    }
    return adv;
}

eos_err_t eos_display_backlight(uint8_t p) { (void)p; return EOS_OK; }


// ------------------------------------------------------------------ checks

static int checks, fails;
#define CK(c, msg) do { checks++; if (!(c)) { fails++; printf("  FAIL %s\n", msg); } } while (0)

static eos_theme_t  theme;
static eos_color_t  ink, paper, bgc;

static void upload(const eos_theme_t *t)
{
    uint32_t rgb[32];
    int i, j;
    for (i = 0; i < EOS_PAL_SIZE; i += 32) {
        for (j = 0; j < 32; j++) {
            eos_rgb_t c = eos_theme_palette_rgb(t, (uint8_t)(i + j));
            rgb[j] = eos_rgb(c.r, c.g, c.b);
        }
        eos_display_palette(rgb, (uint16_t)i, 32);
    }
}

static void clear_fb(void) { memset(fb, (int)bgc, sizeof fb); }

// Rows/cols of anything that is not the background.
static void extent(int *top, int *bot, int *left, int *right)
{
    int x, y;
    *top = H; *bot = -1; *left = W; *right = -1;
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) if (fb[y][x] != bgc) {
        if (y < *top) *top = y;
        if (y > *bot) *bot = y;
        if (x < *left) *left = x;
        if (x > *right) *right = x;
    }
}

static void dump(void)
{
    int x, y;
    if (!getenv("DUMP")) return;
    for (y = 0; y < H; y += 2) {
        for (x = 0; x < W; x++)
            putchar(fb[y][x] == bgc ? ' ' : (fb[y][x] == paper ? '.' :
                    (fb[y][x] == ink ? '#' : '+')));
        putchar('\n');
    }
}

// ------------------------------------------------------------ setup screen

static void test_setup(const char *ssid, const char *psk, bool expect_qr)
{
    eos_setup_view_t v;
    eos_qr_t ref;
    char payload[224];
    int x, y, qx = -1, qy = -1, qw = 0, qh = 0;
    int top, bot, left, right;

    snprintf(payload, sizeof payload, "WIFI:S:%s;T:WPA;P:%s;;", ssid, psk);
    printf("  setup \"%s\" / \"%s\" - %u byte payload\n", ssid, psk,
           (unsigned)strlen(payload));

    memset(&v, 0, sizeof v);
    v.theme = &theme; v.ap_ssid = ssid; v.ap_psk = psk;
    v.url = "http://192.168.4.1"; v.qr = payload;
    v.status = "scan the code, then open the page";

    clear_fb();
    eos_setup_screen_draw(&v);
    dump();

    if (!expect_qr) {
        extent(&top, &bot, &left, &right);
        printf("    text only, ink rows %d..%d cols %d..%d\n", top, bot, left, right);
        CK(!eos_setup_screen_had_qr(), "a payload that will not encode falls back to text");
        CK(bot >= 0, "and the text layout still draws something");
        CK(bot < H && right < W, "and it stays on the panel");
        return;
    }

    CK(eos_setup_screen_had_qr(), "a 240x240 panel gets a symbol");
    if (!eos_setup_screen_had_qr()) return;

    // The widest run of paper pixels is the symbol plus its quiet zone.
    for (y = 0; y < H; y++) {
        int run = 0, start = -1, bestrun = 0, beststart = -1;
        for (x = 0; x < W; x++) {
            if (fb[y][x] == paper) {
                if (run == 0) start = x;
                run++;
                if (run > bestrun) { bestrun = run; beststart = start; }
            } else run = 0;
        }
        if (bestrun > qw) { qw = bestrun; qx = beststart; qy = y; }
    }
    while (qy > 0 && fb[qy - 1][qx] == paper) qy--;
    qh = 0; while (qy + qh < H && fb[qy + qh][qx] == paper) qh++;

    if (eos_qr_encode(&ref, payload) != EOS_QR_OK) { CK(0, "reference encode"); return; }

    {
        int total = ref.size + 2 * EOS_QR_QUIET;
        int scale = qw / (total ? total : 1);
        int quiet_px = EOS_QR_QUIET * scale;
        int mx, my, bad = 0;

        printf("    version %d, %d modules, %d px/module, %dx%d at (%d,%d), quiet %d px\n",
               ref.version, ref.size, scale, qw, qh, qx, qy, quiet_px);

        CK(qw == total * scale && qh == total * scale, "the paper block is exactly the rendered size");
        CK(scale >= 2, "at least two pixels per module, or it is a smudge and not a QR");
        CK(qx >= 0 && qx + qw <= W && qy >= 0 && qy + qh <= H, "the whole symbol is on the panel");

        for (y = 0; y < quiet_px; y++)
            for (x = 0; x < qw; x++)
                if (fb[qy + y][qx + x] != paper || fb[qy + qh - 1 - y][qx + x] != paper) bad++;
        for (x = 0; x < quiet_px; x++)
            for (y = 0; y < qh; y++)
                if (fb[qy + y][qx + x] != paper || fb[qy + y][qx + qw - 1 - x] != paper) bad++;
        CK(bad == 0, "the quiet zone is paper on all four sides");

        bad = 0;
        for (my = 0; my < ref.size; my++)
            for (mx = 0; mx < ref.size; mx++) {
                eos_color_t want = eos_qr_module(&ref, mx, my) ? ink : paper;
                int px0 = qx + quiet_px + mx * scale, py0 = qy + quiet_px + my * scale;
                int i, j;
                for (j = 0; j < scale; j++)
                    for (i = 0; i < scale; i++)
                        if (fb[py0 + j][px0 + i] != want) bad++;
            }
        CK(bad == 0, "every module, every pixel of it, matches eos_qr_module()");
        if (!bad) printf("    %d modules x %d px verified\n", ref.size * ref.size, scale * scale);
    }

    extent(&top, &bot, &left, &right);
    CK(bot < H && right < W, "nothing is drawn past the edge of the panel");
    CK(bot > qy + qh, "the name, the password and the URL are under the symbol");
}

// -------------------------------------------------------- the other screens

static void test_passkey(void)
{
    int top, bot, left, right;

    printf("  passkey screen\n");
    clear_fb();
    eos_setup_screen_passkey(&theme, 428913u, "K809 Keyboard",
        "This keyboard bonds to one board at a time. Pairing it here will "
        "stop it working on the board it was paired to before.");
    dump();
    extent(&top, &bot, &left, &right);
    printf("    ink rows %d..%d, cols %d..%d\n", top, bot, left, right);
    CK(bot >= 0, "it draws something");
    CK(bot < H && right < W && left >= 0 && top >= 0, "and all of it is on the panel");
}

static void test_message(void)
{
    int top, bot, left, right;

    printf("  message screen\n");
    clear_fb();
    eos_setup_screen_message(&theme, "penguinOS", "joining the stored network");
    dump();
    extent(&top, &bot, &left, &right);
    printf("    ink rows %d..%d, cols %d..%d\n", top, bot, left, right);
    CK(bot >= 0, "it draws something");
    CK(bot < H && right < W, "and all of it is on the panel");
}

int main(void)
{
    bool big = (W >= 240 && H >= 240);

    printf("\neos_setup_screen host test, %dx%d panel\n\n", W, H);

    eos_display_init();
    eos_theme_default(&theme);
    upload(&theme);
    ink   = eos_display_match(eos_rgb(0, 0, 0));
    paper = eos_display_match(eos_rgb(255, 255, 255));
    bgc   = eos_theme_role_index(&theme, EOS_ROLE_BG);
    printf("  ink #%06x (idx %u), paper #%06x (idx %u), bg idx %u\n\n",
           (unsigned)pal[ink], (unsigned)ink, (unsigned)pal[paper], (unsigned)paper,
           (unsigned)bgc);
    CK(ink != paper, "the QR's two colours are two different palette entries");

    // The real thing: eos-os-<last4> and a 12-character generated password.
    test_setup("penguinos-f048", "k9mQ2xR7vT4b", big);
    // A longer name pushes the symbol to version 4 and the scale down one step.
    test_setup("penguinos-with-a-much-longer-name", "k9mQ2xR7vT4b", big);
    // Past the version-4 byte-mode capacity: refused, and the text layout runs.
    test_setup("penguinos-f048",
               "0123456789012345678901234567890123456789012345678901234567890123", false);

    test_passkey();
    test_message();

    printf("\n=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
