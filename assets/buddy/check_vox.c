// check_vox — looks at an authored .vox the way the board will.
//
// A generator can only prove that it wrote the voxels it meant to write. What
// a buddy actually looks like is decided by eos_vox_finish(), which throws
// away the interior, and by eos_buddy_render(), which projects three faces
// per voxel in painter order — so this program runs the file through both of
// those, unmodified, and prints the result as ASCII at eight yaw steps plus
// one blinking frame. Every buddy in this directory was tuned against its
// output and none of them looked right the first time.
//
// The non-obvious part is the shade table. It is built from the file's own
// RGBA palette rather than from named colour indices, so any model can be
// checked; two indices holding the SAME RGB share a character family, because
// on the panel they are one colour, and that pair is exactly what the blink
// swaps between. The lid index is forced into the table even though no voxel
// carries it — leave it out and a blink draws two holes, which is a bug in
// this program rather than in the model, and it is a confusing one.
//
// The two numbers to watch per frame are "enclosed gaps", which counts unlit
// pixels walled in on all four sides and should be zero, and "painter
// violations", which is eos_buddy's own audit of whether anything further
// away overwrote something nearer.
//
//   cc -std=c99 -Wall -Wextra -O1 -I../../kernel/avatar/include \
//      ../../kernel/avatar/eos_vox.c ../../kernel/avatar/eos_buddy.c \
//      check_vox.c -o check_vox -lm
//   ./check_vox owl.vox 4 5 44          # file, eye index, lid index, canvas
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eos_vox.h"
#include "eos_buddy.h"

static eos_voxel_t pool[EOS_VOX_MAX_VOXELS];
static uint8_t canvas[160 * 160];
static int32_t audit[160 * 160];
static uint8_t lut[768];

// One three-character family per material, brightest material first, and
// inside a family the characters run top face / y face / x face. Both things
// a reader needs are then in the picture at once: which colour a voxel is,
// and which way its face points.
static const char *FAMILY[] = { "@%#", "&*+", "OoC", "=~-", ":;,", "\'`." };
#define FAM_N ((int)(sizeof FAMILY / sizeof FAMILY[0]))
static char RAMP[1 + FAM_N * 3] = " ";
static int  RAMP_N = 1;

static int luma_of(const eos_vox_pal_t *pal, int ci)
{
    return (pal->rgb[ci][0] * 77 + pal->rgb[ci][1] * 150 + pal->rgb[ci][2] * 29) >> 8;
}

// Materials the model actually uses, brightest first, so the family order is
// stable for one model but not tied to which index the author chose.
static void build_lut(const eos_vox_model_t *m, const eos_vox_pal_t *pal,
                      int eye, int lid)
{
    int used[256], n = 0, seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < m->count; i++)
        if (!seen[m->v[i].ci]) { seen[m->v[i].ci] = 1; used[n++] = m->v[i].ci; }
    // The lid index is deliberately carried by no voxel: it exists in the
    // palette so the blink has somewhere to swap to. Leave it out of the
    // table and a blink draws two holes instead of two closed eyes, which is
    // a bug in this program and not in the model.
    if (eye > 0 && !seen[eye]) { seen[eye] = 1; used[n++] = eye; }
    if (lid > 0 && !seen[lid]) { seen[lid] = 1; used[n++] = lid; }
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (luma_of(pal, used[j]) > luma_of(pal, used[i])) {
                int t = used[i]; used[i] = used[j]; used[j] = t;
            }

    memset(RAMP, 0, sizeof RAMP);
    RAMP[0] = ' ';
    RAMP_N = 1;
    memset(lut, 0, sizeof lut);
    printf("  materials, brightest first:\n");
    int fam = 0;
    for (int i = 0; i < n; i++) {
        int ci = used[i];
        // Two indices holding the same RGB are one colour on the panel - that
        // is the whole point of the eye/lid pair - so they share a family
        // here too, or a blink would look like a colour change.
        int twin = -1;
        for (int j = 0; j < i; j++)
            if (memcmp(pal->rgb[used[j]], pal->rgb[ci], 3) == 0) { twin = used[j]; break; }
        if (twin >= 0) {
            for (int lvl = 0; lvl < 3; lvl++)
                lut[lvl * 256 + ci] = lut[lvl * 256 + twin];
            printf("    %.3s  index %d  #%02x%02x%02x  (same colour as index %d)\n",
                   &RAMP[lut[0 * 256 + ci]], ci,
                   pal->rgb[ci][0], pal->rgb[ci][1], pal->rgb[ci][2], twin);
            continue;
        }
        if (fam >= FAM_N) { printf("    (index %d has no family left)\n", ci); continue; }
        printf("    %s  index %d  #%02x%02x%02x  luma %3d\n", FAMILY[fam], ci,
               pal->rgb[ci][0], pal->rgb[ci][1], pal->rgb[ci][2], luma_of(pal, ci));
        for (int lvl = 0; lvl < 3; lvl++) {
            RAMP[RAMP_N] = FAMILY[fam][lvl];
            lut[lvl * 256 + ci] = (uint8_t)RAMP_N;
            RAMP_N++;
        }
        fam++;
    }
}

static void show(eos_buddy_t *b, int w, int h, const char *label)
{
    eos_buddy_target_t t;
    memset(&t, 0, sizeof t);
    memset(audit, 0, sizeof(int32_t) * (size_t)(w * h));
    t.pixels = canvas; t.w = (uint16_t)w; t.h = (uint16_t)h;
    t.fmt = EOS_BUDDY_PIX_I8; t.clear = true; t.bg_i8 = 0;
    t.audit_depth = audit;
    eos_buddy_render(b, &t);

    int top = h, bot = -1, lo = w, hi = -1, lit = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (canvas[y * w + x]) {
                lit++;
                if (y < top) top = y;
                if (y > bot) bot = y;
                if (x < lo) lo = x;
                if (x > hi) hi = x;
            }
    if (bot < 0) { printf("    %s: EMPTY RENDER\n", label); return; }

    // A hole is an unlit pixel with lit pixels on both sides on its row and
    // both above and below on its column: the body should be opaque.
    int holes = 0;
    for (int y = top + 1; y < bot; y++)
        for (int x = lo + 1; x < hi; x++) {
            if (canvas[y * w + x]) continue;
            int l = 0, r = 0, u = 0, d = 0;
            for (int k = lo; k < x; k++) if (canvas[y * w + k]) l = 1;
            for (int k = x + 1; k <= hi; k++) if (canvas[y * w + k]) r = 1;
            for (int k = top; k < y; k++) if (canvas[k * w + x]) u = 1;
            for (int k = y + 1; k <= bot; k++) if (canvas[k * w + x]) d = 1;
            holes += (l && r && u && d);
        }

    printf("    %-16s yaw %2u  %4u faces  %dx%d px  %d lit  %d enclosed gaps  %u painter violations\n",
           label, (unsigned)eos_buddy_yaw_step(b), (unsigned)b->faces_drawn,
           hi - lo + 1, bot - top + 1, lit, holes, (unsigned)t.audit_violations);
    for (int y = top; y <= bot; y++) {
        printf("      ");
        for (int x = lo; x <= hi; x++) {
            uint8_t v = canvas[y * w + x];
            char c = RAMP[v < RAMP_N ? v : 0];
            putchar(c); putchar(c);
        }
        putchar('\n');
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: check_vox file.vox [eye] [shut] [canvas]\n"); return 2; }
    int eye = argc > 2 ? atoi(argv[2]) : 0;
    int shut = argc > 3 ? atoi(argv[3]) : 0;
    int cw = argc > 4 ? atoi(argv[4]) : 44;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static uint8_t buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    static eos_vox_pal_t pal;
    eos_vox_model_t m;
    eos_vox_err_t e = eos_vox_parse(buf, (uint32_t)n, pool, EOS_VOX_MAX_VOXELS, &pal, &m);
    if (e != EOS_VOX_OK) {
        printf("  PARSE FAILED: %s\n", eos_vox_strerror(e));
        return 1;
    }
    printf("  %s: %zu bytes, %ux%ux%u, %u voxels kept after culling\n",
           argv[1], n, m.sx, m.sy, m.sz, m.count);

    eos_buddy_cfg_t cfg;
    eos_buddy_default_cfg(&cfg);
    cfg.eye_ci = (uint8_t)eye;
    cfg.eye_shut_ci = (uint8_t)shut;
    build_lut(&m, &pal, eye, shut);
    cfg.shade_lut = lut;

    eos_buddy_t b;
    eos_buddy_init(&b, &m, &cfg);

    static const int YAWS[] = { 0, 2, 4, 8, 12, 16, 24, 28 };
    char lab[32];
    for (int i = 0; i < (int)(sizeof YAWS / sizeof YAWS[0]); i++) {
        b.yaw_q8 = b.yaw_target_q8 = (int32_t)YAWS[i] << 8;
        b.bob_q8 = b.shear_q8 = b.squash_q8 = 0;
        b.blink_left_ms = 0;
        snprintf(lab, sizeof lab, "yaw %d", YAWS[i]);
        show(&b, cw, cw, lab);
    }

    // the blink pair: same frame, eyes shut
    b.yaw_q8 = b.yaw_target_q8 = 4 << 8;
    b.blink_left_ms = 100;
    show(&b, cw, cw, "yaw 4 blinking");
    return 0;
}
