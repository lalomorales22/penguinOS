// Host test for eos_vox + eos_buddy. Builds the stock buddy in code, writes a
// real .vox file byte for byte and reads it back, then renders the buddy to an
// ASCII grid at eight yaw steps so a human can watch it turn and decide with
// their own eyes whether it looks good.
//
// The checks that matter: interior culling removes exactly the voxels that
// are buried and leaves exactly the right face masks; the painter sort never
// lets a far pixel land on top of a near one (proved per pixel, not per
// model); nothing is ever written outside the target buffer at any scale or
// canvas size; and a malformed .vox is rejected rather than trusted.
//
// cc -std=c99 -Wall -Wextra -O1 -Iinclude eos_vox.c eos_buddy.c test/test_vox.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eos_vox.h"
#include "eos_buddy.h"

static int checks = 0, fails = 0;
#define CK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("    FAIL: %s\n", msg); } } while (0)

// ------------------------------------------------------------- the buddy

#define BW 11
#define BD  7
#define BH 15

#define CI_BODY   1
#define CI_ACCENT 2
#define CI_DARK   3
#define CI_EYE    4
#define CI_LID    5

static uint8_t grid[BW][BD][BH];

static void paint(int x0, int x1, int y0, int y1, int z0, int z1, uint8_t ci)
{
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++)
                if (x >= 0 && x < BW && y >= 0 && y < BD && z >= 0 && z < BH)
                    grid[x][y][z] = ci;
}

// Big head, small body, feet apart. Painted in order, later wins, so the
// eyes and mouth are cut into the head after the head exists.
static void build_grid(void)
{
    memset(grid, 0, sizeof(grid));

    paint(1, 3, 2, 4, 0, 1, CI_DARK);            // left foot
    paint(7, 9, 2, 4, 0, 1, CI_DARK);            // right foot
    paint(1, 9, 1, 5, 2, 8, CI_BODY);            // torso
    for (int z = 2; z <= 8; z++) {               // bevel the torso corners
        grid[1][1][z] = grid[1][5][z] = 0;
        grid[9][1][z] = grid[9][5][z] = 0;
    }
    paint(3, 7, 1, 1, 3, 6, CI_ACCENT);          // belly patch
    paint(0, 10, 0, 6, 9, 14, CI_BODY);          // head, wider than the body
    for (int z = 9; z <= 14; z++) {              // bevel the head corners
        grid[0][0][z] = grid[0][6][z] = 0;
        grid[10][0][z] = grid[10][6][z] = 0;
    }
    for (int x = 0; x < BW; x++) grid[x][0][14] = grid[x][6][14] = 0;
    for (int y = 0; y < BD; y++) grid[0][y][14] = grid[10][y][14] = 0;

    paint(2, 3, 0, 0, 11, 12, CI_EYE);           // eyes, on the front face
    paint(7, 8, 0, 0, 11, 12, CI_EYE);
    paint(4, 6, 0, 0, 10, 10, CI_DARK);          // mouth
}

static int grid_full(int x, int y, int z)
{
    if (x < 0 || x >= BW || y < 0 || y >= BD || z < 0 || z >= BH) return 0;
    return grid[x][y][z] != 0;
}

static uint8_t grid_faces(int x, int y, int z)
{
    uint8_t f = 0;
    if (!grid_full(x + 1, y, z)) f |= EOS_VOX_FACE_XP;
    if (!grid_full(x - 1, y, z)) f |= EOS_VOX_FACE_XN;
    if (!grid_full(x, y + 1, z)) f |= EOS_VOX_FACE_YP;
    if (!grid_full(x, y - 1, z)) f |= EOS_VOX_FACE_YN;
    if (!grid_full(x, y, z + 1)) f |= EOS_VOX_FACE_ZP;
    if (!grid_full(x, y, z - 1)) f |= EOS_VOX_FACE_ZN;
    return f;
}

// ------------------------------------------------------------- guarded pool

#define POOL_N 2048
#define GUARD  16
static eos_voxel_t pool[POOL_N + GUARD];

static void arm_pool(void) { memset(pool, 0xA5, sizeof(pool)); }

static int pool_guard_ok(void)
{
    const unsigned char *p = (const unsigned char *)&pool[POOL_N];
    size_t n = GUARD * sizeof(eos_voxel_t);
    for (size_t i = 0; i < n; i++) if (p[i] != 0xA5) return 0;
    return 1;
}

static eos_vox_pal_t palette;

static void build_palette(void)
{
    eos_vox_default_palette(&palette);
    static const uint8_t mine[6][3] = {
        {  0,   0,   0 },
        { 96, 200, 216 },   // body, a cold teal
        { 250, 206, 112 },  // belly patch
        { 38,  48,  70 },   // feet, mouth
        { 22,  26,  38 },   // open eye
        { 96, 200, 216 }    // shut eye: the body colour closes over it
    };
    for (int i = 1; i < 6; i++) memcpy(palette.rgb[i], mine[i], 3);
}

// ------------------------------------------------------------- .vox writer

static uint32_t w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    return 4;
}

// Writes a real, MagicaVoxel-loadable file: VOX header, MAIN, then SIZE,
// XYZI and optionally RGBA as MAIN's children.
static uint32_t vox_build(uint8_t *out, uint32_t cap, int sx, int sy, int sz,
                          const uint8_t (*vx)[4], int n, const eos_vox_pal_t *pal)
{
    uint32_t need = 8 + 12 + (12 + 12) + (12 + 4 + 4u * (uint32_t)n) + (pal ? 12 + 1024 : 0);
    if (need > cap) return 0;

    uint8_t *k = out + 8 + 12;
    uint32_t kn = 0;

    memcpy(k + kn, "SIZE", 4); kn += 4;
    kn += w32(k + kn, 12); kn += w32(k + kn, 0);
    kn += w32(k + kn, (uint32_t)sx); kn += w32(k + kn, (uint32_t)sy); kn += w32(k + kn, (uint32_t)sz);

    memcpy(k + kn, "XYZI", 4); kn += 4;
    kn += w32(k + kn, 4 + 4u * (uint32_t)n); kn += w32(k + kn, 0);
    kn += w32(k + kn, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        k[kn++] = vx[i][0]; k[kn++] = vx[i][1];
        k[kn++] = vx[i][2]; k[kn++] = vx[i][3];
    }

    if (pal) {
        memcpy(k + kn, "RGBA", 4); kn += 4;
        kn += w32(k + kn, 1024); kn += w32(k + kn, 0);
        for (int j = 0; j < 255; j++) {
            k[kn++] = pal->rgb[j + 1][0];
            k[kn++] = pal->rgb[j + 1][1];
            k[kn++] = pal->rgb[j + 1][2];
            k[kn++] = 255;
        }
        k[kn++] = 0; k[kn++] = 0; k[kn++] = 0; k[kn++] = 0;   // the unused 256th
    }

    uint32_t t = 0;
    memcpy(out, "VOX ", 4); t = 4;
    t += w32(out + t, 150);
    memcpy(out + t, "MAIN", 4); t += 4;
    t += w32(out + t, 0);
    t += w32(out + t, kn);
    return t + kn;
}

// --------------------------------------------------------------- ASCII view

#define AW 128
#define AH 96
static uint8_t canvas[AW * AH];
static int32_t audit[AW * AH];
static uint8_t shade_lut[768];

// Three shades per material plus two flat eye tones. The ramp runs bright to
// dark inside each material so the ASCII really does show the lighting.
static const char RAMP[] = " #+.%*,@&:O-";

static void build_ascii_lut(void)
{
    for (int lvl = 0; lvl < 3; lvl++)
        for (int ci = 0; ci < 256; ci++) {
            uint8_t o;
            switch (ci) {
            case CI_ACCENT: o = (uint8_t)(4 + lvl); break;
            case CI_DARK:   o = (uint8_t)(7 + lvl); break;
            case CI_EYE:    o = 10; break;
            case CI_LID:    o = 11; break;
            default:        o = (uint8_t)(1 + lvl); break;
            }
            shade_lut[lvl * 256 + ci] = o;
        }
}

static void show(eos_buddy_t *b, int w, int h, const char *label)
{
    eos_buddy_target_t t;
    memset(&t, 0, sizeof(t));
    t.pixels = canvas; t.w = (uint16_t)w; t.h = (uint16_t)h;
    t.fmt = EOS_BUDDY_PIX_I8; t.clear = true; t.bg_i8 = 0;
    eos_buddy_render(b, &t);

    int top = h, bot = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (canvas[y * w + x]) { if (y < top) top = y; if (y > bot) bot = y; }

    printf("    %-22s yaw %2u  %-9s  %4u faces\n", label,
           (unsigned)eos_buddy_yaw_step(b),
           eos_buddy_state_name(eos_buddy_state(b)), (unsigned)b->faces_drawn);
    for (int y = top; y <= bot; y++) {
        printf("      ");
        for (int x = 0; x < w; x++) {
            char c = RAMP[canvas[y * w + x] < sizeof(RAMP) - 1 ? canvas[y * w + x] : 0];
            putchar(c); putchar(c);
        }
        putchar('\n');
    }
    putchar('\n');
}

static void freeze(eos_buddy_t *b, int step)
{
    b->yaw_q8 = b->yaw_target_q8 = (int32_t)step << 8;
    b->bob_q8 = 0; b->shear_q8 = 0; b->squash_q8 = 0;
}

// ------------------------------------------------------------------- main

static uint32_t frng = 0x1234567u;
static uint32_t fr(void) { frng ^= frng << 13; frng ^= frng >> 17; frng ^= frng << 5; return frng; }

int main(int argc, char **argv)
{
    static uint8_t file[96 * 1024];
    static uint8_t mutant[96 * 1024];
    eos_vox_model_t m;
    eos_buddy_cfg_t cfg;
    eos_buddy_t b;

    build_grid();
    build_palette();
    build_ascii_lut();

    // ---------------------------------------------------------- 1. culling
    printf("\n== interior culling ==\n");
    arm_pool();
    eos_vox_model_init(&m, pool, POOL_N, BW, BD, BH, &palette);
    int raw = 0;
    for (int z = 0; z < BH; z++)
        for (int y = 0; y < BD; y++)
            for (int x = 0; x < BW; x++)
                if (grid[x][y][z]) { eos_vox_set(&m, (uint8_t)x, (uint8_t)y, (uint8_t)z, grid[x][y][z]); raw++; }

    int buried = 0;
    for (int x = 0; x < BW; x++)
        for (int y = 0; y < BD; y++)
            for (int z = 0; z < BH; z++)
                if (grid[x][y][z] && grid_faces(x, y, z) == 0) buried++;

    uint16_t removed = eos_vox_finish(&m);
    printf("    %d solid voxels, %d buried, %u removed, %u left (%d%% of the model gone)\n",
           raw, buried, (unsigned)removed, (unsigned)m.count, removed * 100 / raw);

    CK(raw == (int)m.count + removed, "every voxel is either kept or removed");
    CK(removed > 0, "interior culling actually removes voxels");
    CK((int)removed == buried, "it removes exactly the fully buried voxels");
    CK(removed * 100 / raw > 25, "culling is worth doing: over a quarter of the model goes");
    CK(pool_guard_ok(), "building the model stayed inside the pool");

    int mask_ok = 1, kept_ok = 1, any_face = 1;
    for (uint16_t i = 0; i < m.count; i++) {
        const eos_voxel_t *v = &m.v[i];
        if (v->faces == 0) any_face = 0;
        if (v->faces != grid_faces(v->x, v->y, v->z)) mask_ok = 0;
        if (!grid[v->x][v->y][v->z]) kept_ok = 0;
    }
    CK(any_face, "no surviving voxel has an empty face mask");
    CK(mask_ok, "every face mask matches an independent neighbour scan");
    CK(kept_ok, "every surviving voxel is one that was painted");

    int found_all = 1;
    for (int x = 0; x < BW; x++)
        for (int y = 0; y < BD; y++)
            for (int z = 0; z < BH; z++) {
                int want = grid[x][y][z] && grid_faces(x, y, z) != 0;
                if (eos_vox_occupied(&m, x, y, z) != (want != 0)) found_all = 0;
            }
    CK(found_all, "the sorted pool answers occupancy for exactly the kept voxels");
    CK(eos_vox_finish(&m) == 0, "finish() is idempotent and never re-exposes buried faces");

    // ------------------------------------------------------- 2. .vox round trip
    printf("\n== .vox round trip ==\n");
    static uint8_t vx[512][4];
    int nv = 0;
    for (int z = 0; z < 4; z++)
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int on = (x == 1 || x == 2) + (y == 1 || y == 2) + (z == 1 || z == 2);
                if (on >= 2) {
                    vx[nv][0] = (uint8_t)x; vx[nv][1] = (uint8_t)y;
                    vx[nv][2] = (uint8_t)z; vx[nv][3] = (uint8_t)(CI_BODY + (z & 1));
                    nv++;
                }
            }
    uint32_t flen = vox_build(file, sizeof(file), 4, 4, 4, vx, nv, &palette);
    printf("    wrote a %u-byte .vox holding a %d-voxel plus\n", (unsigned)flen, nv);
    CK(flen > 0, "the test wrote a .vox file");

    eos_vox_pal_t rpal;
    arm_pool();
    eos_vox_err_t e = eos_vox_parse(file, flen, pool, POOL_N, &rpal, &m);
    CK(e == EOS_VOX_OK, "the written file parses back");
    CK(m.sx == 4 && m.sy == 4 && m.sz == 4, "SIZE round trips");
    CK(m.culled, "parse leaves the model culled and ready to draw");
    CK(memcmp(rpal.rgb[CI_BODY], palette.rgb[CI_BODY], 3) == 0, "RGBA chunk round trips");
    CK(memcmp(rpal.rgb[CI_ACCENT], palette.rgb[CI_ACCENT], 3) == 0, "RGBA index mapping is off by one, as the format says");
    CK(pool_guard_ok(), "parsing stayed inside the pool");

    int seen = 0;
    for (int z = 0; z < 4; z++)
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int on = (x == 1 || x == 2) + (y == 1 || y == 2) + (z == 1 || z == 2);
                if (on >= 2 && eos_vox_occupied(&m, x, y, z)) seen++;
            }
    CK(seen + (int)0 == (int)m.count, "every parsed voxel sits where it was written");

    uint32_t nolen = vox_build(file, sizeof(file), 4, 4, 4, vx, nv, NULL);
    arm_pool();
    memset(&rpal, 0x11, sizeof(rpal));
    CK(eos_vox_parse(file, nolen, pool, POOL_N, &rpal, &m) == EOS_VOX_OK, "a file with no RGBA chunk parses");
    CK(rpal.rgb[1][0] == 0xFF && rpal.rgb[1][1] == 0xFF && rpal.rgb[1][2] == 0xFF,
       "a file with no RGBA chunk falls back to the stock palette");
    CK(rpal.rgb[255][0] == 0x11 && rpal.rgb[255][2] == 0x11, "stock palette tail is the grey ramp");

    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        if (f) { flen = vox_build(file, sizeof(file), 4, 4, 4, vx, nv, &palette);
                 fwrite(file, 1, flen, f); fclose(f);
                 printf("    also written to %s for MagicaVoxel\n", argv[1]); }
    }

    // ------------------------------------------------- 3. malformed input
    printf("\n== malformed input ==\n");
    flen = vox_build(file, sizeof(file), 4, 4, 4, vx, nv, &palette);

    #define FULL 0xFFFFFFFFu
    struct { const char *what; uint32_t at; uint8_t val[4]; int n; uint32_t len; eos_vox_err_t want; } bad[] = {
        { "empty buffer",            0, {0},                 0, 0,        EOS_VOX_ERR_TRUNCATED },
        { "eight bytes, no MAIN",    0, {0},                 0, 8,        EOS_VOX_ERR_TRUNCATED },
        { "four bytes",              0, {0},                 0, 4,        EOS_VOX_ERR_TRUNCATED },
        { "bad magic",               0, {'B','O','X',' '},   4, FULL,        EOS_VOX_ERR_MAGIC     },
        { "absurd version",          4, {0xE7,0x03,0,0},     4, FULL,        EOS_VOX_ERR_VERSION   },
        { "no MAIN chunk",           8, {'J','U','N','K'},   4, FULL,        EOS_VOX_ERR_CHUNK     },
        { "MAIN children overrun",  16, {0xFF,0xFF,0xFF,0x7F},4, FULL,       EOS_VOX_ERR_CHUNK     },
        { "MAIN content overrun",   12, {0xFF,0xFF,0xFF,0x7F},4, FULL,       EOS_VOX_ERR_CHUNK     },
        { "SIZE chunk overrun",     24, {0xF0,0xFF,0xFF,0xFF},4, FULL,       EOS_VOX_ERR_CHUNK     },
        { "zero dimension",         32, {0,0,0,0},           4, FULL,        EOS_VOX_ERR_DIM       },
        { "dimension over the cap", 32, {33,0,0,0},          4, FULL,        EOS_VOX_ERR_DIM       },
        { "voxel count overflow",   56, {0xFF,0xFF,0xFF,0xFF},4, FULL,       EOS_VOX_ERR_COUNT     },
        { "voxel count lies high",  56, {0x00,0x08,0,0},     4, FULL,        EOS_VOX_ERR_TRUNCATED },
        { "voxel outside SIZE",     60, {9,0,0,1},           4, FULL,        EOS_VOX_ERR_RANGE     },
        { "SIZE content truncated", 24, {8,0,0,0},           4, FULL,     EOS_VOX_ERR_TRUNCATED },
        { "XYZI content truncated", 48, {2,0,0,0},           4, FULL,     EOS_VOX_ERR_TRUNCATED },
        { "RGBA content truncated", 192,{4,0,0,0},           4, FULL,     EOS_VOX_ERR_TRUNCATED },
    };

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memcpy(mutant, file, flen);
        for (int j = 0; j < bad[i].n; j++) mutant[bad[i].at + j] = bad[i].val[j];
        uint32_t use = (bad[i].len == FULL) ? flen : bad[i].len;
        arm_pool();
        eos_vox_err_t r = eos_vox_parse(mutant, use, pool, POOL_N, &rpal, &m);
        printf("    %-24s -> %s\n", bad[i].what, eos_vox_strerror(r));
        CK(r == bad[i].want, bad[i].what);
        CK(pool_guard_ok(), "malformed input stayed inside the pool");
    }

    arm_pool();
    CK(eos_vox_parse(file, flen, pool, 4, &rpal, &m) == EOS_VOX_ERR_POOL,
       "a pool too small is refused, not overflowed");
    CK(pool_guard_ok(), "the refused parse wrote nothing");
    CK(eos_vox_parse(NULL, flen, pool, POOL_N, &rpal, &m) == EOS_VOX_ERR_ARG, "NULL data is refused");
    CK(eos_vox_parse(file, flen, pool, 0, &rpal, &m) == EOS_VOX_ERR_ARG, "a zero-length pool is refused");

    // truncation sweep: every possible short read of a good file
    int trunc_ok = 1, trunc_guard = 1;
    for (uint32_t L = 0; L < flen; L++) {
        arm_pool();
        eos_vox_err_t r = eos_vox_parse(file, L, pool, POOL_N, &rpal, &m);
        if (r == EOS_VOX_OK) trunc_ok = 0;
        if (!pool_guard_ok()) trunc_guard = 0;
    }
    CK(trunc_ok, "no truncated prefix of a good file is ever accepted");
    CK(trunc_guard, "no truncated prefix overruns the pool");

    // random mutation fuzz
    int fuzz_guard = 1, fuzz_sane = 1, fuzz_ok = 0;
    for (int it = 0; it < 8000; it++) {
        memcpy(mutant, file, flen);
        int nmut = 1 + (int)(fr() % 3);
        for (int j = 0; j < nmut; j++) mutant[fr() % flen] = (uint8_t)fr();
        arm_pool();
        eos_vox_err_t r = eos_vox_parse(mutant, flen, pool, POOL_N, &rpal, &m);
        if (!pool_guard_ok()) fuzz_guard = 0;
        if (r == EOS_VOX_OK) {
            fuzz_ok++;
            if (m.count > m.cap) fuzz_sane = 0;
            for (uint16_t i = 0; i < m.count; i++)
                if (m.v[i].x >= m.sx || m.v[i].y >= m.sy || m.v[i].z >= m.sz) fuzz_sane = 0;
        }
    }
    printf("    8000 mutated files: %d parsed, %d rejected\n", fuzz_ok, 8000 - fuzz_ok);
    CK(fuzz_guard, "no mutated file overruns the pool");
    CK(fuzz_sane, "every mutated file that parses yields an in-range model");

    // The sweeps above hand the parser a pointer into a big static array, so a
    // read one byte past the declared length lands on more of that array and
    // nothing notices. Repeat them with the input in an EXACTLY sized malloc
    // block: under -fsanitize=address the redzone turns an over-read into a
    // hard failure, which is the only way this test can see one at all. The
    // heap is a host-test tool; nothing in the kernel path allocates.
    int heap_ok = 1;
    for (uint32_t L = 0; L <= flen; L++) {
        uint8_t *exact = (uint8_t *)malloc(L ? L : 1);
        if (!exact) { heap_ok = 0; break; }
        memcpy(exact, file, L);
        arm_pool();
        eos_vox_err_t r = eos_vox_parse(exact, L, pool, POOL_N, &rpal, &m);
        if (L < flen && r == EOS_VOX_OK) heap_ok = 0;
        if (!pool_guard_ok()) heap_ok = 0;
        free(exact);
    }
    for (int it = 0; it < 20000 && heap_ok; it++) {
        uint8_t *exact = (uint8_t *)malloc(flen);
        if (!exact) { heap_ok = 0; break; }
        memcpy(exact, file, flen);
        int nmut = 1 + (int)(fr() % 6);
        for (int j = 0; j < nmut; j++) exact[fr() % flen] = (uint8_t)fr();
        uint16_t cap = (uint16_t)(1 + fr() % 64);
        eos_voxel_t *heap_pool = (eos_voxel_t *)malloc(sizeof(eos_voxel_t) * cap);
        if (!heap_pool) { free(exact); heap_ok = 0; break; }
        if (eos_vox_parse(exact, flen, heap_pool, cap, &rpal, &m) == EOS_VOX_OK) {
            if (m.count > cap) heap_ok = 0;
            for (uint16_t i = 0; i < m.count; i++)
                if (m.v[i].x >= m.sx || m.v[i].y >= m.sy || m.v[i].z >= m.sz) heap_ok = 0;
        }
        free(heap_pool);
        free(exact);
    }
    printf("    same again on exactly sized buffers so a sanitizer can see an over-read\n");
    CK(heap_ok, "the parser never reads past the length it was given");

    // ------------------------------------------------------ 4. the buddy turns
    printf("\n== the buddy ==\n");
    arm_pool();
    eos_vox_model_init(&m, pool, POOL_N, BW, BD, BH, &palette);
    for (int z = 0; z < BH; z++)
        for (int y = 0; y < BD; y++)
            for (int x = 0; x < BW; x++)
                if (grid[x][y][z]) eos_vox_set(&m, (uint8_t)x, (uint8_t)y, (uint8_t)z, grid[x][y][z]);
    eos_vox_finish(&m);

    eos_buddy_default_cfg(&cfg);
    cfg.eye_ci      = CI_EYE;
    cfg.eye_shut_ci = CI_LID;
    cfg.shade_lut   = shade_lut;
    eos_buddy_init(&b, &m, &cfg);

    freeze(&b, cfg.home_yaw);
    show(&b, 40, 52, "at rest");

    printf("    -- a full turn, one eighth at a time --\n\n");
    for (int s = 0; s < EOS_BUDDY_YAW_STEPS; s += EOS_BUDDY_YAW_STEPS / 8) {
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "%d degrees", s * 360 / EOS_BUDDY_YAW_STEPS);
        freeze(&b, s);
        show(&b, 32, 40, lbl);
    }

    printf("    -- moods --\n\n");
    struct { eos_buddy_state_t s; uint32_t warm; const char *n; } moods[] = {
        { EOS_BUDDY_THINKING,  700, "thinking"  },
        { EOS_BUDDY_TALKING,   160, "talking"   },
        { EOS_BUDDY_HAPPY,     210, "happy"     },
        { EOS_BUDDY_CONFUSED,  950, "confused"  },
        { EOS_BUDDY_SLEEPING, 1100, "sleeping"  }
    };
    for (unsigned i = 0; i < sizeof(moods) / sizeof(moods[0]); i++) {
        eos_buddy_init(&b, &m, &cfg);
        eos_buddy_set_state(&b, moods[i].s);
        if (moods[i].s == EOS_BUDDY_TALKING) b.energy_q8 = 256;
        for (uint32_t t = 0; t < moods[i].warm; t += 20) eos_buddy_tick(&b, 20);
        show(&b, 32, 40, moods[i].n);
    }

    eos_buddy_init(&b, &m, &cfg);
    freeze(&b, cfg.home_yaw);
    b.blink_left_ms = 120;
    show(&b, 32, 40, "blinking");
    CK(eos_buddy_blinking(&b), "the blink is visible to the caller too");

    // ------------------------------------------------- 5. the painter sort
    printf("== painter sort ==\n");
    eos_buddy_init(&b, &m, &cfg);
    eos_buddy_target_t t;
    uint32_t worst = 0, total_px = 0;
    for (int s = 0; s < EOS_BUDDY_YAW_STEPS; s++) {
        freeze(&b, s);
        b.shear_q8 = (int16_t)(s * 23 - 360);      // lean hard both ways
        b.squash_q8 = (int16_t)((s & 3) * 40 - 60);
        memset(&t, 0, sizeof(t));
        t.pixels = canvas; t.w = 64; t.h = 64; t.fmt = EOS_BUDDY_PIX_I8;
        t.clear = true; t.audit_depth = audit;
        eos_buddy_render(&b, &t);
        if (t.audit_violations > worst) worst = t.audit_violations;
        total_px += t.audit_pixels;
    }
    printf("    32 yaw steps, %u pixel writes, %u overwrites by something further away\n",
           (unsigned)total_px, (unsigned)worst);
    CK(worst == 0, "no pixel is ever overwritten by a further-away face");
    CK(total_px > 0, "the audit actually saw the drawing happen");

    // the same at RGB565, and with the model re-sorted cold each time
    worst = 0;
    static uint16_t rgbbuf[AW * AH];
    for (int s = 0; s < EOS_BUDDY_YAW_STEPS; s++) {
        eos_vox_finish(&m);                        // force the cold sort path
        freeze(&b, s);
        memset(&t, 0, sizeof(t));
        t.pixels = rgbbuf; t.w = 96; t.h = 72; t.fmt = EOS_BUDDY_PIX_RGB565;
        t.clear = true; t.bg_565 = 0x0000; t.audit_depth = audit;
        eos_buddy_render(&b, &t);
        if (t.audit_violations > worst) worst = t.audit_violations;
    }
    CK(worst == 0, "the cold sort path is correct too, at RGB565");

    eos_vox_finish(&m);
    CK(m.sorted, "finish() puts the pool back in spatial order after a render");

    // ------------------------------------------------- 6. watertight faces
    printf("\n== watertight surfaces ==\n");
    // A solid box projects to a convex hexagon, so every scanline of it must
    // be ONE unbroken run. Anything else means two faces that share an edge
    // disagreed about where that edge was and left a hole in a flat wall.
    static eos_voxel_t bpool[1024];
    eos_vox_model_t box;
    eos_vox_model_init(&box, bpool, 1024, 9, 9, 9, &palette);
    for (int x = 0; x < 9; x++)
        for (int y = 0; y < 9; y++)
            for (int z = 0; z < 9; z++) eos_vox_set(&box, (uint8_t)x, (uint8_t)y, (uint8_t)z, CI_BODY);
    eos_vox_finish(&box);
    CK(box.count == 9 * 9 * 9 - 7 * 7 * 7, "a solid box culls down to its shell");

    eos_buddy_t bb;
    eos_buddy_init(&bb, &box, &cfg);
    int worst_gap = 0, tested = 0;
    for (int sh = -900; sh <= 900; sh += 150)
        for (int s = 0; s < EOS_BUDDY_YAW_STEPS; s++)
            for (int sc = 0; sc <= 2600; sc += 650) {
                bb.cfg.scale_q8 = (uint16_t)sc;
                bb.fit_scale_q8 = 0;
                freeze(&bb, s);
                bb.shear_q8 = (int16_t)sh;
                // Squash scales x/y and z by different factors, so it is a
                // second chance for two faces to disagree about a shared edge.
                bb.squash_q8 = (int16_t)(((sh / 150) & 3) * 90 - 140);
                memset(&t, 0, sizeof(t));
                t.pixels = canvas; t.w = 96; t.h = 96; t.fmt = EOS_BUDDY_PIX_I8;
                t.clear = true; t.bg_i8 = 0;
                eos_buddy_render(&bb, &t);
                tested++;
                for (int y = 0; y < 96; y++) {
                    int runs = 0, in = 0;
                    for (int x = 0; x < 96; x++) {
                        int on = canvas[y * 96 + x] != 0;
                        if (on && !in) runs++;
                        in = on;
                    }
                    if (runs - 1 > worst_gap) worst_gap = runs - 1;
                }
            }
    printf("    %d projections of a solid box, worst interior gap on any scanline: %d px\n",
           tested, worst_gap);
    CK(worst_gap == 0, "adjacent faces tile exactly: no seams in a flat wall");

    // ------------------------------------------------- 6. buffer bounds
    printf("\n== buffer bounds ==\n");
    #define TW 40
    #define TH 30
    #define PAD 64
    static uint8_t gbuf[PAD + TW * TH + PAD];
    static uint16_t gbuf16[PAD + TW * TH + PAD];

    int guards = 1;
    struct { int w, h; uint16_t scale; } sizes[] = {
        { 1, 1, 0 }, { 3, 2, 0 }, { TW, 1, 0 }, { 1, TH, 0 },
        { TW, TH, 0 }, { TW, TH, 8192 }, { TW, TH, 4096 }, { 2, 2, 8192 },
        { 7, 29, 8192 }, { TW, TH, 256 }
    };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (int rgb = 0; rgb < 2; rgb++) {
            memset(gbuf, 0x5A, sizeof(gbuf));
            memset(gbuf16, 0x5A, sizeof(gbuf16));
            cfg.scale_q8 = sizes[i].scale;
            eos_buddy_init(&b, &m, &cfg);
            for (int s = 0; s < EOS_BUDDY_YAW_STEPS; s += 3) {
                freeze(&b, s);
                b.shear_q8 = 900; b.bob_q8 = 900; b.squash_q8 = -180;
                memset(&t, 0, sizeof(t));
                t.pixels = rgb ? (void *)(gbuf16 + PAD) : (void *)(gbuf + PAD);
                t.w = (uint16_t)sizes[i].w; t.h = (uint16_t)sizes[i].h;
                t.fmt = rgb ? EOS_BUDDY_PIX_RGB565 : EOS_BUDDY_PIX_I8;
                t.clear = true;
                eos_buddy_render(&b, &t);
            }
            for (int j = 0; j < PAD; j++) {
                if (gbuf[j] != 0x5A || gbuf[PAD + TW * TH + j] != 0x5A) guards = 0;
                if (gbuf16[j] != 0x5A5A || gbuf16[PAD + TW * TH + j] != 0x5A5A) guards = 0;
            }
        }
    }
    cfg.scale_q8 = 0;
    printf("    %u canvas sizes x 2 formats x 11 yaws, extreme lean and bob\n",
           (unsigned)(sizeof(sizes) / sizeof(sizes[0])));
    CK(guards, "nothing is ever written outside the target buffer");

    eos_buddy_init(&b, &m, &cfg);
    CK(eos_buddy_render(NULL, &t) == -1, "render refuses a NULL buddy");
    CK(eos_buddy_render(&b, NULL) == -1, "render refuses a NULL target");
    memset(&t, 0, sizeof(t));
    t.pixels = canvas; t.w = 0; t.h = 8;
    CK(eos_buddy_render(&b, &t) == -1, "render refuses a zero-width target");

    eos_vox_model_t empty;
    eos_vox_model_init(&empty, pool, POOL_N, 4, 4, 4, &palette);
    eos_buddy_t eb;
    eos_buddy_init(&eb, &empty, &cfg);
    memset(&t, 0, sizeof(t));
    t.pixels = canvas; t.w = 8; t.h = 8; t.clear = true; t.bg_i8 = 7;
    CK(eos_buddy_render(&eb, &t) == 0, "an empty model draws nothing and says so");
    CK(canvas[0] == 7, "an empty model still clears the target");

    // ------------------------------------------------- 7. the state machine
    printf("\n== megabrain lifecycle ==\n");
    eos_buddy_init(&b, &m, &cfg);
    CK(eos_buddy_state(&b) == EOS_BUDDY_IDLE, "the buddy starts idle");

    eos_buddy_event(&b, EOS_BUDDY_EV_USER_TYPING);
    CK(eos_buddy_state(&b) == EOS_BUDDY_LISTENING, "typing makes it listen");

    eos_buddy_event(&b, EOS_BUDDY_EV_REQUEST_SENT);
    CK(eos_buddy_state(&b) == EOS_BUDDY_THINKING, "sending the request makes it think");
    for (int i = 0; i < 40; i++) eos_buddy_tick(&b, 25);
    int turned = eos_buddy_yaw_step(&b);
    CK(turned != cfg.home_yaw, "thinking turns the head away from the user");

    eos_buddy_event(&b, EOS_BUDDY_EV_STREAM_FIRST);
    CK(eos_buddy_state(&b) == EOS_BUDDY_TALKING, "the first chunk makes it talk");
    CK(b.energy_q8 == 256, "the first chunk spikes the energy");
    CK(b.pop_ms > 0, "the first chunk pops");
    for (int i = 0; i < 30; i++) { eos_buddy_tick(&b, 25); eos_buddy_event(&b, EOS_BUDDY_EV_STREAM_CHUNK); }
    CK(eos_buddy_yaw_step(&b) == cfg.home_yaw, "it turns back to face the user while replying");

    int saw_squash = 0;
    for (int i = 0; i < 60; i++) {
        eos_buddy_tick(&b, 20);
        if (i % 3 == 0) eos_buddy_event(&b, EOS_BUDDY_EV_STREAM_CHUNK);
        if (b.squash_q8 > 8) saw_squash = 1;
    }
    CK(saw_squash, "streaming chunks drive the squash-and-stretch");
    CK(eos_buddy_state(&b) == EOS_BUDDY_TALKING, "it keeps talking while chunks arrive");

    for (int i = 0; i < 60; i++) eos_buddy_tick(&b, 25);
    CK(eos_buddy_state(&b) == EOS_BUDDY_IDLE, "a stream that stops without a done lapses to idle");

    eos_buddy_set_state(&b, EOS_BUDDY_TALKING);
    eos_buddy_event(&b, EOS_BUDDY_EV_STREAM_DONE);
    CK(eos_buddy_state(&b) == EOS_BUDDY_HAPPY, "finishing a reply makes it happy");
    for (int i = 0; i < 80; i++) eos_buddy_tick(&b, 25);
    CK(eos_buddy_state(&b) == EOS_BUDDY_IDLE, "happy is a mood, not a home; it goes back to idle");

    eos_buddy_event(&b, EOS_BUDDY_EV_ERROR);
    CK(eos_buddy_state(&b) == EOS_BUDDY_CONFUSED, "an error confuses it");
    for (int i = 0; i < 100; i++) eos_buddy_tick(&b, 25);
    CK(eos_buddy_state(&b) == EOS_BUDDY_IDLE, "confusion passes");

    // set_state() is public, so TALKING has to survive being entered that way
    // too. It used to inherit a stale since_chunk_ms and lapse on the very next
    // tick, which is the sort of bug that only shows up in the real app.
    eos_buddy_init(&b, &m, &cfg);
    for (int i = 0; i < 120; i++) eos_buddy_tick(&b, 30);      // 3.6s of quiet
    eos_buddy_set_state(&b, EOS_BUDDY_TALKING);
    eos_buddy_tick(&b, 30);
    CK(eos_buddy_state(&b) == EOS_BUDDY_TALKING,
       "set_state(TALKING) after a long quiet does not lapse on the next tick");
    for (int i = 0; i < 40; i++) eos_buddy_tick(&b, 30);       // and still lapses later
    CK(eos_buddy_state(&b) == EOS_BUDDY_IDLE, "TALKING still lapses once the chunks stop");

    CK(strcmp(eos_buddy_state_name(EOS_BUDDY_IDLE), "IDLE") == 0, "states have names");
    CK(strcmp(eos_buddy_state_name((eos_buddy_state_t)-1), "?") == 0,
       "an out of range state name is refused, not indexed");
    CK(strcmp(eos_buddy_state_name((eos_buddy_state_t)EOS_BUDDY_STATE_COUNT), "?") == 0,
       "one past the last state is refused too");

    eos_buddy_init(&b, &m, &cfg);
    eos_buddy_event(&b, EOS_BUDDY_EV_IDLE_TIMEOUT);
    CK(eos_buddy_state(&b) == EOS_BUDDY_SLEEPING, "the idle timeout puts it to sleep");
    int shut_always = 1;
    for (int i = 0; i < 200; i++) { eos_buddy_tick(&b, 25); if (!eos_buddy_blinking(&b)) shut_always = 0; }
    CK(shut_always, "a sleeping buddy keeps its eyes shut");
    CK(b.bob_q8 < 0, "a sleeping buddy sinks down");

    cfg.idle_sleep_ms = 3000;
    eos_buddy_init(&b, &m, &cfg);
    for (int i = 0; i < 200; i++) eos_buddy_tick(&b, 25);
    CK(eos_buddy_state(&b) == EOS_BUDDY_SLEEPING, "sitting idle long enough falls asleep on its own");
    cfg.idle_sleep_ms = 0;

    // blink rate and yaw range over a long idle run
    eos_buddy_init(&b, &m, &cfg);
    int blinks = 0, was = 0, yaw_ok = 1, longest = 0, run = 0;
    for (int i = 0; i < 2000; i++) {          // 60 seconds at 30ms
        eos_buddy_tick(&b, 30);
        int now = b.blink_left_ms > 0;
        if (now && !was) blinks++;
        run = now ? run + 1 : 0;
        if (run > longest) longest = run;
        was = now;
        if (b.yaw_q8 < 0 || b.yaw_q8 >= (EOS_BUDDY_YAW_STEPS << 8)) yaw_ok = 0;
    }
    printf("    60s idle: %d blinks, longest %d frames shut\n", blinks, longest);
    CK(blinks >= 8 && blinks <= 40, "an idle buddy blinks at a human rate");
    CK(longest <= 6, "a blink is a blink, not a nap");
    CK(yaw_ok, "yaw stays inside one turn no matter how long it runs");

    eos_buddy_init(&b, &m, &cfg);
    eos_buddy_tick(&b, 4000000u);
    CK(b.yaw_q8 >= 0 && b.yaw_q8 < (EOS_BUDDY_YAW_STEPS << 8), "a stalled frame does not teleport the buddy");
    eos_buddy_tick(&b, 0);
    CK(eos_buddy_state(&b) == EOS_BUDDY_IDLE, "a zero-length tick is harmless");

    // ------------------------------------------------- 8. shade table
    printf("\n== shade table ==\n");
    static uint8_t disp[16 * 3];
    for (int i = 0; i < 16; i++) { disp[i * 3] = disp[i * 3 + 1] = disp[i * 3 + 2] = (uint8_t)(i * 17); }
    static uint8_t lut[768];
    eos_buddy_build_shade_lut(&palette, disp, 16, cfg.shade, lut);
    CK(lut[0 * 256 + CI_BODY] >= lut[1 * 256 + CI_BODY], "the top face is at least as bright as a y face");
    CK(lut[1 * 256 + CI_BODY] >= lut[2 * 256 + CI_BODY], "a y face is at least as bright as an x face");
    CK(lut[0 * 256 + CI_BODY] > lut[2 * 256 + CI_BODY], "three orientations really do get three shades");

    printf("\n=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
