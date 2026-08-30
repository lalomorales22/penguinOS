// eos_keys — the keybind engine. A flat table mapping (modifiers, keycode) to
// an action plus one integer argument, dispatched against the finished eos_wm
// API.
//
// Keycodes and modifier bits are NOT redefined here: they come from
// kernel/hal's eos_input.h, which already carries the USB HID usage page
// verbatim. One keycode space means a bind fires the same way whether it came
// from the BLE keyboard, a GPIO button or a tap on the phone page.
//
// The non-obvious constraint: keys.json from the SD card is applied ON TOP of
// the compiled-in defaults and is committed only if the whole file parses. A
// truncated or hand-mangled card therefore cannot leave the board holding a
// table you can no longer type your way out of.

#ifndef EOS_KEYS_H
#define EOS_KEYS_H

#include <stdint.h>
#include <stdbool.h>
#include "eos_wm.h"
#include "eos_input.h"
#include "eos_bar.h"

#define EOS_MAX_BINDS 72

// ------------------------------------------------------------------ input
//
// The table stores modifiers in eos_input.h's COLLAPSED form - EOS_MOD_SUPER
// is both GUI bits, EOS_MOD_SHIFT is both shift bits - and matches them
// exactly. Exactly, not "at least": super+shift+1 must not also fire super+1,
// which is why this does not use eos_bind_match().

// Collapses a raw HID report modifier byte into that form. Idempotent, so it
// is safe to run over a byte that is already collapsed.
uint8_t eos_keys_mods_from_hid(uint8_t hid_mods);

// ----------------------------------------------------------------- actions

typedef enum {
    EOS_ACT_NONE = 0,        // an explicit unbind; also what lookup misses give
    EOS_ACT_SPAWN,           // arg = app id, opened as a new window
    EOS_ACT_CLOSE,           // closes the focused window
    EOS_ACT_SPLIT_COLS,      // force the NEXT open to split side by side
    EOS_ACT_SPLIT_ROWS,      // force the NEXT open to stack
    EOS_ACT_FOCUS_LEFT,
    EOS_ACT_FOCUS_RIGHT,
    EOS_ACT_FOCUS_UP,
    EOS_ACT_FOCUS_DOWN,
    EOS_ACT_MOVE_LEFT,       // swap the focused window with its neighbour
    EOS_ACT_MOVE_RIGHT,
    EOS_ACT_MOVE_UP,
    EOS_ACT_MOVE_DOWN,
    EOS_ACT_WORKSPACE,       // arg = workspace index, 0..EOS_WORKSPACES-1
    EOS_ACT_MOVE_TO_WS,      // arg = workspace index
    EOS_ACT_TAB_NEXT,        // next window inside a collapsed tab group
    EOS_ACT_RESIZE,          // arg = permille delta for the focused tile
    EOS_ACT_LAUNCHER,        // toggle the app launcher overlay
    EOS_ACT_TOGGLE_BAR,
    EOS_ACT_CYCLE_THEME,
    EOS_ACT_LOCK,            // toggle lock/sleep
    EOS_ACT__COUNT
} eos_action_t;

// Named eos_keybind_t, not eos_bind_t: the HAL already owns that name for its
// event matcher, and the two are different things.
typedef struct {
    uint8_t      mods;
    uint16_t     key;
    uint8_t      action;     // eos_action_t
    int16_t      arg;
} eos_keybind_t;

typedef struct {
    eos_keybind_t binds[EOS_MAX_BINDS];
    uint8_t       count;
} eos_keymap_t;

// The shell state a keybind can move that is not the window tree. The board
// layer owns the consequences: it decides what "locked" or "theme 2" mean.
typedef struct {
    bool    bar_visible;
    bool    launcher_open;
    bool    locked;
    uint8_t theme;
    uint8_t theme_count;
} eos_shell_state_t;

void eos_shell_state_init(eos_shell_state_t *st, uint8_t theme_count);

typedef struct {
    bool         handled;    // a bind existed and the shell consumed the key
    bool         changed;    // something actually moved; the screen needs work
    eos_action_t action;
    int16_t      arg;
    int16_t      win;        // window opened or closed, else EOS_NONE
} eos_key_result_t;

// ------------------------------------------------------------------- table

// Loads the compiled-in table: Omarchy/Hyprland muscle memory. See README for
// the full list and for the one deliberate deviation (super+h is focus-left,
// so forcing a split lives on super+ctrl+h / super+ctrl+v).
void eos_keys_defaults(eos_keymap_t *km);

void eos_keys_clear(eos_keymap_t *km);

// Adds, or replaces in place if (mods,key) is already bound. EOS_ACT_NONE
// removes the bind. Returns false only when the table is full.
bool eos_keys_bind(eos_keymap_t *km, uint8_t mods, uint16_t key,
                   eos_action_t action, int16_t arg);

const eos_keybind_t *eos_keys_lookup(const eos_keymap_t *km, uint8_t mods, uint16_t key);

// "super+shift+l" -> modifier bits and a HID usage id. Modifier spellings:
// super/mod/win/cmd/gui, shift, ctrl/control, alt/opt. Case insensitive.
bool eos_keys_parse_chord(const char *chord, uint8_t *mods, uint16_t *key);

// ------------------------------------------------------------------- json

typedef enum {
    EOS_KEYS_OK = 0,
    EOS_KEYS_ERR_SYNTAX,     // malformed json
    EOS_KEYS_ERR_KEYNAME,    // "keys" names a chord we cannot resolve
    EOS_KEYS_ERR_ACTION,     // "action" names something we do not implement
    EOS_KEYS_ERR_FULL,       // more binds than EOS_MAX_BINDS
    EOS_KEYS_ERR_IO,         // file missing or unreadable
    EOS_KEYS_ERR_TOO_BIG     // file larger than the scratch buffer
} eos_keys_err_t;

typedef struct {
    eos_keys_err_t err;
    int            offset;   // byte offset in the json at the point of failure
    int            applied;  // binds that parsed before the failure
} eos_keys_load_t;

// Applies overrides from a NUL-terminated json document on top of `km`.
//
//   { "binds": [ { "keys": "super+shift+l", "action": "move_right" },
//                { "keys": "super+f",       "action": "spawn", "arg": 2 },
//                { "keys": "super+b",       "action": "none" } ] }
//
// A bare top-level array works too. Parsing happens into a private copy, so on
// any error `km` is left exactly as it was.
eos_keys_load_t eos_keys_load_json(eos_keymap_t *km, const char *json);

// Same, reading the file into a caller-owned scratch buffer first. No malloc:
// a file that does not fit in `scratch` is refused, not truncated.
eos_keys_load_t eos_keys_load_file(eos_keymap_t *km, const char *path,
                                   char *scratch, int scratch_len);

const char *eos_keys_err_str(eos_keys_err_t e);
const char *eos_keys_action_name(eos_action_t a);

// Renders a bind's chord as "super+shift+l". Returns the length written.
int eos_keys_format(const eos_keybind_t *b, char *out, int n);

// ---------------------------------------------------------------- dispatch

// Runs one action against the window manager and the shell state. Exposed on
// its own so a launcher list or a touch gesture can trigger the same paths.
eos_key_result_t eos_keys_apply(eos_wm_t *wm, eos_shell_state_t *st,
                                eos_rect_t screen, eos_action_t action, int16_t arg);

// The main entry: one keypress in, one dispatched action out.
//
// While locked, everything but EOS_ACT_LOCK is swallowed. While the launcher
// is open, only chords carrying SUPER are dispatched; everything else comes
// back unhandled so the launcher's own text field gets it.
eos_key_result_t eos_keys_feed(const eos_keymap_t *km, eos_wm_t *wm,
                               eos_shell_state_t *st, eos_rect_t screen,
                               uint8_t mods, uint16_t key);

// Fills the bar status fields that only the window manager knows: which
// workspaces hold windows, which one is current, and the focused window's
// title taken from `app_names[app_id]`. Everything else in the status struct
// comes from elsewhere and is left untouched.
void eos_shell_status_sync(const eos_wm_t *wm, eos_bar_status_t *st,
                           const char *const *app_names, int app_count);

#endif
