// eos_app_files — a browser over the internal filesystem, on the panel.
//
// Names, sizes, and into and out of directories. That is the whole feature,
// and the omission is deliberate: there is no delete, no rename and no write.
// Deleting things with a trackpad and a five-pixel row on a 240 px screen is
// how files get lost, and every one of those verbs already exists in the web
// app where there is a cursor and a confirmation. This window answers "what is
// actually on the board", which is the question you have when you are standing
// in front of it.
//
// The one non-obvious constraint: a LittleFS readdir is a run of flash reads
// with the instruction cache off, and the draw runs ONCE PER DISPLAY BAND. So
// the scan happens in the tick and only while the window is visible, the
// result lands in the fixed table below, and the draw does nothing but format
// what is already there. A directory listing that took six flash walks per
// frame would stall the compositor on the same task.
//
// The second constraint is eos_storage's handle pool: EOS_MAX_DIRS is 2 for
// the whole image. This file opens exactly one directory, reads it to the end
// or to the table's capacity, and closes it inside the same function — there
// is never a scan left open across a call.

#include "eos_app_registry.h"
#include "eos_shell_draw.h"

#include <stdio.h>
#include <string.h>

#include "eos_font.h"
#include "eos_storage.h"

// Twenty entries at forty-eight bytes each. A LittleFS partition on this board
// holds the web app, a theme or two and a buddy; twenty is more than any
// directory on it has, and a directory that overflows says so on its last row
// rather than silently showing a prefix.
#ifndef EOS_FILES_MAX
#define EOS_FILES_MAX 20
#endif

// How long a listing is trusted before it is walked again. Long enough that a
// visible window is not re-reading flash every quarter second, short enough
// that a file uploaded through the web app shows up while you are looking.
#ifndef EOS_FILES_RESCAN_MS
#define EOS_FILES_RESCAN_MS 3000
#endif

#ifndef EOS_FILES_ROOT
#define EOS_FILES_ROOT "/int"
#endif

typedef struct {
    char     name[EOS_NAME_MAX];
    uint32_t size;
    bool     is_dir;
} entry_t;

static struct {
    char     path[EOS_PATH_MAX];
    entry_t  e[EOS_FILES_MAX];
    uint8_t  n;
    bool     truncated;      // the directory had more than the table holds
    int8_t   err;            // eos_err_t of the last scan, 0 when it worked
    int16_t  sel;
    bool     need_scan;
    bool     dirty;
    uint32_t scanned_ms;
} F = { EOS_FILES_ROOT, { { { 0 }, 0, false } }, 0, false, 0, 0, true, true, 0 };

uint32_t eos_app_files_bytes(void) { return (uint32_t)sizeof F; }

// ------------------------------------------------------------------ scan

// Directories first, then case-sensitive name order. An insertion sort over at
// most twenty entries, which is a few hundred comparisons on a path that runs
// once every three seconds — a qsort would be more code and more stack for a
// list this size.
static bool before(const entry_t *a, const entry_t *b)
{
    if (a->is_dir != b->is_dir) return a->is_dir;
    return strcmp(a->name, b->name) < 0;
}

static void sort_entries(void)
{
    int i, j;
    for (i = 1; i < (int)F.n; i++) {
        entry_t k = F.e[i];
        for (j = i - 1; j >= 0 && before(&k, &F.e[j]); j--)
            F.e[j + 1] = F.e[j];
        F.e[j + 1] = k;
    }
}

static void scan(uint32_t now_ms)
{
    eos_dirh_t  *d;
    eos_dirent_t de;

    F.n = 0;
    F.truncated = false;
    F.err = 0;
    F.scanned_ms = now_ms ? now_ms : 1u;
    F.dirty = true;
    F.need_scan = false;

    d = eos_storage_opendir(F.path);
    if (!d) {
        F.err = (int8_t)eos_storage_errno();
        return;
    }
    while (eos_storage_readdir(d, &de)) {
        if (F.n >= EOS_FILES_MAX) { F.truncated = true; break; }
        // "." and ".." are not names anybody wants a row for, and the parent is
        // reached with backspace rather than by selecting a dot.
        if (de.name[0] == '.' && (de.name[1] == '\0' ||
            (de.name[1] == '.' && de.name[2] == '\0'))) continue;
        snprintf(F.e[F.n].name, sizeof F.e[F.n].name, "%s", de.name);
        F.e[F.n].size   = de.size;
        F.e[F.n].is_dir = de.is_dir;
        F.n++;
    }
    eos_storage_closedir(d);
    sort_entries();

    if (F.sel >= (int16_t)F.n) F.sel = (int16_t)(F.n ? F.n - 1 : 0);
    if (F.sel < 0) F.sel = 0;
}

void eos_app_files_tick(bool visible, uint32_t now_ms)
{
    if (!visible) return;
    if (F.need_scan || F.scanned_ms == 0 ||
        (uint32_t)(now_ms - F.scanned_ms) >= EOS_FILES_RESCAN_MS)
        scan(now_ms);
}

bool eos_app_files_take_dirty(void)
{
    bool d = F.dirty;
    F.dirty = false;
    return d;
}

// ------------------------------------------------------------- navigation

static void go_into(const entry_t *e)
{
    size_t len  = strlen(F.path);
    size_t nlen = strlen(e->name);
    size_t base;

    if (!e->is_dir) return;

    // "/" is the mount list and its children are already absolute, so the
    // separator is the root's own slash rather than a second one.
    base = (len == 1 && F.path[0] == '/') ? 0u : len;

    // Measured and REFUSED rather than formatted and truncated. A truncated
    // path is a path to somewhere else, and this window would then list that
    // somewhere else's contents under the name of the directory you asked for.
    // It is also the one place a 96-byte buffer and a 40-byte name meet, which
    // is exactly where a snprintf would have been silently clipping.
    if (base + 1u + nlen >= sizeof F.path) return;

    F.path[base] = '/';
    memcpy(F.path + base + 1u, e->name, nlen + 1u);

    F.sel = 0;
    F.need_scan = true;
    F.dirty = true;
}

static void go_up(void)
{
    char *slash;

    if (F.path[0] == '/' && F.path[1] == '\0') return;   // already the mount list

    slash = strrchr(F.path, '/');
    if (!slash || slash == F.path) {
        F.path[0] = '/';
        F.path[1] = '\0';
    } else {
        *slash = '\0';
    }
    F.sel = 0;
    F.need_scan = true;
    F.dirty = true;
}

bool eos_app_files_key(const eos_event_t *e)
{
    if (!e) return false;
    if (e->type != EOS_EV_KEY_DOWN && e->type != EOS_EV_KEY_REPEAT) return false;

    switch (e->key) {
    case EOS_KEY_UP:
        if (F.sel > 0) F.sel--;
        F.dirty = true;
        return true;
    case EOS_KEY_DOWN:
        if (F.sel + 1 < (int16_t)F.n) F.sel++;
        F.dirty = true;
        return true;
    case EOS_KEY_ENTER:
    case EOS_KEY_RIGHT:
        if (F.sel >= 0 && F.sel < (int16_t)F.n) go_into(&F.e[F.sel]);
        return true;
    case EOS_KEY_LEFT:
    case EOS_KEY_BKSP:
        go_up();
        return true;
    case EOS_KEY_ESC:
        snprintf(F.path, sizeof F.path, "%s", EOS_FILES_ROOT);
        F.sel = 0; F.need_scan = true; F.dirty = true;
        return true;
    default:
        break;
    }
    return false;
}

// ------------------------------------------------------------------- draw

// Three characters and a unit, so the size column is a fixed width and the
// names above and below it line up. Bytes up to 1023, then K, then M.
static void size_str(uint32_t n, char *out, size_t cap)
{
    if (n < 1024u)              snprintf(out, cap, "%uB", (unsigned)n);
    else if (n < 1024u * 1024u) snprintf(out, cap, "%uK", (unsigned)(n / 1024u));
    else                        snprintf(out, cap, "%uM", (unsigned)(n / (1024u * 1024u)));
}

// The tail of a path, so a deep directory still shows the part that changes.
// "/int/web/assets" in fourteen columns is "…web/assets", not "/int/web/as".
static const char *path_tail(const char *p, int cols)
{
    int len = (int)strlen(p);
    int i;

    if (len <= cols) return p;
    i = len - cols + 1;
    while (i < len && p[i] != '/') i++;
    return (i < len) ? p + i : p + len - cols;
}

void eos_app_draw_files(const eos_app_ctx_t *c, eos_rect_t r)
{
    int16_t line_h, y, cw;
    int rows, top, i;
    char sbuf[12];

    if (!c->ui || eos_rect_empty(r)) return;
    line_h = (int16_t)(c->ui->h + 1);
    cw     = (int16_t)c->ui->cell_w;
    if (cw <= 0) cw = 6;

    if (r.h < 2 * line_h) {
        eos_app_text(r.x, r.y, c->ui, c->muted, "files", r.w);
        return;
    }

    // The path, in the accent colour, then the rows under it.
    eos_app_text(r.x, r.y, c->ui, c->accent,
                 path_tail(F.path, r.w / cw), r.w);
    y    = (int16_t)(r.y + line_h);
    rows = (r.y + r.h - y) / line_h;
    if (rows < 1) return;

    if (F.err != 0) {
        eos_app_text(r.x, y, c->ui, c->warn, eos_strerr((eos_err_t)F.err), r.w);
        return;
    }
    if (F.n == 0) {
        eos_app_text(r.x, y, c->ui, c->muted,
                     F.scanned_ms ? "empty" : "reading...", r.w);
        return;
    }

    // The scroll window is derived from the selection here rather than kept in
    // the state, because the number of rows is a property of the RECT and the
    // same window is drawn into two different sizes when it moves between
    // tiles. Keeping `top` in the state would leave it correct for whichever
    // tile was drawn last.
    top = F.sel - rows / 2;
    if (top > (int)F.n - rows) top = (int)F.n - rows;
    if (top < 0) top = 0;

    for (i = top; i < (int)F.n && i < top + rows; i++) {
        const entry_t *e = &F.e[i];
        bool on = (i == F.sel);
        eos_color_t fg;
        int16_t sw = 0;

        if (on && c->focused) {
            eos_display_fill(eos_rect(r.x, y, r.w, line_h), c->accent);
            fg = c->bg;
        } else if (on) {
            eos_display_fill(eos_rect(r.x, y, r.w, line_h), c->bunf);
            fg = c->text;
        } else {
            fg = e->is_dir ? c->text : c->muted;
        }

        // The size sits at the right edge and the name gets what is left, so a
        // long name is truncated and the number never is. A truncated size is
        // worse than no size.
        if (!e->is_dir) {
            size_str(e->size, sbuf, sizeof sbuf);
            sw = (int16_t)eos_text_width(c->ui, sbuf, -1);
            if (sw + 2 * cw < r.w)
                eos_app_text((int16_t)(r.x + r.w - sw), y, c->ui, fg, sbuf, sw);
            else
                sw = 0;
        }

        if (e->is_dir) {
            char nb[EOS_NAME_MAX + 2];
            snprintf(nb, sizeof nb, "%s/", e->name);
            eos_app_text(r.x, y, c->ui, fg, nb, (int16_t)(r.w - sw - (sw ? cw : 0)));
        } else {
            eos_app_text(r.x, y, c->ui, fg, e->name,
                         (int16_t)(r.w - sw - (sw ? cw : 0)));
        }
        y = (int16_t)(y + line_h);
    }

    if (F.truncated && y + (int16_t)c->ui->h <= r.y + r.h)
        eos_app_text(r.x, y, c->ui, c->warn, "...more", r.w);
}
