// eos_shell_input — the ONE place an input event becomes a change on the glass.
//
// Every key, every character and every trackpad click on this board arrives in
// kernel/hal's 32-event ring, whichever of the six sources produced it, and
// leaves that ring here. eos_shell_input_next() is exactly `eos_input_poll()`
// and it has exactly one caller: eos_shell_input_pump(). A keystroke reaches
// the window manager through this function or it does not reach it at all, and
// the same is now true of a click — before this the frame loop had a second
// drain of its own for the passes where the desktop was not the visible scene,
// which meant two consumers of one queue and two different opinions about what
// an event off the desktop means.
//
// THE PRECEDENCE, in order, and it is the whole design of this file:
//
//   0. Arrival order. One ring, one pass, one event at a time. Keys and
//      pointer motion interleave exactly as the devices produced them, so a
//      click that happened after a keystroke is dispatched after it.
//   1. INACTIVE. The setup screen and the passkey screen are not the desktop
//      and have nothing an event can move, so while eos_shell_input_set_active()
//      says false every event is drained and dropped. Drained, not left: a
//      keyboard that bonded during setup would otherwise deliver its first
//      thirty-two keys the instant the desktop appeared. Dropped rather than
//      dispatched: a super+q behind a screen the user cannot see closes a
//      window they never asked to close.
//   2. LOCKED. eos_shell_state_t.locked swallows everything but the lock chord
//      itself, and it swallows the pointer too. A lock a click can focus
//      through is not a lock.
//   3. THE LAUNCHER, while it is open. It sees keys before the keymap does and
//      pointer events instead of the tiles: an overlay a click can reach
//      through is not an overlay. It hands back every SUPER chord and every
//      key it does not bind, which is what keeps super+space closing it and
//      super+q closing a window from inside it.
//   4. THE KEYMAP. The global chords. They beat the focused window and that
//      order is not negotiable: an app that decided it wanted the letter q
//      would otherwise stop super+q from closing anything, and a desktop you
//      cannot escape from the focused window is the failure mode this rule
//      exists to prevent. A chord that a bind CLAIMED is consumed here whether
//      or not it moved anything — super+left at the left edge moves no focus
//      and must still not arrive in the chat window as a left arrow.
//   5. THE FOCUSED WINDOW. Whatever is left: printable characters, and the
//      keys no bind claimed. eos_app_key() offers them to the app owning the
//      focused tile.
//   6. Dropped.
//
// The one non-obvious constraint: an app's answer never sets `moved`. `moved`
// means the LAYOUT changed and the whole screen has to be repainted, which is
// the expensive answer on a banded backend. An app that took an arrow key has
// changed one tile and says so through its own dirty flag, which eos_app_damage()
// turns into a single tile-sized rect.

#ifndef EOS_SHELL_INPUT_H
#define EOS_SHELL_INPUT_H

#include <stdint.h>
#include <stdbool.h>

#include "eos_wm.h"
#include "eos_keys.h"
#include "eos_input.h"
#include "eos_launcher.h"
#include "eos_pointer.h"

typedef struct {
    eos_wm_t          *wm;
    eos_shell_state_t *st;
    const eos_keymap_t *km;
    eos_rect_t         screen;

    // The app list. NULL is a legal board: super+space still toggles
    // eos_shell_state_t.launcher_open, nothing is drawn, and every key still
    // reaches the keymap. With one attached the pump becomes a two-stage
    // dispatch — see eos_shell_input_pump() for the order and why it is that
    // way round.
    eos_launcher_t    *launcher;

    // The theme's bar height, kept so EOS_ACT_TOGGLE_BAR can put it back.
    // Hiding the bar means taking it out of eos_wm_cfg_t, not just declining
    // to paint it: the tiles have to grow into the space or the bar is merely
    // invisible, which is a different and worse thing.
    int16_t theme_bar_h;

    // What the chrome does to a tile rect, so a click can find the close box
    // in a tile's header. Zeroed by init(), which means no close box at all
    // until eos_shell_input_chrome() has been called — a board that never
    // calls it clicks exactly the way it did before there was one.
    eos_pointer_chrome_t chrome;

    // False while some other scene owns the panel. See rule 1 above.
    bool active;
} eos_shell_input_t;

void eos_shell_input_init(eos_shell_input_t *in, eos_wm_t *wm,
                          eos_shell_state_t *st, const eos_keymap_t *km,
                          eos_rect_t screen, int16_t theme_bar_h);

// Attaches the launcher the pump routes to. Separate from init() so that a
// board without one, and every caller written before there was one, keeps
// working unchanged. Pass NULL to detach.
void eos_shell_input_launcher(eos_shell_input_t *in, eos_launcher_t *l);

// The tile chrome a click is tested against, from eos_shell_tile_chrome().
// Set it at boot and again after a theme change, for the same reason the
// launcher's geometry is recomputed then: a theme may name a different face,
// and a close box measured on the old face's grid is a close box in the wrong
// place. NULL clears it, which disables the close box and nothing else.
void eos_shell_input_chrome(eos_shell_input_t *in, const eos_pointer_chrome_t *ch);

// Whether the desktop is the scene on the panel. See rule 1 in the header
// comment: false does not stop the pump being called, it changes what the pump
// does with what it finds. init() leaves it false, so a caller that never sets
// it drains and drops, which is the safe half of the rule.
void eos_shell_input_set_active(eos_shell_input_t *in, bool active);

// Pops the next input event from the HAL ring. False = queue empty. There is
// exactly one caller and it is eos_shell_input_pump(); anything else that pops
// this ring is a second dispatcher, which is the thing this file exists to
// prevent.
bool eos_shell_input_next(eos_event_t *out);

// Drains the queue down the ladder in the header comment. Returns true when at
// least one event moved the LAYOUT and the screen needs a full redraw. Call it
// on every pass of the frame loop, active or not: an inactive pump is what
// throws the queue away, and skipping the call is what lets it pile up.
bool eos_shell_input_pump(eos_shell_input_t *in);

#endif // EOS_SHELL_INPUT_H
