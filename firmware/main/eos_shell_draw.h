// eos_shell_draw — the scene. Everything penguinOS puts on the glass in one
// re-runnable function, written entirely against eos_display.h.
//
// This is the layer the boot glue was missing: eos_wm gives out rectangles,
// eos_bar gives out positioned text runs, eos_theme gives out palette indices
// and eos_font gives out glyph bits, and nothing in the kernel joins them up.
// This file joins them up and draws nothing else. It calls no IDF function and
// includes no IDF header, so it compiles for the host against the display
// backend's host build and can be rendered to a PPM before anyone flashes a
// board.
//
// The one non-obvious constraint: eos_shell_draw_frame() runs the scene ONCE
// PER BAND. A banded backend has no framebuffer, so the same drawing calls are
// replayed six times on a 240x240 panel with a different 40-row clip installed
// each time, and every one of them must produce identical pixels. Nothing in
// here may depend on what a previous band did, and nothing may be computed
// from a counter that advances per call — the live numbers arrive in
// eos_shell_view_t, are read, and are never latched.

#ifndef EOS_SHELL_DRAW_H
#define EOS_SHELL_DRAW_H

#include <stdint.h>
#include <stdbool.h>

#include "eos_wm.h"
#include "eos_bar.h"
#include "eos_keys.h"
#include "eos_launcher.h"
#include "eos_pointer.h"
#include "eos_theme.h"
#include "eos_display.h"
#include "eos_buddy.h"

// The app ids, the tile bodies and the one-line summaries used to live here,
// in eos_shell_draw.c and in main.c: three lists that had to be kept in the
// same order by hand, and a switch that had to be edited to match. They are
// one table now — see eos_app_registry.h — and this file is back to what its
// name says: chrome, bar, tabs, overlay and the frame loop.
//
// This header includes the registry and not the other way round, so an app
// body may include this one for the live numbers without making a cycle.
#include "eos_app_registry.h"

const char *const *eos_shell_app_names(void);   // EOS_APP_COUNT entries

// One frame's worth of state. Borrowed for the duration of the draw call; the
// renderer copies nothing and keeps no pointer.
//
// The numeric fields exist because the scene has no way to reach them: heap
// and uptime are IDF's, and the shell must not learn to call esp_timer just to
// draw a clock. main.c reads them once per frame and hands them over.
//
// Tagged, unusually for this codebase, because eos_app_registry.h has to name
// the type before this header exists to define it: an app body is handed a
// pointer to one and the registry cannot include the file that includes it.
typedef struct eos_shell_view {
    const eos_theme_t      *theme;
    const eos_wm_t         *wm;
    const eos_bar_status_t *bar;
    const eos_keymap_t     *keys;

    // The avatar. NULL leaves the buddy tile saying so rather than drawing a
    // hole, which is what a board whose model failed to load should look like.
    //
    // Not const, and this is the reason the whole buddy is rendered ONCE per
    // frame into an offscreen buffer instead of straight onto the glass:
    // eos_buddy_render() reorders the model's voxel array in place, far to
    // near, and the scene below is replayed once per band. Six replays would
    // be six sorts and six renders of an identical picture.
    eos_buddy_t *buddy;

    uint32_t heap_free;
    uint32_t heap_largest;
    uint32_t uptime_ms;

    // The app launcher overlay. NULL, or a launcher that is not open, draws
    // nothing at all and costs the scene one branch. When it IS open it is the
    // last thing painted, over every tile and over the bar, because that is
    // what an overlay means: the list is the only thing the keyboard is
    // talking to while it is up.
    const eos_launcher_t *launcher;

    // The trackpad's cursor. NULL, or a cursor no device has moved yet, draws
    // nothing. It is the very last thing painted, above even the launcher,
    // because an arrow that the thing it is pointing at can cover is not a
    // cursor. The `visible_ms` beside it is the clock the visibility timeout
    // is measured against: eos_pointer_visible() needs a now, the scene is
    // replayed once per band, and reading a clock inside the scene would let
    // two bands disagree about whether the arrow is on screen.
    const eos_pointer_t *pointer;
    uint32_t             pointer_ms;

    // Four lines of board identity for the "board" window. NULL entries are
    // skipped, so a caller that knows less writes less. The first is drawn in
    // the 8x13 face and the rest in the theme's; a 117 px tile holds eighteen
    // 6 px cells, and anything longer is truncated rather than wrapped.
    const char *board_line[4];
} eos_shell_view_t;

// Damage, open the frame, replay the scene into every band, close the frame.
// The caller declares damage BEFORE calling this — see eos_shell_damage_all()
// and the two rect helpers below — because a banded backend fixes its bands at
// frame_begin() and drawing cannot extend them afterwards.
void eos_shell_draw_frame(const eos_shell_view_t *v);

// Builds the (face level, model palette index) -> display index table the
// avatar needs on an 8-bit indexed target, and points cfg->shade_lut at it.
// One table for the image: there is one buddy.
//
// Call it before eos_buddy_init(), and AGAIN after anything reprograms the
// display palette — a theme switch does, and a table built against the old
// palette would leave the buddy wearing the previous theme's colours until the
// next reload. `pal` NULL clears the table and the buddy renders flat.
//
// It resolves each colour through eos_display_match() rather than through
// eos_buddy_build_shade_lut(), because the HAL hands out no copy of its loaded
// palette and a second copy taken from the theme would be a second copy to go
// stale. It also removes the hazard eos_buddy.h warns about: match() searches
// 0..254 and can never return EOS_COLOR_NONE, so the buddy cannot come out
// with holes in its brightest places.
//
// 768 nearest-colour searches over a 255-entry palette. Milliseconds, on a
// path that runs at boot, at a theme change and at a model reload.
void eos_shell_buddy_shade(const eos_vox_pal_t *pal, eos_buddy_cfg_t *cfg);

// The side of the square the avatar is rendered into, and the whole of what
// the buddy window costs in RAM: EOS_SHELL_BUDDY_PX squared bytes of one
// palette index each. It is a fixed box rather than the tile's size because a
// theme with no bar and no tab strip could hand the tile 230x220, and 50 KB of
// BSS for a window that might be behind a tab is not a trade this board can
// make. 80 covers the 110x76 body five windows leave on a 240x240 panel; the
// buddy is fitted inside whatever it gets and centred in the rest.
#ifndef EOS_SHELL_BUDDY_PX
#define EOS_SHELL_BUDDY_PX 80
#endif

// What that box and its shade table cost in BSS, so the boot log can say it.
// The number is compile-time and is reported rather than computed at runtime
// because BSS is claimed before app_main runs: it never shows up as a heap
// step, and a board whose free heap moved would otherwise have no line naming
// the window that took it.
uint32_t eos_shell_buddy_bytes(void);

// The launcher's panel geometry for THIS panel and this theme's UI face. The
// launcher model cannot work it out alone — it knows nothing about fonts or
// about how big the glass is — so the scene, which knows both, hands it over.
// Call it once at boot and again after a theme change, then feed the answer to
// eos_launcher_set_geom(). It is pure, so the hit test and the renderer are
// looking at the same rectangle by construction rather than by agreement.
void eos_shell_launcher_geom(const eos_theme_t *t, eos_launcher_geom_t *g);

// What this theme's chrome does to a tile rect, in the form the pointer needs
// to find a tile's close box. Same shape as the call above and for the same
// reason: the model cannot see a font, the scene can, and the answer is stored
// once so that the x on the glass and the x a click is tested against are one
// rectangle. Call it at boot and again after a theme change.
void eos_shell_tile_chrome(const eos_theme_t *t, eos_pointer_chrome_t *ch);

// Damage helpers. These only compute rectangles; they call eos_display_damage()
// so main.c does not have to re-derive the bar height or re-run the layout.
void eos_shell_damage_all(void);
void eos_shell_damage_bar(const eos_shell_view_t *v);

// Declares the two rects the arrow moved between: the hole it left and the
// place it went. Two and never the whole screen — the backend is banded and a
// 240-row repaint per trackpad report would not keep up, while two 7x11 boxes
// are under two hundred pixels of SPI. Returns false when the cursor has not
// moved since the last committed frame and nothing was damaged.
bool eos_shell_damage_pointer(const eos_shell_view_t *v);

// Damages the visible tile showing `app`, if it has one on the current
// workspace. Returns false when that app is not on screen — hidden behind a
// tab, on another workspace, or never opened — in which case nothing was
// damaged and the caller has nothing to redraw.
bool eos_shell_damage_app(const eos_shell_view_t *v, uint16_t app_id);

// The same question without the answer's side effect. It exists because the
// loop asks it about the buddy for a reason that is not damage — how long to
// sleep — and running the layout to find out, then damaging as a side effect,
// would put a rect on the list on a pass that had nothing to redraw.
bool eos_shell_app_visible(const eos_shell_view_t *v, uint16_t app_id);

#endif // EOS_SHELL_DRAW_H
