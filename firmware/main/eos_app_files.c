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
#include <stdlib.h>
#include <string.h>

#include "eos_font.h"
#include "eos_storage.h"
#include "eos_display.h"

// ------------------------------------------------------------- the viewer
//
// Opening a .565 shows it. The format is what tools/mkimg.py writes: an
// eight-byte header then raw little-endian RGB565, which is EOS_PIXFMT_RGB565
// and therefore something eos_display_blit() takes without conversion.
//
// The board decodes NOTHING. There is no JPEG decoder in this image - the one
// in the tree belongs to firmware-cam, which has 8MB of PSRAM to decode into,
// against the 30KB largest free block on the tightest board here. So the
// decoding happened once on a desktop and what arrives is bytes for the panel.
//
// AND IT NEVER HOLDS A PICTURE. A 240x240 image is 115,200 bytes; the buffer
// below is 3,840. eos_app_camera.c reached the same conclusion about the same
// arithmetic and answered it by drawing one horizontal strip at a time, which
// is what happens here - except that the rows come from a seek instead of a
// socket, so the 65ms-per-request that makes the camera window feel heavy is
// not paid at all.
//
// The strips are chosen by eos_display_clip(): the draw runs once per display
// band, and each pass reads ONLY the rows that fall inside the band it was
// called for. That is what keeps this a pure function of position - the same
// band always produces the same pixels - and it means a full picture costs one
// pass over the file per frame rather than one per band.

#define IMG_MAGIC0 'E'
#define IMG_MAGIC1 '5'
#define IMG_HDR    8      // magic(2) version(1) flags(1) w(2) h(2)

// Rows held at once, and the buffer is TAKEN WHEN A PICTURE IS OPENED rather
// than reserved for the life of the image.
//
// It was static: uint8_t s_rowbuf[EOS_LCD_W * 2 * IMG_ROWS], which on the
// 480-wide 4.0in CYD is 3,840 bytes of BSS. That board is an ESP32 and it links
// with almost nothing to spare - it had already overflowed dram0_0_seg once
// before, by 56 bytes, and this pushed it over by 2,800. The link fails; there
// is no runtime symptom to debug, and the cost was being paid on every board
// whether or not anyone ever opened a picture.
//
// Now it is sized for THIS image and freed on close, so a board that never
// opens one spends nothing at all. malloc rather than a fixed pool because the
// size depends on the picture's width, which is not knowable at build time -
// and this is app code, not the kernel, which is where the no-allocation rule
// lives.
#define IMG_ROWS   4

static struct {
    char     path[96];
    uint16_t w, h;
    bool     open;          // a .565 is being viewed
    bool     bad;           // it was opened and is not one
} V;

static uint8_t *s_rowbuf;      // NULL unless a picture is open
static int      s_rowbuf_cap;  // bytes actually held

// Case-insensitive, because a file that came off a desktop may well be .565 or
// .565 with whatever case a file manager felt like.
static bool is_image(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n < 5) return false;
    return name[n - 4] == '.' && name[n - 3] == '5' &&
           name[n - 2] == '6' && name[n - 1] == '5';
}

// Reads the header only. The pixels are never all in memory at once, so
// "opening" an image is eight bytes and a size check.
static void view_open(const char *dir, const char *name)
{
    eos_file_t *f;
    uint8_t hdr[IMG_HDR];
    size_t base = strlen(dir);

    memset(&V, 0, sizeof V);
    if (base + 1u + strlen(name) >= sizeof V.path) return;
    if (base == 1u && dir[0] == '/') base = 0u;
    memcpy(V.path, dir, base);
    V.path[base] = '/';
    memcpy(V.path + base + 1u, name, strlen(name) + 1u);

    f = eos_storage_open(V.path, EOS_O_READ);
    if (!f) { V.open = true; V.bad = true; return; }
    if (eos_storage_read(f, hdr, IMG_HDR) != IMG_HDR ||
        hdr[0] != IMG_MAGIC0 || hdr[1] != IMG_MAGIC1) {
        eos_storage_close(f);
        V.open = true; V.bad = true;
        return;
    }
    V.w = (uint16_t)(hdr[4] | (hdr[5] << 8));
    V.h = (uint16_t)(hdr[6] | (hdr[7] << 8));
    // A header that claims more than the file holds is a truncated upload, and
    // drawing it would read past the end row after row.
    if (V.w == 0 || V.h == 0 ||
        eos_storage_size(f) < (int64_t)IMG_HDR + (int64_t)V.w * V.h * 2) {
        V.bad = true;
    }
    eos_storage_close(f);

    // One row is the floor - a picture cannot be drawn in less - and IMG_ROWS
    // of them is the ceiling. A board that cannot spare even one row's worth
    // reports the image as unreadable rather than drawing something wrong.
    if (!V.bad) {
        s_rowbuf_cap = (int)V.w * 2 * IMG_ROWS;
        s_rowbuf = (uint8_t *)malloc((size_t)s_rowbuf_cap);
        if (!s_rowbuf) {
            s_rowbuf_cap = (int)V.w * 2;
            s_rowbuf = (uint8_t *)malloc((size_t)s_rowbuf_cap);
        }
        if (!s_rowbuf) { s_rowbuf_cap = 0; V.bad = true; }
    }
    V.open = true;
}

static void view_close(void)
{
    free(s_rowbuf);
    s_rowbuf = NULL;
    s_rowbuf_cap = 0;
    memset(&V, 0, sizeof V);
}

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

    // While a picture is up the window is a viewer, not a browser: anything
    // that means "back" closes it and everything else is swallowed, so an
    // arrow key cannot quietly move the selection behind the image.
    if (V.open) {
        switch (e->key) {
        case EOS_KEY_ESC:
        case EOS_KEY_LEFT:
        case EOS_KEY_BKSP:
        case EOS_KEY_ENTER:
            view_close();
            F.dirty = true;
            return true;
        default:
            return true;
        }
    }

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
        if (F.sel >= 0 && F.sel < (int16_t)F.n) {
            const entry_t *sel = &F.e[F.sel];
            if (!sel->is_dir && is_image(sel->name)) {
                view_open(F.path, sel->name);
                F.dirty = true;
            } else {
                go_into(sel);
            }
        }
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

// Draws only the rows of the picture that land inside the band this call was
// made for. eos_display_clip() is that band (intersected with the window), so
// the loop below is a seek and a read of at most IMG_ROWS rows at a time and
// the whole picture never exists anywhere at once.
static void draw_image(const eos_app_ctx_t *c, eos_rect_t r)
{
    eos_rect_t clip = eos_display_clip();
    eos_file_t *f;
    int16_t ox, oy;
    int y0, y1, y;

    if (V.bad) {
        eos_app_text(r.x, r.y, c->ui, c->muted, "not a .565 image", r.w);
        return;
    }

    // Centred, and clipped by the blit when the picture is larger than the
    // window - which is the common case on a tile, and is a crop rather than a
    // scale because scaling would mean holding rows to resample them.
    ox = (int16_t)(r.x + (r.w - (int16_t)V.w) / 2);
    oy = (int16_t)(r.y + (r.h - (int16_t)V.h) / 2);

    // Which picture rows this band wants. Everything outside is another band's
    // problem and reading it here would multiply the file traffic by the number
    // of bands.
    y0 = clip.y - oy;
    y1 = (clip.y + clip.h) - oy;
    if (y0 < 0) y0 = 0;
    if (y1 > (int)V.h) y1 = (int)V.h;
    if (y0 >= y1) return;

    f = eos_storage_open(V.path, EOS_O_READ);
    if (!f) return;

    if (!s_rowbuf || s_rowbuf_cap <= 0) return;
    {
    const int rows_at_once = s_rowbuf_cap / ((int)V.w * 2);
    if (rows_at_once < 1) return;

    for (y = y0; y < y1; y += rows_at_once) {
        int n = (y + rows_at_once <= y1) ? rows_at_once : (y1 - y);
        int want = n * (int)V.w * 2;
        eos_bitmap_t b;

        if (want > s_rowbuf_cap) break;
        if (eos_storage_seek(f, (int64_t)IMG_HDR + (int64_t)y * V.w * 2,
                             EOS_SEEK_SET) < 0) break;
        if (eos_storage_read(f, s_rowbuf, want) != want) break;

        memset(&b, 0, sizeof b);
        b.pixels = s_rowbuf;
        b.w      = (int16_t)V.w;
        b.h      = (int16_t)n;
        b.stride = (int16_t)(V.w * 2);
        b.fmt    = EOS_PIXFMT_RGB565;
        b.key    = EOS_COLOR_NONE;
        eos_display_blit(ox, (int16_t)(oy + y), &b);
    }
    }
    eos_storage_close(f);
}

void eos_app_draw_files(const eos_app_ctx_t *c, eos_rect_t r)
{
    int16_t line_h, y, cw;
    int rows, top, i;
    char sbuf[12];

    if (!c->ui || eos_rect_empty(r)) return;
    if (V.open) { draw_image(c, r); return; }
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
