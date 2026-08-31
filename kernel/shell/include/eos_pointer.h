// eos_pointer — the cursor, and the two things a cursor can mean on a tiling
// window manager.
//
// The K809 keyboard the owner has is really two devices behind one BLE bond:
// eight-byte keyboard reports on one handle and three-byte HID boot-mouse
// reports from its trackpad on another. The keyboard half has had a home since
// eos_input.c existed. The trackpad half had none — eos_input.h knew about
// keys, touches and connections and had no idea a pointer was a thing — so
// forty-six reports a second arrived and were thrown away.
//
// This file is the home. It owns exactly one piece of state the HAL cannot:
// WHERE the cursor is. The HAL takes absolute pixels because it does not know
// how big the screen is, the radio hands over signed relative counts, and the
// arithmetic in between — acceleration, sub-pixel accumulation, clamping to
// the panel — is the whole of this module's first half.
//
// The second half is the click, and on a tiling WM a click is deliberately
// only two things: focus the tile under it, or raise the tab under it. There
// is no drag and no resize-by-edge, because a tiling layout is computed from a
// tree and not from where windows happen to sit; a dragged window would have
// nowhere to land. Both are pure lookups against the layout eos_wm_layout()
// already computes, which is why nothing here modifies the window tree beyond
// a single eos_wm_focus_win() call.
//
// The one non-obvious constraint: the cursor's position is updated INSIDE the
// BLE callback, before the event ever reaches the ring, and the ring is only
// 32 events deep. A trackpad swipe is hundreds of reports and the main loop
// drains at 30 Hz, so motion events must never be allowed to fill the ring and
// push a click out of it. They cannot: eos_input_inject_pointer() coalesces a
// motion event into the one already waiting at the tail, and the drawn cursor
// reads eos_pointer_t directly rather than replaying the events. Losing a
// motion event costs nothing. Losing a click costs the click.

#ifndef EOS_POINTER_H
#define EOS_POINTER_H

#include <stdint.h>
#include <stdbool.h>

#include "eos_wm.h"
#include "eos_input.h"

// The arrow's bounding box, in pixels, with the hot spot at its top-left
// corner. Named here and not in the renderer because the damage rect is
// computed from it by code that draws nothing.
#define EOS_POINTER_ARROW_W  7
#define EOS_POINTER_ARROW_H 11

// How far the cursor may wander between press and release and still count as
// a click rather than a smudge. Four pixels: a 240x240 panel's smallest tile
// border is one pixel and its tab cells are about twenty wide, so four is
// forgiving of a thumb without ever reaching the next target.
#define EOS_POINTER_SLOP 4

// A pointer that has said nothing for this long stops being drawn, the way a
// desktop cursor fades. It also tells the frame loop it may go back to sleep
// at its idle rate: a visible cursor is the second thing on this board that
// earns a fast loop, and it should not earn one forever after one twitch.
#define EOS_POINTER_IDLE_MS 4000u

// ------------------------------------------------------------- acceleration
//
// Two regimes, all integer, because this chip is an RV32IMAC with no FPU and a
// float per axis per report is not a cost a 100 Hz trackpad should carry.
//
// Below `unity` counts in one report the gain is exactly 1.0, so a slow drag
// moves one pixel per count and can still be parked on a one-pixel tile
// border. Above it the gain rises linearly at `gain_q4` per extra count until
// it hits `max_q4`. Monotone in |d|, so the cursor never travels backwards
// when you push harder, and odd — gain depends on |d| only — so left feels
// exactly like right.
//
// Gains are Q4 fixed point: 16 is 1.0x, 80 is 5.0x.
typedef struct {
    int16_t unity;    // counts per report still moved at exactly 1.0x
    int16_t gain_q4;  // Q4 gain added per count above `unity`
    int16_t max_q4;   // ceiling on the whole gain. Never below 16.
} eos_pointer_accel_t;

// unity 2, +0.625x per count, capped at 5.0x. A one-count nudge is one pixel;
// a brisk flick of ten counts a report crosses this 240 px panel in four
// reports, which at the trackpad's rate is about forty milliseconds.
eos_pointer_accel_t eos_pointer_accel_defaults(void);

// ------------------------------------------------------------------- state

typedef struct {
    int16_t  w, h;              // screen bounds. The cursor never leaves them.
    int16_t  x, y;              // where it is now, in screen pixels
    uint8_t  buttons;           // EOS_BTN_* currently held

    // Q4 remainders, so a gain that is not a whole number does not quantise
    // slow motion away one report at a time.
    int32_t  acc_x, acc_y;

    // Where each button went down, for the slop test. Three entries: left,
    // right, middle, indexed by the bit position in the EOS_BTN_* bitmap.
    int16_t  down_x[3], down_y[3];

    // Where the last drawn frame put the arrow, so the damage rect covers the
    // hole it left as well as the place it went.
    int16_t  drawn_x, drawn_y;
    bool     drawn_valid;

    // The position and visibility ONE frame is committed to, latched by
    // eos_pointer_latch() and cleared by eos_pointer_commit(). See the latch
    // for why the live x,y above cannot be what a frame draws from.
    int16_t  show_x, show_y;
    bool     show_vis;
    bool     show_armed;

    uint32_t last_ms;           // when the last report arrived
    bool     seen;              // false until the first report ever

    eos_pointer_accel_t accel;
} eos_pointer_t;

// Screen size in pixels. The cursor starts in the middle and invisible: it
// appears the first time the trackpad says anything, so a board with no
// pointing device never draws an arrow nobody can move.
void eos_pointer_init(eos_pointer_t *p, int16_t w, int16_t h);

// The one instance the BLE notify path feeds. A cursor is a singleton in the
// same way the event ring is, and threading a handle from app_main down into a
// NimBLE callback would be a pointer stored in a static anyway.
eos_pointer_t *eos_pointer_shared(void);

// ---------------------------------------------------------------- the curve

// Applies the curve to one axis. `d` is the signed count off the wire, `acc`
// is that axis's Q4 remainder and is read and rewritten. Returns whole pixels,
// which may be zero for a tiny motion whose fraction is being saved up.
//
// Exposed rather than hidden because it is the part with a right answer, and
// the answer is easier to check against a table than through a whole feed.
int16_t eos_pointer_scale(const eos_pointer_accel_t *a, int16_t d, int32_t *acc);

// ----------------------------------------------------------------- reports

// One decoded HID boot-mouse report. dx and dy are SIGNED counts, already sign
// extended by whoever read the wire; `buttons` is the EOS_BTN_* bitmap of what
// is held right now, not what changed.
//
// Moves the cursor, clamps it, and pushes the events that resulted:
// EOS_EV_POINTER_MOVE when it actually moved, then EOS_EV_POINTER_DOWN or
// EOS_EV_POINTER_UP for every button whose state differs from last time, then
// EOS_EV_CLICK after an UP that landed within EOS_POINTER_SLOP of its DOWN.
// Motion first, so a button event always carries the position the button was
// released at rather than the one before the same report's movement.
//
// Callable from a BLE callback. It touches only this struct and the HAL's
// injection door, and neither allocates.
void eos_pointer_feed(eos_pointer_t *p, int16_t dx, int16_t dy,
                      uint8_t buttons, uint32_t now_ms);

// The pointing device went away. Releases anything held WITHOUT synthesising
// clicks — a trackpad that disconnected mid-press did not click — and hides
// the cursor.
void eos_pointer_disconnect(eos_pointer_t *p, uint32_t now_ms);

// ------------------------------------------------------------------ drawing

// Drawn only while a device is actually there and recently awake. This reads
// the LIVE state, which is the right answer for "is there a cursor" and the
// wrong one for "what should this frame paint" - see eos_pointer_shown().
bool eos_pointer_visible(const eos_pointer_t *p, uint32_t now_ms);

// Freezes the position and the visibility the next frame will be drawn from.
//
// This exists because x, y and last_ms are written from inside the BLE host
// callback, which preempts the task that draws, and one frame reads them many
// times: eos_shell_damage_pointer() computes the two damage rects from them,
// then the scene is REPLAYED ONCE PER BAND and each replay paints the arrow
// again, then eos_pointer_commit() records where it went. A trackpad report
// landing anywhere in the middle of that leaves the three disagreeing - the
// damage covers one place, a band paints the arrow at a second, and the commit
// records a third - and the pixels painted at the second are then outside
// every rect the next frame repaints, so a fragment of the arrow stays on the
// glass until something unrelated happens to damage it. At thirty frames a
// second against a hundred-hertz pad that is a visible smear, not a rare race.
//
// So the frame loop latches once, before it declares damage, and everything
// downstream reads the latch instead of the live position. Motion that arrives
// mid-frame is not lost: it moves x and y as it always did and the NEXT frame
// latches it.
//
// Calling this is optional. With no latch armed every call below falls back to
// the live position, which is exactly the behaviour that predates it.
void eos_pointer_latch(eos_pointer_t *p, uint32_t now_ms);

// Whether this frame should paint an arrow at all: the latched answer when one
// is armed, the live one otherwise.
bool eos_pointer_shown(const eos_pointer_t *p, uint32_t now_ms);

// The arrow's box at the position THIS FRAME will draw it - the latch when one
// is armed, the live position otherwise - and at the position the last
// committed frame drew it. Both are clipped to the screen, so a cursor parked
// in the bottom-right corner does not damage rows the panel does not have.
eos_rect_t eos_pointer_rect(const eos_pointer_t *p);
eos_rect_t eos_pointer_drawn_rect(const eos_pointer_t *p);

// True when the arrow is somewhere other than where it was last drawn, or has
// just appeared or disappeared. This is what earns the two damage rects.
bool eos_pointer_dirty(const eos_pointer_t *p, uint32_t now_ms);

// Latches "the frame just drawn put the arrow here". Call after the draw, not
// before: the damage rects are computed from the difference and committing
// early collapses it to nothing. It records the LATCHED position when one is
// armed - the place the bands actually painted - and disarms it, so the next
// frame's hole is where the arrow really is and not where the trackpad moved
// to while the frame was still being pushed.
void eos_pointer_commit(eos_pointer_t *p, uint32_t now_ms);

// ------------------------------------------------------------- tile chrome
//
// What the renderer did to the tile rect before it drew the header, so the hit
// test can find the close box without being able to see a font.
//
// The owner asked for windows that can be closed, and super+q is only half of
// that: a keyboard chord is not an affordance, and the trackpad they have was
// pointing at a desktop with nothing on it to press. So every visible tile
// draws an x at the right of its header and this struct is how the pointer
// finds it. The three numbers come from the theme and the UI face, both of
// which live in firmware/main/eos_shell_draw.c — eos_shell_tile_chrome() fills
// one in, and it is recomputed on a theme change for the same reason the
// launcher's geometry is: a theme may name a different face and a hit box on
// the old face's grid is a hit box in the wrong place.
//
// close_w 0 disables the box entirely, which is what a board with a panel too
// narrow to spare the pixels should do. A NULL chrome means the same thing, so
// every call that predates this can pass NULL and get the old behaviour.
typedef struct {
    int16_t border;   // the theme's border width. The renderer insets by border+1.
    int16_t hdr_h;    // the UI face's height: the header strip inside that inset
    int16_t close_w;  // the box's width in pixels. 0 = no close box on this board.
} eos_pointer_chrome_t;

// The smallest box that can carry a legible cross and be aimed at. A box is
// close_w wide or it does not exist: there is no shrinking it to fit, because
// a box too small to draw the x in is a rectangle that closes a window with
// nothing on the glass to say so, which is the worst thing a close control can
// be.
#define EOS_POINTER_CLOSE_MIN 5

// The close box for one tile, in screen pixels, or a zero rect when this tile
// has none — it is not visible, the chrome says there is no box, or the tile
// cannot spare close_w and still keep half its header for the window's name.
// Pure, and it is the SAME function the renderer paints from, so the x on the
// glass and the rectangle a click is tested against cannot drift apart.
eos_rect_t eos_pointer_close_box(const eos_pointer_chrome_t *ch,
                                 const eos_tile_t *t);

// --------------------------------------------------------------- hit testing

typedef enum {
    EOS_HIT_NONE = 0,   // the gap between tiles, or bare desktop
    EOS_HIT_BAR,        // the status bar along the top
    EOS_HIT_TILE,       // a visible tile's content rect
    EOS_HIT_TAB,        // one cell of a collapsed group's tab strip
    EOS_HIT_CLOSE       // the close box at the right of a tile's header
} eos_hit_kind_t;

typedef struct {
    uint8_t kind;       // eos_hit_kind_t
    int16_t win;        // the window under the point, or EOS_NONE
    int8_t  tab_index;  // which cell of the strip, or -1
} eos_hit_t;

// What is under (x, y) on the workspace `wm` currently shows, laid into
// `screen`. Pure: it runs the same eos_wm_layout() the renderer runs and looks
// nothing up anywhere else, so a hit can never disagree with what is on the
// glass.
//
// Order matters and is the drawing order in reverse: the bar is above
// everything, then a tile's own close box, then a tab strip, which is above
// the tile it labels, and a tile is above the desktop. A point that is in no
// tile and no strip is a gap and hits nothing — which is a real answer and not
// a failure, and is why clicking between two tiles changes no focus.
//
// The close box is answered before the tile it sits in, because it is drawn
// inside that tile's rect and every point in it is also a point in the tile.
eos_hit_t eos_pointer_hit(const eos_wm_t *wm, eos_rect_t screen,
                          const eos_pointer_chrome_t *ch,
                          int16_t x, int16_t y);

// Acts on that hit. A tile focuses its window; a tab focuses the window that
// cell belongs to, which inside a collapsed group is also what raises it,
// because eos_wm_focus_win() walks the ancestors marking which child was last
// focused and that is exactly what the layout reads to pick the visible tab.
// A close box closes its window, which is the same eos_wm_close() super+q
// reaches — one path, so a window shut with the trackpad and a window shut
// with the keyboard leave the tree in the same state.
//
// Returns true when something actually happened and the screen needs a redraw.
// Clicking the already-focused tile, the bar, or a gap all return false.
bool eos_pointer_click(eos_wm_t *wm, eos_rect_t screen,
                       const eos_pointer_chrome_t *ch, int16_t x, int16_t y);

// The whole of the pointer's contribution to the shell's event pump: hands one
// event from the ring to the window manager. Anything that is not a click is
// ignored and reported as false, so the caller can pass it every event it pops
// without first learning the pointer's event types.
//
// A true answer means the LAYOUT moved — a focus change or a closed window —
// which is exactly what eos_shell_input_pump() reports to the frame loop as
// "redraw everything".
bool eos_pointer_event(eos_wm_t *wm, eos_rect_t screen,
                       const eos_pointer_chrome_t *ch, const eos_event_t *e);

#endif // EOS_POINTER_H
