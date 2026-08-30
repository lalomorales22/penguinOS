// Key event to window-manager move. See eos_shell_input.h for what is real
// here and for the one function that is a stub.

#include "eos_shell_input.h"

#include <string.h>

void eos_shell_input_init(eos_shell_input_t *in, eos_wm_t *wm,
                          eos_shell_state_t *st, const eos_keymap_t *km,
                          eos_rect_t screen, int16_t theme_bar_h)
{
    if (!in) return;
    memset(in, 0, sizeof(*in));
    in->wm          = wm;
    in->st          = st;
    in->km          = km;
    in->screen      = screen;
    in->theme_bar_h = theme_bar_h;
}

bool eos_shell_input_next(eos_event_t *out)
{
    // THE STUB. eos_input_init(), eos_input_tick() and eos_input_poll() are
    // declared in kernel/hal/include/eos_input.h and implemented nowhere, so
    // this board has no keyboard, no buttons and no touch. When the HAL lands,
    // this whole body becomes:
    //
    //     return eos_input_poll(out);
    //
    // and the dispatch below starts receiving without moving.
    (void)out;
    return false;
}

bool eos_shell_input_pump(eos_shell_input_t *in)
{
    eos_event_t ev;
    bool moved = false;

    if (!in || !in->wm || !in->st || !in->km) return false;

    while (eos_shell_input_next(&ev)) {
        eos_key_result_t r;

        // Only presses and repeats reach the keymap. A key-up never fires a
        // bind — holding super+1 must not switch workspace twice — and TEXT,
        // touch and connect events belong to whatever owns focus, which on a
        // board with no apps running is nothing.
        if (ev.type != EOS_EV_KEY_DOWN && ev.type != EOS_EV_KEY_REPEAT) continue;

        r = eos_keys_feed(in->km, in->wm, in->st, in->screen,
                          eos_keys_mods_from_hid(ev.mods), ev.key);
        if (!r.changed) continue;
        moved = true;

        // eos_keys_apply() records bar_visible and stops there, because what
        // hiding the bar MEANS is the board layer's call. Here it means the
        // tiles get the twelve rows back.
        if (r.action == EOS_ACT_TOGGLE_BAR)
            in->wm->cfg.bar_h = in->st->bar_visible ? in->theme_bar_h : 0;
    }

    return moved;
}
