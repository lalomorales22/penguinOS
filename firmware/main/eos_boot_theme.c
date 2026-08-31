// Theme discovery and palette upload. See eos_boot_theme.h.

#include "eos_boot_theme.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "eos_display.h"

static const char *TAG = "eos.theme";

// One scratch buffer for the whole search, in BSS. The kernel does not
// allocate and neither does its boot glue, so the size is a decision and not a
// growth policy: the four themes in kernel/theme/themes are 1.6 to 1.9 KB and
// eos_theme_parse() reads a length, never a terminator, so a file that does
// not fit is refused rather than truncated into a syntax error.
#define THEME_BUF 4096
static char theme_buf[THEME_BUF];

// The theme linked into the image. This is what makes a board with no card and
// no mounted filesystem still come up in cyd-amber instead of in the neutral
// slate default, and it costs about 1.7 KB of flash to say so. It is also the
// only thing that exercises eos_theme_parse() on target: 213 host checks say
// the parser is right, and none of them ran on a RISC-V.
extern const char theme_embedded_start[] asm("_binary_cyd_amber_json_start");
extern const char theme_embedded_end[]   asm("_binary_cyd_amber_json_end");

const char *eos_boot_theme_src_name(eos_theme_src_t s)
{
    switch (s) {
    case EOS_THEME_SRC_SD:       return "sd";
    case EOS_THEME_SRC_INT:      return "internal fs";
    case EOS_THEME_SRC_EMBEDDED: return "embedded";
    case EOS_THEME_SRC_DEFAULT:  return "compiled-in default";
    }
    return "?";
}

// Reads a file into theme_buf. Returns the byte count, or -1 when there is no
// such file, no such mount, or more file than buffer. Every one of those is a
// normal outcome on a board whose filesystem nothing has mounted yet, so none
// of them is an error here.
static int slurp(const char *mount, const char *leaf)
{
    char path[96];
    FILE *f;
    size_t n;

    if (!mount) return -1;
    snprintf(path, sizeof path, "%s/%s", mount, leaf);

    f = fopen(path, "rb");
    if (!f) return -1;

    n = fread(theme_buf, 1, sizeof theme_buf, f);
    // A file that exactly fills the buffer is indistinguishable from one that
    // overflows it, so both are refused. Silently parsing the first 4096 bytes
    // of a larger file would be a truncated theme presented as a valid one.
    if (n >= sizeof theme_buf) { fclose(f); return -1; }
    fclose(f);

    return (int)n;
}

static bool try_parse(eos_theme_t *out, const char *buf, int len, const char *where)
{
    eos_theme_err_t e;

    if (len <= 0) return false;
    e = eos_theme_parse(out, buf, len);
    if (e == EOS_THEME_OK) return true;

    // eos_theme_parse() has already put the default back into `out`, so this
    // is genuinely just a log line and the caller keeps searching.
    ESP_LOGW(TAG, "%s: %s (%d bytes) - ignored", where, eos_theme_strerror(e), len);
    return false;
}

// What the settings store asked for, or "" for the plain theme.json search.
// One name for the image; eos_boot_theme_prefer() is called once at boot and
// again by a live theme switch, both from the OS loop.
static char pref[EOS_THEME_NAME_MAX];

// Parked next to the scratch buffer for the same reason: eos_boot_theme_read()
// must be able to fail without touching the caller's theme, and eos_theme_parse
// writes the default into its output on every failure. About 700 bytes.
static eos_theme_t scratch;

void eos_boot_theme_prefer(const char *name)
{
    if (!name) { pref[0] = '\0'; return; }
    snprintf(pref, sizeof pref, "%s", name);
}

eos_theme_err_t eos_boot_theme_read(const char *path, eos_theme_t *out)
{
    FILE *f;
    size_t n;
    eos_theme_err_t e;

    if (!path || !out) return EOS_THEME_ERR_ARGS;

    f = fopen(path, "rb");
    if (!f) return EOS_THEME_ERR_EMPTY;
    n = fread(theme_buf, 1, sizeof theme_buf, f);
    fclose(f);
    if (n == 0 || n >= sizeof theme_buf) return EOS_THEME_ERR_EMPTY;

    e = eos_theme_parse(&scratch, theme_buf, (int)n);
    if (e != EOS_THEME_OK) return e;

    *out = scratch;
    return EOS_THEME_OK;
}

// "<mount>/themes/<pref>.json", when the settings store named one. The stem of
// the filename IS the theme name everywhere the picker is concerned — see the
// note in eos_settings_bind.c on why the name inside the file is not used for
// this — so the path is built rather than searched for.
static int slurp_named(const char *mount)
{
    char leaf[EOS_THEME_NAME_MAX + 16];

    if (!pref[0]) return -1;
    snprintf(leaf, sizeof leaf, "themes/%s.json", pref);
    return slurp(mount, leaf);
}

eos_theme_src_t eos_boot_theme_load(const eos_board_t *b, eos_theme_t *out)
{
    int n;

    eos_theme_default(out);
    if (!b || !out) return EOS_THEME_SRC_DEFAULT;

    // The card first: it is the one a human can change without a rebuild, so
    // it outranks everything below it. This board declares no slot, so the
    // mount point is NULL and slurp() refuses immediately.
    if (b->storage.sd && b->storage.sd_point) {
        n = slurp_named(b->storage.sd_point);
        if (try_parse(out, theme_buf, n, b->storage.sd_point)) return EOS_THEME_SRC_SD;
    }
    n = slurp_named(b->storage.int_point);
    if (try_parse(out, theme_buf, n, b->storage.int_point)) return EOS_THEME_SRC_INT;

    if (b->storage.sd && b->storage.sd_point) {
        n = slurp(b->storage.sd_point, "theme.json");
        if (try_parse(out, theme_buf, n, b->storage.sd_point)) return EOS_THEME_SRC_SD;
    }

    // Then the internal filesystem, under the older convention. /int is a
    // mounted LittleFS from app_main step 2 onward, so this is a real open on
    // a real partition; on a board nobody has written a theme to it simply
    // finds nothing, and the embedded copy below answers.
    n = slurp(b->storage.int_point, "theme.json");
    if (try_parse(out, theme_buf, n, b->storage.int_point)) return EOS_THEME_SRC_INT;

    n = (int)(theme_embedded_end - theme_embedded_start);
    // EMBED_TXTFILES appends a NUL that is not part of the document.
    if (n > 0) n--;
    if (try_parse(out, theme_embedded_start, n, "embedded")) return EOS_THEME_SRC_EMBEDDED;

    eos_theme_default(out);
    return EOS_THEME_SRC_DEFAULT;
}

void eos_boot_theme_upload(const eos_theme_t *t)
{
    // 32 entries at a time: 128 bytes of stack instead of a kilobyte, and
    // eight calls instead of one. The display converts and stores; nothing
    // here is on a hot path.
    enum { CHUNK = 32 };
    uint32_t rgb[CHUNK];
    int i, j;

    if (!t) return;
    for (i = 0; i < EOS_PAL_SIZE; i += CHUNK) {
        for (j = 0; j < CHUNK; j++) {
            eos_rgb_t c = eos_theme_palette_rgb(t, (uint8_t)(i + j));
            rgb[j] = eos_rgb(c.r, c.g, c.b);
        }
        eos_display_palette(rgb, (uint16_t)i, (uint16_t)CHUNK);
    }
}
