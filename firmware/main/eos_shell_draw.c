// The scene, drawn once per band. See eos_shell_draw.h for why it exists and
// for the re-runnability rule that shapes every function in here.

#include "eos_shell_draw.h"

#include <stdio.h>
#include <string.h>

#include "eos_font.h"
#include "eos_input.h"

// The five windows the boot glue opens. Short on purpose: a tab label has
// 117/6 = 19 cells to live in on this panel, and a truncated name reads as a
// rendering bug rather than as a long name.
static const char *const APP_NAMES[EOS_APP_COUNT] = {
    "clock", "board", "heap", "keys", "buddy"
};

const char *const *eos_shell_app_names(void) { return APP_NAMES; }

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

typedef struct {
    const eos_font_t *ui, *tiny, *med, *big;
    int16_t border;
    eos_color_t bg, surface, text, muted, accent;
    eos_color_t bfoc, bunf, barbg, barfg, tabact, tabinact;
    eos_color_t ok, warn;
} skin_t;

static const eos_font_t *face_or(const eos_font_t *want, const eos_font_t *fallback)
{
    return want ? want : fallback;
}

static void skin_build(skin_t *s, const eos_theme_t *t)
{
    memset(s, 0, sizeof(*s));

    // The theme names a face; the shell resolves it exactly once, through the
    // font component's own mapping, so an unknown name falls back in one place
    // rather than in every widget.
    s->ui   = eos_font_get(eos_font_id_from_name(eos_theme_font(t)));
    s->tiny = face_or(eos_font_get(EOS_FONT_TINY), s->ui);
    s->med  = face_or(eos_font_get(EOS_FONT_MED),  s->ui);
    s->big  = face_or(eos_font_get(EOS_FONT_BIG),  s->ui);

    s->border = t->m.border > 0 ? t->m.border : 1;

    s->bg       = eos_theme_role_index(t, EOS_ROLE_BG);
    s->surface  = eos_theme_role_index(t, EOS_ROLE_SURFACE);
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
    int n;
    if (!f || !s || max_w <= 0) return 0;
    n = eos_text_fit(f, s, -1, (int)max_w);
    if (n <= 0) return 0;
    return (int16_t)eos_display_text(x, y, f, c, s, n);
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

// --------------------------------------------------------------- tile bodies

static void body_clock(const eos_shell_view_t *v, const skin_t *s, eos_rect_t r)
{
    char buf[16];
    uint32_t sec = v->uptime_ms / 1000u;
    int16_t y;

    snprintf(buf, sizeof buf, "%02u:%02u:%02u",
             (unsigned)(sec / 3600u), (unsigned)((sec / 60u) % 60u), (unsigned)(sec % 60u));

    y = (int16_t)(r.y + (r.h - (int16_t)s->big->h) / 2);
    if (y < r.y) y = r.y;
    eos_display_text_center(r, y, s->big, s->accent, buf);

    y = (int16_t)(y + (int16_t)s->big->h + 3);
    if (y + (int16_t)s->ui->h <= r.y + r.h)
        eos_display_text_center(r, y, s->ui, s->muted, "uptime");
}

static void body_board(const eos_shell_view_t *v, const skin_t *s, eos_rect_t r)
{
    int16_t y = r.y;
    int i;

    for (i = 0; i < 4; i++) {
        const eos_font_t *f = (i == 0) ? s->med : s->ui;
        if (!v->board_line[i]) continue;
        if (y + (int16_t)f->h > r.y + r.h) break;
        text_fit(r.x, y, f, (i == 0) ? s->text : s->muted, v->board_line[i], r.w);
        y = (int16_t)(y + (int16_t)f->h + 2);
    }
}

static void body_heap(const eos_shell_view_t *v, const skin_t *s, eos_rect_t r)
{
    char buf[16];
    int16_t y = r.y;

    text_fit(r.x, y, s->ui, s->muted, "free", r.w);
    y = (int16_t)(y + (int16_t)s->ui->h + 1);
    snprintf(buf, sizeof buf, "%u", (unsigned)v->heap_free);
    text_fit(r.x, y, s->med, s->text, buf, r.w);
    y = (int16_t)(y + (int16_t)s->med->h + 4);

    if (y + (int16_t)s->ui->h + (int16_t)s->med->h + 1 > r.y + r.h) return;
    text_fit(r.x, y, s->ui, s->muted, "largest block", r.w);
    y = (int16_t)(y + (int16_t)s->ui->h + 1);
    snprintf(buf, sizeof buf, "%u", (unsigned)v->heap_largest);
    text_fit(r.x, y, s->med, s->text, buf, r.w);
}

static void body_keys(const eos_shell_view_t *v, const skin_t *s, eos_rect_t r)
{
    // Four binds out of the seventy-two, chosen because they are the ones a
    // human will try first on a board that has no keyboard attached yet.
    static const struct { uint8_t mods; uint16_t key; } SHOW[] = {
        { EOS_MOD_SUPER,                   EOS_KEY_ENTER },
        { EOS_MOD_SUPER,                   EOS_KEY_Q     },
        { EOS_MOD_SUPER,                   EOS_KEY_TAB   },
        { EOS_MOD_SUPER,                   EOS_KEY_2     },
    };
    int16_t y = r.y;
    size_t i;

    if (!v->keys) return;
    for (i = 0; i < sizeof SHOW / sizeof SHOW[0]; i++) {
        const eos_keybind_t *b = eos_keys_lookup(v->keys, SHOW[i].mods, SHOW[i].key);
        char chord[32], line[48];
        if (!b) continue;
        if (y + (int16_t)s->ui->h > r.y + r.h) break;
        eos_keys_format(b, chord, (int)sizeof chord);
        snprintf(line, sizeof line, "%s %s", chord,
                 eos_keys_action_name((eos_action_t)b->action));
        text_fit(r.x, y, s->ui, s->muted, line, r.w);
        y = (int16_t)(y + (int16_t)s->ui->h + 1);
    }
}


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

static void body_buddy(const eos_shell_view_t *v, const skin_t *s, eos_rect_t r)
{
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

static void draw_tile(const eos_shell_view_t *v, const skin_t *s, const eos_tile_t *t)
{
    const char *name = (t->app_id < EOS_APP_COUNT) ? APP_NAMES[t->app_id] : "?";
    eos_rect_t r = t->rect, inner, body;
    int16_t hdr_h, id_w;
    char idbuf[8];

    draw_tab_cell(s, t, name);
    if (!t->visible || eos_rect_empty(r)) return;

    eos_display_fill(r, s->surface);
    eos_display_border(r, s->border, t->focused ? s->bfoc : s->bunf);

    inner = eos_rect_inset(r, (int16_t)(s->border + 1));
    if (eos_rect_empty(inner) || !s->ui) return;
    if (!eos_display_clip_push(inner)) return;

    // Header: name on the left in the focus colour, window id on the right in
    // the 4x6 face. The id is what makes a screenshot readable against the
    // layout arithmetic in eos_wm.c, which is the whole reason it is drawn.
    hdr_h = (int16_t)s->ui->h;
    text_fit(inner.x, inner.y, s->ui, t->focused ? s->text : s->muted, name, inner.w);

    snprintf(idbuf, sizeof idbuf, "%d", (int)t->win);
    id_w = (int16_t)eos_text_width(s->tiny, idbuf, -1);
    if (id_w > 0 && id_w < inner.w / 2)
        text_fit((int16_t)(inner.x + inner.w - id_w), (int16_t)(inner.y + 1),
                 s->tiny, s->muted, idbuf, id_w);

    eos_display_hline(inner.x, (int16_t)(inner.y + hdr_h + 1), inner.w,
                      t->focused ? s->accent : s->bunf);

    body = tile_body(s, t);
    if (!eos_rect_empty(body) && eos_display_clip_push(body)) {
        switch ((eos_app_id_t)t->app_id) {
        case EOS_APP_CLOCK: body_clock(v, s, body); break;
        case EOS_APP_BOARD: body_board(v, s, body); break;
        case EOS_APP_HEAP:  body_heap (v, s, body); break;
        case EOS_APP_KEYS:  body_keys (v, s, body); break;
        case EOS_APP_BUDDY: body_buddy(v, s, body); break;
        case EOS_APP_COUNT: break;
        }
        eos_display_clip_pop();
    }

    eos_display_clip_pop();
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
    for (i = 0; i < n; i++) draw_tile(v, s, &tiles[i]);
}

void eos_shell_draw_frame(const eos_shell_view_t *v)
{
    skin_t s;
    eos_rect_t band;

    if (!v || !v->theme || !v->wm) return;
    skin_build(&s, v->theme);

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
