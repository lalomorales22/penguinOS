// eos_led — the single WS2812 on GPIO8, and the six effects that drive it.
//
// The board descriptor has said `led = EOS_LED_WS2812, led_data = 8` since the
// header was generated and nothing has ever lit it. This file is the driver
// and, more usefully, the effect engine behind the Media window: hue,
// brightness, and an effect chosen from a list, resolved to one RGB triple.
//
// The split is the point. eos_led_color_at() is a PURE function of the state
// and a millisecond clock — no statics, no random, no hardware — so the Media
// and Party windows can draw the exact colour the LED is showing without
// reading the LED back, the host suite can check a whole sweep of an effect
// without an ESP32, and a scene replayed once per band cannot make the light
// flicker. eos_led_tick() is the only thing that touches the RMT peripheral
// and it is main-loop only.
//
// The one non-obvious constraint: the transmit is fire-and-forget into a
// hardware channel with a 64-symbol memory block, and one WS2812 is 24 bits.
// The whole frame fits in the block, so eos_led_tick() never blocks — but it
// also never queues a second frame while one is in flight, because two
// transmits inside the 50 us reset window would be read as one 48-bit frame by
// a strip that does not exist. Hence the wait before the write, not after.
//
// Off target every function here still exists and does everything except the
// last step, so the effect engine compiles and is checked on the host.

#ifndef EOS_LED_H
#define EOS_LED_H

#include <stdint.h>
#include <stdbool.h>

#include "eos_board.h"

typedef enum {
    EOS_LED_FX_OFF = 0,   // dark, and the only state that survives a reboot
    EOS_LED_FX_SOLID,     // the chosen hue at the chosen brightness
    EOS_LED_FX_BREATHE,   // that hue, easing between a tenth and full, 4 s
    EOS_LED_FX_RAINBOW,   // the whole wheel, 6 s a lap
    EOS_LED_FX_STROBE,    // on for 60 ms of every 240. The party one.
    EOS_LED_FX_SPARKLE,   // random-looking flicker from a hash of the clock
    EOS_LED_FX_COUNT
} eos_led_fx_t;

const char *eos_led_fx_name(int fx);

// What the light is being asked for. Copied, never borrowed.
typedef struct {
    uint8_t fx;      // eos_led_fx_t
    uint8_t hue;     // 0..255 around the wheel; 0 is red
    uint8_t sat;     // 0..255
    uint8_t bright;  // 0..255, applied after the effect's own envelope
} eos_led_state_t;

// Hue/saturation/value to 8-bit RGB, integer, no division in the inner path.
// Exposed because the Media window paints its own swatch with it and a second
// implementation would drift from the light.
void eos_led_hsv(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b);

// The colour the LED shows at `now_ms` for this state. Pure: same inputs, same
// answer, on any core at any time. This is what the panel draws and what the
// driver writes, and they cannot disagree because there is one of it.
void eos_led_color_at(const eos_led_state_t *st, uint32_t now_ms,
                      uint8_t *r, uint8_t *g, uint8_t *b);

// True while the effect's colour depends on the clock, which is what tells the
// windows showing it that they have to redraw and the loop that it has to keep
// ticking. SOLID and OFF are false and cost nothing once they have landed.
bool eos_led_fx_animated(int fx);

// Claims the RMT channel the board's led_data pin implies. Safe to call more
// than once; the second call is a no-op that returns the first result. Returns
// EOS_ERR_NODEV on a board that declares no WS2812, which is not a failure —
// every call below then does nothing and the Media window says the board has
// no light rather than pretending.
eos_err_t eos_led_init(const eos_board_t *b);

bool eos_led_present(void);

// The live state. set() takes effect on the next tick; get() is what the
// windows draw from.
void eos_led_set(const eos_led_state_t *st);
void eos_led_get(eos_led_state_t *out);

// Writes the colour for `now_ms` if it differs from the one already on the
// wire. Main loop only, never from a handler, never from a draw. Returns true
// when it actually transmitted, which is the number the Party window's cost
// line is measured from.
bool eos_led_tick(uint32_t now_ms);

// Frames pushed since init, and the last colour written. For the boot log and
// for the host suite; nothing draws from these.
uint32_t eos_led_frames(void);
void     eos_led_last(uint8_t *r, uint8_t *g, uint8_t *b);

#endif // EOS_LED_H
