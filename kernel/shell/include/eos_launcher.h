// eos_launcher — the app list MODEL. super+space opens a single column of
// apps; up and down move a highlight; enter opens the selected one; escape
// puts it away. This file owns the list, the selection and the scroll offset,
// and it draws nothing. The overlay is painted by firmware/main/eos_shell_draw.c
// the same way eos_bar's segments are: the model computes, the scene draws.
//
// It borrows its strings. An item holds two const char * that are NOT copied,
// because copying six names and six sentences into a fixed pool would cost
// most of a kilobyte of BSS to hold text that is already in flash. The caller
// must therefore hand it pointers that outlive the launcher — string literals,
// or the static tables in main.c, which is what it is fed.
//
// The one non-obvious constraint: eos_launcher_geom_t is computed ONCE and
// stored, and both the renderer and the hit test read it back rather than
// recomputing. The scene is replayed once per display band and every replay
// must produce identical pixels, so a row height derived inside the draw call
// from anything that could move between bands would be a bug that only shows
// up as a torn overlay on a banded panel. Geometry changes when the screen or
// the theme's font changes, and at no other time.
//
// Keycodes are eos_input.h's USB HID usages, not a private space, so a row
// moves the same way whether the arrow came from BLE, from a GPIO button or
// from the phone page.

#ifndef EOS_LAUNCHER_H
#define EOS_LAUNCHER_H

#include <stdint.h>
#include <stdbool.h>

#include "eos_input.h"

// 24, not the window manager's 16: this is the list of apps that COULD be
// opened, which is allowed to be longer than the number of windows that can be
// on screen at once. At 12 bytes an item the whole table is 288 bytes.
#define EOS_LAUNCHER_MAX  24

// Not eos_wm.h's EOS_NONE. The model deliberately does not include the window
// manager — it hands back an app id and lets the caller open the window — so
// it cannot borrow that name, and the two must stay numerically equal.
#define EOS_LAUNCHER_NONE (-1)

typedef struct {
    const char *name;    // borrowed. Short: it is drawn in the accent colour first.
    const char *desc;    // borrowed. NULL or "" draws a name-only row.
    uint16_t    app_id;  // what the caller passes to eos_wm_open()
} eos_launcher_item_t;

// Where the overlay sits and how tall a row is. Filled by
// eos_launcher_layout() and then handed to eos_launcher_set_geom(), which is
// the only thing that may change `rows`.
typedef struct {
    int16_t x, y, w, h;   // the whole overlay panel, in screen pixels
    int16_t pad;          // inner margin between the panel edge and its content
    int16_t title_y;      // baseline-top of the "apps" heading
    int16_t rule_y;       // the hairline under the heading
    int16_t list_y;       // top of the first visible row
    int16_t row_h;        // one row, heading excluded
    uint8_t rows;         // rows that fit between list_y and the bottom margin
} eos_launcher_geom_t;

typedef struct {
    eos_launcher_item_t item[EOS_LAUNCHER_MAX];
    eos_launcher_geom_t geom;
    uint8_t             count;
    int16_t             sel;    // EOS_LAUNCHER_NONE when the list is empty
    int16_t             top;    // index of the first visible row
    bool                open;
} eos_launcher_t;

// What one key or one click did. `act` is the whole answer; `changed` says
// whether the overlay's pixels moved, which is a different question — enter on
// an empty list is eaten and changes nothing.
typedef enum {
    EOS_LAUNCHER_PASS = 0,   // not a launcher key: the caller still owns it
    EOS_LAUNCHER_EAT,        // consumed, and the launcher stays open
    EOS_LAUNCHER_LAUNCH,     // open app_id. The launcher has closed itself.
    EOS_LAUNCHER_CLOSE       // escape, or the toggle arriving a second time
} eos_launcher_act_t;

typedef struct {
    eos_launcher_act_t act;
    bool               changed;   // the overlay needs redrawing
    uint16_t           app_id;    // meaningful only when act is LAUNCH
} eos_launcher_res_t;

// --------------------------------------------------------------- lifecycle

void eos_launcher_init(eos_launcher_t *l);

// Appends. False when the table is full or `name` is NULL/empty; a launcher
// row with no name is a row nobody can identify, so it is refused rather than
// drawn blank. `desc` may be NULL.
bool eos_launcher_add(eos_launcher_t *l, const char *name, const char *desc,
                      uint16_t app_id);

void eos_launcher_clear(eos_launcher_t *l);

// -------------------------------------------------------------- open state

// Opening resets the selection to the first item and scrolls to the top: the
// owner's muscle memory is super+space, down, down, enter, and a launcher that
// reopened where it was left would make that sequence mean two different
// things on two consecutive presses.
void eos_launcher_open(eos_launcher_t *l);
void eos_launcher_close(eos_launcher_t *l);

// Returns the new open state. This is what EOS_ACT_LAUNCHER runs.
bool eos_launcher_toggle(eos_launcher_t *l);

// Forces the flag both ways, for the caller that is mirroring
// eos_shell_state_t.launcher_open rather than driving it. Opening through
// this resets the selection exactly as eos_launcher_open() does.
void eos_launcher_set_open(eos_launcher_t *l, bool open);

static inline bool eos_launcher_is_open(const eos_launcher_t *l)
{
    return l && l->open;
}

// ------------------------------------------------------------------ query

int eos_launcher_count(const eos_launcher_t *l);
int eos_launcher_selected(const eos_launcher_t *l);   // EOS_LAUNCHER_NONE if empty
int eos_launcher_top(const eos_launcher_t *l);
int eos_launcher_rows(const eos_launcher_t *l);       // visible rows, >= 1

// NULL when `i` is outside 0..count-1.
const eos_launcher_item_t *eos_launcher_item(const eos_launcher_t *l, int i);

// ----------------------------------------------------------------- moving

// Moves the selection by `delta` rows and WRAPS at both ends: down off the
// last item lands on the first, up off the first lands on the last. Scroll
// follows so the selection is always on screen. False when nothing moved —
// an empty list, or delta 0.
bool eos_launcher_move(eos_launcher_t *l, int delta);

// Absolute, and it does not wrap: out of range is refused, because this is
// what a pointer hovering off the end of a short list calls.
bool eos_launcher_select(eos_launcher_t *l, int index);

// One keypress. `mods` is a raw HID modifier byte or the collapsed form; both
// work. Any chord carrying SUPER is passed straight back — those belong to
// eos_keys, which is what closes the launcher on a second super+space.
//
// Bound: up/down and k/j move, page up/down move a screen, home/end jump,
// tab moves down, enter launches, escape closes. Everything else PASSes, so a
// key the launcher does not want is never swallowed.
eos_launcher_res_t eos_launcher_key(eos_launcher_t *l, uint16_t key, uint8_t mods);

// ---------------------------------------------------------------- geometry

// Computes the overlay's rectangle for a screen and a font cell height. Pure:
// no state is read and none is written, so the renderer and the hit test can
// both ask and always get the same answer. `rows` comes back at least 1 even
// on a panel too short to hold one, because a launcher that reports zero rows
// would divide by it.
void eos_launcher_layout(eos_launcher_geom_t *g, int16_t screen_w,
                         int16_t screen_h, int16_t font_h);

// Installs it. This is the only writer of `rows`, and it re-clamps the scroll
// offset immediately, so shrinking the panel cannot leave the selection off
// the bottom of the list.
void eos_launcher_set_geom(eos_launcher_t *l, const eos_launcher_geom_t *g);

// ----------------------------------------------------------------- pointer
//
// Coordinates, not a pointer object: the launcher answers in screen pixels so
// that a trackpad cursor, a touch panel, or a tap injected from the phone page
// all drive it through the same two calls, and so this file depends on no
// input device that may or may not exist.

// The item index under (x, y), or EOS_LAUNCHER_NONE for a point that is
// outside the panel, on the heading, or past the last row.
int eos_launcher_hit(const eos_launcher_t *l, int16_t x, int16_t y);

// Hover selects. True when the selection actually moved, which is the only
// case worth a redraw — a pointer sitting still inside one row must not
// repaint the overlay sixty times a second.
bool eos_launcher_hover(eos_launcher_t *l, int16_t x, int16_t y);

// A button press. Inside a row it launches, exactly as enter does. Outside the
// panel it closes, which is what clicking away from a menu has meant since
// menus existed. Anywhere else on the panel it is eaten.
eos_launcher_res_t eos_launcher_click(eos_launcher_t *l, int16_t x, int16_t y);

#endif // EOS_LAUNCHER_H
