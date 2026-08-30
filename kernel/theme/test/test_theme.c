// Host test for eos_theme. Runs on the Mac, no hardware needed.
//
//   cc -std=c99 -Wall -Wextra -O1 -I../include ../eos_theme.c test_theme.c -o /tmp/t && /tmp/t
//   cc -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined -I../include \
//      ../eos_theme.c test_theme.c -o /tmp/t && /tmp/t
//
// Loads the four shipped themes off disk, prints them, and checks the three
// resolvers against them. Then it attacks the parser: a table of hand-written
// corruptions, then every truncation of a good file, then every single-byte
// mutation of a good file, each parse run inside a poisoned arena so a stray
// write off either end of the buffer shows up. The invariant under attack is
// always the same one — either the parse succeeded, or the theme is byte for
// byte the compiled-in default.
//
// Every parse is run twice, once in a poisoned static arena and once from an
// exact-size heap block, so the plain build catches writes past the buffer and
// the sanitizer build catches reads past it. See parse_guarded().

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eos_theme.h"

static int checks = 0, fails = 0;
#define CK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("    FAIL: %s\n", msg); } } while (0)

#define MAXDOC 65536
#define GUARD  64

static char arena[GUARD + MAXDOC + GUARD];
static char tokyo[MAXDOC];
static int  tokyo_len = 0;

// ------------------------------------------------------------------- helpers

static uint32_t rgb24v(eos_rgb_t c)
{
    return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

static long sqdist(eos_rgb_t a, eos_rgb_t b)
{
    long dr = (long)a.r - b.r, dg = (long)a.g - b.g, db = (long)a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

static int load(const char *dir, const char *file, char *buf, int cap)
{
    char path[512];
    FILE *f;
    size_t n;
    snprintf(path, sizeof path, "%s/%s", dir, file);
    f = fopen(path, "rb");
    if (!f) return -1;
    n = fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return (int)n;
}

// The test may be run from the repo root, from kernel/theme, or from here.
static const char *find_themes(const char *hint)
{
    static const char *cand[] = {
        "themes", "../themes", "../../themes",
        "kernel/theme/themes", "../kernel/theme/themes"
    };
    static char buf[512];
    char probe[512];
    FILE *f;
    unsigned i;
    if (hint) {
        snprintf(probe, sizeof probe, "%s/tokyonight.json", hint);
        f = fopen(probe, "rb");
        if (f) { fclose(f); snprintf(buf, sizeof buf, "%s", hint); return buf; }
    }
    for (i = 0; i < sizeof cand / sizeof cand[0]; i++) {
        snprintf(probe, sizeof probe, "%s/tokyonight.json", cand[i]);
        f = fopen(probe, "rb");
        if (f) { fclose(f); snprintf(buf, sizeof buf, "%s", cand[i]); return buf; }
    }
    return 0;
}

// Parses the same bytes twice, two different ways, because neither way catches
// everything on its own.
//
// Pass one runs inside a poisoned static arena: a write off either end of the
// buffer shows up as a moved guard band, and that works in the plain build
// with nothing watching. It cannot catch a READ off the end, because the guard
// bytes are perfectly valid memory to read.
//
// Pass two runs from a heap block of exactly `len` bytes, so an address
// sanitizer build has redzones on both sides and traps a read one byte past
// the end — the failure mode that matters most for a parser fed a truncated
// file off a card. Both passes must agree on the result.
static eos_theme_err_t parse_guarded(eos_theme_t *t, const char *text, int len, int *guard_ok)
{
    eos_theme_err_t e, e2;
    eos_theme_t t2;
    char *heap;
    int i, ok = 1;
    if (len > MAXDOC) len = MAXDOC;

    memset(arena, 0xA5, sizeof arena);
    if (len > 0) memcpy(arena + GUARD, text, (size_t)len);
    e = eos_theme_parse(t, arena + GUARD, len);
    for (i = 0; i < GUARD; i++) {
        if ((unsigned char)arena[i] != 0xA5) ok = 0;
        if ((unsigned char)arena[GUARD + len + i] != 0xA5) ok = 0;
    }

    heap = (char *)malloc(len > 0 ? (size_t)len : 1);
    if (heap) {
        if (len > 0) memcpy(heap, text, (size_t)len);
        e2 = eos_theme_parse(&t2, heap, len);
        free(heap);
        if (e2 != e || memcmp(&t2, t, sizeof t2) != 0) ok = 0;
    }

    if (guard_ok) *guard_ok = ok;
    return e;
}

static int is_default(const eos_theme_t *t)
{
    eos_theme_t d;
    eos_theme_default(&d);
    return memcmp(&d, t, sizeof d) == 0;
}

// ------------------------------------------------------------ document maker
//
// Every corruption below is generated from one complete, valid document with
// exactly one thing changed, so a failing case cannot be blamed on the rest of
// the file.

typedef struct {
    const char *name;
    int         omit_role;        // role index to leave out, or -1
    int         omit_ansi;
    int         drop_colors;      // leave the whole colors block out
    int         drop_ansi_block;
    const char *bad_hex;          // what "bg" gets instead of a colour
    const char *bad_metric;       // what "gap" gets instead of a number
    const char *rename_role;      // write role 0 under this key instead
    int         trailing_commas;
    int         comments;
    int         bom;
    int         crlf;
    int         short_hex;        // use #rgb form throughout
    int         no_metrics;
    const char *extra;            // extra top-level entry, verbatim
    const char *tail;             // appended after the closing brace
} gen_t;

static char *gbuf;
static int   gcap, gp;

static void gput(const char *s)
{
    while (*s && gp < gcap - 1) gbuf[gp++] = *s++;
    gbuf[gp] = 0;
}

static void gcolor(int i, int is_ansi, int shrt)
{
    char hx[10];
    if (shrt) snprintf(hx, sizeof hx, "#%x%x%x", (i + 1) & 15, (i + 3) & 15, (i + 7) & 15);
    else if (is_ansi) snprintf(hx, sizeof hx, "#%02x%02x%02x", 0x08 + i * 9, 0x14 + i * 8, 0x22 + i * 7);
    else snprintf(hx, sizeof hx, "#%02x%02x%02x", 0x10 + i * 8, 0x20 + i * 7, 0x30 + i * 6);
    gput(hx);
}

static int gen(char *out, int cap, const gen_t *g)
{
    int i, emitted;
    gbuf = out; gcap = cap; gp = 0;

    if (g->bom) gput("\357\273\277");
    if (g->comments) gput("// a generated theme\n/* and a block comment */\n");
    gput("{\n");
    gput("  \"name\": \"");
    gput(g->name ? g->name : "generated");
    gput("\",\n  \"font\": \"med\"");

    if (!g->no_metrics) {
        gput(",\n  \"metrics\": { \"gap\": ");
        gput(g->bad_metric ? g->bad_metric : "3");
        gput(", \"border\": 1, \"bar_h\": 13, \"tab_h\": 11, \"radius\": 2");
        if (g->trailing_commas) gput(",");
        gput(" }");
    }
    if (g->extra) { gput(",\n  "); gput(g->extra); }

    if (!g->drop_colors) {
        gput(",\n  \"colors\": {");
        emitted = 0;
        for (i = 0; i < EOS_ROLE_COUNT; i++) {
            if (i == g->omit_role) continue;
            gput(emitted ? ",\n    " : "\n    ");
            gput("\"");
            gput((i == 0 && g->rename_role) ? g->rename_role : eos_theme_role_name((eos_role_t)i));
            gput("\": ");
            if (i == 0 && g->bad_hex) { gput("\""); gput(g->bad_hex); gput("\""); }
            else { gput("\""); gcolor(i, 0, g->short_hex); gput("\""); }
            emitted++;
        }
        if (g->trailing_commas) gput(",");
        gput("\n  }");
    }

    if (!g->drop_ansi_block) {
        gput(",\n  \"ansi\": {");
        emitted = 0;
        for (i = 0; i < EOS_ANSI_COUNT; i++) {
            if (i == g->omit_ansi) continue;
            gput(emitted ? ",\n    " : "\n    ");
            gput("\"");
            gput(eos_theme_ansi_name(i));
            gput("\": \"");
            gcolor(i, 1, g->short_hex);
            gput("\"");
            emitted++;
        }
        if (g->trailing_commas) gput(",");
        gput("\n  }");
    }

    gput("\n}\n");
    if (g->tail) gput(g->tail);

    if (g->crlf) {                       // rewrite \n as \r\n, in place, backwards
        int n = gp, k, w;
        for (k = 0, w = 0; k < n; k++) if (out[k] == '\n') w++;
        if (n + w < cap - 1) {
            int src = n - 1, dst = n + w - 1;
            out[n + w] = 0;
            while (src >= 0) {
                out[dst--] = out[src];
                if (out[src] == '\n') out[dst--] = '\r';
                src--;
            }
            gp = n + w;
        }
    }
    return gp;
}

// ------------------------------------------------------------- theme reports

static const char SHADE[] = " .:-=+*#%@";

static void report(const eos_theme_t *t)
{
    static const eos_role_t SHOW[] = {
        EOS_ROLE_BG, EOS_ROLE_SURFACE, EOS_ROLE_TEXT, EOS_ROLE_MUTED,
        EOS_ROLE_ACCENT, EOS_ROLE_BORDER_FOCUSED, EOS_ROLE_BAR_BG, EOS_ROLE_TAB_ACTIVE
    };
    unsigned i;
    printf("  %-18s font %-8s gap %d  border %d  bar %d  tab %d  radius %d\n",
           t->name, eos_theme_font(t), t->m.gap, t->m.border, t->m.bar_h,
           t->m.tab_h, t->m.radius);
    printf("      %-18s %-8s %-6s %-5s %s\n", "role", "hex", "565", "idx", "mono");
    for (i = 0; i < sizeof SHOW / sizeof SHOW[0]; i++) {
        eos_role_t r = SHOW[i];
        eos_rgb_t  c = eos_theme_role_rgb(t, r);
        printf("      %-18s #%06x   %04x   %3u    %c\n",
               eos_theme_role_name(r), rgb24v(c), eos_theme_role_565(t, r),
               eos_theme_role_index(t, r), eos_theme_role_mono(t, r) ? '#' : '.');
    }
    printf("      ansi luma  ");
    for (i = 0; i < EOS_ANSI_COUNT; i++) {
        int l = eos_theme_luma(eos_theme_ansi_rgb(t, (int)i));
        printf("%c", SHADE[l * 9 / 255]);
    }
    printf("\n      ansi mono  ");
    for (i = 0; i < EOS_ANSI_COUNT; i++)
        printf("%c", eos_theme_mono(t, eos_theme_ansi_rgb(t, (int)i)) ? '#' : '.');
    printf("\n      bg luma %u, mono threshold %u\n", t->bg_luma, t->mono_threshold);
}

// --------------------------------------------------------------- theme suite

typedef struct {
    const char *file;
    const char *name;
    uint32_t    bg, text, accent;
} expect_t;

static const expect_t EXPECT[] = {
    { "tokyonight.json",       "tokyonight",       0x1a1b26, 0xc0caf5, 0x7aa2f7 },
    { "gruvbox.json",          "gruvbox",          0x282828, 0xebdbb2, 0xfabd2f },
    { "catppuccin-mocha.json", "catppuccin-mocha", 0x1e1e2e, 0xcdd6f4, 0xcba6f7 },
    { "cyd-amber.json",        "cyd-amber",        0x262624, 0xe0deda, 0xd88e56 }
};

static void check_resolvers(const eos_theme_t *t)
{
    int i, bad;
    long worst = 0, worst_cube = 0;
    int r, g, b;

    for (i = 0, bad = 0; i < EOS_ROLE_COUNT; i++) {
        if (t->role_idx[i] != (uint8_t)(EOS_PAL_ROLE_BASE + i)) bad++;
        if (t->pal565[t->role_idx[i]] != eos_theme_rgb565(t->role[i])) bad++;
    }
    CK(bad == 0, "every role sits at its own palette slot and round-trips to 565");

    for (i = 0, bad = 0; i < EOS_ANSI_COUNT; i++) {
        if (t->ansi_idx[i] != (uint8_t)(EOS_PAL_ANSI_BASE + i)) bad++;
        if (t->pal565[t->ansi_idx[i]] != eos_theme_rgb565(t->ansi[i])) bad++;
    }
    CK(bad == 0, "every ansi colour sits at its own palette slot");

    // A colour the palette can actually hold must come back exactly. Note the
    // lookup runs on the value the panel would really show, not the role's raw
    // 8-bit value: 565 truncation moves that by a few counts, and for
    // tokyonight's muted and cyd-amber's surface it moves them near enough to
    // a neighbouring ramp step that the nearest slot is legitimately not the
    // role's own. That is the quantiser working, not a bug.
    for (i = 0, bad = 0; i < EOS_ROLE_COUNT; i++) {
        eos_rgb_t shown = eos_theme_un565(eos_theme_rgb565(t->role[i]));
        if (t->pal565[eos_theme_index(t, shown)] != eos_theme_rgb565(t->role[i])) bad++;
    }
    CK(bad == 0, "nearest-match lands exactly on a colour the palette holds");

    for (i = 0, bad = 0; i < EOS_ANSI_COUNT; i++) {
        eos_rgb_t shown = eos_theme_un565(eos_theme_rgb565(t->ansi[i]));
        if (t->pal565[eos_theme_index(t, shown)] != eos_theme_rgb565(t->ansi[i])) bad++;
    }
    CK(bad == 0, "the same holds for the ansi colours");

    for (i = 0, bad = 0; i < EOS_ROLE_COUNT; i++) {
        eos_rgb_t own = eos_theme_un565(t->pal565[t->role_idx[i]]);
        eos_rgb_t got = eos_theme_un565(t->pal565[eos_theme_index(t, t->role[i])]);
        if (sqdist(t->role[i], got) > sqdist(t->role[i], own)) bad++;
    }
    CK(bad == 0, "the search never returns a slot worse than the role's own");

    CK(t->pal565[EOS_PAL_TEXT_RAMP] == eos_theme_role_565(t, EOS_ROLE_BG),
       "text ramp starts at bg");
    CK(t->pal565[EOS_PAL_TEXT_RAMP + EOS_PAL_RAMP_STEPS - 1] == eos_theme_role_565(t, EOS_ROLE_TEXT),
       "text ramp ends at text");
    CK(t->pal565[EOS_PAL_ACCENT_RAMP] == eos_theme_role_565(t, EOS_ROLE_BG),
       "accent ramp starts at bg");
    CK(t->pal565[EOS_PAL_ACCENT_RAMP + EOS_PAL_RAMP_STEPS - 1] == eos_theme_role_565(t, EOS_ROLE_ACCENT),
       "accent ramp ends at accent");
    CK(t->pal565[EOS_PAL_CUBE_BASE] == 0x0000, "cube starts at black");
    CK(t->pal565[EOS_PAL_SIZE - 1] == 0xFFFF, "cube ends at white");

    CK(!eos_theme_role_mono(t, EOS_ROLE_BG),     "bg is unlit on 1bpp");
    CK(eos_theme_role_mono(t, EOS_ROLE_TEXT),    "text is lit on 1bpp");
    CK(eos_theme_role_mono(t, EOS_ROLE_ACCENT),  "accent is lit on 1bpp");
    CK(eos_theme_role_mono(t, EOS_ROLE_MUTED),   "muted is lit on 1bpp");
    CK(!eos_theme_role_mono(t, EOS_ROLE_SURFACE), "surface is unlit on 1bpp");

    CK(eos_theme_luma(t->role[EOS_ROLE_BG]) < eos_theme_luma(t->role[EOS_ROLE_TEXT]),
       "text is brighter than bg");
    CK(rgb24v(t->role[EOS_ROLE_BG]) != rgb24v(t->role[EOS_ROLE_TEXT]),
       "bg and text are different colours");

    for (r = 0; r < 256; r += 17)
        for (g = 0; g < 256; g += 17)
            for (b = 0; b < 256; b += 17) {
                eos_rgb_t c;
                long d;
                c.r = (uint8_t)r; c.g = (uint8_t)g; c.b = (uint8_t)b;
                d = sqdist(c, eos_theme_un565(t->pal565[eos_theme_index(t, c)]));
                if (d > worst) worst = d;
                d = sqdist(c, eos_theme_un565(t->pal565[eos_theme_cube_index(c)]));
                if (d > worst_cube) worst_cube = d;
            }
    printf("      worst quantisation: search %ld, direct cube %ld (squared rgb)\n",
           worst, worst_cube);
    CK(worst <= 4096, "nearest-match error stays inside the 6x8x4 cube bound");
    CK(worst_cube <= 4096, "direct cube index error stays inside the same bound");
    CK(worst <= worst_cube, "searching is never worse than going straight to the cube");
}

// ------------------------------------------------------------ corruption set

typedef struct { const char *what; const char *doc; int len; } bad_t;

static char bad_store[32][MAXDOC];
static bad_t bad_list[40];
static int   bad_n = 0;

static void add_bad(const char *what, const char *doc, int len)
{
    if (bad_n >= 32) return;
    if (len > MAXDOC) len = MAXDOC;
    memcpy(bad_store[bad_n], doc, (size_t)len);
    bad_list[bad_n].what = what;
    bad_list[bad_n].doc  = bad_store[bad_n];
    bad_list[bad_n].len  = len;
    bad_n++;
}

static void add_bad_str(const char *what, const char *doc)
{
    add_bad(what, doc, (int)strlen(doc));
}

static void add_bad_gen(const char *what, const gen_t *g)
{
    static char tmp[MAXDOC];
    int n = gen(tmp, sizeof tmp, g);
    add_bad(what, tmp, n);
}

int main(int argc, char **argv)
{
    static char doc[MAXDOC];
    eos_theme_t t, d;
    const char *dir;
    unsigned e;
    int i, n, guard, guards_held, worst_case_ok;

    printf("=== eos_theme host test ===\n\n");

    dir = find_themes(argc > 1 ? argv[1] : 0);
    if (!dir) {
        printf("    FAIL: cannot find the themes directory; pass it as argv[1]\n");
        printf("\n=== 1 checks, 1 failed ===\n");
        return 1;
    }
    printf("themes from %s\n\n", dir);

    // ------------------------------------------------------------ the four
    for (e = 0; e < sizeof EXPECT / sizeof EXPECT[0]; e++) {
        eos_theme_err_t err;
        n = load(dir, EXPECT[e].file, doc, MAXDOC);
        checks++;
        if (n <= 0) { fails++; printf("    FAIL: cannot read %s\n", EXPECT[e].file); continue; }

        err = parse_guarded(&t, doc, n, &guard);
        CK(err == EOS_THEME_OK, EXPECT[e].file);
        if (err != EOS_THEME_OK) {
            printf("      (%s)\n", eos_theme_strerror(err));
            continue;
        }
        CK(guard, "parse stayed inside the buffer it was given");
        CK(strcmp(t.name, EXPECT[e].name) == 0, "name matches the file");
        CK(t.provided_roles == 0xFFFFu, "all sixteen roles came from the file");
        CK(t.provided_ansi == 0xFFFFu, "all sixteen ansi colours came from the file");
        CK(rgb24v(t.role[EOS_ROLE_BG]) == EXPECT[e].bg, "bg is the published value");
        CK(rgb24v(t.role[EOS_ROLE_TEXT]) == EXPECT[e].text, "text is the published value");
        CK(rgb24v(t.role[EOS_ROLE_ACCENT]) == EXPECT[e].accent, "accent is the published value");
        CK(t.m.gap >= 0 && t.m.gap <= 32 && t.m.border >= 0 && t.m.border <= 8 &&
           t.m.bar_h >= 0 && t.m.bar_h <= 64 && t.m.tab_h >= 0 && t.m.tab_h <= 64 &&
           t.m.radius >= 0 && t.m.radius <= 32, "metrics are inside their documented ranges");
        CK(t.m.font[0] != 0, "a font is named");
        CK(t.m.bar_h >= t.m.tab_h, "the bar is at least as tall as a tab strip");

        report(&t);
        check_resolvers(&t);
        printf("\n");

        if (strcmp(EXPECT[e].file, "tokyonight.json") == 0) {
            memcpy(tokyo, doc, (size_t)n);
            tokyo_len = n;
        }
    }

    // --------------------------------------------------- fixed colour maths
    printf("resolvers\n");
    {
        eos_rgb_t white = {255, 255, 255}, black = {0, 0, 0};
        eos_rgb_t red = {255, 0, 0}, grn = {0, 255, 0}, blu = {0, 0, 255};
        int worst = 0;
        CK(eos_theme_rgb565(white) == 0xFFFF, "white is 0xffff in 565");
        CK(eos_theme_rgb565(black) == 0x0000, "black is 0x0000 in 565");
        CK(eos_theme_rgb565(red) == 0xF800, "red is 0xf800 in 565");
        CK(eos_theme_rgb565(grn) == 0x07E0, "green is 0x07e0 in 565");
        CK(eos_theme_rgb565(blu) == 0x001F, "blue is 0x001f in 565");
        CK(eos_theme_luma(white) == 255, "luma of white is 255");
        CK(eos_theme_luma(black) == 0, "luma of black is 0");
        CK(rgb24v(eos_theme_un565(0xFFFF)) == 0xFFFFFF, "565 white decodes back to full white");
        CK(rgb24v(eos_theme_un565(0x0000)) == 0x000000, "565 black decodes back to black");
        for (i = 0; i < 256; i++) {
            eos_rgb_t c, p;
            int dr, dg, db;
            c.r = (uint8_t)i; c.g = (uint8_t)(255 - i); c.b = (uint8_t)((i * 7) & 0xFF);
            p = eos_theme_un565(eos_theme_rgb565(c));
            dr = c.r - p.r; dg = c.g - p.g; db = c.b - p.b;
            if (dr < 0) dr = -dr;
            if (dg < 0) dg = -dg;
            if (db < 0) db = -db;
            if (dr > worst) worst = dr;
            if (dg > worst) worst = dg;
            if (db > worst) worst = db;
        }
        CK(worst <= 8, "565 round-trip loses no more than 8 counts per channel");
        CK(eos_theme_cube_index(black) == EOS_PAL_CUBE_BASE, "black lands on the first cube slot");
        // The last cube slot is EOS_COLOR_NONE in the display HAL, so the cube
        // gives it up and white steps back one green level to 251. Anything
        // that resolves to 255 silently does not draw on tier SOFT.
        CK(eos_theme_cube_index(white) == EOS_PAL_CUBE_NONE - EOS_PAL_CUBE_B,
           "white steps back off the reserved slot, onto 251");
        {
            eos_theme_t dt; int r, g, b; long none_hits = 0;
            eos_theme_default(&dt);
            for (r = 0; r < 256; r++)
                for (g = 0; g < 256; g += 5)
                    for (b = 0; b < 256; b += 5) {
                        eos_rgb_t c;
                        c.r = (uint8_t)r; c.g = (uint8_t)g; c.b = (uint8_t)b;
                        if (eos_theme_cube_index(c) == EOS_PAL_CUBE_NONE) none_hits++;
                        if (eos_theme_index(&dt, c) == EOS_PAL_CUBE_NONE) none_hits++;
                    }
            CK(none_hits == 0, "no colour resolves to the reserved slot 255");
        }
    }

    // --------------------------------------------------------------- tables
    CK(strcmp(eos_theme_role_name(EOS_ROLE_BG), "bg") == 0, "role 0 is named bg");
    CK(strcmp(eos_theme_role_name(EOS_ROLE_TAB_INACTIVE), "tab_inactive") == 0,
       "the last role is named tab_inactive");
    CK(eos_theme_role_name((eos_role_t)EOS_ROLE_COUNT) == 0, "role name range is checked");
    CK(eos_theme_role_name((eos_role_t)-1) == 0, "negative role name is checked");
    CK(strcmp(eos_theme_ansi_name(0), "black") == 0, "ansi 0 is named black");
    CK(strcmp(eos_theme_ansi_name(15), "bright_white") == 0, "ansi 15 is named bright_white");
    CK(eos_theme_ansi_name(16) == 0 && eos_theme_ansi_name(-1) == 0, "ansi name range is checked");
    for (i = 0, n = 0; i <= EOS_THEME_ERR_DEPTH; i++)
        if (eos_theme_strerror((eos_theme_err_t)i) == 0) n++;
    CK(n == 0, "every error code has a message");

    // -------------------------------------------------------------- default
    eos_theme_default(&d);
    CK(strcmp(d.name, "eos-default") == 0, "the fallback names itself");
    CK(d.provided_roles == 0xFFFFu && d.provided_ansi == 0xFFFFu, "the fallback is complete");
    CK(d.pal565[EOS_PAL_SIZE - 1] == 0xFFFF, "the fallback has a built palette");
    CK(eos_theme_palette565(&d) == d.pal565, "palette accessor hands back the CLUT");
    CK(eos_theme_text_ramp(-5) == EOS_PAL_TEXT_RAMP, "text ramp clamps below");
    CK(eos_theme_text_ramp(999) == EOS_PAL_TEXT_RAMP + EOS_PAL_RAMP_STEPS - 1, "text ramp clamps above");
    CK(eos_theme_accent_ramp(-5) == EOS_PAL_ACCENT_RAMP, "accent ramp clamps below");
    CK(eos_theme_accent_ramp(999) == EOS_PAL_ACCENT_RAMP + EOS_PAL_RAMP_STEPS - 1, "accent ramp clamps above");
    CK(eos_theme_role_565(0, EOS_ROLE_BG) == 0 && eos_theme_font(0)[0] == 0,
       "accessors survive a null theme");
    CK(eos_theme_role_index(&d, (eos_role_t)999) == d.role_idx[0], "out of range role clamps");
    CK(eos_theme_ansi_index(&d, 999) == d.ansi_idx[0], "out of range ansi clamps");

    // ---------------------------------------------------- tolerated input
    printf("\ntolerated input\n");
    {
        gen_t g;
        eos_theme_err_t err;
        memset(&g, 0, sizeof g);
        g.omit_role = -1; g.omit_ansi = -1;

        n = gen(doc, MAXDOC, &g);
        err = parse_guarded(&t, doc, n, &guard);
        CK(err == EOS_THEME_OK && guard, "a plain generated document parses");
        CK(strcmp(t.name, "generated") == 0, "its name comes through");
        CK(t.m.gap == 3 && t.m.border == 1 && t.m.radius == 2, "its metrics come through");

        g.comments = 1;
        n = gen(doc, MAXDOC, &g);
        CK(parse_guarded(&t, doc, n, &guard) == EOS_THEME_OK && guard,
           "// and /* */ comments are skipped");

        g.trailing_commas = 1;
        n = gen(doc, MAXDOC, &g);
        CK(parse_guarded(&t, doc, n, &guard) == EOS_THEME_OK && guard,
           "trailing commas are tolerated");

        g.bom = 1;
        n = gen(doc, MAXDOC, &g);
        CK(parse_guarded(&t, doc, n, &guard) == EOS_THEME_OK && guard, "a utf-8 BOM is skipped");

        g.crlf = 1;
        n = gen(doc, MAXDOC, &g);
        CK(parse_guarded(&t, doc, n, &guard) == EOS_THEME_OK && guard, "CRLF line endings are fine");

        memset(&g, 0, sizeof g);
        g.omit_role = -1; g.omit_ansi = -1;
        g.extra = "\"future\": { \"nested\": [1, 2, { \"deep\": true }], \"s\": \"x\" }";
        n = gen(doc, MAXDOC, &g);
        CK(parse_guarded(&t, doc, n, &guard) == EOS_THEME_OK && guard,
           "unknown top-level keys are skipped, whatever they contain");

        memset(&g, 0, sizeof g);
        g.omit_role = -1; g.omit_ansi = -1; g.short_hex = 1;
        n = gen(doc, MAXDOC, &g);
        err = parse_guarded(&t, doc, n, &guard);
        CK(err == EOS_THEME_OK, "the #rgb shorthand parses");
        CK(t.role[0].r == 0x11 && t.role[0].g == 0x33 && t.role[0].b == 0x77,
           "#137 expands to #113377");

        memset(&g, 0, sizeof g);
        g.omit_role = -1; g.omit_ansi = -1; g.no_metrics = 1;
        n = gen(doc, MAXDOC, &g);
        err = parse_guarded(&t, doc, n, &guard);
        CK(err == EOS_THEME_OK, "metrics are optional");
        CK(t.m.gap == d.m.gap && t.m.bar_h == d.m.bar_h, "omitted metrics keep the default values");

        memset(&g, 0, sizeof g);
        g.omit_role = -1; g.omit_ansi = -1;
        g.bad_metric = "99999";
        n = gen(doc, MAXDOC, &g);
        err = parse_guarded(&t, doc, n, &guard);
        CK(err == EOS_THEME_OK, "an absurd metric does not fail the theme");
        CK(t.m.gap == 32, "an absurd metric clamps to its range");
    }

    // ------------------------------------------------------------- corrupt
    printf("\ncorrupt input\n");
    {
        gen_t g;
        static char tmp[MAXDOC];
        int k;

        add_bad("empty file", "", 0);
        add_bad_str("one open brace", "{");
        add_bad_str("not an object", "[\"#ffffff\", \"#000000\"]");
        add_bad_str("bare text", "this is not json at all");
        add_bad_str("unterminated string", "{\"name\": \"tokyo");
        add_bad_str("comment running to eof", "{ // no closing brace\n");
        add_bad_str("unterminated block comment", "{ /* forever ");

        memcpy(tmp, "{\"name\": \"x\", \"colors\": {\"bg\": \"#000000\"}}\0\0\0garbage", 52);
        add_bad("embedded NUL then junk", tmp, 52);

        for (k = 0; k < 256; k++) tmp[k] = (char)((k * 37 + 11) & 0xFF);
        add_bad("256 bytes of binary", tmp, 256);

        add_bad("truncated real theme", tokyo, tokyo_len * 3 / 5);
        add_bad("real theme minus its last brace", tokyo, tokyo_len - 2);

        memset(&g, 0, sizeof g);
        g.omit_role = -1; g.omit_ansi = -1;

        g.bad_hex = "#12g456";  add_bad_gen("bad hex digit", &g);
        g.bad_hex = "#abcd";    add_bad_gen("hex of the wrong length", &g);
        g.bad_hex = "1a1b26";   add_bad_gen("hex without the hash", &g);
        g.bad_hex = "";         add_bad_gen("empty colour string", &g);
        g.bad_hex = "rebeccapurple"; add_bad_gen("a named colour", &g);
        g.bad_hex = 0;

        g.omit_role = EOS_ROLE_ACCENT;  add_bad_gen("one role missing", &g);
        g.omit_role = EOS_ROLE_BG;      add_bad_gen("bg missing", &g);
        g.omit_role = -1;
        g.omit_ansi = EOS_ANSI_BR_CYAN; add_bad_gen("one ansi colour missing", &g);
        g.omit_ansi = -1;
        g.drop_colors = 1;              add_bad_gen("no colors block", &g);
        g.drop_colors = 0;
        g.drop_ansi_block = 1;          add_bad_gen("no ansi block", &g);
        g.drop_ansi_block = 0;
        g.rename_role = "bgg";          add_bad_gen("a typo'd role name", &g);
        g.rename_role = 0;
        g.bad_metric = "\"four\"";      add_bad_gen("a string where a number goes", &g);
        g.bad_metric = 0;
        g.tail = "{\"and\": \"more\"}"; add_bad_gen("junk after the closing brace", &g);
        g.tail = 0;

        {
            static char deep[MAXDOC];
            int p = 0;
            const char *head = "{ \"future\": ";
            memcpy(deep, head, strlen(head));
            p = (int)strlen(head);
            for (k = 0; k < 20; k++) deep[p++] = '[';
            deep[p++] = '1';
            for (k = 0; k < 20; k++) deep[p++] = ']';
            deep[p++] = '}';
            add_bad("twenty nested arrays", deep, p);
        }

        guards_held = 1;
        for (i = 0; i < bad_n; i++) {
            eos_theme_err_t err = parse_guarded(&t, bad_list[i].doc, bad_list[i].len, &guard);
            checks++;
            if (err == EOS_THEME_OK) {
                fails++;
                printf("    FAIL: accepted %s\n", bad_list[i].what);
            } else if (!is_default(&t)) {
                fails++;
                printf("    FAIL: %s did not fall back to the default\n", bad_list[i].what);
            } else {
                printf("      %-32s -> %s\n", bad_list[i].what, eos_theme_strerror(err));
            }
            if (!guard) guards_held = 0;
        }
        CK(guards_held, "no corrupt document wrote outside its buffer");
        CK(bad_n >= 20, "the corruption table is still populated");
    }

    // ------------------------------------------------------- null arguments
    CK(eos_theme_parse(0, "{}", 2) == EOS_THEME_ERR_ARGS, "a null theme is refused");
    CK(eos_theme_parse(&t, 0, 10) == EOS_THEME_ERR_EMPTY && is_default(&t),
       "a null buffer falls back");
    CK(eos_theme_parse(&t, "{}", 0) == EOS_THEME_ERR_EMPTY && is_default(&t),
       "a zero length falls back");
    CK(eos_theme_parse(&t, "{}", -99) == EOS_THEME_ERR_EMPTY && is_default(&t),
       "a negative length falls back");

    // -------------------------------------------------------- sweeps
    printf("\nsweeps over %d bytes of tokyonight.json\n", tokyo_len);
    if (tokyo_len <= 0) {
        CK(0, "tokyonight.json never loaded, cannot sweep");
    } else {
        int ok = 1, accepted = 0;
        for (i = 0; i <= tokyo_len; i++) {
            eos_theme_err_t err = parse_guarded(&t, tokyo, i, &guard);
            if (!guard) ok = 0;
            if (err == EOS_THEME_OK) accepted++;
            else if (!is_default(&t)) ok = 0;
        }
        CK(ok, "every truncation either parses or leaves the default untouched");
        printf("      %d of %d truncations were still valid documents\n", accepted, tokyo_len + 1);
    }

    {
        static const unsigned char POISON[] = { 0x00, 0xFF, 0x7B, 0x22, 0x2F };
        int ok = 1, accepted = 0, tried = 0;
        unsigned p;
        for (i = 0; i < tokyo_len; i++) {
            for (p = 0; p < sizeof POISON; p++) {
                static char mut[MAXDOC];
                eos_theme_err_t err;
                memcpy(mut, tokyo, (size_t)tokyo_len);
                mut[i] = (char)POISON[p];
                err = parse_guarded(&t, mut, tokyo_len, &guard);
                tried++;
                if (!guard) ok = 0;
                if (err == EOS_THEME_OK) accepted++;
                else if (!is_default(&t)) ok = 0;
            }
        }
        CK(ok, "every single-byte mutation either parses or leaves the default untouched");
        printf("      %d mutations tried, %d still parsed\n", tried, accepted);
        worst_case_ok = (tried > 1000);
        CK(worst_case_ok, "the mutation sweep actually ran");
    }

    // Truncating tokyonight.json and flipping one of its bytes cannot, between
    // them, produce a buffer that ends in the middle of a token the real file
    // never contains — an open block comment, a bare true/false/null, a \u
    // escape. Those are exactly where an off-by-one in a bounds test hides,
    // because the scanner looks one byte ahead of the one it just accepted.
    // So: a handful of seeds that do contain those tokens, parsed at every
    // prefix length. The last byte of the buffer lands mid-token in turn, and
    // the heap pass inside parse_guarded() puts a redzone right behind it.
    //
    // These seeds are not required to parse. Truncating "true" to "tru" is
    // supposed to fail; the check is that it fails without reading past the
    // end and without leaving a half-applied theme.
    {
        static const char *const SEED[] = {
            "{\"a\":true}", "{\"a\":false}", "{\"a\":null}",
            "{\"a\":\"\\u0041\"}", "{\"a\":\"\\uFFFF\"}", "{\"a\":\"\\\\\"}",
            "{\"a\":\"\\n\"}", "{/* c */\"a\":1}", "{\"a\":1}/* t */",
            "{//c\n\"a\":1}", "{\"a\":\"tail*\"}", "{/*\052",
            "{\"a\":-12.75}", "{\"a\":[1,[2,[3]]]}", "{\"a\":{\"b\":{\"c\":1}}}",
            "\xef\xbb\xbf{\"a\":1}", "{\"colors\":{\"bg\":\"#1a1b26\"}}",
            "{\"name\":\"x\",\"font\":\"med\",\"metrics\":{\"gap\":4}}"
        };
        int ok = 1, tried = 0;
        unsigned s;
        for (s = 0; s < sizeof SEED / sizeof SEED[0]; s++) {
            int n = (int)strlen(SEED[s]);
            for (i = 0; i <= n; i++) {
                eos_theme_err_t err = parse_guarded(&t, SEED[s], i, &guard);
                tried++;
                if (!guard) ok = 0;
                if (err != EOS_THEME_OK && !is_default(&t)) ok = 0;
            }
        }
        CK(ok, "every prefix of every mid-token seed leaves the default untouched");
        printf("      %d mid-token prefixes over %d seeds\n",
               tried, (int)(sizeof SEED / sizeof SEED[0]));
    }

    printf("\n=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
