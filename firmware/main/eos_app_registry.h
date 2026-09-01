// eos_app_registry — the one table of windows penguinOS can open, and the only
// thing an app body is allowed to see while it draws.
//
// Before this file the app list existed four times: an enum in
// eos_shell_draw.h, a name array in eos_shell_draw.c, a switch in draw_tile(),
// and a summary array in main.c that /api/apps read. Adding a window meant
// editing all four and getting the order right in each, and the launcher the
// owner asked for would have been a fifth. One table with a draw function in
// it collapses that to a row and a function: the tab label, the picker entry,
// /api/apps, sys.autostart and the launcher all read the same seventy bytes.
//
// The one non-obvious constraint is inherited and absolute: a draw function is
// called ONCE PER BAND with a different clip installed, and it must produce
// identical pixels every time. So a draw function may not advance a counter,
// may not touch storage, may not set the buddy's mood, and may not remember
// that it ran. Everything that moves happens in eos_app_tick(), which the OS
// loop calls exactly once per pass, and the draw reads what the tick left.
// That split is why every app in this repo can be rendered off-target and
// diffed pixel for pixel.
//
// Nothing here allocates. Each app's state is a file-static in its own
// translation unit, sized at compile time, and eos_app_bss_bytes() reports
// what they add up to so the boot log can name the cost.

#ifndef EOS_APP_REGISTRY_H
#define EOS_APP_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>

#include "eos_board.h"
#include "eos_display.h"
#include "eos_input.h"
#include "eos_buddy.h"
#include "eos_pointer.h"
#include "eos_httpd.h"
#include "eos_settings.h"

// The live frame state. Declared in eos_shell_draw.h, which includes this
// header; an app body that wants the numbers includes that one too.
struct eos_shell_view;

// ------------------------------------------------------------------- ids
//
// The index into the table AND the eos_wm app_id, which is why the order is
// load-bearing: main.c's win_of[] is indexed by it and sys.autostart resolves
// through it. The first five are the windows that were verified on hardware
// and they keep their numbers; everything the owner asked for is appended.

typedef enum {
    EOS_APP_CLOCK = 0,   // uptime, in the 12x20 face
    EOS_APP_BOARD,       // what the descriptor says this board is
    EOS_APP_HEAP,        // free and largest-block, live
    EOS_APP_KEYS,        // the compiled-in keymap, formatted from the table
    EOS_APP_BUDDY,       // the voxel avatar, and the mood it is in
    EOS_APP_CHAT,        // a megabrain conversation, on the panel
    EOS_APP_SETTINGS,    // what the web Settings page shows, read-only
    EOS_APP_FILES,       // a read-only browser over /int
    EOS_APP_MEDIA,       // the WS2812 on GPIO8. Light, not sound.
    EOS_APP_PARTY,       // the ten-second demo
    EOS_APP_CAMERA,      // a viewfinder onto a penguinOS camera node
    EOS_APP_COUNT
} eos_app_id_t;

// ------------------------------------------------------------------- ctx
//
// Every colour and face the scene resolved for this frame, plus the live
// numbers and whether this particular tile has the focus. It is built once per
// frame by eos_shell_draw.c and handed to each body by value-pointer; an app
// keeps no copy of it, because the fonts and palette indices change under a
// theme switch and a stale one would draw in the previous theme's colours.
typedef struct {
    const eos_font_t *ui, *tiny, *med, *big;
    int16_t border;

    // What the chrome does to a tile rect before the body gets what is left,
    // in the form eos_pointer.c needs to find the close box. Resolved once per
    // frame beside the colours because it is the same kind of fact: it comes
    // out of the theme, it changes only when the theme does, and two copies of
    // it is how the drawn x and the clickable x end up in different places.
    eos_pointer_chrome_t chrome;

    eos_color_t bg, surface, overlay, text, muted, accent;
    eos_color_t bfoc, bunf, barbg, barfg, tabact, tabinact;
    eos_color_t ok, warn;

    // Never NULL inside a draw call. Borrowed for the length of it.
    const struct eos_shell_view *view;

    // Whether the tile being drawn is the focused one. Apps that take keys use
    // it to decide whether to draw a cursor or a selection bar: an unfocused
    // chat window showing a blinking caret is a lie about where the keys go.
    bool focused;
} eos_app_ctx_t;

// Draws the app's body into `r`, which is the tile's content area with the
// border, the header and the rule under it already taken off. It is clipped to
// `r` by the caller, but an app that draws outside it is still a bug: the clip
// is a safety net and the host suite checks the rule directly.
//
// `r` ranges from about 110x76 (one of five tiles on a 240x240 panel) to
// 228x203 (the only window). An app that cannot be useful at the small end
// says so in words rather than drawing a mess.
typedef void (*eos_app_draw_fn)(const eos_app_ctx_t *c, eos_rect_t r);

// Offered a key event when the app's window has the focus and no global bind
// claimed it. Returns true when the app consumed it. NULL means the app takes
// no keys, which is most of them. Runs on the OS loop, never during a draw, so
// it may change state freely.
typedef bool (*eos_app_key_fn)(const eos_event_t *e);

typedef struct {
    const char *id;        // sys.autostart and /api/apps spell it this way
    const char *name;      // the tab label. Short: a tab cell can be 14 px.
    const char *summary;   // one line for the picker and the web app
    uint8_t     tier_min;  // lowest eos_tier_t this window runs on
    eos_app_draw_fn draw;  // never NULL; the table is checked at boot and on host
    eos_app_key_fn  key;   // may be NULL
} eos_app_t;

// ------------------------------------------------------------ one helper
//
// Draws as much of `s` as fits in max_w and returns the pen advance. Every
// string in every app and in the chrome goes through this: a tile is 110 px
// wide and almost every label in it is capable of being one character too
// long, so truncation is the normal case and not an error path — and two
// implementations of it is how a tab label and a tile header start clipping at
// different widths.
int16_t eos_app_text(int16_t x, int16_t y, const eos_font_t *f,
                     eos_color_t c, const char *s, int16_t max_w);

// ------------------------------------------------------------------ table

int              eos_app_count(void);              // EOS_APP_COUNT
const eos_app_t *eos_app_at(int i);                // NULL out of range
const eos_app_t *eos_app_by_id(const char *id);    // NULL for an unknown id
int              eos_app_index_of(const char *id); // -1 for an unknown id

// True when every row has an id, a name and a draw function and no two rows
// share an id. Cheap enough to assert at boot and it is what stops a launcher
// from offering a window that cannot be drawn.
bool eos_app_table_ok(void);

// ---------------------------------------------------------------- wiring
//
// What the boot glue hands the apps. All four may be NULL, and each app says
// so in its own tile rather than drawing a blank: a board with no settings
// store still has a Settings window, and it reads "no settings".
//
// `h` is the server, and it is here for one reason. The megabrain relay is
// exposed as four function pointers on eos_httpd_ports_t — ask, read, cancel,
// status — and they are the ONLY lock-protected way into the brain task from
// another thread. The Chat window uses the same four the web app does, so
// there is one relay and not two, and a reply cannot be half in the browser
// and half on the glass.
void eos_app_bind(eos_httpd_t *h, const eos_settings_t *set,
                  const eos_board_t *b, eos_buddy_t *buddy);

// Everything that moves. Called once per pass of the OS loop, on the task that
// draws, and never from a handler. Apps use `v` to find out whether they are
// on the glass: the Files window does no directory scan while it is behind a
// tab, and the Party window does not drive the LED for a screen nobody is
// looking at.
void eos_app_tick(const struct eos_shell_view *v, uint32_t now_ms);

// Declares damage for every visible app whose picture changed since the last
// call, and returns true when it declared any. Call it BEFORE opening the
// frame, like all damage. This is what puts a streaming reply on the panel
// without the loop having to know that chat exists.
bool eos_app_damage(const struct eos_shell_view *v);

// True while a visible window is mid-animation and the loop should run at the
// avatar's rate rather than the idle one. Cheap: two flags the tick left.
bool eos_app_wants_fast(void);

// Offers one event to the app owning the focused window. False when that app
// took no keys, which is the signal to drop the event.
bool eos_app_key(uint16_t app_id, const eos_event_t *e);

// What every app's state costs in static RAM, reported rather than measured
// because BSS is claimed before app_main runs and never shows up as a heap
// step.
uint32_t eos_app_bss_bytes(void);

// ------------------------------------------------- what each app implements
//
// One row, one function. They are declared here rather than in nine headers
// because the table is the only caller and a header per app would be nine
// files that say one line each.

void eos_app_draw_clock(const eos_app_ctx_t *c, eos_rect_t r);
void eos_app_draw_board(const eos_app_ctx_t *c, eos_rect_t r);
void eos_app_draw_heap(const eos_app_ctx_t *c, eos_rect_t r);
void eos_app_draw_keys(const eos_app_ctx_t *c, eos_rect_t r);
void eos_app_draw_settings(const eos_app_ctx_t *c, eos_rect_t r);

// In eos_shell_draw.c, and it is the one body that is not in an app file. The
// avatar is rendered into an offscreen box BEFORE the frame opens, because
// eos_buddy_render() reorders the model in place and the scene is replayed once
// per band; that machinery and this body have to see the same box.
void eos_app_draw_buddy(const eos_app_ctx_t *c, eos_rect_t r);

void eos_app_draw_chat(const eos_app_ctx_t *c, eos_rect_t r);
bool eos_app_chat_key(const eos_event_t *e);
void eos_app_chat_bind(eos_httpd_t *h);
void eos_app_chat_tick(uint32_t now_ms);
bool eos_app_chat_take_dirty(void);
uint32_t eos_app_chat_bytes(void);

void eos_app_draw_files(const eos_app_ctx_t *c, eos_rect_t r);
bool eos_app_files_key(const eos_event_t *e);
void eos_app_files_tick(bool visible, uint32_t now_ms);
bool eos_app_files_take_dirty(void);
uint32_t eos_app_files_bytes(void);

void eos_app_draw_media(const eos_app_ctx_t *c, eos_rect_t r);
bool eos_app_media_key(const eos_event_t *e);
bool eos_app_media_take_dirty(void);

void eos_app_draw_party(const eos_app_ctx_t *c, eos_rect_t r);
void eos_app_draw_camera(const eos_app_ctx_t *c, eos_rect_t r);
bool eos_app_party_key(const eos_event_t *e);

// Party owns the LED and the buddy's mood while it is on the glass and gives
// both back when it is not, which is why it is told rather than asked.
// `phase` is the number of milliseconds the party has been visible, and it is
// what every moving thing in that window is a pure function of — the draw
// reads it and never advances it.
void     eos_app_party_tick(bool visible, uint32_t now_ms, eos_buddy_t *buddy);
uint32_t eos_app_party_phase(void);
bool     eos_app_party_active(void);

// Where the settings and board pointers land. Set by eos_app_bind() and read
// by the two bodies that need them; NULL is a legal value and each body prints
// its own sentence about it.
const eos_settings_t *eos_app_settings(void);
const eos_board_t    *eos_app_board(void);

#endif // EOS_APP_REGISTRY_H
