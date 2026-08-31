// The app list, the selection and the scroll. See eos_launcher.h for why the
// strings are borrowed and why the geometry is stored rather than recomputed.
//
// Every function that can move the selection ends in scroll_to_sel(), and that
// is the whole scrolling design: there is no separate scroll command and no
// way to scroll the list away from the highlight. A launcher whose view and
// whose selection can disagree is a launcher where enter opens something the
// user cannot see.

#include "eos_launcher.h"

#include <string.h>

void eos_launcher_init(eos_launcher_t *l)
{
    if (!l) return;
    memset(l, 0, sizeof(*l));
    l->sel = EOS_LAUNCHER_NONE;
    // A launcher that has never been laid out still has to answer
    // eos_launcher_rows(). One row is the honest answer before the renderer
    // has said how tall the panel is, and it makes every clamp below safe.
    l->geom.rows  = 1;
    l->geom.row_h = 1;
}

bool eos_launcher_add(eos_launcher_t *l, const char *name, const char *desc,
                      uint16_t app_id)
{
    eos_launcher_item_t *it;

    if (!l || !name || !name[0]) return false;
    if (l->count >= EOS_LAUNCHER_MAX) return false;

    it = &l->item[l->count++];
    it->name   = name;
    it->desc   = (desc && desc[0]) ? desc : NULL;
    it->app_id = app_id;

    if (l->sel == EOS_LAUNCHER_NONE) l->sel = 0;
    return true;
}

void eos_launcher_clear(eos_launcher_t *l)
{
    if (!l) return;
    memset(l->item, 0, sizeof(l->item));
    l->count = 0;
    l->sel   = EOS_LAUNCHER_NONE;
    l->top   = 0;
}

// ---------------------------------------------------------------- scrolling

static int rows_of(const eos_launcher_t *l)
{
    return (l->geom.rows > 0) ? (int)l->geom.rows : 1;
}

// The one invariant: the selection is on screen, and the view never scrolls
// past the end of a list that fits.
static void scroll_to_sel(eos_launcher_t *l)
{
    int rows = rows_of(l);
    int last;

    if (l->count == 0 || l->sel == EOS_LAUNCHER_NONE) { l->top = 0; return; }
    if (l->count <= rows)                             { l->top = 0; return; }

    if (l->sel < l->top)                l->top = l->sel;
    if (l->sel > l->top + rows - 1)     l->top = (int16_t)(l->sel - rows + 1);

    last = l->count - rows;                 // > 0 here
    if (l->top > last) l->top = (int16_t)last;
    if (l->top < 0)    l->top = 0;
}

// --------------------------------------------------------------- open state

void eos_launcher_open(eos_launcher_t *l)
{
    if (!l) return;
    l->open = true;
    l->sel  = (l->count > 0) ? 0 : EOS_LAUNCHER_NONE;
    l->top  = 0;
}

void eos_launcher_close(eos_launcher_t *l)
{
    if (!l) return;
    l->open = false;
}

bool eos_launcher_toggle(eos_launcher_t *l)
{
    if (!l) return false;
    if (l->open) eos_launcher_close(l);
    else         eos_launcher_open(l);
    return l->open;
}

void eos_launcher_set_open(eos_launcher_t *l, bool open)
{
    if (!l) return;
    // Re-opening an already-open launcher must not reset the selection: this
    // is called every pass by a caller mirroring eos_shell_state_t, and
    // resetting on every mirror would pin the highlight to the first row.
    if (open == l->open) return;
    if (open) eos_launcher_open(l);
    else      eos_launcher_close(l);
}

// -------------------------------------------------------------------- query

int eos_launcher_count(const eos_launcher_t *l)    { return l ? (int)l->count : 0; }
int eos_launcher_top(const eos_launcher_t *l)      { return l ? (int)l->top   : 0; }
int eos_launcher_rows(const eos_launcher_t *l)     { return l ? rows_of(l)    : 1; }

int eos_launcher_selected(const eos_launcher_t *l)
{
    if (!l || l->count == 0) return EOS_LAUNCHER_NONE;
    if (l->sel < 0 || l->sel >= (int16_t)l->count) return EOS_LAUNCHER_NONE;
    return (int)l->sel;
}

const eos_launcher_item_t *eos_launcher_item(const eos_launcher_t *l, int i)
{
    if (!l || i < 0 || i >= (int)l->count) return NULL;
    return &l->item[i];
}

// ------------------------------------------------------------------- moving

bool eos_launcher_move(eos_launcher_t *l, int delta)
{
    int n, cur, next;

    if (!l || l->count == 0 || delta == 0) return false;
    n   = (int)l->count;
    cur = (l->sel >= 0 && l->sel < n) ? (int)l->sel : 0;

    // Wrap at both ends. The modulo is written this way rather than with a
    // while loop because a page-down on a two-item list is a delta larger than
    // the list, and one subtraction would not bring it back in range.
    next = (cur + delta) % n;
    if (next < 0) next += n;

    l->sel = (int16_t)next;
    scroll_to_sel(l);
    return next != cur;
}

bool eos_launcher_select(eos_launcher_t *l, int index)
{
    if (!l || index < 0 || index >= (int)l->count) return false;
    if (l->sel == (int16_t)index) { scroll_to_sel(l); return false; }
    l->sel = (int16_t)index;
    scroll_to_sel(l);
    return true;
}

// --------------------------------------------------------------------- keys

static eos_launcher_res_t res(eos_launcher_act_t a, bool changed, uint16_t app_id)
{
    eos_launcher_res_t r;
    r.act     = a;
    r.changed = changed;
    r.app_id  = app_id;
    return r;
}

// Enter, and the click that means the same thing. Closing before returning is
// deliberate: the caller opens a window, which relayouts every tile, and an
// overlay still flagged open would be painted over the result of the thing it
// just launched.
static eos_launcher_res_t launch_selected(eos_launcher_t *l)
{
    int s = eos_launcher_selected(l);
    if (s == EOS_LAUNCHER_NONE) return res(EOS_LAUNCHER_EAT, false, 0);
    eos_launcher_close(l);
    return res(EOS_LAUNCHER_LAUNCH, true, l->item[s].app_id);
}

eos_launcher_res_t eos_launcher_key(eos_launcher_t *l, uint16_t key, uint8_t mods)
{
    int page;

    if (!l || !l->open) return res(EOS_LAUNCHER_PASS, false, 0);

    // Super chords are eos_keys'. Passing them back is what lets a second
    // super+space close the launcher through the same path that opened it,
    // and it keeps super+q closing a window from inside the overlay.
    if (mods & EOS_MOD_SUPER) return res(EOS_LAUNCHER_PASS, false, 0);

    page = rows_of(l) - 1;
    if (page < 1) page = 1;

    switch (key) {
    case EOS_KEY_UP:
    case EOS_KEY_K:
        return res(EOS_LAUNCHER_EAT, eos_launcher_move(l, -1), 0);

    case EOS_KEY_DOWN:
    case EOS_KEY_J:
    case EOS_KEY_TAB:
        return res(EOS_LAUNCHER_EAT, eos_launcher_move(l, +1), 0);

    case EOS_KEY_PGUP:
        return res(EOS_LAUNCHER_EAT, eos_launcher_move(l, -page), 0);

    case EOS_KEY_PGDN:
        return res(EOS_LAUNCHER_EAT, eos_launcher_move(l, +page), 0);

    case EOS_KEY_HOME:
        return res(EOS_LAUNCHER_EAT, eos_launcher_select(l, 0), 0);

    case EOS_KEY_END:
        return res(EOS_LAUNCHER_EAT, eos_launcher_select(l, (int)l->count - 1), 0);

    case EOS_KEY_ENTER:
        return launch_selected(l);

    case EOS_KEY_ESC:
        eos_launcher_close(l);
        return res(EOS_LAUNCHER_CLOSE, true, 0);

    default:
        break;
    }
    return res(EOS_LAUNCHER_PASS, false, 0);
}

// ----------------------------------------------------------------- geometry

void eos_launcher_layout(eos_launcher_geom_t *g, int16_t screen_w,
                         int16_t screen_h, int16_t font_h)
{
    int16_t margin, pad, avail;
    int     rows;

    if (!g) return;
    memset(g, 0, sizeof(*g));

    if (font_h < 4) font_h = 4;      // no shipped face is smaller; be safe anyway
    if (screen_w < 16) screen_w = 16;
    if (screen_h < 16) screen_h = 16;

    // The overlay is inset rather than full-bleed so the desktop still shows
    // at the edges: on a 240x240 panel with no window chrome an opaque
    // full-screen list reads as a mode change rather than as a menu.
    margin = (int16_t)(screen_w >= 200 ? 8 : 2);
    pad    = (int16_t)(screen_w >= 200 ? 4 : 1);

    g->x = margin;
    g->y = margin;
    g->w = (int16_t)(screen_w - 2 * margin);
    g->h = (int16_t)(screen_h - 2 * margin);
    g->pad = pad;

    g->title_y = (int16_t)(g->y + pad);
    g->rule_y  = (int16_t)(g->title_y + font_h + 2);
    g->list_y  = (int16_t)(g->rule_y + 3);

    // Four pixels of leading. Rows any tighter and the accent bar behind the
    // selection touches the glyphs above and below it on a 6x8 face.
    g->row_h = (int16_t)(font_h + 4);

    avail = (int16_t)((g->y + g->h - pad) - g->list_y);
    rows  = (avail > 0) ? (avail / g->row_h) : 0;
    if (rows < 1)   rows = 1;
    if (rows > 255) rows = 255;
    g->rows = (uint8_t)rows;
}

void eos_launcher_set_geom(eos_launcher_t *l, const eos_launcher_geom_t *g)
{
    if (!l || !g) return;
    l->geom = *g;
    if (l->geom.rows  == 0) l->geom.rows  = 1;
    if (l->geom.row_h <= 0) l->geom.row_h = 1;
    scroll_to_sel(l);
}

// ------------------------------------------------------------------ pointer

int eos_launcher_hit(const eos_launcher_t *l, int16_t x, int16_t y)
{
    const eos_launcher_geom_t *g;
    int row, idx;

    if (!l || !l->open || l->count == 0) return EOS_LAUNCHER_NONE;
    g = &l->geom;
    if (g->row_h <= 0) return EOS_LAUNCHER_NONE;

    if (x < g->x || x >= (int16_t)(g->x + g->w)) return EOS_LAUNCHER_NONE;
    if (y < g->list_y) return EOS_LAUNCHER_NONE;                  // heading, or above
    if (y >= (int16_t)(g->y + g->h)) return EOS_LAUNCHER_NONE;

    row = (y - g->list_y) / g->row_h;
    if (row < 0 || row >= rows_of(l)) return EOS_LAUNCHER_NONE;

    idx = (int)l->top + row;
    if (idx >= (int)l->count) return EOS_LAUNCHER_NONE;           // past the last item
    return idx;
}

bool eos_launcher_hover(eos_launcher_t *l, int16_t x, int16_t y)
{
    int idx = eos_launcher_hit(l, x, y);
    if (idx == EOS_LAUNCHER_NONE) return false;   // off a row leaves the highlight put
    return eos_launcher_select(l, idx);
}

eos_launcher_res_t eos_launcher_click(eos_launcher_t *l, int16_t x, int16_t y)
{
    const eos_launcher_geom_t *g;
    int idx;

    if (!l || !l->open) return res(EOS_LAUNCHER_PASS, false, 0);
    g = &l->geom;

    if (x < g->x || x >= (int16_t)(g->x + g->w) ||
        y < g->y || y >= (int16_t)(g->y + g->h)) {
        eos_launcher_close(l);
        return res(EOS_LAUNCHER_CLOSE, true, 0);
    }

    idx = eos_launcher_hit(l, x, y);
    if (idx == EOS_LAUNCHER_NONE) return res(EOS_LAUNCHER_EAT, false, 0);

    eos_launcher_select(l, idx);
    return launch_selected(l);
}
