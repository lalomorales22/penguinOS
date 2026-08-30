#include "eos_theme.h"
#include <string.h>

#define KEY_MAX 40

// ------------------------------------------------------------------- tables

static const char *ROLE_NAME[EOS_ROLE_COUNT] = {
    "bg", "surface", "overlay", "text", "muted", "accent", "accent_alt",
    "ok", "warn", "err", "border_focused", "border_unfocused",
    "bar_bg", "bar_fg", "tab_active", "tab_inactive"
};

static const char *ANSI_NAME[EOS_ANSI_COUNT] = {
    "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
    "bright_black", "bright_red", "bright_green", "bright_yellow",
    "bright_blue", "bright_magenta", "bright_cyan", "bright_white"
};

// The compiled-in fallback. Neutral slate with a cyan accent: perfectly usable,
// and unlike any of the four themes on the card, so a board that fell back
// says so on its own screen without anyone having to read a log.
static const uint32_t DEF_ROLE[EOS_ROLE_COUNT] = {
    0x101014, 0x1b1b22, 0x262630, 0xe6e6ec, 0x8a8a99, 0x4ec9d4, 0xc586c0,
    0x6ac46a, 0xd7a844, 0xe05555, 0x4ec9d4, 0x33333d,
    0x08080b, 0xc8c8d2, 0x4ec9d4, 0x33333d
};
static const uint32_t DEF_ANSI[EOS_ANSI_COUNT] = {
    0x101014, 0xe05555, 0x6ac46a, 0xd7a844, 0x5a9bd8, 0xc586c0, 0x4ec9d4, 0xc8c8d2,
    0x33333d, 0xff7b7b, 0x8fe08f, 0xf0c65c, 0x7fb8ee, 0xdfa6da, 0x7fe3ec, 0xffffff
};

// ------------------------------------------------------------------- colour

static eos_rgb_t rgb24(uint32_t v)
{
    eos_rgb_t c;
    c.r = (uint8_t)(v >> 16);
    c.g = (uint8_t)(v >> 8);
    c.b = (uint8_t)v;
    return c;
}

uint16_t eos_theme_rgb565(eos_rgb_t c)
{
    return (uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
}

// Bit replication, not a shift: 0x1F must come back as 0xFF, not 0xF8, or the
// palette's white is a slightly grey white and every nearest-match is biased.
eos_rgb_t eos_theme_un565(uint16_t v)
{
    eos_rgb_t c;
    uint8_t r5 = (uint8_t)((v >> 11) & 0x1F);
    uint8_t g6 = (uint8_t)((v >> 5) & 0x3F);
    uint8_t b5 = (uint8_t)(v & 0x1F);
    c.r = (uint8_t)((r5 << 3) | (r5 >> 2));
    c.g = (uint8_t)((g6 << 2) | (g6 >> 4));
    c.b = (uint8_t)((b5 << 3) | (b5 >> 2));
    return c;
}

uint8_t eos_theme_luma(eos_rgb_t c)
{
    return (uint8_t)((77 * (int)c.r + 150 * (int)c.g + 29 * (int)c.b) >> 8);
}

static eos_rgb_t blend(eos_rgb_t a, eos_rgb_t b, int num, int den)
{
    eos_rgb_t o;
    o.r = (uint8_t)((a.r * (den - num) + b.r * num + den / 2) / den);
    o.g = (uint8_t)((a.g * (den - num) + b.g * num + den / 2) / den);
    o.b = (uint8_t)((a.b * (den - num) + b.b * num + den / 2) / den);
    return o;
}

uint8_t eos_theme_cube_index(eos_rgb_t c)
{
    int ri = (c.r * (EOS_PAL_CUBE_R - 1) + 127) / 255;
    int gi = (c.g * (EOS_PAL_CUBE_G - 1) + 127) / 255;
    int bi = (c.b * (EOS_PAL_CUBE_B - 1) + 127) / 255;
    int idx = EOS_PAL_CUBE_BASE +
              (ri * EOS_PAL_CUBE_G + gi) * EOS_PAL_CUBE_B + bi;

    // Slot 255 belongs to the display HAL, which reserves it as EOS_COLOR_NONE
    // — the transparency sentinel that never resolves to a pixel. So the cube
    // may not hand out its top cell, or every white pixel silently vanishes.
    // The cell steps back one GREEN level rather than one blue one: green is
    // quantised in eight levels and blue in four, so giving up green costs 36
    // counts where giving up blue would cost 85. (255,255,255) therefore
    // renders as (255,219,255), inside the cube's own worst-case error.
    if (idx == EOS_PAL_CUBE_NONE) idx -= EOS_PAL_CUBE_B;
    return (uint8_t)idx;
}

uint8_t eos_theme_index(const eos_theme_t *t, eos_rgb_t c)
{
    int best = 0;
    long bestd = 1L << 30;
    int i;
    if (!t) return eos_theme_cube_index(c);
    // EOS_PAL_SIZE - 1, not EOS_PAL_SIZE: slot 255 is the HAL's EOS_COLOR_NONE
    // and a colour resolved to it would not draw. See eos_theme_cube_index.
    for (i = 0; i < EOS_PAL_SIZE - 1; i++) {
        eos_rgb_t p = eos_theme_un565(t->pal565[i]);
        long dr = (long)c.r - p.r, dg = (long)c.g - p.g, db = (long)c.b - p.b;
        long d = dr * dr + dg * dg + db * db;
        if (d < bestd) { bestd = d; best = i; if (d == 0) break; }
    }
    return (uint8_t)best;
}

bool eos_theme_mono(const eos_theme_t *t, eos_rgb_t c)
{
    int d;
    if (!t) return eos_theme_luma(c) >= 128;
    d = (int)eos_theme_luma(c) - (int)t->bg_luma;
    if (d < 0) d = -d;
    return d >= (int)t->mono_threshold;
}

// ------------------------------------------------------------------ palette

static void build_palette(eos_theme_t *t)
{
    int i, ri, gi, bi, bl, tl, thr;

    for (i = 0; i < EOS_ROLE_COUNT; i++) {
        t->pal565[EOS_PAL_ROLE_BASE + i] = eos_theme_rgb565(t->role[i]);
        t->role_idx[i] = (uint8_t)(EOS_PAL_ROLE_BASE + i);
    }
    for (i = 0; i < EOS_ANSI_COUNT; i++) {
        t->pal565[EOS_PAL_ANSI_BASE + i] = eos_theme_rgb565(t->ansi[i]);
        t->ansi_idx[i] = (uint8_t)(EOS_PAL_ANSI_BASE + i);
    }
    for (i = 0; i < EOS_PAL_RAMP_STEPS; i++) {
        t->pal565[EOS_PAL_TEXT_RAMP + i] = eos_theme_rgb565(
            blend(t->role[EOS_ROLE_BG], t->role[EOS_ROLE_TEXT], i, EOS_PAL_RAMP_STEPS - 1));
        t->pal565[EOS_PAL_ACCENT_RAMP + i] = eos_theme_rgb565(
            blend(t->role[EOS_ROLE_BG], t->role[EOS_ROLE_ACCENT], i, EOS_PAL_RAMP_STEPS - 1));
    }
    for (ri = 0; ri < EOS_PAL_CUBE_R; ri++) {
        for (gi = 0; gi < EOS_PAL_CUBE_G; gi++) {
            for (bi = 0; bi < EOS_PAL_CUBE_B; bi++) {
                eos_rgb_t c;
                c.r = (uint8_t)((ri * 255 + (EOS_PAL_CUBE_R - 1) / 2) / (EOS_PAL_CUBE_R - 1));
                c.g = (uint8_t)((gi * 255 + (EOS_PAL_CUBE_G - 1) / 2) / (EOS_PAL_CUBE_G - 1));
                c.b = (uint8_t)((bi * 255 + (EOS_PAL_CUBE_B - 1) / 2) / (EOS_PAL_CUBE_B - 1));
                t->pal565[EOS_PAL_CUBE_BASE +
                          (ri * EOS_PAL_CUBE_G + gi) * EOS_PAL_CUBE_B + bi] =
                    eos_theme_rgb565(c);
            }
        }
    }

    bl  = eos_theme_luma(t->role[EOS_ROLE_BG]);
    tl  = eos_theme_luma(t->role[EOS_ROLE_TEXT]);
    thr = (tl > bl ? tl - bl : bl - tl) / 3;
    if (thr < 24) thr = 24;          // a low-contrast theme must not light everything
    t->bg_luma        = (uint8_t)bl;
    t->mono_threshold = (uint8_t)thr;
}

// ------------------------------------------------------------------ default

static void set_str(char *dst, int cap, const char *src)
{
    int i = 0;
    memset(dst, 0, (size_t)cap);
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
}

void eos_theme_default(eos_theme_t *out)
{
    int i;
    if (!out) return;
    memset(out, 0, sizeof *out);
    set_str(out->name, EOS_THEME_NAME_MAX, "eos-default");
    for (i = 0; i < EOS_ROLE_COUNT; i++) out->role[i] = rgb24(DEF_ROLE[i]);
    for (i = 0; i < EOS_ANSI_COUNT; i++) out->ansi[i] = rgb24(DEF_ANSI[i]);
    out->m.gap    = 4;
    out->m.border = 1;
    out->m.bar_h  = 14;
    out->m.tab_h  = 12;
    out->m.radius = 2;
    set_str(out->m.font, EOS_THEME_FONT_MAX, "small");
    out->provided_roles = 0xFFFFu;
    out->provided_ansi  = 0xFFFFu;
    build_palette(out);
}

// -------------------------------------------------------------- json scanner
//
// Everything below is bounded by j->n. There is no NUL-terminated string
// anywhere in the input path, because a theme read off a card can contain
// anything, including embedded zero bytes.

typedef struct {
    const char     *s;
    int             n;
    int             p;
    int             depth;
    eos_theme_err_t err;
} jp_t;

typedef bool (*kv_fn)(jp_t *j, const char *key, void *ctx);

static bool jp_skip(jp_t *j);
static bool jp_object(jp_t *j, kv_fn fn, void *ctx);

static void jp_ws(jp_t *j)
{
    while (j->p < j->n) {
        unsigned char c = (unsigned char)j->s[j->p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            j->p++;
            continue;
        }
        if (c == '/' && j->p + 1 < j->n && j->s[j->p + 1] == '/') {
            j->p += 2;
            while (j->p < j->n && j->s[j->p] != '\n') j->p++;
            continue;
        }
        if (c == '/' && j->p + 1 < j->n && j->s[j->p + 1] == '*') {
            int q = j->p + 2;
            while (q + 1 < j->n && !(j->s[q] == '*' && j->s[q + 1] == '/')) q++;
            if (q + 1 >= j->n) { j->err = EOS_THEME_ERR_SYNTAX; j->p = j->n; return; }
            j->p = q + 2;
            continue;
        }
        break;
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void put(char *out, int cap, int *o, char c)
{
    if (out && cap > 0 && *o < cap - 1) out[*o] = c;
    (*o)++;
}

// Reads a JSON string. `out` may be NULL to discard. Over-long strings are
// truncated rather than rejected; an unterminated one is a syntax error.
static bool jp_string(jp_t *j, char *out, int cap)
{
    int o = 0;
    jp_ws(j);
    if (j->err) return false;
    if (j->p >= j->n || j->s[j->p] != '"') { j->err = EOS_THEME_ERR_SYNTAX; return false; }
    j->p++;
    while (j->p < j->n) {
        char c = j->s[j->p++];
        if (c == '"') {
            if (out && cap > 0) out[o < cap - 1 ? o : cap - 1] = 0;
            return true;
        }
        if (c != '\\') { put(out, cap, &o, c); continue; }
        if (j->p >= j->n) break;
        c = j->s[j->p++];
        switch (c) {
        case 'n': put(out, cap, &o, '\n'); break;
        case 't': put(out, cap, &o, '\t'); break;
        case 'r': put(out, cap, &o, '\r'); break;
        case 'b': put(out, cap, &o, '\b'); break;
        case 'f': put(out, cap, &o, '\f'); break;
        case 'u': {
            int k, v = 0;
            for (k = 0; k < 4; k++) {
                int h;
                if (j->p >= j->n) { j->err = EOS_THEME_ERR_SYNTAX; return false; }
                h = hexval(j->s[j->p++]);
                if (h < 0) { j->err = EOS_THEME_ERR_SYNTAX; return false; }
                v = (v << 4) | h;
            }
            put(out, cap, &o, (char)(v < 0x80 ? v : '?'));
            break;
        }
        default: put(out, cap, &o, c); break;   // \" \\ \/ and anything else, literally
        }
    }
    j->err = EOS_THEME_ERR_SYNTAX;
    return false;
}

// Integers. A fractional part is consumed and discarded; nobody needs a
// half-pixel gap. An exponent is left where it sits and trips the caller.
static bool jp_int(jp_t *j, long *out)
{
    long v = 0;
    int digits = 0, neg = 0;
    jp_ws(j);
    if (j->err) return false;
    if (j->p < j->n && (j->s[j->p] == '-' || j->s[j->p] == '+')) {
        neg = (j->s[j->p] == '-');
        j->p++;
    }
    while (j->p < j->n && j->s[j->p] >= '0' && j->s[j->p] <= '9') {
        if (v < 1000000L) v = v * 10 + (j->s[j->p] - '0');
        j->p++;
        digits++;
    }
    if (!digits) { j->err = EOS_THEME_ERR_TYPE; return false; }
    if (j->p < j->n && j->s[j->p] == '.') {
        j->p++;
        while (j->p < j->n && j->s[j->p] >= '0' && j->s[j->p] <= '9') j->p++;
    }
    *out = neg ? -v : v;
    return true;
}

static bool jp_lit(jp_t *j, const char *w)
{
    int k = 0;
    while (w[k]) {
        if (j->p + k >= j->n || j->s[j->p + k] != w[k]) return false;
        k++;
    }
    j->p += k;
    return true;
}

static bool skip_kv(jp_t *j, const char *key, void *ctx)
{
    (void)key;
    (void)ctx;
    return jp_skip(j);
}

static bool jp_skip(jp_t *j)
{
    char c;
    jp_ws(j);
    if (j->err) return false;
    if (j->p >= j->n) { j->err = EOS_THEME_ERR_SYNTAX; return false; }
    c = j->s[j->p];
    if (c == '"') return jp_string(j, NULL, 0);
    if (c == '{') return jp_object(j, skip_kv, NULL);
    if (c == '[') {
        j->p++;
        if (++j->depth > EOS_THEME_MAX_DEPTH) { j->err = EOS_THEME_ERR_DEPTH; return false; }
        for (;;) {
            jp_ws(j);
            if (j->err) return false;
            if (j->p >= j->n) { j->err = EOS_THEME_ERR_SYNTAX; return false; }
            if (j->s[j->p] == ']') { j->p++; j->depth--; return true; }
            if (!jp_skip(j)) return false;
            jp_ws(j);
            if (j->err) return false;
            if (j->p < j->n && j->s[j->p] == ',') { j->p++; continue; }
            if (j->p < j->n && j->s[j->p] == ']') { j->p++; j->depth--; return true; }
            j->err = EOS_THEME_ERR_SYNTAX;
            return false;
        }
    }
    if (jp_lit(j, "true") || jp_lit(j, "false") || jp_lit(j, "null")) return true;
    { long v; return jp_int(j, &v); }
}

// Walks one object, handing every key to `fn`. A comma before the closing
// brace is tolerated, because hand-edited files always end up with one.
static bool jp_object(jp_t *j, kv_fn fn, void *ctx)
{
    char key[KEY_MAX];
    jp_ws(j);
    if (j->err) return false;
    if (j->p >= j->n || j->s[j->p] != '{') { j->err = EOS_THEME_ERR_SYNTAX; return false; }
    j->p++;
    if (++j->depth > EOS_THEME_MAX_DEPTH) { j->err = EOS_THEME_ERR_DEPTH; return false; }
    for (;;) {
        jp_ws(j);
        if (j->err) return false;
        if (j->p >= j->n) { j->err = EOS_THEME_ERR_SYNTAX; return false; }
        if (j->s[j->p] == '}') { j->p++; j->depth--; return true; }
        if (!jp_string(j, key, KEY_MAX)) return false;
        jp_ws(j);
        if (j->err) return false;
        if (j->p >= j->n || j->s[j->p] != ':') { j->err = EOS_THEME_ERR_SYNTAX; return false; }
        j->p++;
        if (!fn(j, key, ctx)) return false;
        jp_ws(j);
        if (j->err) return false;
        if (j->p < j->n && j->s[j->p] == ',') { j->p++; continue; }
        if (j->p < j->n && j->s[j->p] == '}') { j->p++; j->depth--; return true; }
        j->err = EOS_THEME_ERR_SYNTAX;
        return false;
    }
}

// --------------------------------------------------------------- theme rules

// #rrggbb, or #rgb as a shorthand where each digit doubles. Nothing else:
// named colours would need a table nobody can remember the contents of.
static bool hex_color(const char *s, eos_rgb_t *o)
{
    int n = 0, d[6], i;
    if (!s || s[0] != '#') return false;
    s++;
    while (s[n]) n++;
    if (n != 6 && n != 3) return false;
    for (i = 0; i < n; i++) {
        d[i] = hexval(s[i]);
        if (d[i] < 0) return false;
    }
    if (n == 3) {
        o->r = (uint8_t)(d[0] * 17);
        o->g = (uint8_t)(d[1] * 17);
        o->b = (uint8_t)(d[2] * 17);
    } else {
        o->r = (uint8_t)((d[0] << 4) | d[1]);
        o->g = (uint8_t)((d[2] << 4) | d[3]);
        o->b = (uint8_t)((d[4] << 4) | d[5]);
    }
    return true;
}

static bool jp_color(jp_t *j, eos_rgb_t *o)
{
    char s[24];
    jp_ws(j);
    if (j->err) return false;
    if (j->p >= j->n || j->s[j->p] != '"') { j->err = EOS_THEME_ERR_TYPE; return false; }
    if (!jp_string(j, s, (int)sizeof s)) return false;
    if (!hex_color(s, o)) { j->err = EOS_THEME_ERR_COLOR; return false; }
    return true;
}

static int16_t clampi(long v, long lo, long hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int16_t)v;
}

static bool color_kv(jp_t *j, const char *key, void *ctx)
{
    eos_theme_t *t = (eos_theme_t *)ctx;
    int i;
    for (i = 0; i < EOS_ROLE_COUNT; i++) {
        eos_rgb_t c;
        if (strcmp(key, ROLE_NAME[i]) != 0) continue;
        if (!jp_color(j, &c)) return false;
        t->role[i] = c;
        t->provided_roles |= (uint16_t)(1u << i);
        return true;
    }
    return jp_skip(j);      // a typo'd role name lands here and shows up as MISSING
}

static bool ansi_kv(jp_t *j, const char *key, void *ctx)
{
    eos_theme_t *t = (eos_theme_t *)ctx;
    int i;
    for (i = 0; i < EOS_ANSI_COUNT; i++) {
        eos_rgb_t c;
        if (strcmp(key, ANSI_NAME[i]) != 0) continue;
        if (!jp_color(j, &c)) return false;
        t->ansi[i] = c;
        t->provided_ansi |= (uint16_t)(1u << i);
        return true;
    }
    return jp_skip(j);
}

static bool metric_kv(jp_t *j, const char *key, void *ctx)
{
    eos_theme_t *t = (eos_theme_t *)ctx;
    long v;
    if (strcmp(key, "gap") == 0)    { if (!jp_int(j, &v)) return false; t->m.gap    = clampi(v, 0, 32); return true; }
    if (strcmp(key, "border") == 0) { if (!jp_int(j, &v)) return false; t->m.border = clampi(v, 0,  8); return true; }
    if (strcmp(key, "bar_h") == 0)  { if (!jp_int(j, &v)) return false; t->m.bar_h  = clampi(v, 0, 64); return true; }
    if (strcmp(key, "tab_h") == 0)  { if (!jp_int(j, &v)) return false; t->m.tab_h  = clampi(v, 0, 64); return true; }
    if (strcmp(key, "radius") == 0) { if (!jp_int(j, &v)) return false; t->m.radius = clampi(v, 0, 32); return true; }
    return jp_skip(j);
}

static bool root_kv(jp_t *j, const char *key, void *ctx)
{
    eos_theme_t *t = (eos_theme_t *)ctx;
    if (strcmp(key, "name") == 0)    return jp_string(j, t->name, EOS_THEME_NAME_MAX);
    if (strcmp(key, "font") == 0)    return jp_string(j, t->m.font, EOS_THEME_FONT_MAX);
    if (strcmp(key, "colors") == 0)  return jp_object(j, color_kv, t);
    if (strcmp(key, "ansi") == 0)    return jp_object(j, ansi_kv, t);
    if (strcmp(key, "metrics") == 0) return jp_object(j, metric_kv, t);
    return jp_skip(j);
}

eos_theme_err_t eos_theme_parse(eos_theme_t *out, const char *buf, int len)
{
    jp_t j;
    bool ok;

    if (!out) return EOS_THEME_ERR_ARGS;
    eos_theme_default(out);
    if (!buf || len <= 0) return EOS_THEME_ERR_EMPTY;

    j.s = buf; j.n = len; j.p = 0; j.depth = 0; j.err = EOS_THEME_OK;

    // Editors on Windows leave one of these on the front of the file.
    if (len >= 3 && (unsigned char)buf[0] == 0xEF &&
                    (unsigned char)buf[1] == 0xBB &&
                    (unsigned char)buf[2] == 0xBF) j.p = 3;

    out->provided_roles = 0;
    out->provided_ansi  = 0;

    ok = jp_object(&j, root_kv, out);
    if (ok) {
        jp_ws(&j);
        if (!j.err && j.p < j.n) j.err = EOS_THEME_ERR_SYNTAX;   // trailing junk
    }
    if (!ok && j.err == EOS_THEME_OK) j.err = EOS_THEME_ERR_SYNTAX;
    if (j.err == EOS_THEME_OK &&
        (out->provided_roles != 0xFFFFu || out->provided_ansi != 0xFFFFu))
        j.err = EOS_THEME_ERR_MISSING;

    if (j.err != EOS_THEME_OK) {
        eos_theme_default(out);
        return j.err;
    }
    build_palette(out);
    return EOS_THEME_OK;
}

// ------------------------------------------------------------------ accessors

const char *eos_theme_strerror(eos_theme_err_t e)
{
    switch (e) {
    case EOS_THEME_OK:           return "ok";
    case EOS_THEME_ERR_ARGS:     return "no output theme";
    case EOS_THEME_ERR_EMPTY:    return "empty file";
    case EOS_THEME_ERR_SYNTAX:   return "malformed json";
    case EOS_THEME_ERR_TYPE:     return "wrong value type";
    case EOS_THEME_ERR_COLOR:    return "bad colour, want #rrggbb";
    case EOS_THEME_ERR_MISSING:  return "missing role or ansi colour";
    case EOS_THEME_ERR_DEPTH:    return "nested too deep";
    }
    return "unknown";
}

const char *eos_theme_role_name(eos_role_t r)
{
    if ((int)r < 0 || (int)r >= EOS_ROLE_COUNT) return 0;
    return ROLE_NAME[(int)r];
}

const char *eos_theme_ansi_name(int i)
{
    if (i < 0 || i >= EOS_ANSI_COUNT) return 0;
    return ANSI_NAME[i];
}

static int role_clamp(eos_role_t r)
{
    int i = (int)r;
    return (i < 0 || i >= EOS_ROLE_COUNT) ? 0 : i;
}

static int ansi_clamp(int i)
{
    return (i < 0 || i >= EOS_ANSI_COUNT) ? 0 : i;
}

eos_rgb_t eos_theme_role_rgb(const eos_theme_t *t, eos_role_t r)
{
    eos_rgb_t z = {0, 0, 0};
    return t ? t->role[role_clamp(r)] : z;
}

uint16_t eos_theme_role_565(const eos_theme_t *t, eos_role_t r)
{
    return t ? t->pal565[t->role_idx[role_clamp(r)]] : 0;
}

uint8_t eos_theme_role_index(const eos_theme_t *t, eos_role_t r)
{
    return t ? t->role_idx[role_clamp(r)] : 0;
}

bool eos_theme_role_mono(const eos_theme_t *t, eos_role_t r)
{
    return t ? eos_theme_mono(t, t->role[role_clamp(r)]) : false;
}

eos_rgb_t eos_theme_ansi_rgb(const eos_theme_t *t, int i)
{
    eos_rgb_t z = {0, 0, 0};
    return t ? t->ansi[ansi_clamp(i)] : z;
}

uint16_t eos_theme_ansi_565(const eos_theme_t *t, int i)
{
    return t ? t->pal565[t->ansi_idx[ansi_clamp(i)]] : 0;
}

uint8_t eos_theme_ansi_index(const eos_theme_t *t, int i)
{
    return t ? t->ansi_idx[ansi_clamp(i)] : 0;
}

const uint16_t *eos_theme_palette565(const eos_theme_t *t)
{
    return t ? t->pal565 : 0;
}

eos_rgb_t eos_theme_palette_rgb(const eos_theme_t *t, uint8_t idx)
{
    eos_rgb_t z = {0, 0, 0};
    return t ? eos_theme_un565(t->pal565[idx]) : z;
}

static uint8_t ramp(int base, int step)
{
    if (step < 0) step = 0;
    if (step >= EOS_PAL_RAMP_STEPS) step = EOS_PAL_RAMP_STEPS - 1;
    return (uint8_t)(base + step);
}

uint8_t eos_theme_text_ramp(int step)   { return ramp(EOS_PAL_TEXT_RAMP, step); }
uint8_t eos_theme_accent_ramp(int step) { return ramp(EOS_PAL_ACCENT_RAMP, step); }

const char *eos_theme_font(const eos_theme_t *t)
{
    return t ? t->m.font : "";
}
