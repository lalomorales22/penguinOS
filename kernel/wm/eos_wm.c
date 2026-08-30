#include "eos_wm.h"
#include <string.h>

static const eos_rect_t ZERO_RECT = {0, 0, 0, 0};

// ---------------------------------------------------------------- node pool

static int node_alloc(eos_wm_t *wm)
{
    for (int i = 0; i < EOS_MAX_NODES; i++) {
        if (wm->node_used[i]) continue;
        wm->node_used[i] = true;
        memset(&wm->nodes[i], 0, sizeof(eos_node_t));
        wm->nodes[i].parent   = EOS_NONE;
        wm->nodes[i].win      = EOS_NONE;
        wm->nodes[i].child[0] = EOS_NONE;
        wm->nodes[i].child[1] = EOS_NONE;
        return i;
    }
    return EOS_NONE;
}

static void node_free(eos_wm_t *wm, int n)
{
    if (n >= 0 && n < EOS_MAX_NODES) wm->node_used[n] = false;
}

// Follows the `last` breadcrumbs down to the leaf the user touched most
// recently in this subtree. This is what a collapsed group shows.
static int active_leaf(const eos_wm_t *wm, int n)
{
    while (n >= 0 && wm->nodes[n].kind == 1) n = wm->nodes[n].child[wm->nodes[n].last];
    return n;
}

static void collect_leaves(const eos_wm_t *wm, int n, int16_t *out, int *cnt, int max)
{
    if (n < 0 || *cnt >= max) return;
    if (wm->nodes[n].kind == 0) { out[(*cnt)++] = (int16_t)n; return; }
    collect_leaves(wm, wm->nodes[n].child[0], out, cnt, max);
    collect_leaves(wm, wm->nodes[n].child[1], out, cnt, max);
}

// Replaces `oldn` with `newn` in oldn's parent, fixing up the root pointer.
static void replace_child(eos_wm_t *wm, int ws, int oldn, int newn)
{
    int p = wm->nodes[oldn].parent;
    if (newn >= 0) wm->nodes[newn].parent = p;
    if (p == EOS_NONE) wm->root[ws] = (int16_t)newn;
    else wm->nodes[p].child[wm->nodes[p].child[0] == oldn ? 0 : 1] = (int16_t)newn;
}

// ------------------------------------------------------------------ layout

typedef struct { const eos_wm_t *wm; eos_tile_t *out; int n, max; } lay_ctx_t;

static void emit(lay_ctx_t *c, int leaf, eos_rect_t r, bool visible,
                 int tab_group, int ti, int tc, eos_rect_t tabr)
{
    if (c->n >= c->max) return;
    int w = c->wm->nodes[leaf].win;
    eos_tile_t *t = &c->out[c->n++];
    t->win       = (int16_t)w;
    t->app_id    = (w >= 0) ? c->wm->win[w].app_id : 0;
    t->rect      = visible ? r : ZERO_RECT;
    t->visible   = visible;
    t->focused   = (w == c->wm->focus);
    t->tab_group = (int16_t)tab_group;
    t->tab_index = (int8_t)ti;
    t->tab_count = (int8_t)tc;
    t->tab_rect  = tabr;
}

static void lay(lay_ctx_t *c, int n, eos_rect_t r)
{
    if (n < 0) return;
    const eos_wm_t *wm = c->wm;
    const eos_wm_cfg_t *g = &wm->cfg;

    if (wm->nodes[n].kind == 0) {
        emit(c, n, r, true, EOS_NONE, 0, 0, ZERO_RECT);
        return;
    }

    bool cols = (wm->nodes[n].dir == EOS_SPLIT_COLS);
    bool fits = cols ? (r.w >= 2 * g->min_tile_w + g->gap)
                     : (r.h >= 2 * g->min_tile_h + g->gap);

    if (fits) {
        int ratio = wm->nodes[n].ratio;
        if (cols) {
            int avail = r.w - g->gap;
            int w0 = avail * ratio / 1000;
            if (w0 < g->min_tile_w)         w0 = g->min_tile_w;
            if (avail - w0 < g->min_tile_w) w0 = avail - g->min_tile_w;
            eos_rect_t a = { r.x, r.y, (int16_t)w0, r.h };
            eos_rect_t b = { (int16_t)(r.x + w0 + g->gap), r.y, (int16_t)(avail - w0), r.h };
            lay(c, wm->nodes[n].child[0], a);
            lay(c, wm->nodes[n].child[1], b);
        } else {
            int avail = r.h - g->gap;
            int h0 = avail * ratio / 1000;
            if (h0 < g->min_tile_h)         h0 = g->min_tile_h;
            if (avail - h0 < g->min_tile_h) h0 = avail - g->min_tile_h;
            eos_rect_t a = { r.x, r.y, r.w, (int16_t)h0 };
            eos_rect_t b = { r.x, (int16_t)(r.y + h0 + g->gap), r.w, (int16_t)(avail - h0) };
            lay(c, wm->nodes[n].child[0], a);
            lay(c, wm->nodes[n].child[1], b);
        }
        return;
    }

    // Too tight to split: flatten this whole subtree into one tab group so the
    // user gets N legible tabs instead of N unreadable slivers.
    int16_t leaves[EOS_MAX_WINDOWS];
    int cnt = 0;
    collect_leaves(wm, n, leaves, &cnt, EOS_MAX_WINDOWS);
    int act = active_leaf(wm, n);

    eos_rect_t strip = ZERO_RECT;
    eos_rect_t content = r;
    if (g->tab_h > 0 && r.h >= g->tab_h + g->gap + g->min_tile_h) {
        strip     = (eos_rect_t){ r.x, r.y, r.w, g->tab_h };
        content.y = (int16_t)(r.y + g->tab_h + g->gap);
        content.h = (int16_t)(r.h - g->tab_h - g->gap);
    }
    for (int i = 0; i < cnt; i++)
        emit(c, leaves[i], content, leaves[i] == act, n, i, cnt, strip);
}

int eos_wm_layout(const eos_wm_t *wm, eos_rect_t screen, eos_tile_t *out, int max)
{
    lay_ctx_t c = { wm, out, 0, max };
    eos_rect_t r = screen;
    r.y = (int16_t)(r.y + wm->cfg.bar_h);
    r.h = (int16_t)(r.h - wm->cfg.bar_h);
    r.x = (int16_t)(r.x + wm->cfg.gap);
    r.y = (int16_t)(r.y + wm->cfg.gap);
    r.w = (int16_t)(r.w - 2 * wm->cfg.gap);
    r.h = (int16_t)(r.h - 2 * wm->cfg.gap);
    if (r.w <= 0 || r.h <= 0) return 0;
    lay(&c, wm->root[wm->ws], r);
    return c.n;
}

// ------------------------------------------------------------------- focus

void eos_wm_focus_win(eos_wm_t *wm, int win)
{
    if (win < 0 || win >= EOS_MAX_WINDOWS || !wm->win[win].alive) return;
    wm->focus = (int16_t)win;
    int n = wm->win[win].node;
    int p = wm->nodes[n].parent;
    while (p != EOS_NONE) {
        wm->nodes[p].last = (wm->nodes[p].child[0] == n) ? 0 : 1;
        n = p;
        p = wm->nodes[p].parent;
    }
}

bool eos_wm_focus_dir(eos_wm_t *wm, eos_dir_t dir, eos_rect_t screen)
{
    eos_tile_t t[EOS_MAX_WINDOWS * 2];
    int n = eos_wm_layout(wm, screen, t, EOS_MAX_WINDOWS * 2);
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (t[i].visible && t[i].win == wm->focus) { cur = i; break; }
    if (cur < 0) return false;

    int cx = t[cur].rect.x + t[cur].rect.w / 2;
    int cy = t[cur].rect.y + t[cur].rect.h / 2;
    int best = -1;
    long best_score = 0;

    for (int i = 0; i < n; i++) {
        if (i == cur || !t[i].visible) continue;
        int dx = (t[i].rect.x + t[i].rect.w / 2) - cx;
        int dy = (t[i].rect.y + t[i].rect.h / 2) - cy;
        long prim, sec;
        switch (dir) {
        case EOS_DIR_LEFT:  if (dx >= 0) continue; prim = -dx; sec = dy < 0 ? -dy : dy; break;
        case EOS_DIR_RIGHT: if (dx <= 0) continue; prim =  dx; sec = dy < 0 ? -dy : dy; break;
        case EOS_DIR_UP:    if (dy >= 0) continue; prim = -dy; sec = dx < 0 ? -dx : dx; break;
        default:            if (dy <= 0) continue; prim =  dy; sec = dx < 0 ? -dx : dx; break;
        }
        long score = prim + sec * 3;   // strongly prefer the straight-ahead tile
        if (best < 0 || score < best_score) { best = i; best_score = score; }
    }
    if (best < 0) return false;
    eos_wm_focus_win(wm, t[best].win);
    return true;
}

bool eos_wm_focus_tab_next(eos_wm_t *wm, eos_rect_t screen)
{
    eos_tile_t t[EOS_MAX_WINDOWS * 2];
    int n = eos_wm_layout(wm, screen, t, EOS_MAX_WINDOWS * 2);
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (t[i].visible && t[i].win == wm->focus) { cur = i; break; }
    if (cur < 0 || t[cur].tab_group == EOS_NONE || t[cur].tab_count < 2) return false;

    int group = t[cur].tab_group;
    int want  = (t[cur].tab_index + 1) % t[cur].tab_count;
    for (int i = 0; i < n; i++)
        if (t[i].tab_group == group && t[i].tab_index == want) {
            eos_wm_focus_win(wm, t[i].win);
            return true;
        }
    return false;
}

// ------------------------------------------------------------ open / close

void eos_wm_init(eos_wm_t *wm, const eos_wm_cfg_t *cfg)
{
    memset(wm, 0, sizeof(*wm));
    wm->cfg = *cfg;
    for (int i = 0; i < EOS_WORKSPACES; i++) wm->root[i] = EOS_NONE;
    for (int i = 0; i < EOS_MAX_WINDOWS; i++) { wm->win[i].node = EOS_NONE; wm->win[i].ws = -1; }
    wm->focus = EOS_NONE;
    wm->next_split = EOS_SPLIT_COLS;
}

void eos_wm_set_split(eos_wm_t *wm, eos_split_t dir)
{
    wm->next_split = dir;
    wm->next_split_forced = true;
}

int eos_wm_open(eos_wm_t *wm, uint16_t app_id, eos_rect_t screen)
{
    int w = EOS_NONE;
    for (int i = 0; i < EOS_MAX_WINDOWS; i++)
        if (!wm->win[i].alive) { w = i; break; }
    if (w == EOS_NONE) return EOS_NONE;

    int leaf = node_alloc(wm);
    if (leaf == EOS_NONE) return EOS_NONE;
    wm->nodes[leaf].kind = 0;
    wm->nodes[leaf].win  = (int16_t)w;

    wm->win[w].alive  = true;
    wm->win[w].app_id = app_id;
    wm->win[w].ws     = wm->ws;
    wm->win[w].node   = (int16_t)leaf;

    int ws = wm->ws;
    if (wm->root[ws] == EOS_NONE) {
        wm->root[ws] = (int16_t)leaf;
    } else {
        int target = EOS_NONE;
        if (wm->focus >= 0 && wm->win[wm->focus].alive && wm->win[wm->focus].ws == ws)
            target = wm->win[wm->focus].node;
        if (target == EOS_NONE) target = active_leaf(wm, wm->root[ws]);

        eos_split_t dir = wm->next_split;
        if (!wm->next_split_forced) {
            // Dynamic tiling: cut the focused tile along its longer edge.
            eos_tile_t t[EOS_MAX_WINDOWS * 2];
            int n = eos_wm_layout(wm, screen, t, EOS_MAX_WINDOWS * 2);
            int tw = wm->nodes[target].win;
            dir = EOS_SPLIT_COLS;
            for (int i = 0; i < n; i++)
                if (t[i].win == tw && t[i].visible) {
                    dir = (t[i].rect.w >= t[i].rect.h) ? EOS_SPLIT_COLS : EOS_SPLIT_ROWS;
                    break;
                }
        }

        int sp = node_alloc(wm);
        if (sp == EOS_NONE) {
            node_free(wm, leaf);
            wm->win[w].alive = false;
            wm->win[w].node  = EOS_NONE;
            return EOS_NONE;
        }
        wm->nodes[sp].kind     = 1;
        wm->nodes[sp].dir      = (uint8_t)dir;
        wm->nodes[sp].ratio    = 500;
        wm->nodes[sp].child[0] = (int16_t)target;
        wm->nodes[sp].child[1] = (int16_t)leaf;
        wm->nodes[sp].last     = 1;

        replace_child(wm, ws, target, sp);
        wm->nodes[target].parent = (int16_t)sp;
        wm->nodes[leaf].parent   = (int16_t)sp;
    }

    wm->next_split_forced = false;
    eos_wm_focus_win(wm, w);
    return w;
}

// Unhooks a leaf from its tree; its sibling takes the parent's place.
static void detach_leaf(eos_wm_t *wm, int ws, int leaf)
{
    int p = wm->nodes[leaf].parent;
    if (p == EOS_NONE) {
        wm->root[ws] = EOS_NONE;
        return;
    }
    int sib = (wm->nodes[p].child[0] == leaf) ? wm->nodes[p].child[1] : wm->nodes[p].child[0];
    replace_child(wm, ws, p, sib);
    node_free(wm, p);
    wm->nodes[leaf].parent = EOS_NONE;
}

static void refocus_workspace(eos_wm_t *wm, int ws)
{
    int l = (wm->root[ws] >= 0) ? active_leaf(wm, wm->root[ws]) : EOS_NONE;
    if (l >= 0) eos_wm_focus_win(wm, wm->nodes[l].win);
    else wm->focus = EOS_NONE;
}

bool eos_wm_close(eos_wm_t *wm, int win)
{
    if (win < 0 || win >= EOS_MAX_WINDOWS || !wm->win[win].alive) return false;
    int ws   = wm->win[win].ws;
    int leaf = wm->win[win].node;

    detach_leaf(wm, ws, leaf);
    node_free(wm, leaf);
    wm->win[win].alive = false;
    wm->win[win].node  = EOS_NONE;
    wm->win[win].ws    = -1;

    if (wm->focus == win) refocus_workspace(wm, ws);
    return true;
}

// -------------------------------------------------------------- workspaces

void eos_wm_goto_workspace(eos_wm_t *wm, int n)
{
    if (n < 0 || n >= EOS_WORKSPACES) return;
    wm->ws = (int8_t)n;
    refocus_workspace(wm, n);
}

bool eos_wm_move_to_workspace(eos_wm_t *wm, int win, int n)
{
    if (win < 0 || win >= EOS_MAX_WINDOWS || !wm->win[win].alive) return false;
    if (n < 0 || n >= EOS_WORKSPACES) return false;
    int ws = wm->win[win].ws;
    if (ws == n) return false;

    int leaf = wm->win[win].node;
    detach_leaf(wm, ws, leaf);
    wm->win[win].ws = (int8_t)n;

    if (wm->root[n] == EOS_NONE) {
        wm->root[n] = (int16_t)leaf;
        wm->nodes[leaf].parent = EOS_NONE;
    } else {
        int target = active_leaf(wm, wm->root[n]);
        int sp = node_alloc(wm);
        if (sp == EOS_NONE) {           // pool exhausted: put it back where it was
            wm->win[win].ws = (int8_t)ws;
            if (wm->root[ws] == EOS_NONE) wm->root[ws] = (int16_t)leaf;
            return false;
        }
        wm->nodes[sp].kind     = 1;
        wm->nodes[sp].dir      = EOS_SPLIT_COLS;
        wm->nodes[sp].ratio    = 500;
        wm->nodes[sp].child[0] = (int16_t)target;
        wm->nodes[sp].child[1] = (int16_t)leaf;
        wm->nodes[sp].last     = 1;
        replace_child(wm, n, target, sp);
        wm->nodes[target].parent = (int16_t)sp;
        wm->nodes[leaf].parent   = (int16_t)sp;
    }

    if (wm->focus == win) refocus_workspace(wm, ws);
    return true;
}

bool eos_wm_resize(eos_wm_t *wm, int16_t delta_permille)
{
    if (wm->focus < 0 || !wm->win[wm->focus].alive) return false;
    int n = wm->win[wm->focus].node;
    int p = wm->nodes[n].parent;
    if (p == EOS_NONE) return false;
    int r = wm->nodes[p].ratio + ((wm->nodes[p].child[0] == n) ? delta_permille : -delta_permille);
    if (r < 100) r = 100;
    if (r > 900) r = 900;
    wm->nodes[p].ratio = (uint16_t)r;
    return true;
}
