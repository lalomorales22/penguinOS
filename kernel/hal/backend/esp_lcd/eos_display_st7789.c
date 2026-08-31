// eos_display on an ST7789 over esp_lcd — the first backend that puts real
// pixels on real glass. It composites into a small RGB565 strip in DMA-capable
// SRAM and hands that strip to esp_lcd_panel_draw_bitmap(); the panel's own GRAM
// is the frame store, so this backend never owns a full framebuffer.
//
// The one non-obvious thing: the strip is deliberately small even though this
// board could hold a whole 240x240 frame. A backend that only works where RAM
// is plentiful is not a backend — the same file has to serve the 20KB CYD and
// the 320x480 wavvy panels, so the band loop is the only loop, and the scene
// callback is re-run once per band. Everything that follows from that — damage
// fixed before the frame opens, the band as the clip floor, two strips in
// flight so SPI overlaps compositing — is a consequence of that single choice.
//
// Wire order: the ST7789 clocks RGB565 big-endian. The palette LUT therefore
// holds ALREADY-SWAPPED halfwords, so the draw path never byte-swaps per pixel.
// Anything that reads a pixel back out of a band (A8 blending) swaps on the way
// in and out, which is the only place the cost is paid.

#include "eos_display.h"
#include "eos_board.h"
#include "eos_theme.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#endif

// Two strips, not one. draw_bitmap() queues the transfer and returns, so with a
// second strip the CPU composites band N+1 while band N is still on the wire.
// It costs one more strip of SRAM (19,200 bytes here) and buys roughly a factor
// of two on a 40MHz bus. BUF_COUNT 1 is a legal build; it just blocks more.
#define BUF_COUNT 2

// LEDC settings, copied from the verified probe rather than re-derived.
#define BL_TIMER   LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_FREQ_HZ 5000
#define BL_MAX     1023   // 10-bit resolution

// ---------------------------------------------------------------- state

typedef struct {
    bool                inited;
    const eos_board_t  *b;
    eos_display_info_t  info;

    // Palette resolved once, at eos_display_palette() time, into wire order.
    // Index EOS_COLOR_NONE is never read: it is the transparency sentinel and
    // the theme leaves that cube cell unreachable on purpose.
    uint16_t lut[EOS_PALETTE_MAX];
    uint8_t  bg_index;          // theme's EOS_ROLE_BG slot, used for the boot clear

    // Damage, declared before the frame opens. Never grows during one.
    eos_rect_t dmg[EOS_DAMAGE_MAX];
    int        dmg_n;

    // Frame walk.
    bool       open;
    int        di;              // damage rect being banded
    int16_t    ry;              // first undelivered row inside dmg[di]
    int        cur;             // strip currently being composited into
    bool       cur_valid;       // that strip holds a band not yet pushed
    eos_rect_t cur_band;

    // clip[0] is the band, installed by frame_band() and not poppable.
    eos_rect_t clip[EOS_CLIP_DEPTH + 1];
    int        clip_n;

    uint16_t  *buf[BUF_COUNT];
    uint32_t   buf_px;          // pixels one strip can hold
    int        inflight;        // strips queued to the panel and not yet retired

    uint8_t    bl_pct;

#ifdef ESP_PLATFORM
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t    panel;
    SemaphoreHandle_t         done;
    StaticSemaphore_t         done_mem;
#endif
} eos_st7789_t;

static eos_st7789_t st;

#ifndef ESP_PLATFORM
// The host build composites into BSS and pushes nowhere. It exists so the
// compositor logic in this file parses and runs off-target; it drives no panel.
#define HOST_BAND_PX 9600
static uint16_t host_band[HOST_BAND_PX * BUF_COUNT];
#endif

// ---------------------------------------------------------------- colour

static inline uint16_t swap16(uint16_t v)
{
    return (uint16_t)((uint16_t)(v >> 8) | (uint16_t)(v << 8));
}

// Host order to wire order. Whether that is a swap at all is a property of the
// board, not of RGB565: the C6 panels want the bytes reversed and the
// ESP32-S3-Touch-LCD-1.47 wants them as stored. This was a hardcoded swap until
// a board disagreed, and the symptom was pure red rendering as yellow.
//
// The whole compositor works in wire order because only the palette LUT is
// converted - so this runs 256 times at palette load, not once per pixel.
static inline uint16_t wire16(uint16_t v)
{
    return (st.b && !st.b->panel.byte_swap) ? v : swap16(v);
}

// Blend two host-order 565 values in their own 5/6/5 fields. Cheaper and
// closer than round-tripping through 8-bit channels, and the error is below
// one step of the 5-bit axes.
static uint16_t blend565(uint16_t under, uint16_t over, uint8_t a)
{
    unsigned ia = 255u - (unsigned)a;
    unsigned r = ((((unsigned)under >> 11) & 0x1Fu) * ia + (((unsigned)over >> 11) & 0x1Fu) * a) / 255u;
    unsigned g = ((((unsigned)under >>  5) & 0x3Fu) * ia + (((unsigned)over >>  5) & 0x3Fu) * a) / 255u;
    unsigned b = ((((unsigned)under      ) & 0x1Fu) * ia + (((unsigned)over      ) & 0x1Fu) * a) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint32_t un565(uint16_t v)
{
    unsigned r = (v >> 11) & 0x1Fu, g = (v >> 5) & 0x3Fu, b = v & 0x1Fu;
    unsigned r8 = (r << 3) | (r >> 2);
    unsigned g8 = (g << 2) | (g >> 4);
    unsigned b8 = (b << 3) | (b >> 2);
    return ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | (uint32_t)b8;
}

// The compiled-in theme is the palette until something loads one off the card.
// Without this the first frame draws in whatever was in BSS, which on a bring-up
// board reads as a driver bug rather than as a missing palette upload.
static void lut_seed_from_theme(void)
{
    static eos_theme_t th;    // BSS, ~700 bytes; the main task stack is 3584
    eos_theme_default(&th);
    const uint16_t *p = eos_theme_palette565(&th);
    for (int i = 0; i < EOS_PALETTE_MAX; i++) st.lut[i] = wire16(p[i]);
    st.bg_index = th.role_idx[EOS_ROLE_BG];
}

eos_err_t eos_display_palette(const uint32_t *rgb888, uint16_t first, uint16_t count)
{
    if (!rgb888) return EOS_ERR_ARG;
    if ((uint32_t)first + (uint32_t)count > (uint32_t)EOS_PALETTE_MAX) return EOS_ERR_ARG;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = (uint16_t)(first + i);
        if (idx == (uint16_t)EOS_COLOR_NONE) continue;   // sentinel, never a colour
        st.lut[idx] = wire16(eos_rgb565(rgb888[i]));
    }
    return EOS_OK;
}

eos_color_t eos_display_match(uint32_t rgb888)
{
    int tr = (int)((rgb888 >> 16) & 0xFF);
    int tg = (int)((rgb888 >>  8) & 0xFF);
    int tb = (int)( rgb888        & 0xFF);
    uint32_t best = 0xFFFFFFFFu;
    int bi = 0;
    for (int i = 0; i < (int)EOS_COLOR_NONE; i++) {     // 0..254, never the sentinel
        uint32_t c = un565(wire16(st.lut[i]));
        int dr = tr - (int)((c >> 16) & 0xFF);
        int dg = tg - (int)((c >>  8) & 0xFF);
        int db = tb - (int)( c        & 0xFF);
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < best) { best = d; bi = i; if (d == 0) break; }
    }
    return (eos_color_t)bi;
}

// ---------------------------------------------------------------- damage

static eos_rect_t screen_rect(void) { return eos_rect(0, 0, st.info.w, st.info.h); }

void eos_display_damage(eos_rect_t r)
{
    if (!st.inited || st.open) return;   // declared before the frame, never during
    r = eos_rect_isect(r, screen_rect());
    if (eos_rect_empty(r)) return;

    for (int i = 0; i < st.dmg_n; i++) {
        if (eos_rect_overlap(st.dmg[i], r)) {
            st.dmg[i] = eos_rect_union(st.dmg[i], r);
            return;
        }
    }
    if (st.dmg_n < EOS_DAMAGE_MAX) { st.dmg[st.dmg_n++] = r; return; }

    // Full. Fold into whichever neighbour grows least — repaint more, never
    // less. Dropping the rect would leave stale pixels on the panel forever.
    int best = 0;
    int32_t bestcost = 0x7FFFFFFF;
    for (int i = 0; i < EOS_DAMAGE_MAX; i++) {
        eos_rect_t u = eos_rect_union(st.dmg[i], r);
        int32_t cost = (int32_t)u.w * (int32_t)u.h
                     - (int32_t)st.dmg[i].w * (int32_t)st.dmg[i].h;
        if (cost < bestcost) { bestcost = cost; best = i; }
    }
    st.dmg[best] = eos_rect_union(st.dmg[best], r);
}

void eos_display_damage_all(void)
{
    if (!st.inited || st.open) return;
    st.dmg[0] = screen_rect();
    st.dmg_n  = 1;
}

static void damage_coalesce(void)
{
    bool merged = true;
    while (merged) {
        merged = false;
        for (int i = 0; i < st.dmg_n && !merged; i++) {
            for (int j = i + 1; j < st.dmg_n && !merged; j++) {
                if (!eos_rect_overlap(st.dmg[i], st.dmg[j])) continue;
                st.dmg[i] = eos_rect_union(st.dmg[i], st.dmg[j]);
                st.dmg[j] = st.dmg[--st.dmg_n];
                merged = true;
            }
        }
    }
}

// ------------------------------------------------------------ band engine

#ifdef ESP_PLATFORM
static bool IRAM_ATTR color_done_cb(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ed,
                                    void *ctx)
{
    BaseType_t hp = pdFALSE;
    (void)io; (void)ed; (void)ctx;
    xSemaphoreGiveFromISR(st.done, &hp);
    return hp == pdTRUE;
}
#endif

// The completion token comes from the panel's ISR, so a transfer that was never
// queued is a token that never arrives. Counting an unqueued band would leave
// eos_display_frame_end() waiting on it for the rest of the boot, with the
// backlight up and one stale band on the glass — the hardest possible symptom
// to read. So inflight only counts transfers the driver accepted, and a
// refused band is a band that is simply not drawn.
static void band_push(void)
{
#ifdef ESP_PLATFORM
    if (esp_lcd_panel_draw_bitmap(st.panel,
                                  st.cur_band.x, st.cur_band.y,
                                  (int)st.cur_band.x + (int)st.cur_band.w,
                                  (int)st.cur_band.y + (int)st.cur_band.h,
                                  st.buf[st.cur]) == ESP_OK)
        st.inflight++;
#endif
}

static void band_retire_one(void)
{
#ifdef ESP_PLATFORM
    if (st.inflight > 0) {
        xSemaphoreTake(st.done, portMAX_DELAY);
        st.inflight--;
    }
#endif
}

// Rows per strip is not a constant: a narrow damage rect gets more of them,
// because the strip is sized in pixels, not in rows. buf_px >= info.w is
// guaranteed at init, so this never returns zero rows.
static bool next_band(eos_rect_t *out)
{
    while (st.di < st.dmg_n) {
        eos_rect_t r = st.dmg[st.di];
        if (st.ry >= r.h) { st.di++; st.ry = 0; continue; }
        int rows = (int)(st.buf_px / (uint32_t)r.w);
        if (rows < 1) rows = 1;
        if (rows > (int)r.h - (int)st.ry) rows = (int)r.h - (int)st.ry;
        *out = eos_rect(r.x, (int16_t)(r.y + st.ry), r.w, (int16_t)rows);
        st.ry = (int16_t)(st.ry + rows);
        return true;
    }
    return false;
}

void eos_display_frame_begin(void)
{
    if (!st.inited || st.open) return;
    damage_coalesce();
    st.di        = 0;
    st.ry        = 0;
    st.cur       = BUF_COUNT - 1;   // first band lands in buf[0]
    st.cur_valid = false;
    st.clip_n    = 0;
    st.open      = true;
}

bool eos_display_frame_band(eos_rect_t *band)
{
    if (!st.open) return false;

    if (st.cur_valid) { band_push(); st.cur_valid = false; }

    eos_rect_t nb;
    if (!next_band(&nb)) return false;

    // Never composite into a strip the DMA engine is still reading.
    while (st.inflight >= BUF_COUNT) band_retire_one();

    st.cur       = (st.cur + 1) % BUF_COUNT;
    st.cur_band  = nb;
    st.cur_valid = true;
    st.clip[0]   = nb;
    st.clip_n    = 1;
    if (band) *band = nb;
    return true;
}

void eos_display_frame_end(void)
{
    if (!st.open) return;
    if (st.cur_valid) { band_push(); st.cur_valid = false; }   // caller broke out early
    while (st.inflight > 0) band_retire_one();
    st.dmg_n  = 0;
    st.clip_n = 0;
    st.open   = false;
}

// ------------------------------------------------------------- clip stack

eos_rect_t eos_display_clip(void)
{
    if (st.clip_n <= 0) return eos_rect(0, 0, 0, 0);
    return st.clip[st.clip_n - 1];
}

bool eos_display_clip_push(eos_rect_t r)
{
    if (st.clip_n <= 0 || st.clip_n > EOS_CLIP_DEPTH) return false;
    eos_rect_t n = eos_rect_isect(r, st.clip[st.clip_n - 1]);
    if (eos_rect_empty(n)) return false;
    st.clip[st.clip_n++] = n;
    return true;
}

void eos_display_clip_pop(void)
{
    if (st.clip_n > 1) st.clip_n--;   // the band is the floor
}

// ---------------------------------------------------------------- drawing

static inline uint16_t *band_px(int x, int y)
{
    return st.buf[st.cur]
         + (size_t)(y - (int)st.cur_band.y) * (size_t)st.cur_band.w
         + (size_t)(x - (int)st.cur_band.x);
}

static inline bool drawable(void)
{
    return st.open && st.cur_valid && st.clip_n > 0;
}

void eos_display_fill(eos_rect_t r, eos_color_t c)
{
    if (!drawable() || c == EOS_COLOR_NONE) return;
    r = eos_rect_isect(r, st.clip[st.clip_n - 1]);
    if (eos_rect_empty(r)) return;

    uint16_t v = st.lut[c];
    uint16_t *row0 = band_px(r.x, r.y);
    for (int x = 0; x < r.w; x++) row0[x] = v;
    for (int y = 1; y < r.h; y++)
        memcpy(row0 + (size_t)y * (size_t)st.cur_band.w, row0, (size_t)r.w * 2u);
}

void eos_display_blit(int16_t x, int16_t y, const eos_bitmap_t *b)
{
    if (!drawable() || !b || !b->pixels || b->w <= 0 || b->h <= 0) return;
    if (b->fmt == EOS_PIXFMT_RGB565 && !(st.info.caps & EOS_CAP_RGB565)) return;

    eos_rect_t r = eos_rect_isect(eos_rect(x, y, b->w, b->h), st.clip[st.clip_n - 1]);
    if (eos_rect_empty(r)) return;

    const uint8_t *src    = (const uint8_t *)b->pixels;
    int            stride = (int)eos_bitmap_stride(b);
    int            ox     = (int)r.x - (int)x;
    int            oy     = (int)r.y - (int)y;

    bool     has_tint = (b->tint != EOS_COLOR_NONE);
    bool     has_bg   = (b->bg   != EOS_COLOR_NONE);
    uint16_t tint     = has_tint ? st.lut[b->tint] : 0;
    uint16_t bg       = has_bg   ? st.lut[b->bg]   : 0;

    for (int j = 0; j < r.h; j++) {
        const uint8_t *sr  = src + (size_t)(oy + j) * (size_t)stride;
        uint16_t      *dst = band_px(r.x, (int)r.y + j);

        switch (b->fmt) {
        case EOS_PIXFMT_I8:
            for (int i = 0; i < r.w; i++) {
                uint8_t idx = sr[ox + i];
                if (idx == b->key || idx == EOS_COLOR_NONE) continue;
                dst[i] = st.lut[idx];
            }
            break;

        case EOS_PIXFMT_MONO1:
            for (int i = 0; i < r.w; i++) {
                int      sx  = ox + i;
                bool     set = (sr[sx >> 3] & (uint8_t)(0x80u >> (sx & 7))) != 0;
                if (set) { if (has_tint) dst[i] = tint; }
                else     { if (has_bg)   dst[i] = bg;   }
            }
            break;

        case EOS_PIXFMT_A8:
            if (!has_tint) break;
            for (int i = 0; i < r.w; i++) {
                uint8_t a = sr[ox + i];
                if (a == 0) { if (has_bg) dst[i] = bg; continue; }
                if (a == 255) { dst[i] = tint; continue; }
                uint16_t under = has_bg ? bg : dst[i];
                dst[i] = wire16(blend565(wire16(under), wire16(tint), a));
            }
            break;

        case EOS_PIXFMT_RGB565:
            for (int i = 0; i < r.w; i++) {
                const uint8_t *p = sr + (size_t)(ox + i) * 2u;
                // Source is little-endian 565; the panel wants it big-endian.
                dst[i] = (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
            }
            break;

        default:
            return;
        }
    }
}

// Glyph rows are MSB-first and PADDED TO WHOLE BYTES, glyphs concatenated in
// codepoint order — a glyph is therefore already an EOS_PIXFMT_MONO1 bitmap
// with stride (w+7)/8, which is why kernel/font/ emits it that way. The row
// pitch is the padded byte count, not the glyph width; getting those two
// confused silently shears every face wider than one byte.
static void glyph_draw(const eos_font_t *f, int gi, int gw,
                       int px, int py, uint16_t v, eos_rect_t cl)
{
    uint32_t pitch = (uint32_t)((gw + 7) / 8) * 8u;    // bits per glyph row
    uint32_t base  = f->offsets ? f->offsets[gi]
                                : (uint32_t)gi * pitch * (uint32_t)f->h;

    int y0 = (int)cl.y - py;                if (y0 < 0) y0 = 0;
    int y1 = (int)cl.y + (int)cl.h - py;    if (y1 > (int)f->h) y1 = (int)f->h;
    int x0 = (int)cl.x - px;                if (x0 < 0) x0 = 0;
    int x1 = (int)cl.x + (int)cl.w - px;    if (x1 > gw) x1 = gw;

    for (int row = y0; row < y1; row++) {
        uint32_t  bit = base + (uint32_t)row * pitch + (uint32_t)x0;
        uint16_t *dst = band_px(px + x0, py + row);
        for (int cx = x0; cx < x1; cx++, bit++, dst++) {
            if (f->bits[bit >> 3] & (uint8_t)(0x80u >> (bit & 7u))) *dst = v;
        }
    }
}

int eos_display_text(int16_t x, int16_t y, const eos_font_t *f,
                     eos_color_t c, const char *s, int len)
{
    if (!f || !s || !f->bits) return 0;
    if (len < 0) { len = 0; while (s[len]) len++; }

    int adv = eos_text_width(f, s, len);
    if (!drawable() || c == EOS_COLOR_NONE || len == 0) return adv;

    eos_rect_t cl = st.clip[st.clip_n - 1];
    if (eos_rect_empty(eos_rect_isect(eos_rect(x, y, (int16_t)adv, (int16_t)f->h), cl)))
        return adv;

    uint16_t v   = st.lut[c];
    int      pen = (int)x;
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        int gw = eos_font_glyph_w(f, ch);
        if (pen >= (int)cl.x + (int)cl.w) break;          // ran off the right edge
        if (pen + gw > (int)cl.x)
            glyph_draw(f, eos_font_glyph(f, ch), gw, pen, (int)y, v, cl);
        pen += gw;
        if (i + 1 < len) pen += (int)f->gap;
    }
    return adv;
}

// -------------------------------------------------------------- backlight

eos_err_t eos_display_backlight(uint8_t percent)
{
    if (!st.inited) return EOS_ERR_STATE;
    if (!eos_pin_ok(st.b->panel.bl)) return EOS_ERR_NODEV;
    if (percent > 100) return EOS_ERR_ARG;

    // No PWM on this pin means the call snaps rather than lying about dimming.
    if (!st.b->panel.bl_pwm) percent = (percent >= 50) ? 100 : 0;
    st.bl_pct = percent;

#ifdef ESP_PLATFORM
    uint32_t duty = ((uint32_t)percent * BL_MAX) / 100u;
    if (st.b->panel.bl_active_low) duty = BL_MAX - duty;
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, duty) != ESP_OK) return EOS_ERR_IO;
    if (ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL) != ESP_OK) return EOS_ERR_IO;
#endif
    return EOS_OK;
}

// ------------------------------------------------------------------- init

#ifdef ESP_PLATFORM
static eos_err_t backlight_start(const eos_board_t *b)
{
    if (!eos_pin_ok(b->panel.bl)) return EOS_OK;

    ledc_timer_config_t lt = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = BL_TIMER,
        .freq_hz         = BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
        .deconfigure     = false,
    };
    if (ledc_timer_config(&lt) != ESP_OK) return EOS_ERR_IO;

    ledc_channel_config_t lc = {
        .gpio_num   = b->panel.bl,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BL_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BL_TIMER,
        .duty       = b->panel.bl_active_low ? BL_MAX : 0,   // dark until the panel is clean
        .hpoint     = 0,
    };
    if (ledc_channel_config(&lc) != ESP_OK) return EOS_ERR_IO;
    return EOS_OK;
}

// The registry's spi_host is 1 for SPI2/HSPI and 2 for SPI3/VSPI. Single-host
// parts (the C6 among them) have no SPI3 symbol at all, so it is compiled out
// rather than tested at runtime.
static spi_host_device_t spi_host_of(const eos_board_t *b)
{
#if SOC_SPI_PERIPH_NUM > 2
    if (b->panel.spi_host == 2) return SPI3_HOST;
#endif
    (void)b;
    return SPI2_HOST;
}

// Panel bring-up, lifted from boards/waveshare-c6-lcd-13/probe/main/main.c,
// which is the only sequence known to light this glass. Do not reorder it.
static eos_err_t panel_start(const eos_board_t *b, uint32_t max_xfer)
{
    spi_bus_config_t bus = {
        .sclk_io_num     = b->panel.sck,
        .mosi_io_num     = b->panel.mosi,
        .miso_io_num     = b->panel.miso,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = (int)max_xfer,
    };
    if (spi_bus_initialize(spi_host_of(b), &bus, SPI_DMA_CH_AUTO) != ESP_OK) return EOS_ERR_IO;

    esp_lcd_panel_io_spi_config_t iocfg = {
        .dc_gpio_num         = b->panel.dc,
        .cs_gpio_num         = b->panel.cs,
        .pclk_hz             = b->panel.hz,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .spi_mode            = 0,
        .trans_queue_depth   = BUF_COUNT + 2,
        .on_color_trans_done = color_done_cb,
        .user_ctx            = NULL,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host_of(b), &iocfg, &st.io) != ESP_OK)
        return EOS_ERR_IO;

    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = b->panel.rst,
        .rgb_ele_order  = b->panel.bgr ? LCD_RGB_ELEMENT_ORDER_BGR
                                       : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(st.io, &pc, &st.panel) != ESP_OK) return EOS_ERR_IO;

    if (esp_lcd_panel_reset(st.panel) != ESP_OK) return EOS_ERR_IO;
    if (esp_lcd_panel_init(st.panel)  != ESP_OK) return EOS_ERR_IO;
    if (esp_lcd_panel_invert_color(st.panel, b->panel.invert) != ESP_OK) return EOS_ERR_IO;

    // rotation tracks how the panel is MOUNTED. 0 is the verified orientation
    // on this board; 1..3 follow the usual ST77xx mapping and are untested.
    // Each rotation pairs swap_xy with exactly one mirror, which is what makes
    // it a rotation rather than a reflection - but WHICH mirror depends on the
    // panel's default scan order, and panels disagree. The board's mirror_x and
    // mirror_y carry that difference and are XORed in, so a panel needing
    // swap_xy with no mirror at all is expressible without a fifth case.
    //
    // Getting this wrong is invisible in logs: the picture is the right size,
    // the right colours, and the right way up. Only handedness changes, so it
    // shows up as text and avatars rendering backwards.
    bool sw, mx, my;
    switch (b->panel.rotation & 3) {
    case 0:  sw = false; mx = false; my = false; break;
    case 1:  sw = true;  mx = true;  my = false; break;
    case 2:  sw = false; mx = true;  my = true;  break;
    default: sw = true;  mx = false; my = true;  break;
    }
    mx = (mx != b->panel.mirror_x);
    my = (my != b->panel.mirror_y);
    esp_lcd_panel_swap_xy(st.panel, sw);
    esp_lcd_panel_mirror(st.panel, mx, my);

    if (b->panel.col_offset || b->panel.row_offset) {
        int cx = b->panel.col_offset, cy = b->panel.row_offset;
        if (b->panel.rotation & 1) { int t = cx; cx = cy; cy = t; }
        esp_lcd_panel_set_gap(st.panel, cx, cy);
    }

    if (esp_lcd_panel_disp_on_off(st.panel, true) != ESP_OK) return EOS_ERR_IO;
    return EOS_OK;
}
#endif /* ESP_PLATFORM */

// Paints the whole panel before the backlight comes up, so the first thing a
// human sees is the theme background and not power-on GRAM noise.
static void panel_clear(uint16_t wire)
{
    int rows = (int)(st.buf_px / (uint32_t)st.info.w);
    if (rows < 1) rows = 1;
    for (uint32_t i = 0; i < st.buf_px; i++) st.buf[0][i] = wire;
    for (int y = 0; y < st.info.h; y += rows) {
        int n = (int)st.info.h - y;
        if (n > rows) n = rows;
#ifdef ESP_PLATFORM
        // Same rule as band_push(): wait only for a transfer that was queued.
        // A boot that hangs here is a board with a dark panel and no console
        // line saying why.
        if (esp_lcd_panel_draw_bitmap(st.panel, 0, y, (int)st.info.w, y + n,
                                      st.buf[0]) != ESP_OK) return;
        xSemaphoreTake(st.done, portMAX_DELAY);   // buf[0] is reused next pass
#else
        (void)n;
#endif
    }
}

eos_err_t eos_display_init(void)
{
    if (st.inited) return EOS_ERR_STATE;

    const eos_board_t *b = eos_board_get();
    if (!b) return EOS_ERR_NODEV;
    if (b->panel.panel != EOS_PANEL_ST7789) return EOS_ERR_UNSUPPORTED;
    if (b->panel.bus   != EOS_BUS_SPI)      return EOS_ERR_UNSUPPORTED;
    if (b->panel.color_depth != 16 || b->panel.wire_bytes != 2) return EOS_ERR_UNSUPPORTED;

    memset(&st, 0, sizeof(st));
    st.b = b;

    int16_t w = eos_board_screen_w(b);
    int16_t h = eos_board_screen_h(b);
    if (w <= 0 || h <= 0) return EOS_ERR_ARG;

    // Rows per strip come from the registry, which decided them once with a
    // heap measurement in hand. Shrink only if heap_budget cannot hold
    // BUF_COUNT of them; never grow past what was tested.
    int rows = (b->render.full_framebuffer || b->render.band_h <= 0)
             ? (int)h : (int)b->render.band_h;
    if (rows > (int)h) rows = (int)h;

    uint32_t budget = b->render.heap_budget;
    while (rows > 1 && (uint32_t)w * (uint32_t)rows * 2u * BUF_COUNT > budget) rows--;
    if ((uint32_t)w * (uint32_t)rows * 2u * BUF_COUNT > budget) return EOS_ERR_POOL;

    uint32_t strip_px    = (uint32_t)w * (uint32_t)rows;

#ifdef ESP_PLATFORM
    // THE ONE ALLOCATION IN THE WHOLE HAL. BUF_COUNT strips, contiguous,
    // DMA-capable because esp_lcd hands the pointer straight to the SPI engine.
    // Taken once at boot, never resized, never freed. Everything else in this
    // file draws into memory that already exists.
    uint32_t strip_bytes = strip_px * 2u;
    uint8_t *chunk = heap_caps_malloc((size_t)strip_bytes * BUF_COUNT,
                                      MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!chunk) return EOS_ERR_POOL;
    for (int i = 0; i < BUF_COUNT; i++)
        st.buf[i] = (uint16_t *)(void *)(chunk + (size_t)i * strip_bytes);

    st.done = xSemaphoreCreateCountingStatic(BUF_COUNT + 2, 0, &st.done_mem);
    if (!st.done) { heap_caps_free(chunk); return EOS_ERR_POOL; }
#else
    if (strip_px > HOST_BAND_PX) { strip_px = HOST_BAND_PX; rows = (int)(strip_px / (uint32_t)w); }
    for (int i = 0; i < BUF_COUNT; i++) st.buf[i] = host_band + (size_t)i * strip_px;
#endif

    st.buf_px = strip_px;

    st.info.w           = w;
    st.info.h           = h;
    st.info.tier        = b->tier;
    st.info.fmt         = EOS_PIXFMT_RGB565;
    st.info.palette_len = EOS_COLOR_NONE;          // 255 drawable entries
    st.info.band_h      = (int16_t)rows;
    st.info.max_bands   = (int16_t)(((int)h + rows - 1) / rows);
    st.info.caps        = EOS_CAP_RGB565 | EOS_CAP_BLEND | EOS_CAP_PALETTE;
    if (rows >= (int)h) st.info.caps |= EOS_CAP_RETAINED;
    if (eos_pin_ok(b->panel.bl)) {
        st.info.caps |= EOS_CAP_BACKLIGHT;
        if (b->panel.bl_pwm) st.info.caps |= EOS_CAP_DIM;
    }
    if (b->render.animations) st.info.caps |= EOS_CAP_ANIM;

    lut_seed_from_theme();

#ifdef ESP_PLATFORM
    eos_err_t e = backlight_start(b);
    if (e == EOS_OK) e = panel_start(b, strip_bytes + 64u);
    if (e != EOS_OK) {
        // Boot failed, so give the strip back and leave the descriptor blank
        // rather than pointing at freed DMA memory. The IDF-side handles stay
        // up; a board whose panel will not start is not going to be retried.
        heap_caps_free(chunk);
        memset(&st, 0, sizeof(st));
        return e;
    }
#endif

    st.inited = true;
    panel_clear(st.lut[st.bg_index]);
    eos_display_backlight(100);
    eos_display_damage_all();
    return EOS_OK;
}

const eos_display_info_t *eos_display_info(void)
{
    return &st.info;
}

#ifndef ESP_PLATFORM
// Host-only test seam. Off-target there is no panel, so the only way to check
// that a band composited correctly is to read the strip back out. It is inside
// the same #ifndef that gives the host its BSS strip, so no firmware image ever
// contains it.
const uint16_t *eos_display_host_band(eos_rect_t *band)
{
    if (!st.open || !st.cur_valid) return NULL;
    if (band) *band = st.cur_band;
    return st.buf[st.cur];
}
#endif
