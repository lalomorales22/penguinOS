// eos_shell_input — where key events become window-manager moves.
//
// This is the seam, and it exists now so that it does not have to be invented
// later in the middle of a BLE bring-up. Everything from the keycode to the
// relayout is written and reachable: eos_keys_feed() is called with the real
// compiled-in keymap against the real eos_wm_t, EOS_ACT_TOGGLE_BAR really
// takes the bar out of the layout, and the caller really redraws when the
// result says something moved.
//
// The source of events is kernel/hal/eos_input.c, which is the implementation
// behind eos_input.h: a 32-event ring fed by the NimBLE HID host in
// kernel/svc/eos_ble.c, by the board's GPIO buttons, and by anything the phone
// page injects. eos_shell_input_next() is exactly `eos_input_poll(out)` and
// there is deliberately no second dispatch path anywhere: a keystroke reaches
// the window manager through this function or it does not reach it at all.

#ifndef EOS_SHELL_INPUT_H
#define EOS_SHELL_INPUT_H

#include <stdint.h>
#include <stdbool.h>

#include "eos_wm.h"
#include "eos_keys.h"
#include "eos_input.h"

typedef struct {
    eos_wm_t          *wm;
    eos_shell_state_t *st;
    const eos_keymap_t *km;
    eos_rect_t         screen;

    // The theme's bar height, kept so EOS_ACT_TOGGLE_BAR can put it back.
    // Hiding the bar means taking it out of eos_wm_cfg_t, not just declining
    // to paint it: the tiles have to grow into the space or the bar is merely
    // invisible, which is a different and worse thing.
    int16_t theme_bar_h;
} eos_shell_input_t;

void eos_shell_input_init(eos_shell_input_t *in, eos_wm_t *wm,
                          eos_shell_state_t *st, const eos_keymap_t *km,
                          eos_rect_t screen, int16_t theme_bar_h);

// Pops the next input event from the HAL ring. False = queue empty.
bool eos_shell_input_next(eos_event_t *out);

// Drains the queue through eos_keys_feed(). Returns true when at least one
// event actually moved something and the screen needs a full redraw.
bool eos_shell_input_pump(eos_shell_input_t *in);

#endif // EOS_SHELL_INPUT_H
