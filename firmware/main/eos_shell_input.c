// The dispatcher. See eos_shell_input.h for the precedence ladder every branch
// below is one rung of, and for why the ladder is that way round.
//
// The one thing worth knowing that the header does not say: eos_shell_input_next()
// is a POP and not a peek. Every event it returns is consumed, so the loop
// below is the only consumer in the image and anything that wants to watch the
// keystream without eating it has to use eos_input_peek().

#include "eos_shell_input.h"

#include "eos_pointer.h"
#include "eos_app_registry.h"

#include <string.h>

// The app owning the focused window, or EOS_APP_COUNT when nothing is focused.
// Read-only against eos_wm_t, which is finished and must not grow an accessor
// for this.
static uint16_t focused_app(const eos_shell_input_t *in)
{
    int f = in->wm->focus;

    if (f < 0 || f >= EOS_MAX_WINDOWS) return (uint16_t)EOS_APP_COUNT;
    if (!in->wm->win[f].alive) return (uint16_t)EOS_APP_COUNT;
    return in->wm->win[f].app_id;
}

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
    // Inactive until somebody says the desktop is up. The safe half of rule 1:
    // a caller that forgets throws events away, which is recoverable, rather
    // than dispatching them behind a screen nobody can see, which is not.
    in->active      = false;
}

void eos_shell_input_chrome(eos_shell_input_t *in, const eos_pointer_chrome_t *ch)
{
    if (!in) return;
    if (ch) in->chrome = *ch;
    else    memset(&in->chrome, 0, sizeof in->chrome);
}

void eos_shell_input_set_active(eos_shell_input_t *in, bool active)
{
    if (in) in->active = active;
}

void eos_shell_input_launcher(eos_shell_input_t *in, eos_launcher_t *l)
{
    if (!in) return;
    in->launcher = l;
    // Adopt whatever the shell already thinks. A launcher attached while the
    // flag happens to be set would otherwise be an overlay nothing draws.
    if (l && in->st) eos_launcher_set_open(l, in->st->launcher_open);
}

bool eos_shell_input_next(eos_event_t *out)
{
    return eos_input_poll(out);
}

// Raise, or open. Picking an app that is already on the current workspace
// focuses that window instead of opening a second copy of it: five windows
// already fill a 240x240 panel, and a launcher that answers "buddy" with a
// sixth buddy tile is a launcher that makes the desktop worse every time it
// is used.
static bool launcher_spawn(eos_shell_input_t *in, uint16_t app_id)
{
    int i;

    for (i = 0; i < EOS_MAX_WINDOWS; i++) {
        if (!in->wm->win[i].alive) continue;
        if (in->wm->win[i].app_id != app_id) continue;
        if (in->wm->win[i].ws != in->wm->ws) continue;
        if (in->wm->focus == i) return false;   // already looking at it
        eos_wm_focus_win(in->wm, i);
        return true;
    }
    return eos_keys_apply(in->wm, in->st, in->screen,
                          EOS_ACT_SPAWN, (int16_t)app_id).changed;
}

// What the launcher's answer means to the rest of the shell. The model has
// already closed itself on a launch or a close; this mirrors that back into
// eos_shell_state_t, which is what eos_keys_feed() reads to decide whether a
// plain key belongs to the list or to the desktop.
static bool launcher_settle(eos_shell_input_t *in, eos_launcher_res_t lr)
{
    bool moved = lr.changed;

    switch (lr.act) {
    case EOS_LAUNCHER_LAUNCH:
        in->st->launcher_open = false;
        if (launcher_spawn(in, lr.app_id)) moved = true;
        break;
    case EOS_LAUNCHER_CLOSE:
        in->st->launcher_open = false;
        break;
    default:
        break;
    }
    return moved;
}

// Whether this event carries a cursor rather than a keycode. Four types and one
// question, so no branch below has to list them again.
static bool is_pointer(const eos_event_t *ev)
{
    return ev->type == EOS_EV_POINTER_MOVE || ev->type == EOS_EV_POINTER_DOWN ||
           ev->type == EOS_EV_POINTER_UP   || ev->type == EOS_EV_CLICK;
}

bool eos_shell_input_pump(eos_shell_input_t *in)
{
    eos_event_t ev;
    bool moved = false;

    if (!in || !in->wm || !in->st || !in->km) return false;

    while (eos_shell_input_next(&ev)) {
        eos_key_result_t r;

        // ---- rung 1: some other scene owns the panel.
        //
        // Drained and dropped. This branch is the reason the frame loop no
        // longer has a drain of its own: one queue with two consumers is two
        // places that have to agree about what "not the desktop" means, and
        // they did not — the loop's copy ran AFTER the desktop branch, so the
        // pass that arrived at the desktop dispatched the setup screen's
        // backlog before anything got round to throwing it away.
        if (!in->active) continue;

        // ---- rung 2: locked.
        //
        // eos_keys_feed() swallows every chord but EOS_ACT_LOCK while
        // st->locked, and that used to be the whole of the rule — which left
        // every path that does not go through it working normally on a locked
        // board: the pointer, printable text, and any key no bind claimed,
        // which went straight on to the focused window. A lock you can type
        // into and click through is not a lock.
        //
        // So the lock is enforced HERE, once, for all three: nothing but a
        // key-down or a repeat reaches the keymap, and nothing at all reaches
        // the launcher or an app.
        if (in->st->locked) {
            if (ev.type == EOS_EV_KEY_DOWN || ev.type == EOS_EV_KEY_REPEAT) {
                r = eos_keys_feed(in->km, in->wm, in->st, in->screen,
                                  eos_keys_mods_from_hid(ev.mods), ev.key);
                if (r.changed) moved = true;
            }
            continue;
        }

        // ---- rung 3a: the launcher, for anything carrying a cursor.
        //
        // The pointer's clicks are dispatched here for the same reason the
        // keys are: one consumer, one path. A click asks the window manager
        // for exactly what super+h and super+tab ask it for, so it goes
        // through the same pump and its answer joins the same `moved` flag
        // that decides whether the screen is redrawn.
        if (is_pointer(&ev)) {
            if (in->st->launcher_open) {
                // The list gets them instead of the tiles underneath. A move
                // under the cursor selects the row it is over; a CLICK — press
                // and release on the same spot, so a drag that slid off the
                // row is not one — opens it. Nothing here reaches the tiles.
                if (in->launcher) {
                    if (ev.type == EOS_EV_POINTER_MOVE) {
                        if (eos_launcher_hover(in->launcher, ev.x, ev.y))
                            moved = true;
                    } else if (ev.type == EOS_EV_CLICK) {
                        if (launcher_settle(in,
                                eos_launcher_click(in->launcher, ev.x, ev.y)))
                            moved = true;
                    }
                }
                continue;
            }
            // Focus a tile, raise a tab, or close a window: all three are
            // eos_pointer_event()'s, all three move the layout, and the close
            // box it tests against is the one eos_shell_draw.c painted.
            if (eos_pointer_event(in->wm, in->screen, &in->chrome, &ev))
                moved = true;
            continue;
        }

        // ---- rung 5, taken early: a printable character.
        //
        // It belongs to whatever owns focus and never to the keymap: a bind is
        // a chord and this is already a letter, with the layout and the
        // modifiers applied down in the HAL. It is what makes typing into the
        // Chat window work the moment a keyboard bonds, and it is deliberately
        // not gated on one having bonded — the phone page and the serial
        // console inject the same event. The launcher has no text field, so
        // while it is open a character is dropped rather than typed into the
        // window behind it.
        if (ev.type == EOS_EV_TEXT) {
            if (!in->st->launcher_open) (void)eos_app_key(focused_app(in), &ev);
            continue;   // a character is never a window move
        }

        // Only presses and repeats reach the keymap. A key-up never fires a
        // bind — holding super+1 must not switch workspace twice — and touch
        // and connect events belong to whatever owns focus, which on a board
        // with no apps running is nothing.
        if (ev.type != EOS_EV_KEY_DOWN && ev.type != EOS_EV_KEY_REPEAT) continue;

        // ---- rung 3b: the launcher, for keys, and it goes BEFORE the keymap.
        //
        // It has to: the keymap binds bare arrows to nothing, so asking it
        // first would work today, but it binds bare escape to nothing either
        // while super+escape is the lock — and the day someone rebinds a plain
        // key in keys.json, the list would silently stop receiving it.
        // Offering the overlay the key first and taking it back when the
        // overlay declines is the order that does not depend on the keymap
        // staying empty.
        //
        // eos_launcher_key() hands every SUPER chord straight back, which is
        // what keeps super+space closing the list and super+q closing a window
        // from inside it.
        if (in->launcher && eos_launcher_is_open(in->launcher)) {
            eos_launcher_res_t lr =
                eos_launcher_key(in->launcher, ev.key,
                                 eos_keys_mods_from_hid(ev.mods));
            if (lr.act != EOS_LAUNCHER_PASS) {
                if (launcher_settle(in, lr)) moved = true;
                continue;
            }
        }

        // ---- rung 4: the keymap.
        r = eos_keys_feed(in->km, in->wm, in->st, in->screen,
                          eos_keys_mods_from_hid(ev.mods), ev.key);

        // EOS_ACT_LAUNCHER moves the shell's flag and nothing else, so the
        // overlay is brought into step here rather than in eos_keys_apply(),
        // which must keep building with no launcher present.
        if (in->launcher) eos_launcher_set_open(in->launcher, in->st->launcher_open);

        if (r.changed) {
            moved = true;

            // eos_keys_apply() records bar_visible and stops there, because
            // what hiding the bar MEANS is the board layer's call. Here it
            // means the tiles get the twelve rows back.
            if (r.action == EOS_ACT_TOGGLE_BAR)
                in->wm->cfg.bar_h = in->st->bar_visible ? in->theme_bar_h : 0;
            continue;
        }

        // HANDLED, not CHANGED. A bind that fired and found nothing to do
        // still ate the key: super+left at the left edge moves no focus, and
        // testing `changed` here is what used to send that chord on to the
        // focused window, where the chat tile read it as a bare left arrow.
        // `changed` is about the SCREEN; `handled` is about the key.
        if (r.handled) continue;

        // ---- rung 5: the focused window.
        //
        // A key no bind claimed. The app's answer does NOT set `moved` — see
        // the constraint at the foot of eos_shell_input.h.
        if (!in->st->launcher_open) (void)eos_app_key(focused_app(in), &ev);
    }

    return moved;
}
