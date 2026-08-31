// The WS2812 and its six effects. See eos_led.h for why the colour function is
// pure and why the peripheral is touched in exactly one place.

#include "eos_led.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "driver/rmt_tx.h"
#include "esp_log.h"
static const char *TAG = "eos_led";
#endif

static const char *const FX_NAME[EOS_LED_FX_COUNT] = {
    "off", "solid", "breathe", "rainbow", "strobe", "sparkle"
};

const char *eos_led_fx_name(int fx)
{
    if (fx < 0 || fx >= (int)EOS_LED_FX_COUNT) return "?";
    return FX_NAME[fx];
}

bool eos_led_fx_animated(int fx)
{
    return fx == EOS_LED_FX_BREATHE || fx == EOS_LED_FX_RAINBOW ||
           fx == EOS_LED_FX_STROBE  || fx == EOS_LED_FX_SPARKLE;
}

// ----------------------------------------------------------------- colour

void eos_led_hsv(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    // Six 43-wide sectors over the 256-step wheel. The last sector is 41 steps
    // rather than 43 because 256 does not divide by six, which puts a two-step
    // wobble at the top of magenta that nothing can see on a 5 mm LED and that
    // a float would only hide behind a rounding error somewhere else.
    unsigned sector = (unsigned)h / 43u;
    unsigned within = ((unsigned)h - sector * 43u) * 255u / 42u;
    unsigned p = (unsigned)v * (255u - s) / 255u;
    unsigned q = (unsigned)v * (255u - (unsigned)s * within / 255u) / 255u;
    unsigned t = (unsigned)v * (255u - (unsigned)s * (255u - within) / 255u) / 255u;
    unsigned rr, gg, bb;

    switch (sector) {
    case 0:  rr = v; gg = t; bb = p; break;
    case 1:  rr = q; gg = v; bb = p; break;
    case 2:  rr = p; gg = v; bb = t; break;
    case 3:  rr = p; gg = q; bb = v; break;
    case 4:  rr = t; gg = p; bb = v; break;
    default: rr = v; gg = p; bb = q; break;
    }
    if (r) *r = (uint8_t)rr;
    if (g) *g = (uint8_t)gg;
    if (b) *b = (uint8_t)bb;
}

// A hash, not a random number generator. SPARKLE has to be a pure function of
// the clock or the panel could not draw the colour the LED is showing, so the
// flicker comes from stirring the bits of the time rather than from a state
// that only the driver would hold.
static uint32_t stir(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// A triangle wave over `period` scaled to 0..255. Used by BREATHE, and kept
// separate because the ease below reads better than a fold inline.
static unsigned triangle(uint32_t now_ms, uint32_t period)
{
    uint32_t t = period ? (now_ms % period) : 0u;
    uint32_t half = period / 2u;
    if (half == 0u) return 255u;
    if (t < half) return (unsigned)(t * 255u / half);
    return (unsigned)((period - t) * 255u / half);
}

void eos_led_color_at(const eos_led_state_t *st, uint32_t now_ms,
                      uint8_t *r, uint8_t *g, uint8_t *b)
{
    unsigned v;
    uint8_t hue;

    if (r) *r = 0;
    if (g) *g = 0;
    if (b) *b = 0;
    if (!st) return;

    hue = st->hue;
    v   = st->bright;

    switch (st->fx) {
    case EOS_LED_FX_OFF:
        return;

    case EOS_LED_FX_SOLID:
        break;

    case EOS_LED_FX_BREATHE: {
        // A tenth to full over four seconds, squared so the dark half lasts
        // longer than the bright one. A linear ramp on an LED reads as a saw.
        unsigned e = triangle(now_ms, 4000u);
        e = 26u + (e * e / 255u) * 229u / 255u;
        v = v * e / 255u;
        break;
    }

    case EOS_LED_FX_RAINBOW:
        hue = (uint8_t)((now_ms * 256u / 6000u) & 0xFFu);
        break;

    case EOS_LED_FX_STROBE:
        // 60 ms of every 240. Short enough to read as a flash and slow enough
        // that a phone camera still catches it.
        if ((now_ms % 240u) >= 60u) v = 0;
        break;

    case EOS_LED_FX_SPARKLE: {
        // One new level every 80 ms, and the hue wanders with it. The floor is
        // 24 rather than 0 so the light never looks broken.
        uint32_t h = stir(now_ms / 80u);
        v   = v * (24u + (h & 0xFFu) * 231u / 255u) / 255u;
        hue = (uint8_t)(hue + ((h >> 8) & 0x3Fu) - 32u);
        break;
    }

    default:
        return;
    }

    eos_led_hsv(hue, st->sat, (uint8_t)(v > 255u ? 255u : v), r, g, b);
}

// ------------------------------------------------------------------ state

static eos_led_state_t s_state = { EOS_LED_FX_OFF, 0, 255, 128 };
static bool     s_present;
static bool     s_inited;
static uint32_t s_frames;
static uint8_t  s_last[3];
static bool     s_have_last;

bool eos_led_present(void) { return s_present; }

void eos_led_set(const eos_led_state_t *st)
{
    if (!st) return;
    s_state = *st;
    if (s_state.fx >= (uint8_t)EOS_LED_FX_COUNT) s_state.fx = EOS_LED_FX_OFF;
}

void eos_led_get(eos_led_state_t *out)
{
    if (out) *out = s_state;
}

uint32_t eos_led_frames(void) { return s_frames; }

void eos_led_last(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (r) *r = s_last[0];
    if (g) *g = s_last[1];
    if (b) *b = s_last[2];
}

// ----------------------------------------------------------------- the pin

#ifdef ESP_PLATFORM

// 10 MHz, so one tick is 100 ns and every WS2812 interval below is an exact
// integer. A faster clock would only buy resolution the part cannot use.
#define LED_RES_HZ 10000000u

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;

static eos_err_t led_open(eos_pin_t pin)
{
    rmt_tx_channel_config_t cc;
    rmt_bytes_encoder_config_t ec;

    memset(&cc, 0, sizeof cc);
    cc.gpio_num          = (int)pin;
    cc.clk_src           = RMT_CLK_SRC_DEFAULT;
    cc.resolution_hz     = LED_RES_HZ;
    cc.mem_block_symbols = 64;   // 24 bits fits; nothing is ever queued behind it
    cc.trans_queue_depth = 2;
    if (rmt_new_tx_channel(&cc, &s_chan) != ESP_OK) return EOS_ERR_IO;

    // WS2812B: a zero is 0.3 us high then 0.9 us low, a one is 0.9 then 0.3.
    memset(&ec, 0, sizeof ec);
    ec.bit0.level0 = 1; ec.bit0.duration0 = 3;
    ec.bit0.level1 = 0; ec.bit0.duration1 = 9;
    ec.bit1.level0 = 1; ec.bit1.duration0 = 9;
    ec.bit1.level1 = 0; ec.bit1.duration1 = 3;
    ec.flags.msb_first = 1;
    if (rmt_new_bytes_encoder(&ec, &s_enc) != ESP_OK) {
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return EOS_ERR_IO;
    }
    if (rmt_enable(s_chan) != ESP_OK) {
        rmt_del_encoder(s_enc);
        rmt_del_channel(s_chan);
        s_enc = NULL; s_chan = NULL;
        return EOS_ERR_IO;
    }
    return EOS_OK;
}

static bool led_write(uint8_t r, uint8_t g, uint8_t b)
{
    rmt_transmit_config_t tc;
    uint8_t grb[3];

    if (!s_chan || !s_enc) return false;
    grb[0] = g; grb[1] = r; grb[2] = b;   // WS2812 wants green first

    // Before, not after. The reset gap that latches the frame is the silence
    // that follows it, and a second transmit inside that gap would be shifted
    // into the same part as bits 25..48.
    rmt_tx_wait_all_done(s_chan, 100);

    memset(&tc, 0, sizeof tc);
    tc.loop_count = 0;
    return rmt_transmit(s_chan, s_enc, grb, sizeof grb, &tc) == ESP_OK;
}

#else   /* host */

static eos_err_t led_open(eos_pin_t pin) { (void)pin; return EOS_OK; }
static bool led_write(uint8_t r, uint8_t g, uint8_t b)
{
    (void)r; (void)g; (void)b;
    return true;
}

#endif

eos_err_t eos_led_init(const eos_board_t *b)
{
    eos_err_t e;

    if (s_inited) return s_present ? EOS_OK : EOS_ERR_NODEV;
    s_inited = true;

    if (!b || b->extras.led != EOS_LED_WS2812 || b->extras.led_data == EOS_PIN_NONE)
        return EOS_ERR_NODEV;

    e = led_open(b->extras.led_data);
    if (e != EOS_OK) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "no RMT channel for the WS2812 on GPIO%d - Media has no light",
                 (int)b->extras.led_data);
#endif
        return e;
    }
    s_present = true;

    // Dark, explicitly. A WS2812 comes out of reset holding whatever noise the
    // data line fed it, and a board that boots with a random bright pixel next
    // to the panel reads as a fault.
    led_write(0, 0, 0);
    s_have_last = true;
    s_last[0] = s_last[1] = s_last[2] = 0;
    s_frames = 1;
    return EOS_OK;
}

bool eos_led_tick(uint32_t now_ms)
{
    uint8_t r, g, b;

    if (!s_present) return false;
    eos_led_color_at(&s_state, now_ms, &r, &g, &b);

    if (s_have_last && r == s_last[0] && g == s_last[1] && b == s_last[2])
        return false;
    if (!led_write(r, g, b)) return false;

    s_last[0] = r; s_last[1] = g; s_last[2] = b;
    s_have_last = true;
    s_frames++;
    return true;
}
