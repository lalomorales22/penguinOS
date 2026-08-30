// eos_theme — one JSON file on the microSD reskins the entire OS.
//
// A theme is a fixed set of named colour roles, the sixteen ANSI terminal
// colours, and a handful of scalars the shell and the window manager consume.
// It is read off the card at boot, so changing how ESP-OS looks never means
// rebuilding it. The parser is hand-rolled recursive descent over a buffer the
// caller owns: no allocation anywhere, no third-party library, and no parsed
// document tree, because the tier 0 board sits at roughly 20KB of free heap
// with WiFi and BLE up and cannot afford any of the three.
//
// The rule that outranks everything else here: a bad theme file must never
// stop the board booting. Truncated file, bad hex, missing role, garbage
// bytes, empty file — every one of them leaves the caller holding the
// compiled-in default and returns a reason code. There is no partial theme.
//
// Three resolvers, because ESP-OS draws on three kinds of panel: RGB565 for
// the LVGL tiers and the SPI LCDs, an 8-bit palette index for the tier 0
// indexed compositor, and one lit/unlit bit for the SSD1306.

#ifndef EOS_THEME_H
#define EOS_THEME_H

#include <stdint.h>
#include <stdbool.h>

#define EOS_THEME_NAME_MAX 32
#define EOS_THEME_FONT_MAX 16
#define EOS_ANSI_COUNT     16
#define EOS_PAL_SIZE       256

// The 256-entry palette the tier 0 compositor programs as its CLUT. The layout
// is fixed so drawing code can reach a shade without a search:
//
//   index    | contents
//   ---------|-------------------------------------------------------------
//   0..15    | the colour roles, in eos_role_t order, exact
//   16..31   | the sixteen ANSI colours, exact
//   32..47   | 16-step ramp, bg at 32 .. text at 47
//   48..63   | 16-step ramp, bg at 48 .. accent at 63
//   64..255  | 6x8x4 RGB cube for arbitrary content (photos, app pixels)
//
// Green gets eight levels and blue four because that is where the eye is and
// is not. Worst-case cube error is about 42 counts on blue.
//
// One cell of that cube is unreachable. Slot 255 is EOS_COLOR_NONE in
// eos_display.h — the transparency sentinel that never resolves to a pixel —
// so neither eos_theme_cube_index() nor eos_theme_index() will ever return it,
// and pure white quantises to slot 251, (255,219,255), one green step down.
// pal565[255] still holds white so the CLUT is a complete cube for anything
// that reads it directly; nothing may DRAW with the index.
#define EOS_PAL_ROLE_BASE    0
#define EOS_PAL_ANSI_BASE   16
#define EOS_PAL_TEXT_RAMP   32
#define EOS_PAL_ACCENT_RAMP 48
#define EOS_PAL_RAMP_STEPS  16
#define EOS_PAL_CUBE_BASE   64
#define EOS_PAL_CUBE_R       6
#define EOS_PAL_CUBE_G       8
#define EOS_PAL_CUBE_B       4

// The one cube slot the display HAL has reserved. Kept as a name so the tie to
// EOS_COLOR_NONE survives someone changing the cube dimensions above.
#define EOS_PAL_CUBE_NONE  (EOS_PAL_SIZE - 1)

typedef struct { uint8_t r, g, b; } eos_rgb_t;

typedef enum {
    EOS_ROLE_BG = 0,          // desktop / window interior
    EOS_ROLE_SURFACE,         // raised panel, list background
    EOS_ROLE_OVERLAY,         // menus, dialogs, the launcher
    EOS_ROLE_TEXT,            // primary foreground
    EOS_ROLE_MUTED,           // secondary text, disabled, comments
    EOS_ROLE_ACCENT,          // focus colour; also the avatar accent
    EOS_ROLE_ACCENT_ALT,      // secondary highlight
    EOS_ROLE_OK,
    EOS_ROLE_WARN,
    EOS_ROLE_ERR,
    EOS_ROLE_BORDER_FOCUSED,
    EOS_ROLE_BORDER_UNFOCUSED,
    EOS_ROLE_BAR_BG,
    EOS_ROLE_BAR_FG,
    EOS_ROLE_TAB_ACTIVE,      // the tab a collapsed group is showing
    EOS_ROLE_TAB_INACTIVE,
    EOS_ROLE_COUNT
} eos_role_t;

typedef enum {
    EOS_ANSI_BLACK = 0, EOS_ANSI_RED,     EOS_ANSI_GREEN,   EOS_ANSI_YELLOW,
    EOS_ANSI_BLUE,      EOS_ANSI_MAGENTA, EOS_ANSI_CYAN,    EOS_ANSI_WHITE,
    EOS_ANSI_BR_BLACK,  EOS_ANSI_BR_RED,  EOS_ANSI_BR_GREEN, EOS_ANSI_BR_YELLOW,
    EOS_ANSI_BR_BLUE,   EOS_ANSI_BR_MAGENTA, EOS_ANSI_BR_CYAN, EOS_ANSI_BR_WHITE
} eos_ansi_t;

// Scalars. gap, bar_h and tab_h are copied straight into eos_wm_cfg_t by the
// shell; the theme deliberately does not include min_tile_w/min_tile_h, which
// belong to the panel, not to the look. `font` only names a face — resolving
// it is the renderer's job, and an unknown name falls back to its default.
//
// The names are the eos_font_id_t values in eos_display.h, lowercased without
// the EOS_FONT_ prefix: "tiny" (4x6), "small" (6x8, the workhorse), "med"
// (8x13), "big" (12x20). The theme does not include eos_display.h to say so —
// it is a string either way — but a name outside that set resolves to nothing,
// so keep the two in step. The font tables themselves are not written yet.
typedef struct {
    int16_t gap;        // 0..32
    int16_t border;     // 0..8
    int16_t bar_h;      // 0..64
    int16_t tab_h;      // 0..64
    int16_t radius;     // 0..32, corner rounding; tier 0 may ignore it
    char    font[EOS_THEME_FONT_MAX];
} eos_theme_metrics_t;

typedef struct {
    char                name[EOS_THEME_NAME_MAX];
    eos_rgb_t           role[EOS_ROLE_COUNT];
    eos_rgb_t           ansi[EOS_ANSI_COUNT];
    eos_theme_metrics_t m;

    // Derived by the loader. Never write these by hand; eos_theme_parse() and
    // eos_theme_default() are the only things that may.
    uint16_t pal565[EOS_PAL_SIZE];
    uint8_t  role_idx[EOS_ROLE_COUNT];   // palette slot of each role
    uint8_t  ansi_idx[EOS_ANSI_COUNT];
    uint8_t  bg_luma;                    // luma of EOS_ROLE_BG
    uint8_t  mono_threshold;             // see eos_theme_mono()
    uint16_t provided_roles;             // bit per role that the file supplied
    uint16_t provided_ansi;              // bit per ansi colour, likewise
} eos_theme_t;

typedef enum {
    EOS_THEME_OK = 0,
    EOS_THEME_ERR_ARGS,      // no output struct
    EOS_THEME_ERR_EMPTY,     // null buffer, or nothing in it
    EOS_THEME_ERR_SYNTAX,    // not JSON, truncated, or trailing junk
    EOS_THEME_ERR_TYPE,      // right key, wrong kind of value
    EOS_THEME_ERR_COLOR,     // not #rgb or #rrggbb
    EOS_THEME_ERR_MISSING,   // a role or an ansi colour was not supplied
    EOS_THEME_ERR_DEPTH      // nested past EOS_THEME_MAX_DEPTH
} eos_theme_err_t;

#define EOS_THEME_MAX_DEPTH 8

// ---------------------------------------------------------------- file format
//
// JSON with two concessions to people editing on an SD card in a text editor:
// // and /* */ comments are skipped, and a trailing comma before a } or ] is
// allowed. A UTF-8 BOM is skipped. Unknown keys are ignored, so a newer theme
// file still loads on older firmware.
//
//   {
//     "name": "tokyonight",
//     "font": "med",
//     "metrics": { "gap": 4, "border": 1, "bar_h": 14, "tab_h": 12,
//                  "radius": 3 },
//     "colors": { "bg": "#1a1b26", ... all sixteen roles ... },
//     "ansi":   { "black": "#15161e", ... all sixteen colours ... }
//   }
//
// "colors" keys are the eos_role_t names lowercased without the EOS_ROLE_
// prefix: bg surface overlay text muted accent accent_alt ok warn err
// border_focused border_unfocused bar_bg bar_fg tab_active tab_inactive.
//
// "ansi" keys are: black red green yellow blue magenta cyan white and the
// same eight again prefixed bright_.
//
// All sixteen roles and all sixteen ansi colours are REQUIRED. A file that
// omits one is a half-theme and renders as something broken rather than
// something plain, so it is rejected outright and the default is kept.
// "name", "font" and "metrics" are optional; metrics out of range clamp
// rather than fail, because a silly gap cannot corrupt anything.

// Fills `out` with the compiled-in default: a neutral slate theme, named
// "eos-default", deliberately unlike any of the four shipped themes so that a
// fallback is obvious on the panel rather than silent. Always succeeds.
void eos_theme_default(eos_theme_t *out);

// Parses `len` bytes at `buf`. Reads nothing outside [buf, buf+len); embedded
// NULs are data, not terminators. On success `out` holds the parsed theme with
// its palette and derived indices built. On ANY failure `out` holds the
// default, unchanged from what eos_theme_default() would have written.
eos_theme_err_t eos_theme_parse(eos_theme_t *out, const char *buf, int len);

const char *eos_theme_strerror(eos_theme_err_t e);
const char *eos_theme_role_name(eos_role_t r);   // NULL if out of range
const char *eos_theme_ansi_name(int i);          // NULL if out of range

// ------------------------------------------------------------------ resolvers

// Straight 5-6-5 truncation. Byte order is the driver's problem, not ours.
uint16_t eos_theme_rgb565(eos_rgb_t c);
eos_rgb_t eos_theme_un565(uint16_t v);           // decode, bits replicated

// ITU-R BT.601 luma, 0..255.
uint8_t eos_theme_luma(eos_rgb_t c);

// Nearest entry in this theme's palette, plain squared RGB distance over the
// 255 drawable entries — slot EOS_PAL_CUBE_NONE is skipped. Fine for UI; for
// bulk pixel conversion use eos_theme_cube_index.
uint8_t eos_theme_index(const eos_theme_t *t, eos_rgb_t c);

// Direct cube slot, no search: the fast path for blitting arbitrary images.
// Never returns EOS_PAL_CUBE_NONE; see the palette layout note above.
uint8_t eos_theme_cube_index(eos_rgb_t c);

// True where the pixel should be lit on a 1bpp panel. The threshold is a
// luminance distance from this theme's own background rather than an absolute
// mid-grey, because gruvbox's err sits at luma 124 and its ansi blue at 114,
// and an absolute mid-grey would drop both into the background on the SSD1306.
// Anchoring it means bg is always unlit and text is always lit, whatever the
// theme does — including a light theme, where an absolute threshold inverts
// the panel outright.
bool eos_theme_mono(const eos_theme_t *t, eos_rgb_t c);

// ------------------------------------------------------------- role shortcuts

eos_rgb_t eos_theme_role_rgb  (const eos_theme_t *t, eos_role_t r);
uint16_t  eos_theme_role_565  (const eos_theme_t *t, eos_role_t r);
uint8_t   eos_theme_role_index(const eos_theme_t *t, eos_role_t r);
bool      eos_theme_role_mono (const eos_theme_t *t, eos_role_t r);

eos_rgb_t eos_theme_ansi_rgb  (const eos_theme_t *t, int i);
uint16_t  eos_theme_ansi_565  (const eos_theme_t *t, int i);
uint8_t   eos_theme_ansi_index(const eos_theme_t *t, int i);

// The CLUT itself: EOS_PAL_SIZE entries, hand it to the compositor.
const uint16_t *eos_theme_palette565(const eos_theme_t *t);
eos_rgb_t       eos_theme_palette_rgb(const eos_theme_t *t, uint8_t idx);

// Ramp slots, step clamped to 0..EOS_PAL_RAMP_STEPS-1. Step 0 is bg.
uint8_t eos_theme_text_ramp(int step);
uint8_t eos_theme_accent_ramp(int step);

const char *eos_theme_font(const eos_theme_t *t);

#endif
