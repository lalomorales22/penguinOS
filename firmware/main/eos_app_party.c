// eos_app_party — the ten-second demo. Pip dancing, the LED cycling, the
// colours moving. The window you open when somebody asks what the thing does.
//
// The owner asked for a Party window without saying what one is, so this is
// the reading: the tile that shows off everything the board can already do at
// once. It drives three things and invents nothing. It puts the avatar into
// HAPPY, which is a real state of a real state machine and shows up in the
// buddy window and in the status bar's mood glyph. It takes the WS2812 and
// runs it round the wheel. And it paints an equaliser whose bars are a pure
// function of elapsed time, coloured through the theme's own 6x8x4 cube so it
// looks like this theme having a party rather than like a different program.
//
// The non-obvious constraint, and the reason `phase` exists: the scene is
// replayed once per display band and every replay must produce identical
// pixels. Nothing in the draw may advance anything. So the tick — which runs
// exactly once per pass of the OS loop — stamps how long the party has been
// visible, and every moving thing in the window is a function of that one
// number. Six bands read the same phase and draw the same bars.
//
// It also gives everything back. The LED state the Media window was holding is
// saved on the way in and restored on the way out, so opening Party and
// closing it again does not silently rewrite somebody's lighting.
//
// The cost is in the report at the bottom of this file's damage path: the
// window is dirty on every pass it is visible, which is one tile-sized damage
// rect. On a 114x91 tile that is about 20 KB of SPI per frame at the loop's
// 10 Hz buddy rate — the same order as the avatar, which is already paid for.

#include "eos_app_registry.h"
#include "eos_shell_draw.h"
#include "eos_led.h"

#include <stdio.h>
#include <string.h>

#include "eos_font.h"
#include "eos_theme.h"

// Seven bars. Enough to read as an equaliser at 110 pixels wide, few enough
// that each one is still at least twelve pixels and not a hairline.
#define PARTY_BARS 7

static struct {
    bool     active;
    uint32_t start_ms;
    uint32_t phase_ms;
    eos_led_state_t saved;   // what the Media window was holding
    bool     saved_valid;
    uint32_t mood_ms;        // when HAPPY was last renewed
} P;

bool     eos_app_party_active(void) { return P.active; }
uint32_t eos_app_party_phase(void)  { return P.phase_ms; }

// HAPPY lapses back to IDLE after 1400 ms inside eos_buddy, which is correct
// for a mood that a megabrain reply caused and wrong for a party. Renewing it
// just inside that window keeps Pip grinning for as long as the tile is up and
// leaves the state machine exactly as it was the moment the tile goes away.
#define MOOD_RENEW_MS 1200

void eos_app_party_tick(bool visible, uint32_t now_ms, eos_buddy_t *buddy)
{
    if (visible && !P.active) {
        eos_led_state_t st;

        eos_led_get(&st);
        P.saved = st;
        P.saved_valid = true;

        st.fx     = EOS_LED_FX_RAINBOW;
        st.sat    = 255;
        st.bright = 200;   // not 255: this LED sits next to the panel you are
                           // reading, and full white through a diffuser is glare
        eos_led_set(&st);

        P.active   = true;
        P.start_ms = now_ms;
        P.mood_ms  = 0;
    } else if (!visible && P.active) {
        if (P.saved_valid) eos_led_set(&P.saved);
        P.active = false;
        P.phase_ms = 0;
        return;
    }

    if (!P.active) return;

    P.phase_ms = now_ms - P.start_ms;

    if (buddy && (P.mood_ms == 0 || (uint32_t)(now_ms - P.mood_ms) >= MOOD_RENEW_MS)) {
        eos_buddy_set_state(buddy, EOS_BUDDY_HAPPY);
        P.mood_ms = now_ms ? now_ms : 1u;
    }
}

bool eos_app_party_key(const eos_event_t *e)
{
    // The party takes no keys and says so, which matters: returning false here
    // is what lets a key pressed over this tile fall through to nothing rather
    // than being eaten by a window that had no use for it.
    (void)e;
    return false;
}

// ------------------------------------------------------------------- draw

// A bar's height, 0..255, as a pure function of the phase and which bar it is.
// Three sine-ish triangles at different rates beaten together: it is not
// music, it does not pretend to be, and it never repeats visibly inside the
// ten seconds anybody watches it for.
static unsigned bar_level(unsigned i, uint32_t phase)
{
    uint32_t a = (phase / 3u + i * 137u) % 512u;
    uint32_t b = (phase / 7u + i * 311u) % 512u;
    unsigned ta = (unsigned)(a < 256u ? a : 511u - a);
    unsigned tb = (unsigned)(b < 256u ? b : 511u - b);
    unsigned v  = (ta * 2u + tb) / 3u;
    return v > 255u ? 255u : v;
}

void eos_app_draw_party(const eos_app_ctx_t *c, eos_rect_t r)
{
    uint32_t phase = P.active ? P.phase_ms : 0u;
    int16_t line_h, y, bw, bx, base_y, bar_h;
    uint8_t cr, cg, cb;
    eos_led_state_t st;
    eos_rgb_t rgb;
    char buf[32];
    int i;

    if (!c->ui || eos_rect_empty(r)) return;
    line_h = (int16_t)(c->ui->h + 1);

    // Two lines is the floor: the title and one sentence. Below that there is
    // nothing a demo can demonstrate, so it says so rather than drawing three
    // pixels of an equaliser.
    if (r.h < 3 * line_h || r.w < 8 * (int16_t)c->ui->cell_w) {
        eos_app_text(r.x, r.y, c->ui, c->accent, "party", r.w);
        if (r.h >= 2 * line_h)
            eos_app_text(r.x, (int16_t)(r.y + line_h), c->ui, c->muted,
                         "needs room", r.w);
        return;
    }

    // The title, cycling through the wheel a step every 40 ms. It is drawn
    // through the theme's cube rather than through a role, which is the whole
    // "the theme shifting" half of the demo: the colour is not in the palette
    // the theme named, it is the nearest thing the theme's cube can say.
    eos_led_hsv((uint8_t)((phase / 40u) & 0xFFu), 255, 255, &cr, &cg, &cb);
    rgb.r = cr; rgb.g = cg; rgb.b = cb;
    eos_app_text(r.x, r.y, c->ui, eos_theme_cube_index(rgb), "PARTY", r.w);

    // The equaliser fills everything between the title and the bottom two
    // lines. The bars grow UP from a fixed baseline, so a tall tile gets tall
    // bars and a short one gets short ones without the layout moving.
    y      = (int16_t)(r.y + line_h);
    base_y = (int16_t)(r.y + r.h - 2 * line_h);
    bar_h  = (int16_t)(base_y - y);
    bw     = (int16_t)(r.w / PARTY_BARS);

    if (bar_h > 4 && bw >= 3) {
        for (i = 0; i < PARTY_BARS; i++) {
            unsigned lvl = bar_level((unsigned)i, phase);
            int16_t h = (int16_t)((int32_t)bar_h * (int32_t)lvl / 255);
            if (h < 1) h = 1;
            bx = (int16_t)(r.x + i * bw);
            eos_led_hsv((uint8_t)((phase / 24u + (uint32_t)i * 30u) & 0xFFu),
                        255, 255, &cr, &cg, &cb);
            rgb.r = cr; rgb.g = cg; rgb.b = cb;
            eos_display_fill(eos_rect(bx, (int16_t)(base_y - h),
                                      (int16_t)(bw - 1), h),
                             eos_theme_cube_index(rgb));
        }
    }

    // The two lines at the bottom say what is really happening, because the
    // interesting half of this window is not in this window: Pip is grinning
    // in the buddy tile and the LED next to the panel is going round.
    y = base_y;
    eos_led_get(&st);
    if (eos_led_present()) {
        eos_led_color_at(&st, c->view->uptime_ms, &cr, &cg, &cb);
        rgb.r = cr; rgb.g = cg; rgb.b = cb;
        eos_display_fill(eos_rect(r.x, y, (int16_t)(c->ui->h), (int16_t)(c->ui->h)),
                         eos_theme_cube_index(rgb));
        snprintf(buf, sizeof buf, " led %s", eos_led_fx_name(st.fx));
        eos_app_text((int16_t)(r.x + c->ui->h), y, c->ui, c->muted, buf,
                     (int16_t)(r.w - c->ui->h));
    } else {
        eos_app_text(r.x, y, c->ui, c->muted, "no LED on this board", r.w);
    }

    y = (int16_t)(y + line_h);
    if (y + (int16_t)c->ui->h <= r.y + r.h) {
        if (P.active) {
            snprintf(buf, sizeof buf, "pip is dancing  %u.%us",
                     (unsigned)(phase / 1000u), (unsigned)((phase / 100u) % 10u));
            eos_app_text(r.x, y, c->ui, c->accent, buf, r.w);
        } else {
            eos_app_text(r.x, y, c->ui, c->muted, "focus me to start", r.w);
        }
    }
}
