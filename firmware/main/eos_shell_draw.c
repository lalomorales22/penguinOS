// The scene, drawn once per band. See eos_shell_draw.h for why it exists and
// for the re-runnability rule that shapes every function in here.

#include "eos_shell_draw.h"

#include <stdio.h>
#include <string.h>

#include "eos_font.h"
#include "eos_input.h"

// The names, taken from the one table rather than kept as a second list in
// the same order. eos_shell_status_sync() and main.c both want a plain array
// of pointers and the registry is an array of structs, so the pointers are
// restacked here on every call. Ten stores, no allocation, and no way for the
// two lists to fall out of step — which is exactly what they used to do.
static const char *name_of[EOS_APP_COUNT];

const char *const *eos_shell_app_names(void)
{
    int i;
    for (i = 0; i < EOS_APP_COUNT; i++) {
        const eos_app_t *a = eos_app_at(i);
        name_of[i] = a ? a->name : "?";
    }
    return name_of;
}

// Pixels between two bar segments, and the bar's own left/right margin. The
// bar model lays segments out inside a width it is given, so the margin is
// taken off that width here and added back when the run is drawn.
#define BAR_PAD    3
#define BAR_MARGIN 2

// ------------------------------------------------------------------- skin
//
// Every colour and face the scene uses, resolved once per frame instead of per
// band. eos_theme_role_index() is a struct read, but the four eos_font_get()
// calls and the theme's font-name lookup are not, and they would otherwise run
// six times for an identical answer.

// It is eos_app_ctx_t under its old name. The chrome below has always called
// it the skin and every line that reads s->accent still does; the app bodies
// are handed the same struct, so there is one description of "what this theme
// resolved to this frame" and not one for the scene and one for the apps.
typedef eos_app_ctx_t skin_t;

static const eos_font_t *face_or(const eos_font_t *want, const eos_font_t *fallback)
{
    return want ? want : fallback;
}

static void skin_build(skin_t *s, const eos_shell_view_t *v)
{
    const eos_theme_t *t = v->theme;

    memset(s, 0, sizeof(*s));
    s->view = v;

    // The theme names a face; the shell resolves it exactly once, through the
    // font component's own mapping, so an unknown name falls back in one place
    // rather than in every widget.
    s->ui   = eos_font_get(eos_font_id_from_name(eos_theme_font(t)));
    s->tiny = face_or(eos_font_get(EOS_FONT_TINY), s->ui);
    s->med  = face_or(eos_font_get(EOS_FONT_MED),  s->ui);
    s->big  = face_or(eos_font_get(EOS_FONT_BIG),  s->ui);

    s->border = t->m.border > 0 ? t->m.border : 1;
    eos_shell_tile_chrome(t, &s->chrome);

    s->bg       = eos_theme_role_index(t, EOS_ROLE_BG);
    s->surface  = eos_theme_role_index(t, EOS_ROLE_SURFACE);
    s->overlay  = eos_theme_role_index(t, EOS_ROLE_OVERLAY);
    s->text     = eos_theme_role_index(t, EOS_ROLE_TEXT);
    s->muted    = eos_theme_role_index(t, EOS_ROLE_MUTED);
    s->accent   = eos_theme_role_index(t, EOS_ROLE_ACCENT);
    s->bfoc     = eos_theme_role_index(t, EOS_ROLE_BORDER_FOCUSED);
    s->bunf     = eos_theme_role_index(t, EOS_ROLE_BORDER_UNFOCUSED);
    s->barbg    = eos_theme_role_index(t, EOS_ROLE_BAR_BG);
    s->barfg    = eos_theme_role_index(t, EOS_ROLE_BAR_FG);
    s->tabact   = eos_theme_role_index(t, EOS_ROLE_TAB_ACTIVE);
    s->tabinact = eos_theme_role_index(t, EOS_ROLE_TAB_INACTIVE);
    s->ok       = eos_theme_role_index(t, EOS_ROLE_OK);
    s->warn     = eos_theme_role_index(t, EOS_ROLE_WARN);
}

// ------------------------------------------------------------------- text

// Draws as much of `s` as fits in max_w and returns the pen advance. Every
// text call in this file goes through here: a tile is 117 px wide and almost
// every string in it is capable of being one character too long, so truncation
// is the normal case and not an error path.
static int16_t text_fit(int16_t x, int16_t y, const eos_font_t *f,
                        eos_color_t c, const char *s, int16_t max_w)
{
    // One line, because the app bodies need the identical rule and a second
    // copy of it is how a tab label and a tile label start truncating at
    // different widths. See eos_app_text() in eos_app_registry.c.
    return eos_app_text(x, y, f, c, s, max_w);
}

// --------------------------------------------------------------- status bar

static eos_color_t bar_role_color(const skin_t *s, eos_bar_role_t r)
{
    // The mapping eos_bar.h documents. It lives here because the bar model
    // deliberately does not include eos_theme.h.
    switch (r) {
    case EOS_BAR_ROLE_FG:     return s->barfg;
    case EOS_BAR_ROLE_MUTED:  return s->muted;
    case EOS_BAR_ROLE_ACCENT: return s->accent;
    case EOS_BAR_ROLE_OK:     return s->ok;
    case EOS_BAR_ROLE_WARN:   return s->warn;
    }
    return s->barfg;
}

static void draw_bar(const eos_shell_view_t *v, const skin_t *s, eos_rect_t bar)
{
    eos_bar_seg_t seg[EOS_BAR_SEGS];
    eos_bar_metrics_t m;
    int n, i;
    int16_t ty;

    if (eos_rect_empty(bar)) return;
    eos_display_fill(bar, s->barbg);
    if (!s->ui || !v->bar) return;

    // char_w, not a measure callback: the shipped faces have gap 0, so
    // cell_w * strlen is exact and the callback would only cost indirect calls.
    eos_bar_metrics_init(&m, (int16_t)s->ui->cell_w, BAR_PAD);
    n = eos_bar_build(v->bar, &m, (int16_t)(bar.w - 2 * BAR_MARGIN), seg, EOS_BAR_SEGS);

    ty = (int16_t)(bar.y + (bar.h - (int16_t)s->ui->h) / 2);
    for (i = 0; i < n; i++)
        text_fit((int16_t)(bar.x + BAR_MARGIN + seg[i].x), ty, s->ui,
                 bar_role_color(s, seg[i].role), seg[i].text, seg[i].w);
}

// The tile bodies used to be here, five functions and a switch. They are one
// per app file now and the table in eos_app_registry.c says which is which;
// the only one still in this file is the buddy, below, because the avatar is
// rendered into an offscreen box before the frame opens and that machinery and
// its body have to agree on where the box landed.

// The rect a tile's body gets, once the border, the header and the rule under
// it are taken off. Shared with buddy_prepare(), which has to know where the
// avatar will land a whole frame before draw_tile() gets there; two copies of
// this arithmetic is how the picture ends up one pixel off the box it was
// measured for.
static eos_rect_t tile_body(const skin_t *s, const eos_tile_t *t)
{
    eos_rect_t inner = eos_rect_inset(t->rect, (int16_t)(s->border + 1));
    int16_t hdr_h = s->ui ? (int16_t)s->ui->h : 0;

    if (eos_rect_empty(inner)) return eos_rect(0, 0, 0, 0);
    return eos_rect(inner.x, (int16_t)(inner.y + hdr_h + 3),
                    inner.w, (int16_t)(inner.h - hdr_h - 3));
}

// ------------------------------------------------------------------ buddy
//
// The avatar is the one thing in this file that cannot be drawn by replaying
// the scene: eos_buddy_render() writes whole pixels into a buffer of its own
// and reorders the model while it does it, and the scene runs once per band.
// So it is rendered ONCE per frame, into the box below, before the frame is
// even opened — and the per-band job is one clipped blit of an image that is
// already finished.
//
// The box is a fixed compile-time size and the render is fitted inside it,
// rather than the box being sized to the tile. A 240x240 panel with five
// windows gives the buddy a 110x76 body; a theme with no bar and no tab strip
// could give it 230x220, and 50 KB of BSS for a tile that might never be on
// screen is not a trade this board can make. The buddy is centred in whatever
// it gets.

static uint8_t   buddy_px[EOS_SHELL_BUDDY_PX * EOS_SHELL_BUDDY_PX];
static uint8_t   buddy_lut[768];
static eos_bitmap_t buddy_bm;
static int16_t   buddy_at_x, buddy_at_y;
static bool      buddy_ready;

void eos_shell_buddy_shade(const eos_vox_pal_t *pal, eos_buddy_cfg_t *cfg)
{
    int lvl, ci;

    if (!cfg) return;
    if (!pal) { cfg->shade_lut = NULL; return; }

    for (lvl = 0; lvl < 3; lvl++) {
        unsigned k = cfg->shade[lvl] ? cfg->shade[lvl] : 255u;
        for (ci = 0; ci < 256; ci++) {
            unsigned r = ((unsigned)pal->rgb[ci][0] * k) / 255u;
            unsigned g = ((unsigned)pal->rgb[ci][1] * k) / 255u;
            unsigned b = ((unsigned)pal->rgb[ci][2] * k) / 255u;
            buddy_lut[lvl * 256 + ci] =
                eos_display_match(eos_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b));
        }
    }
    cfg->shade_lut = buddy_lut;
}

// What the window costs in BSS, so the boot log can name it. Reported rather
// than computed at run time because BSS is claimed before app_main runs: it
// never shows up as a heap step, and a board whose free heap moved would
// otherwise have no line saying which window took it.
uint32_t eos_shell_buddy_bytes(void)
{
    return (uint32_t)(sizeof buddy_px + sizeof buddy_lut + sizeof buddy_bm);
}

// How much of the body the mood line takes off the bottom. The word is what
// makes the state machine visible: the bar's mood glyph is one character and
// reads as decoration, and "thinking" does not.
static int16_t mood_h(const skin_t *s)
{
    return (int16_t)((s->ui ? (int16_t)s->ui->h : 0) + 2);
}

// Renders the avatar for this frame. Called from eos_shell_draw_frame() before
// the frame is opened, which is the only place in this file where drawing into
// something other than the display is legal.
static void buddy_prepare(const eos_shell_view_t *v, const skin_t *s)
{
    eos_tile_t tiles[EOS_MAX_WINDOWS * 2];
    const eos_display_info_t *info;
    eos_buddy_target_t t;
    eos_rect_t body;
    int n, i;
    int16_t w, h;

    buddy_ready = false;
    if (!v->buddy || !v->buddy->model || v->buddy->model->count == 0) return;

    info = eos_display_info();
    n = eos_wm_layout(v->wm, eos_rect(0, 0, info->w, info->h),
                      tiles, EOS_MAX_WINDOWS * 2);
    for (i = 0; i < n; i++) {
        if (tiles[i].visible && tiles[i].app_id == EOS_APP_BUDDY) break;
    }
    if (i == n) return;                 // behind a tab, or on another workspace

    body = tile_body(s, &tiles[i]);
    body.h = (int16_t)(body.h - mood_h(s));
    if (eos_rect_empty(body)) return;

    w = body.w < EOS_SHELL_BUDDY_PX ? body.w : (int16_t)EOS_SHELL_BUDDY_PX;
    h = body.h < EOS_SHELL_BUDDY_PX ? body.h : (int16_t)EOS_SHELL_BUDDY_PX;
    if (w < 8 || h < 8) return;         // too small to read as anything

    // clear to the surface colour rather than to a keyed sentinel: the tile
    // under it is already that colour, so an opaque blit and a keyed one put
    // the same pixels on the glass and the opaque one cannot pick a key that
    // some shade of the model also resolves to.
    memset(&t, 0, sizeof t);
    t.pixels = buddy_px;
    t.w      = (uint16_t)w;
    t.h      = (uint16_t)h;
    t.fmt    = EOS_BUDDY_PIX_I8;
    t.clear  = true;
    t.bg_i8  = s->surface;
    if (eos_buddy_render(v->buddy, &t) < 0) return;

    memset(&buddy_bm, 0, sizeof buddy_bm);
    buddy_bm.pixels = buddy_px;
    buddy_bm.w      = w;
    buddy_bm.h      = h;
    buddy_bm.stride = w;
    buddy_bm.fmt    = EOS_PIXFMT_I8;
    buddy_bm.key    = EOS_COLOR_NONE;   // no keying: the box is opaque
    buddy_at_x = (int16_t)(body.x + (body.w - w) / 2);
    buddy_at_y = (int16_t)(body.y + (body.h - h) / 2);
    buddy_ready = true;
}

void eos_app_draw_buddy(const eos_app_ctx_t *s, eos_rect_t r)
{
    const eos_shell_view_t *v = s->view;
    char mood[12];
    const char *name;
    int16_t y;
    int i;

    if (buddy_ready) {
        eos_display_blit(buddy_at_x, buddy_at_y, &buddy_bm);
    } else if (!v->buddy || !v->buddy->model || v->buddy->model->count == 0) {
        // The two ways there is nothing to draw, told apart because they need
        // different things from the owner: no avatar at all is a boot that went
        // wrong, and an empty model is a .vox that parsed to nothing.
        eos_display_text_center(r, (int16_t)(r.y + (r.h - (int16_t)s->ui->h) / 2),
                                s->ui, s->muted,
                                v->buddy ? "no model" : "no buddy");
    }
    // The third way — a tile too small for even an 8x8 avatar — says nothing
    // and leaves the mood line, because there is a buddy and it is fine. A
    // "no model" there would send someone looking for a file that is present.

    // The mood, under it, in the accent colour: this is the megabrain request
    // lifecycle showing through, and it is the whole reason the state machine
    // is ticked on every pass of the loop and not only when this tile is up.
    //
    // Lowercased, the way /api/buddy reports it and the way every other label
    // in this file is spelled. eos_buddy_state_name() shouts because it is a
    // debug name; "THINKING" next to "uptime" and "free" reads as an error.
    name = v->buddy ? eos_buddy_state_name(eos_buddy_state(v->buddy)) : "-";
    for (i = 0; i < (int)sizeof mood - 1 && name[i]; i++)
        mood[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] + 32) : name[i];
    mood[i] = '\0';

    y = (int16_t)(r.y + r.h - (int16_t)s->ui->h);
    if (y >= r.y) eos_display_text_center(r, y, s->ui, s->accent, mood);
}

// ---------------------------------------------------------------- one tile

static void draw_tab_cell(const skin_t *s, const eos_tile_t *t, const char *label)
{
    eos_rect_t strip = t->tab_rect, cell;
    int16_t cw;

    if (eos_rect_empty(strip) || t->tab_count <= 0) return;

    cw   = (int16_t)(strip.w / t->tab_count);
    cell = eos_rect((int16_t)(strip.x + t->tab_index * cw), strip.y, cw, strip.h);
    // The last cell absorbs the remainder, so the strip has no unpainted
    // column when the width does not divide evenly.
    if (t->tab_index == t->tab_count - 1)
        cell.w = (int16_t)(strip.x + strip.w - cell.x);
    if (eos_rect_empty(cell)) return;

    eos_display_fill(cell, t->visible ? s->tabact : s->tabinact);
    if (!s->ui) return;
    if (!eos_display_clip_push(cell)) return;
    eos_display_text_center(cell, (int16_t)(cell.y + (cell.h - (int16_t)s->ui->h) / 2),
                            s->ui, s->bg, label);
    eos_display_clip_pop();
}

// --------------------------------------------------------------- close box
//
// The owner asked to be able to close windows, and pointed at a trackpad while
// asking. super+q was already the keyboard half; this is the other one.
//
// EOS_POINTER_CLOSE_W is the box's width and it is a constant rather than a
// multiple of the face height because the x inside it is drawn from pixels and
// not from a glyph: eleven columns is what a legible diagonal cross needs at
// this scale with a pixel of air on each side, and a box that grew with the
// font would leave the cross rattling around inside it.
#define CLOSE_W 11

// The metrics eos_pointer.c needs to find that box without being able to see a
// font. Pure, and the box is computed by eos_pointer_close_box() on BOTH sides
// — the painter below calls it too — so the x on the glass and the rectangle a
// click is tested against are the same rectangle by construction.
void eos_shell_tile_chrome(const eos_theme_t *t, eos_pointer_chrome_t *ch)
{
    const eos_font_t *ui;

    if (!ch) return;
    ui = t ? eos_font_get(eos_font_id_from_name(eos_theme_font(t))) : NULL;
    ch->border  = t && t->m.border > 0 ? t->m.border : 1;
    ch->hdr_h   = ui ? (int16_t)ui->h : 8;
    ch->close_w = CLOSE_W;
}

// The cross, drawn pixel by pixel inside `box`, centred, with one column and
// one row of air all round. Pixels rather than an 'x' from the UI face for two
// reasons: the face is the theme's to choose and a proportional one would put
// the glyph somewhere the hit box is not, and a lower-case x in a 6x8 face is
// four pixels of smudge that reads as dirt on the panel rather than as a
// control.
//
// Two loops over the same span, so the two diagonals are the same length and
// the cross is symmetric at every size. Nothing is latched: the function is
// pure over (box, colour) and is safe to replay once per band.
static void draw_close_x(eos_rect_t box, eos_color_t c)
{
    int16_t n, x0, y0, i;

    if (box.w < 5 || box.h < 5) return;

    // The largest odd square that leaves a pixel of margin, so the two
    // diagonals cross on a single pixel instead of between two of them.
    n = box.w < box.h ? box.w : box.h;
    n = (int16_t)(n - 4);
    if (n < 3) n = 3;
    if ((n & 1) == 0) n--;

    x0 = (int16_t)(box.x + (box.w - n) / 2);
    y0 = (int16_t)(box.y + (box.h - n) / 2);
    for (i = 0; i < n; i++) {
        eos_display_pixel((int16_t)(x0 + i), (int16_t)(y0 + i), c);
        eos_display_pixel((int16_t)(x0 + n - 1 - i), (int16_t)(y0 + i), c);
    }
}

static void draw_tile(const skin_t *s, const eos_tile_t *t)
{
    const eos_app_t *app = eos_app_at((int)t->app_id);
    const char *name = app ? app->name : "?";
    eos_rect_t r = t->rect, inner, body, close;
    int16_t hdr_h, id_w;
    char idbuf[8];

    draw_tab_cell(s, t, name);
    if (!t->visible || eos_rect_empty(r)) return;

    eos_display_fill(r, s->surface);
    eos_display_border(r, s->border, t->focused ? s->bfoc : s->bunf);

    inner = eos_rect_inset(r, (int16_t)(s->border + 1));
    if (eos_rect_empty(inner) || !s->ui) return;
    if (!eos_display_clip_push(inner)) return;

    // Header: name on the left in the focus colour, then the window id in the
    // 4x6 face, then the close box hard against the right edge. The id is what
    // makes a screenshot readable against the layout arithmetic in eos_wm.c,
    // which is the whole reason it is drawn.
    hdr_h = (int16_t)s->ui->h;

    // The close box takes the right-hand end of the header, so the window id
    // that used to sit there moves left by exactly its width. The id is the
    // number eos_wm.c's layout arithmetic is checked against in a screenshot
    // and it stays; it just stops being the thing under the cursor when
    // somebody aims at the x.
    close = eos_pointer_close_box(&s->chrome, t);
    if (!eos_rect_empty(close))
        draw_close_x(close, t->focused ? s->text : s->muted);

    text_fit(inner.x, inner.y, s->ui, t->focused ? s->text : s->muted, name,
             (int16_t)(inner.w - close.w));

    snprintf(idbuf, sizeof idbuf, "%d", (int)t->win);
    id_w = (int16_t)eos_text_width(s->tiny, idbuf, -1);
    if (id_w > 0 && id_w + close.w < inner.w / 2)
        text_fit((int16_t)(inner.x + inner.w - close.w - id_w),
                 (int16_t)(inner.y + 1), s->tiny, s->muted, idbuf, id_w);

    eos_display_hline(inner.x, (int16_t)(inner.y + hdr_h + 1), inner.w,
                      t->focused ? s->accent : s->bunf);

    body = tile_body(s, t);
    if (app && app->draw && !eos_rect_empty(body) && eos_display_clip_push(body)) {
        // The one place the ctx differs per tile. It is copied rather than
        // mutated in place because `s` is the frame's skin and is shared by
        // every tile in every band; writing the focus flag into it would leave
        // the last tile drawn deciding what the others thought they were.
        eos_app_ctx_t c = *s;
        c.focused = t->focused;
        app->draw(&c, body);
        eos_display_clip_pop();
    }

    eos_display_clip_pop();
}

// --------------------------------------------------------------- launcher
//
// The app list the owner asked for: super+space, a single column, arrow keys,
// enter. The MODEL is kernel/shell/eos_launcher.c — which item is highlighted
// and where the view is scrolled to are decided there and only read here.
//
// The geometry is read back out of the launcher rather than recomputed,
// because this function runs once per band and the hit test that decides what
// the trackpad is pointing at runs somewhere else entirely. One stored
// rectangle means the pixels and the pointer cannot drift apart.

void eos_shell_launcher_geom(const eos_theme_t *t, eos_launcher_geom_t *g)
{
    const eos_display_info_t *info = eos_display_info();
    const eos_font_t *ui;
    int16_t h;

    if (!g) return;
    ui = t ? eos_font_get(eos_font_id_from_name(eos_theme_font(t))) : NULL;
    h  = ui ? (int16_t)ui->h : 8;
    eos_launcher_layout(g, info->w, info->h, h);
}

// One cell's advance in a face that may be proportional. eos_font_t carries
// cell_w only for the fixed faces, so this asks the measurer instead of
// reading a field that is zero on half the fonts in the tree.
static int16_t cell_adv(const eos_font_t *f)
{
    int16_t w = f ? (int16_t)eos_text_width(f, "n", -1) : 6;
    return w > 0 ? w : 6;
}

// One row. The name is drawn first in full and the description gets whatever
// is left, because a truncated name is a row you cannot identify and a
// truncated sentence is still a hint.
static void draw_launcher_row(const skin_t *s, const eos_launcher_geom_t *g,
                              const eos_launcher_item_t *it, int16_t row_y,
                              bool selected)
{
    eos_rect_t row = eos_rect((int16_t)(g->x + g->pad), row_y,
                              (int16_t)(g->w - 2 * g->pad), g->row_h);
    int16_t tx, ty, name_w, left, cw;
    eos_color_t cname, cdesc;

    if (eos_rect_empty(row) || !s->ui) return;

    if (selected) {
        eos_display_fill(row, s->accent);
        cname = s->bg;          // the accent's own background, so the row reads
        cdesc = s->bg;          // as one block of colour and not as two inks
    } else {
        cname = s->text;
        cdesc = s->muted;
    }

    // Two pixels of gutter inside the highlight bar, and the text sits on the
    // row's vertical centre: (12 - 8) / 2 is 2 on the shipped 6x8 face.
    cw = cell_adv(s->ui);
    tx = (int16_t)(row.x + 2);
    ty = (int16_t)(row.y + (row.h - (int16_t)s->ui->h) / 2);
    left = (int16_t)(row.w - 4);

    name_w = text_fit(tx, ty, s->ui, cname, it->name, left);
    if (!it->desc) return;

    // Three cells of gap. Below about eight cells of remaining width the
    // description is nothing but an ellipsis, so it is dropped instead.
    tx   = (int16_t)(tx + name_w + 3 * cw);
    left = (int16_t)(row.x + row.w - 2 - tx);
    if (left < 8 * cw) return;
    text_fit(tx, ty, s->ui, cdesc, it->desc, left);
}

static void draw_launcher(const eos_shell_view_t *v, const skin_t *s)
{
    const eos_launcher_t *l = v->launcher;
    const eos_launcher_geom_t *g;
    eos_rect_t panel;
    char head[24];
    int top, rows, count, i;

    if (!l || !eos_launcher_is_open(l) || !s->ui) return;

    g     = &l->geom;
    panel = eos_rect(g->x, g->y, g->w, g->h);
    if (eos_rect_empty(panel)) return;

    eos_display_fill(panel, s->overlay);
    eos_display_border(panel, 1, s->accent);
    if (!eos_display_clip_push(panel)) return;

    count = eos_launcher_count(l);
    top   = eos_launcher_top(l);
    rows  = eos_launcher_rows(l);

    // The heading carries the position in the list, which is the only thing
    // that tells a reader there is more list off the bottom of a full screen.
    snprintf(head, sizeof head, "apps  %d/%d",
             (count > 0) ? eos_launcher_selected(l) + 1 : 0, count);
    text_fit((int16_t)(g->x + g->pad + 2), g->title_y, s->ui, s->muted,
             head, (int16_t)(g->w - 2 * g->pad - 4));
    eos_display_hline((int16_t)(g->x + g->pad), g->rule_y,
                      (int16_t)(g->w - 2 * g->pad), s->accent);

    if (count == 0) {
        text_fit((int16_t)(g->x + g->pad + 2), g->list_y, s->ui, s->muted,
                 "no apps", (int16_t)(g->w - 2 * g->pad - 4));
        eos_display_clip_pop();
        return;
    }

    for (i = 0; i < rows; i++) {
        int idx = top + i;
        if (idx >= count) break;
        draw_launcher_row(s, g, &l->item[idx],
                          (int16_t)(g->list_y + i * g->row_h),
                          idx == eos_launcher_selected(l));
    }

    // Two carets at the right edge saying which way the list continues. They
    // are drawn over the row rather than beside it because there is no spare
    // column on a 224 px panel, and a row whose description runs under the
    // caret is a row that was already truncated.
    {
        int16_t cw = cell_adv(s->ui);
        int16_t cx = (int16_t)(g->x + g->w - g->pad - 2 - cw);
        if (top > 0)
            text_fit(cx, g->list_y, s->ui, s->text, "^", cw);
        if (top + rows < count)
            text_fit(cx, (int16_t)(g->list_y + (rows - 1) * g->row_h),
                     s->ui, s->text, "v", cw);
    }

    eos_display_clip_pop();
}

// ---------------------------------------------------------------- cursor
//
// An arrow that has to stay visible on cyd-amber's near-black surface and on
// carbon's near-white one, over a tile body, over an accent-coloured tab and
// over the bar. No single colour does that, so the arrow is drawn twice: the
// triangle's interior in the theme's TEXT colour and its boundary in the
// theme's BG colour. Those two are the pair every theme is required to keep
// legible against each other, so whichever of them disappears into whatever is
// underneath, the other one does not, and the shape survives as either a light
// arrow with a dark rim or a dark arrow with a light rim.
//
// The shape is the triangle (0,0)-(6,6)-(0,10): the simplified pointer every
// windowing system has drawn since the eighties, at the smallest size where it
// still reads as an arrow next to a 6x8 font. The hot spot is its tip, at the
// top-left pixel, which is what makes the click land where the point is.
//
// Two 11-entry bitmask tables rather than a bitmap: eos_display_blit() takes a
// bitmap of palette indices and would need a 77-byte buffer built per frame,
// while these are 22 bytes of flash and one shift per pixel. Bit n of a row is
// x offset n.
#define CUR_H 11
static const uint8_t CUR_EDGE[CUR_H] = {
    0x01, 0x03, 0x05, 0x09, 0x11, 0x21, 0x41, 0x11, 0x09, 0x05, 0x03
};
static const uint8_t CUR_FILL[CUR_H] = {
    0x00, 0x00, 0x02, 0x06, 0x0E, 0x1E, 0x3E, 0x0E, 0x06, 0x02, 0x00
};

static void draw_cursor(const eos_shell_view_t *v, const skin_t *s)
{
    eos_rect_t box;
    int16_t px, py;
    int row, col;

    if (!v->pointer) return;

    // The LATCHED position and the latched visibility, never the live ones.
    // x, y and last_ms are written from the NimBLE host task, which preempts
    // this one, and this function runs once per band: reading them here would
    // let band 1 paint the arrow in one place and band 2 in another, and the
    // pixels of the first are then outside every rect the next frame repaints.
    // eos_pointer_latch() froze both before the frame opened.
    if (!eos_pointer_shown(v->pointer, v->pointer_ms)) return;
    box = eos_pointer_rect(v->pointer);
    if (eos_rect_empty(box)) return;

    // box is the arrow's box clipped to the panel, and the clip only ever
    // takes rows and columns off its far edges - x and y are already inside
    // the screen because the cursor is clamped - so box.x, box.y IS the hot
    // spot, and the shape below is drawn from it exactly as before.
    px = box.x;
    py = box.y;

    // Per pixel, not per run. Seventy-seven pixel calls is nothing next to the
    // band the frame is already pushing, and the clip stack drops the ones
    // outside the band on its own — which is what makes the arrow correct when
    // it straddles two bands, and is the whole re-runnability rule applied to
    // the one thing on screen that moves every frame.
    for (row = 0; row < CUR_H; row++) {
        uint8_t edge = CUR_EDGE[row], fill = CUR_FILL[row];
        for (col = 0; col < 8; col++) {
            uint8_t bit = (uint8_t)(1u << col);
            if (fill & bit)
                eos_display_pixel((int16_t)(px + col), (int16_t)(py + row), s->text);
            else if (edge & bit)
                eos_display_pixel((int16_t)(px + col), (int16_t)(py + row), s->bg);
        }
    }
}

// ------------------------------------------------------------------ frame

static eos_rect_t bar_rect(const eos_shell_view_t *v)
{
    const eos_display_info_t *info = eos_display_info();
    int16_t h = v->wm->cfg.bar_h;
    if (h <= 0) return eos_rect(0, 0, 0, 0);
    if (h > info->h) h = info->h;
    return eos_rect(0, 0, info->w, h);
}

static void scene(const eos_shell_view_t *v, const skin_t *s)
{
    eos_tile_t tiles[EOS_MAX_WINDOWS * 2];
    const eos_display_info_t *info = eos_display_info();
    int n, i;

    eos_display_clear(s->bg);
    draw_bar(v, s, bar_rect(v));

    n = eos_wm_layout(v->wm, eos_rect(0, 0, info->w, info->h),
                      tiles, EOS_MAX_WINDOWS * 2);
    for (i = 0; i < n; i++) draw_tile(s, &tiles[i]);

    // Last, over everything. An overlay that a tile could paint over is not
    // an overlay.
    draw_launcher(v, s);

    // And the arrow over that. It is the one thing on the glass that is
    // pointing AT the overlay, so it cannot be under it.
    draw_cursor(v, s);
}

void eos_shell_draw_frame(const eos_shell_view_t *v)
{
    skin_t s;
    eos_rect_t band;

    if (!v || !v->theme || !v->wm) return;
    skin_build(&s, v);

    // Once, before the frame opens. See the buddy section: this is the whole
    // reason the avatar has an offscreen box at all.
    buddy_prepare(v, &s);

    eos_display_frame_begin();
    while (eos_display_frame_band(&band)) scene(v, &s);
    eos_display_frame_end();
}

// ------------------------------------------------------------------ damage

void eos_shell_damage_all(void) { eos_display_damage_all(); }

void eos_shell_damage_bar(const eos_shell_view_t *v)
{
    if (!v || !v->wm) return;
    eos_display_damage(bar_rect(v));
}

bool eos_shell_damage_pointer(const eos_shell_view_t *v)
{
    eos_rect_t was, now;

    if (!v || !v->pointer) return false;
    if (!eos_pointer_dirty(v->pointer, v->pointer_ms)) return false;

    // The order is deliberate: the hole first, the arrow second. The damage
    // list coalesces neighbours once it is full, and when the cursor has moved
    // only a pixel or two these two boxes overlap and collapse into one band
    // barely larger than either of them.
    was = eos_pointer_drawn_rect(v->pointer);
    now = eos_pointer_rect(v->pointer);
    if (!eos_rect_empty(was)) eos_display_damage(was);
    if (!eos_rect_empty(now) && eos_pointer_visible(v->pointer, v->pointer_ms))
        eos_display_damage(now);
    return true;
}

static bool app_tiles(const eos_shell_view_t *v, uint16_t app_id, bool damage)
{
    eos_tile_t tiles[EOS_MAX_WINDOWS * 2];
    const eos_display_info_t *info;
    int n, i;
    bool hit = false;

    if (!v || !v->wm) return false;
    info = eos_display_info();
    n = eos_wm_layout(v->wm, eos_rect(0, 0, info->w, info->h),
                      tiles, EOS_MAX_WINDOWS * 2);
    for (i = 0; i < n; i++) {
        if (!tiles[i].visible || tiles[i].app_id != app_id) continue;
        if (damage) eos_display_damage(tiles[i].rect);
        hit = true;
    }
    return hit;
}

bool eos_shell_damage_app(const eos_shell_view_t *v, uint16_t app_id)
{
    return app_tiles(v, app_id, true);
}

bool eos_shell_app_visible(const eos_shell_view_t *v, uint16_t app_id)
{
    return app_tiles(v, app_id, false);
}
