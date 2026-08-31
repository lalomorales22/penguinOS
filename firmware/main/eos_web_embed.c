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
// Paths are ANCHORED to the run root and then matched on the last component.
// This comment used to say the basename alone was matched, "so the same table
// serves both modes" - and that was the bug. Every asset here belongs to the
// RUN app; eos_httpd's BUILTIN_SETUP inlines its own script and needs none of
// them. Matching a bare basename made a SETUP request for
// "/int/setup/index.html" resolve to the RUN app's index.html, serving 20 KB
// where 2,756 bytes were intended and making BUILTIN_SETUP unreachable.
// See eos_web_path.h, which holds the arithmetic and is host-testable.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "eos_httpd.h"
#include "eos_web_path.h"

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

// Every asset here belongs to the RUN application: web/index.html loads
// style.css, setup.js, voxel-editor.js and app.js. SETUP mode wants none of
// them - eos_httpd's BUILTIN_SETUP page inlines its own script and is entirely
// self-contained.
//
// So the lookup is anchored to the run root. It used to match on BASENAME
// alone, discarding everything up to the last '/', which meant a SETUP-mode
// request resolving to "/int/setup/index.html" matched the RUN app's
// "index.html" and served the 20 KB application in place of the 2,756-byte
// panel - making BUILTIN_SETUP unreachable code.
//
// That was not only the wrong page. It was the only response the server could
// produce larger than CONFIG_LWIP_TCP_SND_BUF_DEFAULT (5,760 bytes), and
// send() returns EAGAIN precisely when it cannot place one byte for the whole
// send timeout - so it was also the only way setup mode could hang. It did, on
// the board with the least internal DRAM in the fleet.
static const char *s_root = "/int/web";

static const asset_t *find(const char *path)
{
    const char *base = eos_web_asset_base(s_root, path);
    int i;

    if (!base) return NULL;
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
    // Take the run root from the config rather than repeating the string, so
    // changing cfg.root_run cannot silently desync the asset lookup from it.
    if (h->cfg.root_run && h->cfg.root_run[0]) s_root = h->cfg.root_run;
    h->ports.file_open  = web_open;
    h->ports.file_read  = web_read;
    h->ports.file_close = web_close;
}
