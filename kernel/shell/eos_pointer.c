// The cursor. See eos_pointer.h for why it exists and for the ring-depth
// constraint that shapes the motion path.
//
// Everything here is integer and allocation-free, and every function except
// eos_pointer_shared() takes its state as an argument, so the host suite can
// run twenty cursors side by side without a reset between them.

#include "eos_pointer.h"

#include <string.h>

// ------------------------------------------------------------- small helpers

static int16_t clamp16(int32_t v, int16_t lo, int16_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (int16_t)v;
}

static int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

// The hit test is two comparisons per axis and is written out rather than
// borrowed from eos_display.h, which is the only other place in the tree that
// has one. This file deliberately includes no display header: it computes
// rectangles and never paints them.
static bool rect_hit(eos_rect_t r, int16_t x, int16_t y)
{
    return r.w > 0 && r.h > 0 &&
           x >= r.x && x < (int16_t)(r.x + r.w) &&
           y >= r.y && y < (int16_t)(r.y + r.h);
}

static eos_rect_t rect_clip(eos_rect_t r, int16_t w, int16_t h)
{
    int32_t x0 = r.x, y0 = r.y;
    int32_t x1 = (int32_t)r.x + r.w, y1 = (int32_t)r.y + r.h;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) {
        eos_rect_t z; z.x = 0; z.y = 0; z.w = 0; z.h = 0;
        return z;
    }
    {
        eos_rect_t o;
        o.x = (int16_t)x0;
        o.y = (int16_t)y0;
        o.w = (int16_t)(x1 - x0);
        o.h = (int16_t)(y1 - y0);
        return o;
    }
}

// A signed difference, so the comparison survives the 49-day wrap of the
// millisecond clock. Same idiom as eos_input.c's due().
static bool elapsed(uint32_t now, uint32_t then, uint32_t span)
{
    return (uint32_t)(now - then) >= span;
}

// -------------------------------------------------------------- the curve

eos_pointer_accel_t eos_pointer_accel_defaults(void)
{
    eos_pointer_accel_t a;
    a.unity   = 2;
    a.gain_q4 = 10;   // +0.625x per count past `unity`
    a.max_q4  = 80;   // 5.0x ceiling
    return a;
}

int16_t eos_pointer_scale(const eos_pointer_accel_t *a, int16_t d, int32_t *acc)
{
    int32_t mag, gain, out, px;
    eos_pointer_accel_t def;

    if (!acc) return 0;
    if (!a) { def = eos_pointer_accel_defaults(); a = &def; }
    if (d == 0) return 0;

    mag = iabs32(d);

    // Below the unity threshold the gain is exactly 1.0 and the arithmetic
    // below is a no-op. Keeping that as a branch rather than as a max() is
    // deliberate: the flat zone is the reason a trackpad can land on a
    // one-pixel tile border, and it should be visible as its own rule.
    gain = 16;
    if (mag > a->unity && a->gain_q4 > 0)
        gain += (mag - a->unity) * a->gain_q4;

    // A gain under 1.0 would make the cursor slower than the pad and is never
    // what anyone wants; a configured ceiling below 16 is treated as 16.
    if (a->max_q4 >= 16 && gain > a->max_q4) gain = a->max_q4;

    out = (int32_t)d * gain + *acc;

    // Truncation toward zero, which C99 guarantees for integer division, so
    // the remainder keeps the sign of `out` and the curve stays odd: a run of
    // -1s covers exactly as much ground as the same run of +1s. An arithmetic
    // right shift would round toward negative infinity and quietly make
    // leftward motion one pixel faster than rightward.
    px   = out / 16;
    *acc = out - px * 16;
    return (int16_t)px;
}

// ------------------------------------------------------------------- state

void eos_pointer_init(eos_pointer_t *p, int16_t w, int16_t h)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->w = w > 0 ? w : 1;
    p->h = h > 0 ? h : 1;
    p->x = (int16_t)(p->w / 2);
    p->y = (int16_t)(p->h / 2);
    p->drawn_x = p->x;
    p->drawn_y = p->y;
    p->accel = eos_pointer_accel_defaults();
}

eos_pointer_t *eos_pointer_shared(void)
{
    static eos_pointer_t s_ptr;
    return &s_ptr;
}

// ----------------------------------------------------------------- reports

// Which down_x/down_y slot a single-bit button mask uses. Left is 0, right 1,
// middle 2; anything else is refused rather than folded, because a vendor
// report with the fourth button set must not scribble past the array.
static int btn_slot(uint8_t bit)
{
    switch (bit) {
    case EOS_BTN_LEFT:   return 0;
    case EOS_BTN_RIGHT:  return 1;
    case EOS_BTN_MIDDLE: return 2;
    default:             return -1;
    }
}

void eos_pointer_feed(eos_pointer_t *p, int16_t dx, int16_t dy,
                      uint8_t buttons, uint32_t now_ms)
{
    int16_t px, py, ox, oy;
    uint8_t changed;
    int b;

    if (!p) return;
    if (p->w <= 0 || p->h <= 0) return;   // never initialised; refuse rather than divide

    p->seen    = true;
    p->last_ms = now_ms;

    ox = p->x;
    oy = p->y;

    px = eos_pointer_scale(&p->accel, dx, &p->acc_x);
    py = eos_pointer_scale(&p->accel, dy, &p->acc_y);

    // HID's y axis grows DOWNWARD, and so does the panel's. No inversion here
    // is not an oversight: inverting would be the bug.
    p->x = clamp16((int32_t)p->x + px, 0, (int16_t)(p->w - 1));
    p->y = clamp16((int32_t)p->y + py, 0, (int16_t)(p->h - 1));

    // Motion first, so the button events below carry the position the button
    // was actually pressed or released at.
    if (p->x != ox || p->y != oy)
        eos_input_inject_pointer(EOS_EV_POINTER_MOVE, p->x, p->y,
                                 p->buttons, EOS_SRC_MOUSE, now_ms);

    // Only the three boot-mouse buttons. A report with bit 3 set is a device
    // with more buttons than HOGP's boot protocol describes, and guessing at
    // what it meant is worse than ignoring it.
    buttons &= (uint8_t)(EOS_BTN_LEFT | EOS_BTN_RIGHT | EOS_BTN_MIDDLE);
    changed  = (uint8_t)(buttons ^ p->buttons);

    for (b = 0; b < 3; b++) {
        uint8_t bit = (uint8_t)(1u << b);
        int slot;

        if (!(changed & bit)) continue;
        slot = btn_slot(bit);
        if (slot < 0) continue;

        if (buttons & bit) {
            p->buttons |= bit;
            p->down_x[slot] = p->x;
            p->down_y[slot] = p->y;
            eos_input_inject_pointer(EOS_EV_POINTER_DOWN, p->x, p->y, bit,
                                     EOS_SRC_MOUSE, now_ms);
        } else {
            p->buttons &= (uint8_t)~bit;
            eos_input_inject_pointer(EOS_EV_POINTER_UP, p->x, p->y, bit,
                                     EOS_SRC_MOUSE, now_ms);

            // A click is a release that landed where its press did. There is
            // no time limit on purpose: nothing on this board consumes a drag,
            // so a slow deliberate press should still select a tab rather than
            // silently doing nothing.
            if (iabs32((int32_t)p->x - p->down_x[slot]) <= EOS_POINTER_SLOP &&
                iabs32((int32_t)p->y - p->down_y[slot]) <= EOS_POINTER_SLOP)
                eos_input_inject_pointer(EOS_EV_CLICK, p->x, p->y, bit,
                                         EOS_SRC_MOUSE, now_ms);
        }
    }
}

void eos_pointer_disconnect(eos_pointer_t *p, uint32_t now_ms)
{
    int b;

    if (!p) return;
    for (b = 0; b < 3; b++) {
        uint8_t bit = (uint8_t)(1u << b);
        if (!(p->buttons & bit)) continue;
        eos_input_inject_pointer(EOS_EV_POINTER_UP, p->x, p->y, bit,
                                 EOS_SRC_MOUSE, now_ms);
    }
    p->buttons = 0;
    p->acc_x   = 0;
    p->acc_y   = 0;
    p->seen    = false;   // hides the arrow; the position is kept for next time
}

// ------------------------------------------------------------------ drawing

bool eos_pointer_visible(const eos_pointer_t *p, uint32_t now_ms)
{
    if (!p || !p->seen) return false;
    return !elapsed(now_ms, p->last_ms, EOS_POINTER_IDLE_MS);
}

// One read of the live state, kept for the whole frame. See the long comment
// on eos_pointer_latch() in the header: everything below that answers "where
// is the arrow this frame" reads the latch when one is armed, and only falls
// back to the live x, y when nothing has latched - which is what keeps every
// caller written before the latch existed behaving exactly as it did.
void eos_pointer_latch(eos_pointer_t *p, uint32_t now_ms)
{
    if (!p) return;
    p->show_x     = p->x;
    p->show_y     = p->y;
    p->show_vis   = eos_pointer_visible(p, now_ms);
    p->show_armed = true;
}

bool eos_pointer_shown(const eos_pointer_t *p, uint32_t now_ms)
{
    if (!p) return false;
    return p->show_armed ? p->show_vis : eos_pointer_visible(p, now_ms);
}

static eos_rect_t box_at(const eos_pointer_t *p, int16_t x, int16_t y)
{
    eos_rect_t r;
    r.x = x;
    r.y = y;
    r.w = EOS_POINTER_ARROW_W;
    r.h = EOS_POINTER_ARROW_H;
    return rect_clip(r, p->w, p->h);
}

eos_rect_t eos_pointer_rect(const eos_pointer_t *p)
{
    eos_rect_t z; z.x = 0; z.y = 0; z.w = 0; z.h = 0;
    if (!p) return z;
    if (p->show_armed) return box_at(p, p->show_x, p->show_y);
    return box_at(p, p->x, p->y);
}

eos_rect_t eos_pointer_drawn_rect(const eos_pointer_t *p)
{
    eos_rect_t z; z.x = 0; z.y = 0; z.w = 0; z.h = 0;
    if (!p || !p->drawn_valid) return z;
    return box_at(p, p->drawn_x, p->drawn_y);
}

bool eos_pointer_dirty(const eos_pointer_t *p, uint32_t now_ms)
{
    bool vis;
    int16_t sx, sy;

    if (!p) return false;

    // Against the LATCH, not against the live position. The damage this
    // answers for is the damage the bands are about to paint into, and the
    // bands paint the latch.
    if (p->show_armed) { vis = p->show_vis; sx = p->show_x; sy = p->show_y; }
    else               { vis = eos_pointer_visible(p, now_ms); sx = p->x; sy = p->y; }

    // Appearing and disappearing both count: the frame that stops drawing the
    // arrow still has to repaint the hole it leaves.
    if (vis != p->drawn_valid) return true;
    if (!vis) return false;
    return sx != p->drawn_x || sy != p->drawn_y;
}

void eos_pointer_commit(eos_pointer_t *p, uint32_t now_ms)
{
    if (!p) return;
    if (p->show_armed) {
        p->drawn_valid = p->show_vis;
        p->drawn_x     = p->show_x;
        p->drawn_y     = p->show_y;
        p->show_armed  = false;   // the next frame latches its own
        return;
    }
    p->drawn_valid = eos_pointer_visible(p, now_ms);
    p->drawn_x     = p->x;
    p->drawn_y     = p->y;
}

// --------------------------------------------------------------- hit testing

// One tab cell, computed exactly the way firmware/main/eos_shell_draw.c paints
// it: equal widths from integer division, and the LAST cell absorbs whatever
// the division left over. Duplicating the rule is unavoidable — the renderer
// cannot be called from the kernel — so it is written the same way in both
// places and the host suite checks a click against every cell of a real strip.
static eos_rect_t tab_cell(const eos_tile_t *t)
{
    eos_rect_t strip = t->tab_rect, cell;
    int16_t cw;

    cell.x = 0; cell.y = 0; cell.w = 0; cell.h = 0;
    if (strip.w <= 0 || strip.h <= 0 || t->tab_count <= 0) return cell;
    if (t->tab_index < 0 || t->tab_index >= t->tab_count) return cell;

    cw     = (int16_t)(strip.w / t->tab_count);
    cell.x = (int16_t)(strip.x + t->tab_index * cw);
    cell.y = strip.y;
    cell.w = cw;
    cell.h = strip.h;
    if (t->tab_index == t->tab_count - 1)
        cell.w = (int16_t)(strip.x + strip.w - cell.x);
    return cell;
}

// The close box, computed exactly the way firmware/main/eos_shell_draw.c paints
// it: the tile is inset by border+1, the header is the first hdr_h rows of what
// is left, and the box is the right-hand close_w columns of that header grown
// UP through the border to the tile's own top edge. Growing it up is not
// cosmetic — on this panel hdr_h is eight pixels and a box eight rows tall is
// a target a trackpad misses; the border rows above it are dead pixels that
// belong to no other control, so the hit box takes them and the drawn x stays
// centred in the header where it reads as part of the title row.
//
// Half the header is the ceiling, and the box is close_w wide or it is not
// there. Shrinking it to fit was the first version and it was wrong: a tile
// four pixels across came back with a two-pixel box, which draw_close_x()
// refuses to paint and eos_pointer_hit() happily closes a window through. A
// control that is clickable and invisible is the one shape this must not take.
eos_rect_t eos_pointer_close_box(const eos_pointer_chrome_t *ch,
                                 const eos_tile_t *t)
{
    eos_rect_t z, inner, box;
    int16_t inset, w;

    z.x = 0; z.y = 0; z.w = 0; z.h = 0;
    if (!ch || !t) return z;
    if (ch->close_w <= 0 || ch->hdr_h <= 0) return z;
    if (!t->visible) return z;

    inset = (int16_t)(ch->border + 1);
    inner.x = (int16_t)(t->rect.x + inset);
    inner.y = (int16_t)(t->rect.y + inset);
    inner.w = (int16_t)(t->rect.w - 2 * inset);
    inner.h = (int16_t)(t->rect.h - 2 * inset);
    if (inner.w <= 0 || inner.h <= 0) return z;
    if (inner.h < ch->hdr_h) return z;

    w = ch->close_w;
    if (w < EOS_POINTER_CLOSE_MIN) return z;
    if (w * 2 > inner.w) return z;

    box.x = (int16_t)(inner.x + inner.w - w);
    box.y = t->rect.y;
    box.w = w;
    box.h = (int16_t)(inner.y + ch->hdr_h - t->rect.y);
    if (box.h < EOS_POINTER_CLOSE_MIN) return z;
    return box;
}

eos_hit_t eos_pointer_hit(const eos_wm_t *wm, eos_rect_t screen,
                          const eos_pointer_chrome_t *ch,
                          int16_t x, int16_t y)
{
    eos_tile_t tiles[EOS_MAX_WINDOWS * 2];
    eos_hit_t  h;
    int n, i;

    h.kind = EOS_HIT_NONE;
    h.win  = EOS_NONE;
    h.tab_index = -1;
    if (!wm) return h;

    // The bar is painted over the top of the screen rect and is not part of
    // any tile, so it is answered before the layout is even run. A click on it
    // does nothing today, but it must not fall through to the tile underneath.
    if (wm->cfg.bar_h > 0 && y >= screen.y &&
        y < (int16_t)(screen.y + wm->cfg.bar_h) &&
        x >= screen.x && x < (int16_t)(screen.x + screen.w)) {
        h.kind = EOS_HIT_BAR;
        return h;
    }

    n = eos_wm_layout(wm, screen, tiles, EOS_MAX_WINDOWS * 2);

    // The close boxes first. Each one lives INSIDE its tile's rect, so a tile
    // pass that ran before this would answer every point in the box with
    // EOS_HIT_TILE and the x would be decoration. Tab strips do not overlap
    // them — a strip sits outside the tile it labels — so the order between
    // those two is free, and this one is not.
    for (i = 0; i < n; i++) {
        if (rect_hit(eos_pointer_close_box(ch, &tiles[i]), x, y)) {
            h.kind      = EOS_HIT_CLOSE;
            h.win       = tiles[i].win;
            h.tab_index = tiles[i].tab_index;
            return h;
        }
    }

    // Tab strips before tiles. A collapsed group's strip sits above the tile
    // it labels and every member of the group reports the same strip with its
    // own index, so walking every tile walks every cell exactly once.
    for (i = 0; i < n; i++) {
        if (rect_hit(tab_cell(&tiles[i]), x, y)) {
            h.kind      = EOS_HIT_TAB;
            h.win       = tiles[i].win;
            h.tab_index = tiles[i].tab_index;
            return h;
        }
    }

    for (i = 0; i < n; i++) {
        if (!tiles[i].visible) continue;
        if (rect_hit(tiles[i].rect, x, y)) {
            h.kind      = EOS_HIT_TILE;
            h.win       = tiles[i].win;
            h.tab_index = tiles[i].tab_index;
            return h;
        }
    }

    return h;   // a gap, and that is a real answer
}

bool eos_pointer_click(eos_wm_t *wm, eos_rect_t screen,
                       const eos_pointer_chrome_t *ch, int16_t x, int16_t y)
{
    eos_hit_t h;

    if (!wm) return false;
    h = eos_pointer_hit(wm, screen, ch, x, y);
    if (h.win == EOS_NONE) return false;

    // The close box goes through eos_wm_close(), which is the same call
    // EOS_ACT_CLOSE makes for super+q. There is one way to shut a window on
    // this board and this is a second door onto it, not a second implementation
    // of it: eos_wm owns which sibling absorbs the space and where the focus
    // lands afterwards, and neither of those decisions is repeated here.
    if (h.kind == EOS_HIT_CLOSE) return eos_wm_close(wm, h.win);

    if (h.kind != EOS_HIT_TILE && h.kind != EOS_HIT_TAB) return false;
    if (wm->focus == h.win) return false;   // already there; nothing to redraw

    eos_wm_focus_win(wm, h.win);
    return true;
}

bool eos_pointer_event(eos_wm_t *wm, eos_rect_t screen,
                       const eos_pointer_chrome_t *ch, const eos_event_t *e)
{
    if (!wm || !e) return false;
    if (e->type != EOS_EV_CLICK) return false;

    // Only the left button acts. Right and middle travel through the ring so
    // an app can bind them, but neither means anything to the window manager.
    if (!(e->key & EOS_BTN_LEFT)) return false;

    return eos_pointer_click(wm, screen, ch, e->x, e->y);
}
