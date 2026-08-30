// eos_web_embed — serves the web app out of flash instead of a filesystem.
//
// eos_httpd asks for files through three ports and does not care where the
// bytes come from. kernel/hal has no storage backend, so on this board every
// static route answered 404 and the only page that existed was the built-in
// setup form. A board that has joined a network and then serves nothing is a
// board you reach with an arp scan, which is not a product.
//
// So the assets are linked into the image with EMBED_FILES and these ports read
// them straight out of the flash cache. The one non-obvious constraint: the
// symbols are placed in rodata and are memory-mapped, so file_read is a memcpy
// and never touches the heap — which is why the handle pool is four fixed slots
// and there is no allocation anywhere in this file.
//
// Paths are matched on the LAST component only. eos_httpd builds a full path
// from its document root ("/int/web/app.js") and also probes "<path>.gz"
// first; neither root exists here, so matching the basename lets the same
// table serve both modes without pretending to be a directory tree.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "eos_httpd.h"

#define ASSET(sym)                                                       \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start");    \
    extern const uint8_t sym##_end[]   asm("_binary_" #sym "_end");

ASSET(index_html)
ASSET(app_js)
ASSET(style_css)
ASSET(setup_js)
ASSET(voxel_editor_js)

typedef struct {
    const char    *name;
    const uint8_t *start;
    const uint8_t *end;
} asset_t;

static const asset_t ASSETS[] = {
    { "index.html",      index_html_start,      index_html_end      },
    { "app.js",          app_js_start,          app_js_end          },
    { "style.css",       style_css_start,       style_css_end       },
    { "setup.js",        setup_js_start,        setup_js_end        },
    { "voxel-editor.js", voxel_editor_js_start, voxel_editor_js_end },
};
#define ASSET_COUNT ((int)(sizeof ASSETS / sizeof ASSETS[0]))

// Four concurrent reads. eos_httpd runs four workers and each holds at most one
// open file, so this cannot be exhausted by a client no matter how it behaves.
typedef struct {
    bool           used;
    const uint8_t *p;
    const uint8_t *end;
} slot_t;

static slot_t s_slots[4];

static const asset_t *find(const char *path)
{
    const char *base;
    int i;

    if (!path) return NULL;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;

    // "/" and "/int/web/" both mean the app itself.
    if (!base[0]) base = "index.html";

    for (i = 0; i < ASSET_COUNT; i++)
        if (strcmp(base, ASSETS[i].name) == 0) return &ASSETS[i];
    return NULL;
}

static void *web_open(void *ctx, const char *path, long *size_out)
{
    const asset_t *a = find(path);
    int i;

    (void)ctx;
    if (!a) return NULL;

    for (i = 0; i < (int)(sizeof s_slots / sizeof s_slots[0]); i++) {
        if (s_slots[i].used) continue;
        s_slots[i].used = true;
        s_slots[i].p    = a->start;
        s_slots[i].end  = a->end;
        if (size_out) *size_out = (long)(a->end - a->start);
        return &s_slots[i];
    }
    return NULL;   // all four in flight; eos_httpd renders this as a 404
}

static int web_read(void *ctx, void *fh, void *buf, int n)
{
    slot_t *s = (slot_t *)fh;
    long left;

    (void)ctx;
    if (!s || !buf || n <= 0) return 0;
    left = (long)(s->end - s->p);
    if (left <= 0) return 0;
    if ((long)n > left) n = (int)left;
    memcpy(buf, s->p, (size_t)n);
    s->p += n;
    return n;
}

static void web_close(void *ctx, void *fh)
{
    slot_t *s = (slot_t *)fh;
    (void)ctx;
    if (s) { s->used = false; s->p = NULL; s->end = NULL; }
}

void eos_web_embed_bind(eos_httpd_t *h)
{
    if (!h) return;
    h->ports.file_open  = web_open;
    h->ports.file_read  = web_read;
    h->ports.file_close = web_close;
}
