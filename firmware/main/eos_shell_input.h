// eos_shell_input — where key events become window-manager moves.
//
// This is the seam, and it exists now so that it does not have to be invented
// later in the middle of a BLE bring-up. Everything from the keycode to the
// relayout is written and reachable: eos_keys_feed() is called with the real
// compiled-in keymap against the real eos_wm_t, EOS_ACT_TOGGLE_BAR really
// takes the bar out of the layout, and the caller really redraws when the
// result says something moved.
//
// The one thing that is NOT here is a source of events. eos_input.h declares a
// queue that nothing implements yet — no BLE HID host, no button poll — so
// eos_shell_input_next() below is the single stub in this file and it always
// reports an empty queue. It is one line away from real: when the input HAL
// lands, its body becomes `return eos_input_poll(out);` and nothing else in
// ESP-OS changes. Do not scatter a second dispatch path around it.

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

// STUB. Pops the next input event, and today there is never one because
// nothing implements eos_input.h yet. Returns false = queue empty.
bool eos_shell_input_next(eos_event_t *out);

// Drains the queue through eos_keys_feed(). Returns true when at least one
// event actually moved something and the screen needs a full redraw.
bool eos_shell_input_pump(eos_shell_input_t *in);

#endif // EOS_SHELL_INPUT_H
