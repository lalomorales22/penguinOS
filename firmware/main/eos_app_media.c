// eos_app_media — the light panel. Colour, brightness and six effects on the
// single WS2812 the board descriptor has always declared on GPIO8.
//
// It is called "media" in the app table because that is the name the owner
// asked for, and its label on the glass is "light" and its summary says "no
// audio on this board" in its first line. That is not pedantry: this board has
// no DAC, no I2S codec and no speaker, and a window called Media that a person
// opens expecting sound is a promise the silicon cannot keep. Naming it for
// what it drives is cheaper than explaining it later.
//
// The one non-obvious constraint: the swatch this window paints and the colour
// the LED is actually showing come from the SAME pure function,
// eos_led_color_at(). The window does not read the LED back and the driver
// does not ask the window what to send — both evaluate one expression over the
// state and the clock. That is what makes a strobe on the glass and a strobe
// on the LED the same strobe, and it is why the draw can be replayed once per
// display band without the colour moving between bands.

#include "eos_app_registry.h"
#include "eos_shell_draw.h"
#include "eos_led.h"

#include <stdio.h>
#include <string.h>

#include "eos_font.h"
#include "eos_theme.h"

// One key press of hue is a sixteenth of the wheel and one of brightness is a
// sixteenth of the range: sixteen presses to go all the way round or all the
// way up, which is a reasonable number to hold an arrow down for.
#define HUE_STEP    16
#define BRIGHT_STEP 16

static bool s_changed = true;

// The colour the swatch shows, resolved through the theme's own 6x8x4 cube so
// that it lands on a palette index the panel definitely has. It is a nearest
// match, so a deep blue LED shows as the nearest blue the theme's cube can
// name and not as the exact one — which is the honest thing for an 8-bit
// indexed panel to draw.
static eos_color_t swatch_index(uint8_t r, uint8_t g, uint8_t b)
{
    eos_rgb_t c;
    c.r = r; c.g = g; c.b = b;
    return eos_theme_cube_index(c);
}

bool eos_app_media_take_dirty(void)
{
    eos_led_state_t st;
    bool d = s_changed;

    s_changed = false;
    eos_led_get(&st);
    // An animated effect is dirty on every pass by definition: the swatch is
    // the light, and the light is moving. A tile is 110x76, so repainting it
    // at loop rate is about eight kilobytes of SPI and not a full-width band.
    return d || eos_led_fx_animated(st.fx);
}

bool eos_app_media_key(const eos_event_t *e)
{
    eos_led_state_t st;

    if (!e) return false;
    if (e->type != EOS_EV_KEY_DOWN && e->type != EOS_EV_KEY_REPEAT) return false;

    eos_led_get(&st);

    switch (e->key) {
    case EOS_KEY_LEFT:  st.hue = (uint8_t)(st.hue - HUE_STEP); break;
    case EOS_KEY_RIGHT: st.hue = (uint8_t)(st.hue + HUE_STEP); break;

    case EOS_KEY_UP:
        st.bright = (uint8_t)(st.bright > 255 - BRIGHT_STEP ? 255
                                                            : st.bright + BRIGHT_STEP);
        // Turning the brightness up on a light that is off means turning it
        // on. Anything else leaves the owner pressing up at a dark LED.
        if (st.fx == EOS_LED_FX_OFF) st.fx = EOS_LED_FX_SOLID;
        break;

    case EOS_KEY_DOWN:
        st.bright = (uint8_t)(st.bright < BRIGHT_STEP ? 0 : st.bright - BRIGHT_STEP);
        break;

    case EOS_KEY_ENTER:
    case EOS_KEY_SPACE:
        st.fx = (uint8_t)((st.fx + 1) % (uint8_t)EOS_LED_FX_COUNT);
        if (st.fx != EOS_LED_FX_OFF && st.bright == 0) st.bright = 128;
        break;

    case EOS_KEY_ESC:
        st.fx = EOS_LED_FX_OFF;
        break;

    default:
        return false;
    }

    eos_led_set(&st);
    s_changed = true;
    return true;
}

// ------------------------------------------------------------------- draw

void eos_app_draw_media(const eos_app_ctx_t *c, eos_rect_t r)
{
    eos_led_state_t st;
    uint8_t cr, cg, cb;
    int16_t line_h, y, sw, bar_w, fill;
    char buf[32];

    if (!c->ui || eos_rect_empty(r)) return;
    line_h = (int16_t)(c->ui->h + 1);

    if (!eos_led_present()) {
        eos_app_text(r.x, r.y, c->ui, c->muted, "no LED on this board", r.w);
        return;
    }

    eos_led_get(&st);
    // The clock is the view's, not a counter of this function's own: the scene
    // is replayed once per band and a phase that advanced per call would put a
    // different colour in each strip of the same frame.
    eos_led_color_at(&st, c->view->uptime_ms, &cr, &cg, &cb);

    // The swatch: a square as tall as two text lines, on the left, with the
    // effect name and the numbers beside it. It is a fill and not a gradient
    // because one palette index is all this panel can promise.
    sw = (int16_t)(2 * line_h);
    if (sw > r.w / 3) sw = (int16_t)(r.w / 3);
    if (sw > r.h)     sw = r.h;
    if (sw > 4) {
        eos_display_fill(eos_rect(r.x, r.y, sw, sw), swatch_index(cr, cg, cb));
        eos_display_border(eos_rect(r.x, r.y, sw, sw), 1, c->bunf);
    }

    y = r.y;
    eos_app_text((int16_t)(r.x + sw + 3), y, c->ui, c->accent,
                 eos_led_fx_name(st.fx), (int16_t)(r.w - sw - 3));
    y = (int16_t)(y + line_h);
    if (y + (int16_t)c->ui->h <= r.y + r.h) {
        snprintf(buf, sizeof buf, "hue %u", (unsigned)st.hue);
        eos_app_text((int16_t)(r.x + sw + 3), y, c->ui, c->muted, buf,
                     (int16_t)(r.w - sw - 3));
    }

    // The brightness bar, full width under the swatch. A number alone does not
    // read as a level; a bar does, and it costs two fills.
    y = (int16_t)(r.y + sw + 3);
    if (y + line_h <= r.y + r.h) {
        bar_w = r.w;
        fill  = (int16_t)((int32_t)bar_w * st.bright / 255);
        eos_display_fill(eos_rect(r.x, y, bar_w, (int16_t)(line_h - 2)), c->bunf);
        if (fill > 0)
            eos_display_fill(eos_rect(r.x, y, fill, (int16_t)(line_h - 2)), c->accent);
        y = (int16_t)(y + line_h);
    }

    if (y + (int16_t)c->ui->h <= r.y + r.h) {
        snprintf(buf, sizeof buf, "bright %u%%",
                 (unsigned)(((unsigned)st.bright * 100u + 127u) / 255u));
        eos_app_text(r.x, y, c->ui, c->text, buf, r.w);
        y = (int16_t)(y + line_h);
    }

    // The keys, last, and only when there is a row left for them. They are the
    // whole interface: with no keyboard bonded this window is a readout, and
    // saying which keys would move it is how the owner knows the window is
    // waiting rather than broken.
    if (y + (int16_t)c->ui->h <= r.y + r.h)
        eos_app_text(r.x, y, c->ui, c->muted, "arrows: hue, level", r.w);
    y = (int16_t)(y + line_h);
    if (y + (int16_t)c->ui->h <= r.y + r.h)
        eos_app_text(r.x, y, c->ui, c->muted, "enter: next effect", r.w);
    y = (int16_t)(y + line_h);
    if (y + (int16_t)c->ui->h <= r.y + r.h)
        eos_app_text(r.x, y, c->ui, c->muted, "light only. no audio.", r.w);
}
