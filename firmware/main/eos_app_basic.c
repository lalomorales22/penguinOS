// The five windows that are pure readouts: clock, board, heap, keys and
// settings. Four of them are lifted verbatim out of eos_shell_draw.c, where
// they were a switch; the fifth is new and is the panel's half of the web
// Settings page.
//
// Nothing in this file remembers anything. Every one of these bodies is a
// function of the frame state it is handed, which is what lets the scene
// replay them once per band and get identical pixels every time — and it is
// also why Settings is READ-ONLY here. Editing a value would need a cursor,
// a keyboard and a write path, and the board already has all three in the web
// app; the window exists so that someone standing in front of the panel can
// tell which theme, which brightness and which megabrain it is talking to
// without going and finding a phone.
//
// The one non-obvious constraint is width, not memory: the same body renders
// into a 110x76 tile — eighteen columns of the 6x8 face — and into a 228x203
// one. Every row here is laid out from the rect it is given rather than from a
// constant, and the label/value split collapses into two lines when the value
// would not otherwise get five columns to live in.

#include "eos_app_registry.h"
#include "eos_shell_draw.h"

#include <stdio.h>
#include <string.h>

#include "eos_font.h"
#include "eos_input.h"

// ------------------------------------------------------------------ clock

void eos_app_draw_clock(const eos_app_ctx_t *c, eos_rect_t r)
{
    char buf[16];
    uint32_t sec = c->view->uptime_ms / 1000u;
    const eos_font_t *f;
    int16_t y;

    snprintf(buf, sizeof buf, "%02u:%02u:%02u",
             (unsigned)(sec / 3600u), (unsigned)((sec / 60u) % 60u), (unsigned)(sec % 60u));

    // The largest face the tile can actually hold, not the largest face there
    // is. The 12x20 digits are twenty rows tall and a tile body can be twelve;
    // centring a face that does not fit puts half a digit outside the rect,
    // which the tile's clip used to hide and the host suite now does not.
    f = c->big;
    if ((int16_t)f->h > r.h) f = c->med;
    if ((int16_t)f->h > r.h) f = c->ui;
    if ((int16_t)f->h > r.h) return;

    y = (int16_t)(r.y + (r.h - (int16_t)f->h) / 2);
    if (y < r.y) y = r.y;
    eos_display_text_center(r, y, f, c->accent, buf);

    y = (int16_t)(y + (int16_t)f->h + 3);
    if (y + (int16_t)c->ui->h <= r.y + r.h)
        eos_display_text_center(r, y, c->ui, c->muted, "uptime");
}

// ------------------------------------------------------------------ board

void eos_app_draw_board(const eos_app_ctx_t *c, eos_rect_t r)
{
    int16_t y = r.y;
    int i;

    for (i = 0; i < 4; i++) {
        const eos_font_t *f = (i == 0) ? c->med : c->ui;
        if (!c->view->board_line[i]) continue;
        if (y + (int16_t)f->h > r.y + r.h) break;
        eos_app_text(r.x, y, f, (i == 0) ? c->text : c->muted,
                     c->view->board_line[i], r.w);
        y = (int16_t)(y + (int16_t)f->h + 2);
    }
}

// ------------------------------------------------------------------- heap

// A label and a number under it, in that order, and only while both still fit
// inside the rect. Every guard here is a bottom-edge check rather than a row
// count, because the two faces are different heights and the tile's own clip
// is not allowed to be what keeps a glyph inside its window.
static int16_t heap_pair(const eos_app_ctx_t *c, eos_rect_t r, int16_t y,
                         const char *label, uint32_t value)
{
    char buf[16];

    if (y + (int16_t)c->ui->h > r.y + r.h) return (int16_t)(r.y + r.h + 1);
    eos_app_text(r.x, y, c->ui, c->muted, label, r.w);
    y = (int16_t)(y + (int16_t)c->ui->h + 1);

    if (y + (int16_t)c->med->h > r.y + r.h) return (int16_t)(r.y + r.h + 1);
    snprintf(buf, sizeof buf, "%u", (unsigned)value);
    eos_app_text(r.x, y, c->med, c->text, buf, r.w);
    return (int16_t)(y + (int16_t)c->med->h + 4);
}

void eos_app_draw_heap(const eos_app_ctx_t *c, eos_rect_t r)
{
    int16_t y = heap_pair(c, r, r.y, "free", c->view->heap_free);
    heap_pair(c, r, y, "largest block", c->view->heap_largest);
}

// ------------------------------------------------------------------- keys

void eos_app_draw_keys(const eos_app_ctx_t *c, eos_rect_t r)
{
    // Six binds out of the seventy-two, chosen because they are the ones a
    // human will try first — and super+space is at the top now that it opens
    // something. A tall tile shows all six; a 76-pixel one stops when it runs
    // out of rows rather than drawing over its own border.
    static const struct { uint8_t mods; uint16_t key; } SHOW[] = {
        { EOS_MOD_SUPER,                   EOS_KEY_SPACE },
        { EOS_MOD_SUPER,                   EOS_KEY_ENTER },
        { EOS_MOD_SUPER,                   EOS_KEY_Q     },
        { EOS_MOD_SUPER,                   EOS_KEY_TAB   },
        { EOS_MOD_SUPER,                   EOS_KEY_2     },
        { EOS_MOD_SUPER | EOS_MOD_SHIFT,   EOS_KEY_B     },
    };
    int16_t y = r.y;
    size_t i;

    if (!c->view->keys) return;
    for (i = 0; i < sizeof SHOW / sizeof SHOW[0]; i++) {
        const eos_keybind_t *b = eos_keys_lookup(c->view->keys, SHOW[i].mods, SHOW[i].key);
        char chord[32], line[48];
        if (!b) continue;
        if (y + (int16_t)c->ui->h > r.y + r.h) break;
        eos_keys_format(b, chord, (int)sizeof chord);
        snprintf(line, sizeof line, "%s %s", chord,
                 eos_keys_action_name((eos_action_t)b->action));
        eos_app_text(r.x, y, c->ui, c->muted, line, r.w);
        y = (int16_t)(y + (int16_t)c->ui->h + 1);
    }
}

// --------------------------------------------------------------- settings
//
// One row is a label and a value. They share a line when the value can have at
// least five columns after the label column, and stack when it cannot, which
// is what happens on a panel narrower than this one rather than on this one.
// Returns the y the next row starts at, or a y past the bottom to stop.

#define SET_LABEL_CELLS 7

static int16_t set_row(const eos_app_ctx_t *c, eos_rect_t r, int16_t y,
                       const char *label, const char *value, eos_color_t vc)
{
    int16_t lw   = (int16_t)(SET_LABEL_CELLS * (int16_t)c->ui->cell_w);
    int16_t line = (int16_t)(c->ui->h + 1);

    if (y + (int16_t)c->ui->h > r.y + r.h) return (int16_t)(r.y + r.h + 1);

    if (r.w - lw >= 5 * (int16_t)c->ui->cell_w) {
        eos_app_text(r.x, y, c->ui, c->muted, label, lw);
        eos_app_text((int16_t)(r.x + lw), y, c->ui, vc, value, (int16_t)(r.w - lw));
        return (int16_t)(y + line);
    }

    eos_app_text(r.x, y, c->ui, c->muted, label, r.w);
    y = (int16_t)(y + line);
    if (y + (int16_t)c->ui->h > r.y + r.h) return (int16_t)(r.y + r.h + 1);
    eos_app_text((int16_t)(r.x + 2), y, c->ui, vc, value, (int16_t)(r.w - 2));
    return (int16_t)(y + line);
}

void eos_app_draw_settings(const eos_app_ctx_t *c, eos_rect_t r)
{
    const eos_settings_t *v = eos_app_settings();
    const eos_board_t    *b = eos_app_board();
    // Sized off the store's own longest string plus room for ":65535", so the
    // compiler can see it cannot truncate. A shorter buffer here builds fine
    // on the host and fails -Wformat-truncation on the target, which is the
    // sort of difference that gets found at the wrong moment.
    char buf[EOS_SETTINGS_BHOST_MAX + 8];
    int16_t y = r.y;

    if (!v) {
        eos_app_text(r.x, r.y, c->ui, c->muted, "no settings", r.w);
        return;
    }

    // The theme is read from the LIVE theme rather than from the stored key.
    // They disagree exactly when the file names a theme that would not load,
    // and that is the case where a settings window that echoed the file back
    // would be lying about what is on the glass.
    y = set_row(c, r, y, "theme",
                c->view->theme && c->view->theme->name[0] ? c->view->theme->name
                                                          : v->ui_theme,
                c->text);

    // Percent, the way the Settings page shows it, not the 0..255 the store
    // keeps. Nobody reads 204 as four fifths.
    snprintf(buf, sizeof buf, "%u%%", (unsigned)(((unsigned)v->ui_bright * 100u + 127u) / 255u));
    y = set_row(c, r, y, "bright", buf, c->text);

    y = set_row(c, r, y, "board",
                (b && b->name) ? b->name : "unknown", c->text);

    y = set_row(c, r, y, "host",
                v->net_host[0] ? v->net_host : "penguinos", c->muted);

    if (v->brain_port && v->brain_port != 80)
        snprintf(buf, sizeof buf, "%s:%u",
                 v->brain_host[0] ? v->brain_host : "auto", (unsigned)v->brain_port);
    else
        snprintf(buf, sizeof buf, "%s", v->brain_host[0] ? v->brain_host : "auto");
    y = set_row(c, r, y, "brain", buf, c->text);

    y = set_row(c, r, y, "model", v->brain_model[0] ? v->brain_model : "default", c->text);

    // The last line is the one thing on the tile that is not a setting: it says
    // this window will not change any of them. Without it the window reads as
    // an editor that is refusing to respond to the keyboard.
    if (y + (int16_t)c->ui->h <= r.y + r.h)
        eos_app_text(r.x, (int16_t)(r.y + r.h - c->ui->h), c->ui, c->muted,
                     "read-only. edit in the web app", r.w);
}
