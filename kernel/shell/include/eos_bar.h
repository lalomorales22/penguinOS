// eos_bar — the status bar MODEL. It turns a snapshot of system state into a
// list of positioned segments carrying text, a theme colour role and an x/w in
// pixels. It draws nothing and it knows nothing about LVGL or the compositor;
// the renderer just walks the list.
//
// The whole design problem is width. A 480px ILI9488 fits everything spelled
// out; a 128px SSD1306 fits about twenty characters total. So every segment
// carries up to three written forms, longest to shortest, and the fitter runs
// in two passes: take segments in strict priority order at their SHORTEST form
// until one no longer fits, then spend whatever is left upgrading them back
// towards their longest. Once a segment is refused, every lower-priority
// segment is refused too - the surviving set is always a prefix of the
// priority order, never a hole in the middle.

#ifndef EOS_BAR_H
#define EOS_BAR_H

#include <stdint.h>
#include <stdbool.h>

#define EOS_BAR_TEXT_MAX 32
#define EOS_BAR_SEGS     7

// How many workspace pips the bar will draw. Deliberately a literal and not
// an include of eos_wm.h - the bar model must build with no window manager
// present - but it must stay equal to EOS_WORKSPACES.
#define EOS_WORKSPACE_PIPS 9

typedef enum {
    EOS_SEG_PIPS = 0,    // workspace pips: which are occupied, which is live
    EOS_SEG_TITLE,       // focused window title
    EOS_SEG_MOOD,        // buddy mood glyph
    EOS_SEG_HEAP,        // free heap
    EOS_SEG_BRAIN,       // megabrain reachability and model
    EOS_SEG_WIFI,        // wifi state and signal
    EOS_SEG_CLOCK
} eos_bar_seg_id_t;

// Colour roles, not colours. The theme owns the palette; the bar only says
// what a run of text means. Deliberately NOT an include of eos_theme.h - the
// bar model must build and be testable with no theme present - so the renderer
// maps these five onto eos_role_t itself:
//
//   FG -> EOS_ROLE_BAR_FG   MUTED -> EOS_ROLE_MUTED   ACCENT -> EOS_ROLE_ACCENT
//   OK -> EOS_ROLE_OK       WARN  -> EOS_ROLE_WARN
typedef enum {
    EOS_BAR_ROLE_FG = 0,
    EOS_BAR_ROLE_MUTED,
    EOS_BAR_ROLE_ACCENT,
    EOS_BAR_ROLE_OK,
    EOS_BAR_ROLE_WARN
} eos_bar_role_t;

typedef enum { EOS_ALIGN_LEFT = 0, EOS_ALIGN_CENTER, EOS_ALIGN_RIGHT } eos_bar_align_t;

typedef enum {
    EOS_WIFI_OFF = 0,
    EOS_WIFI_JOINING,
    EOS_WIFI_DOWN,
    EOS_WIFI_UP
} eos_bar_wifi_t;

// Deliberately the same seven values in the same order as the avatar's
// eos_buddy_state_t, so the renderer maps one to the other with a cast rather
// than a switch it will forget to update. The bar still does not include
// eos_buddy.h - it has no business depending on the avatar to draw text.
typedef enum {
    EOS_MOOD_IDLE = 0,
    EOS_MOOD_THINKING,
    EOS_MOOD_TALKING,
    EOS_MOOD_LISTENING,
    EOS_MOOD_SLEEPING,
    EOS_MOOD_HAPPY,
    EOS_MOOD_CONFUSED
} eos_bar_mood_t;

// A snapshot. The two string pointers are borrowed for the duration of the
// build call only; nothing is copied out of them beyond what fits a segment.
typedef struct {
    uint16_t       ws_occupied;   // bit n set = workspace n holds windows
    uint8_t        ws_active;
    const char    *title;         // NULL or "" drops the title segment
    eos_bar_wifi_t wifi;
    int8_t         wifi_rssi;     // dBm, meaningful only when wifi is UP
    bool           brain_up;
    const char    *brain_model;
    uint32_t       free_heap;     // bytes
    uint32_t       heap_warn;     // heap at or below this turns the segment WARN
    eos_bar_mood_t mood;
    uint8_t        hour, minute;  // 24h
    bool           clock_valid;   // no time source yet renders "--:--"
} eos_bar_status_t;

// Text measurement. `measure` exists because LVGL fonts are proportional and
// guessing their widths is how a bar ends up one pixel over the edge. Leave it
// NULL for the bitmap fonts on tier 0 and char_w is used instead.
typedef struct {
    int16_t  char_w;    // advance of one cell when `measure` is NULL
    int16_t  pad;       // pixels between two adjacent segments
    char     ellipsis;  // marks a truncated segment; '~' exists in every font
    int16_t (*measure)(const char *s, void *ud);
    void    *ud;
} eos_bar_metrics_t;

void eos_bar_metrics_init(eos_bar_metrics_t *m, int16_t char_w, int16_t pad);

typedef struct {
    char            text[EOS_BAR_TEXT_MAX];
    eos_bar_seg_id_t id;
    eos_bar_role_t   role;
    eos_bar_align_t  align;
    uint8_t          priority;   // higher survives longer; see eos_bar_priority
    int16_t          x, w;       // pixels, already laid out inside the bar
} eos_bar_seg_t;

void eos_bar_status_init(eos_bar_status_t *st);

// Builds the bar for `width` pixels. Returns the number of segments written,
// in left-to-right order, each with x and w set. Never writes past `width` and
// never keeps a segment while dropping a higher-priority one.
//
// `max` bounds the answer the same way `width` does - it is applied while
// choosing, not by cutting the tail off the finished list - so a caller with
// room for three segments gets the three that matter, not the leftmost three.
//
// Costs about 900 bytes of stack for the candidate table. No allocation.
int eos_bar_build(const eos_bar_status_t *st, const eos_bar_metrics_t *m,
                  int16_t width, eos_bar_seg_t *out, int max);

uint8_t     eos_bar_priority(eos_bar_seg_id_t id);
const char *eos_bar_seg_name(eos_bar_seg_id_t id);
const char *eos_bar_mood_glyph(eos_bar_mood_t m);

#endif
