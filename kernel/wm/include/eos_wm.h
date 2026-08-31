// eos_wm — penguinOS tiling window manager core.
//
// A binary space partition tree, one per workspace, laid out into a caller
// supplied rect. Pure logic: no allocation, no hardware, no LVGL. The same
// tree drives an SSD1306 at 128x64 and an ILI9488 at 480x320 — only the
// layout pass differs, because a split that cannot give both children the
// configured minimum tile size COLLAPSES INTO A TAB GROUP instead of
// producing two unusable slivers.
//
// That single rule is what makes tiling work on a 2.4" panel.

#ifndef EOS_WM_H
#define EOS_WM_H

#include <stdint.h>
#include <stdbool.h>

#define EOS_MAX_WINDOWS 16
#define EOS_MAX_NODES   (EOS_MAX_WINDOWS * 2 - 1)
#define EOS_WORKSPACES  9
#define EOS_NONE        (-1)

typedef struct { int16_t x, y, w, h; } eos_rect_t;

typedef enum { EOS_SPLIT_COLS = 0, EOS_SPLIT_ROWS = 1 } eos_split_t;
typedef enum { EOS_DIR_LEFT, EOS_DIR_RIGHT, EOS_DIR_UP, EOS_DIR_DOWN } eos_dir_t;

typedef struct {
    int16_t min_tile_w;  // a COLS split needs 2*min_tile_w + gap, else it tabs
    int16_t min_tile_h;  // a ROWS split needs 2*min_tile_h + gap, else it tabs
    int16_t gap;         // gap between tiles and around the screen edge
    int16_t bar_h;       // status bar reserved along the top
    int16_t tab_h;       // tab strip height drawn above a collapsed group
} eos_wm_cfg_t;

// One entry per window that belongs to the laid-out workspace. Windows inside
// a collapsed group are all reported so the shell can label every tab, but
// only the active one is `visible` and carries a content rect.
typedef struct {
    int16_t    win;
    uint16_t   app_id;
    eos_rect_t rect;       // content area; zero-sized when !visible
    bool       visible;
    bool       focused;
    int16_t    tab_group;  // owning split node, or EOS_NONE when freely tiled
    int8_t     tab_index;
    int8_t     tab_count;
    eos_rect_t tab_rect;   // the group's strip; zero-sized if it did not fit
} eos_tile_t;

typedef struct {
    uint8_t  kind;      // 0 = leaf, 1 = split
    int16_t  parent;
    int16_t  win;       // leaf only
    uint8_t  dir;       // split only, eos_split_t
    uint16_t ratio;     // split only, permille given to child[0]
    int16_t  child[2];  // split only
    uint8_t  last;      // split only, which child was focused most recently
} eos_node_t;

typedef struct {
    eos_wm_cfg_t cfg;
    eos_node_t   nodes[EOS_MAX_NODES];
    bool         node_used[EOS_MAX_NODES];
    struct {
        uint16_t app_id;
        bool     alive;
        int8_t   ws;
        int16_t  node;
    } win[EOS_MAX_WINDOWS];
    int16_t     root[EOS_WORKSPACES];
    int8_t      ws;
    int16_t     focus;
    eos_split_t next_split;
    bool        next_split_forced;
} eos_wm_t;

void eos_wm_init(eos_wm_t *wm, const eos_wm_cfg_t *cfg);

// Opens a window on the current workspace, splitting the focused tile. The
// split direction follows the focused tile's aspect unless eos_wm_set_split()
// forced one. Returns the window id, or EOS_NONE if full.
int  eos_wm_open(eos_wm_t *wm, uint16_t app_id, eos_rect_t screen);
bool eos_wm_close(eos_wm_t *wm, int win);

// Forces the direction of the NEXT open only (super+h / super+v).
void eos_wm_set_split(eos_wm_t *wm, eos_split_t dir);

bool eos_wm_focus_dir(eos_wm_t *wm, eos_dir_t dir, eos_rect_t screen);
bool eos_wm_focus_tab_next(eos_wm_t *wm, eos_rect_t screen);
void eos_wm_focus_win(eos_wm_t *wm, int win);

void eos_wm_goto_workspace(eos_wm_t *wm, int n);
bool eos_wm_move_to_workspace(eos_wm_t *wm, int win, int n);

// Grows/shrinks the focused tile within its parent split, in permille.
bool eos_wm_resize(eos_wm_t *wm, int16_t delta_permille);

// Lays the current workspace into `screen`. Returns the tile count written.
int  eos_wm_layout(const eos_wm_t *wm, eos_rect_t screen, eos_tile_t *out, int max);

#endif
