// The encoder. Every step of ISO/IEC 18004 that a version 1..4, byte mode,
// ECC level L symbol needs, and none of the steps it does not.
//
// The one thing worth knowing before reading: GF(256) arithmetic here is done
// by shift-and-xor, with no exp/log tables. Tables would be 768 bytes of .bss
// initialised on first use, which means mutable global state and a first-call
// branch in a kernel that has neither anywhere else. A version-4 symbol needs
// 80 x 20 = 1,600 field multiplies to build its ECC; at eight iterations each
// that is a few tens of microseconds on the C6, once, while the SoftAP is
// still coming up. The table is not worth the state.

#include "eos_qr.h"
#include <string.h>

// ------------------------------------------------------------------- tables
//
// ECC level L, versions 1..4. Sources: ISO/IEC 18004 tables 7, 9 and 13.
// `blocks` is 1 for all four, which is why the interleaving step below is a
// single-block walk rather than the two-group split larger versions need. The
// field is present and honoured anyway, so extending the table upward is a
// data change and not a code change.

typedef struct {
    uint8_t size;        // modules per side
    uint8_t total_cw;    // codewords in the symbol
    uint8_t data_cw;     // of which data
    uint8_t ecc_cw;      // of which ECC
    uint8_t blocks;      // ECC blocks
    uint8_t capacity;    // byte-mode payload bytes
    uint8_t remainder;   // remainder bits after the last codeword
    uint8_t align;       // second alignment centre, 0 when the version has none
} eos_qr_ver_t;

static const eos_qr_ver_t VER[EOS_QR_MAX_VERSION + 1] = {
    {  0,   0,  0,  0, 0,  0, 0,  0 },   // no version 0
    { 21,  26, 19,  7, 1, 17, 0,  0 },
    { 25,  44, 34, 10, 1, 32, 7, 18 },
    { 29,  70, 55, 15, 1, 53, 7, 22 },
    { 33, 100, 80, 20, 1, 78, 7, 26 },
};

// Penalty weights, ISO/IEC 18004 table 11.
#define PEN_N1 3
#define PEN_N2 3
#define PEN_N3 40
#define PEN_N4 10

// Byte mode, and the character-count indicator width for versions 1..9.
#define MODE_BYTE      0x4
#define MODE_BITS      4
#define COUNT_BITS     8
#define PAD_A          0xEC
#define PAD_B          0x11

// Format information: 5 data bits, BCH(15,5) with generator 0x537, then the
// mandatory 0x5412 mask so an all-zero format is never all-light.
#define FMT_ECL_L      0x1
#define FMT_GEN        0x537
#define FMT_MASK       0x5412

const char *eos_qr_strerror(eos_qr_err_t e)
{
    switch (e) {
    case EOS_QR_OK:           return "ok";
    case EOS_QR_ERR_NULL:     return "null argument";
    case EOS_QR_ERR_EMPTY:    return "empty payload";
    case EOS_QR_ERR_TOO_LONG: return "payload too long for version 4 at ECC L";
    case EOS_QR_ERR_VERSION:  return "version outside 1..4";
    }
    return "unknown error";
}

int eos_qr_capacity(int version)
{
    if (version < EOS_QR_MIN_VERSION || version > EOS_QR_MAX_VERSION) return 0;
    return VER[version].capacity;
}

int eos_qr_version_for(size_t len)
{
    int v;
    for (v = EOS_QR_MIN_VERSION; v <= EOS_QR_MAX_VERSION; v++)
        if (len <= (size_t)VER[v].capacity) return v;
    return 0;
}

// --------------------------------------------------------------- module I/O
//
// Row-major, EOS_QR_STRIDE bytes per row at every version, MSB first inside a
// byte. Set bit is dark. Identical layout for `modules` and `reserved`.

static inline bool bm_get(const uint8_t *b, int x, int y)
{
    return (b[y * EOS_QR_STRIDE + (x >> 3)] >> (7 - (x & 7))) & 1;
}

static inline void bm_set(uint8_t *b, int x, int y, bool v)
{
    uint8_t m = (uint8_t)(0x80u >> (x & 7));
    int i = y * EOS_QR_STRIDE + (x >> 3);
    if (v) b[i] |= m; else b[i] &= (uint8_t)~m;
}

static inline void bm_xor(uint8_t *b, int x, int y)
{
    b[y * EOS_QR_STRIDE + (x >> 3)] ^= (uint8_t)(0x80u >> (x & 7));
}

bool eos_qr_module(const eos_qr_t *qr, int x, int y)
{
    if (!qr || qr->size == 0) return false;
    if (x < 0 || y < 0 || x >= qr->size || y >= qr->size) return false;
    return bm_get(qr->modules, x, y);
}

int eos_qr_size(const eos_qr_t *qr)
{
    return qr ? qr->size : 0;
}

// ------------------------------------------------------------------ GF(256)
//
// Primitive polynomial x^8 + x^4 + x^3 + x^2 + 1, which is 0x11D; truncated to
// eight bits after the shift that is the 0x1D below.

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        b = (uint8_t)(b >> 1);
        a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1D : 0x00));
    }
    return r;
}

// g(x) = product of (x - alpha^i) for i in 0..ec-1, coefficients in descending
// order with the leading 1 implied. `out` must hold `ec` bytes.
static void rs_generator(uint8_t *out, int ec)
{
    int i, j;
    uint8_t root = 1;

    memset(out, 0, (size_t)ec);
    out[ec - 1] = 1;                      // start from the polynomial 1

    for (i = 0; i < ec; i++) {
        // Multiply the accumulated polynomial by (x + root).
        for (j = 0; j < ec; j++) {
            out[j] = gf_mul(out[j], root);
            if (j + 1 < ec) out[j] ^= out[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
}

// The remainder of data(x) * x^ec divided by g(x). `out` must hold `ec` bytes.
static void rs_remainder(const uint8_t *data, int len, const uint8_t *gen,
                         int ec, uint8_t *out)
{
    int i, j;

    memset(out, 0, (size_t)ec);
    for (i = 0; i < len; i++) {
        uint8_t factor = (uint8_t)(data[i] ^ out[0]);
        memmove(out, out + 1, (size_t)ec - 1);
        out[ec - 1] = 0;
        for (j = 0; j < ec; j++) out[j] ^= gf_mul(gen[j], factor);
    }
}

// ------------------------------------------------------------------ payload
//
// Mode indicator, character count, the bytes, terminator, byte alignment, then
// the alternating 0xEC/0x11 pad. Writes exactly data_cw codewords.

static void bits_put(uint8_t *buf, int *pos, uint32_t value, int n)
{
    int i;
    for (i = n - 1; i >= 0; i--) {
        int b = *pos;
        uint8_t m = (uint8_t)(0x80u >> (b & 7));
        if ((value >> i) & 1) buf[b >> 3] |= m; else buf[b >> 3] &= (uint8_t)~m;
        (*pos)++;
    }
}

static void encode_payload(eos_qr_t *qr, const uint8_t *data, int len)
{
    const int cap_bits = qr->data_cw * 8;
    int pos = 0, i;
    uint8_t pad = PAD_A;

    memset(qr->codewords, 0, sizeof qr->codewords);

    bits_put(qr->codewords, &pos, MODE_BYTE, MODE_BITS);
    bits_put(qr->codewords, &pos, (uint32_t)len, COUNT_BITS);
    for (i = 0; i < len; i++) bits_put(qr->codewords, &pos, data[i], 8);

    // Terminator: up to four zero bits, fewer if the capacity runs out first.
    for (i = 0; i < 4 && pos < cap_bits; i++) bits_put(qr->codewords, &pos, 0, 1);
    // Zero-fill to the next codeword boundary.
    while (pos % 8) bits_put(qr->codewords, &pos, 0, 1);
    // Then the fixed pad pattern for the rest of the data capacity.
    while (pos < cap_bits) {
        bits_put(qr->codewords, &pos, pad, 8);
        pad = (pad == PAD_A) ? PAD_B : PAD_A;
    }
}

// Split the data codewords into blocks, append each block's ECC, and
// interleave. For every version this file supports `blocks` is 1, so the walk
// below runs one short block and the interleave degenerates to a copy — but it
// is written as the general two-group algorithm so the table stays the only
// thing that has to change for versions 5 and up.
static void append_ecc(eos_qr_t *qr)
{
    uint8_t gen[EOS_QR_MAX_ECC];
    uint8_t ecc[EOS_QR_MAX_ECC];
    uint8_t data[EOS_QR_MAX_CODEWORDS];
    uint8_t out[EOS_QR_MAX_CODEWORDS];
    const int nb    = qr->blocks;
    const int ec    = qr->ecc_cw / nb;      // ECC codewords per block
    const int shortl = qr->data_cw / nb;    // data codewords in a short block
    const int nlong = qr->data_cw % nb;     // this many blocks get one more
    int b, i, k = 0, off = 0;
    uint8_t blk_ecc[EOS_QR_MAX_CODEWORDS];
    uint8_t blk_len[EOS_QR_MAX_CODEWORDS];
    uint8_t blk_off[EOS_QR_MAX_CODEWORDS];

    memcpy(data, qr->codewords, (size_t)qr->data_cw);
    rs_generator(gen, ec);

    for (b = 0; b < nb; b++) {
        int len = shortl + (b >= nb - nlong ? 1 : 0);
        blk_off[b] = (uint8_t)off;
        blk_len[b] = (uint8_t)len;
        rs_remainder(data + off, len, gen, ec, ecc);
        memcpy(blk_ecc + (size_t)b * (size_t)ec, ecc, (size_t)ec);
        off += len;
    }

    // Data codewords, column by column across the blocks.
    for (i = 0; i <= shortl; i++)
        for (b = 0; b < nb; b++)
            if (i < blk_len[b]) out[k++] = data[blk_off[b] + i];

    // Then the ECC codewords, same walk. Every block has the same ECC length.
    for (i = 0; i < ec; i++)
        for (b = 0; b < nb; b++)
            out[k++] = blk_ecc[(size_t)b * (size_t)ec + (size_t)i];

    memcpy(qr->codewords, out, (size_t)k);
}

// ---------------------------------------------------------- function modules

static void mark(eos_qr_t *qr, int x, int y, bool dark)
{
    if (x < 0 || y < 0 || x >= qr->size || y >= qr->size) return;
    bm_set(qr->modules, x, y, dark);
    bm_set(qr->reserved, x, y, true);
}

// A finder pattern plus its separator: the 7x7 pattern with a light ring
// around it, clipped at the symbol edge.
static void draw_finder(eos_qr_t *qr, int ox, int oy)
{
    int dx, dy;
    for (dy = -1; dy <= 7; dy++) {
        for (dx = -1; dx <= 7; dx++) {
            bool dark = (dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6) &&
                        (dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
                         (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4));
            mark(qr, ox + dx, oy + dy, dark);
        }
    }
}

// The 5x5 alignment pattern: dark ring, light ring, dark centre.
static void draw_alignment(eos_qr_t *qr, int cx, int cy)
{
    int dx, dy;
    for (dy = -2; dy <= 2; dy++)
        for (dx = -2; dx <= 2; dx++) {
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            int d = ax > ay ? ax : ay;
            mark(qr, cx + dx, cy + dy, d != 1);
        }
}

static void draw_function_patterns(eos_qr_t *qr)
{
    const int n = qr->size;
    int i;

    memset(qr->modules, 0, sizeof qr->modules);
    memset(qr->reserved, 0, sizeof qr->reserved);

    draw_finder(qr, 0, 0);
    draw_finder(qr, n - 7, 0);
    draw_finder(qr, 0, n - 7);

    // Timing patterns. Row 6 and column 6, dark on even coordinates. The ends
    // fall inside the finder separators, which mark() has already reserved;
    // rewriting them with the same values is harmless and keeps the loop plain.
    for (i = 8; i < n - 8; i++) {
        mark(qr, i, 6, (i % 2) == 0);
        mark(qr, 6, i, (i % 2) == 0);
    }

    // Versions 2..4 carry exactly one alignment pattern. The other three
    // centre combinations collide with the finders and are omitted by the
    // standard, so there is nothing to skip here.
    if (VER[qr->version].align) {
        int c = VER[qr->version].align;
        draw_alignment(qr, c, c);
    }

    // Format information areas. The bits go in per mask later; reserving them
    // now is what keeps the data walk and the mask off them.
    for (i = 0; i <= 8; i++) {
        if (i != 6) { mark(qr, 8, i, false); mark(qr, i, 8, false); }
    }
    for (i = 0; i < 8; i++) {
        mark(qr, n - 1 - i, 8, false);
        mark(qr, 8, n - 1 - i, false);
    }

    // The one module that is always dark, at (8, 4 * version + 9).
    mark(qr, 8, n - 8, true);
}

// ------------------------------------------------------------- format info

static void draw_format(eos_qr_t *qr, int mask)
{
    const int n = qr->size;
    uint32_t data = (uint32_t)((FMT_ECL_L << 3) | mask);
    uint32_t rem = data;
    uint32_t bits;
    int i;

    for (i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * FMT_GEN);
    bits = ((data << 10) | (rem & 0x3FF)) ^ FMT_MASK;   // 15 bits, bit 0 is LSB

#define FBIT(k) (((bits) >> (k)) & 1u)
    // First copy, wrapped around the top-left finder.
    for (i = 0; i <= 5; i++) bm_set(qr->modules, 8, i, FBIT(i));
    bm_set(qr->modules, 8, 7, FBIT(6));
    bm_set(qr->modules, 8, 8, FBIT(7));
    bm_set(qr->modules, 7, 8, FBIT(8));
    for (i = 9; i < 15; i++) bm_set(qr->modules, 14 - i, 8, FBIT(i));

    // Second copy, split between the other two finders.
    for (i = 0; i < 8; i++)  bm_set(qr->modules, n - 1 - i, 8, FBIT(i));
    for (i = 8; i < 15; i++) bm_set(qr->modules, 8, n - 15 + i, FBIT(i));
    bm_set(qr->modules, 8, n - 8, true);   // the always-dark module
#undef FBIT
}

// -------------------------------------------------------------- data layout
//
// Two-module-wide columns, right to left, alternating upward and downward,
// skipping the vertical timing column. Anything already reserved is stepped
// over. Bits run out before modules do on every version that has remainder
// bits; those trailing modules stay light, which is what the standard says.

static void draw_data(eos_qr_t *qr)
{
    const int n = qr->size;
    const int nbits = qr->total_cw * 8;
    int right, vert, j, bit = 0;

    for (right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;          // the timing column is not a data column
        for (vert = 0; vert < n; vert++) {
            for (j = 0; j < 2; j++) {
                int x = right - j;
                bool upward = ((right + 1) & 2) == 0;
                int y = upward ? n - 1 - vert : vert;
                if (bm_get(qr->reserved, x, y)) continue;
                if (bit < nbits) {
                    int v = (qr->codewords[bit >> 3] >> (7 - (bit & 7))) & 1;
                    bm_set(qr->modules, x, y, v != 0);
                    bit++;
                }
                // else: a remainder module, already light from the memset.
            }
        }
    }
}

// -------------------------------------------------------------------- masks

static bool mask_bit(int mask, int x, int y)
{
    switch (mask) {
    case 0: return ((y + x) % 2) == 0;
    case 1: return (y % 2) == 0;
    case 2: return (x % 3) == 0;
    case 3: return ((y + x) % 3) == 0;
    case 4: return ((y / 2 + x / 3) % 2) == 0;
    case 5: return ((y * x) % 2 + (y * x) % 3) == 0;
    case 6: return (((y * x) % 2 + (y * x) % 3) % 2) == 0;
    case 7: return (((y + x) % 2 + (y * x) % 3) % 2) == 0;
    default: return false;
    }
}

// XOR is its own inverse, so the same call applies and removes a mask. That is
// what lets all eight be scored in place with no second matrix.
static void apply_mask(eos_qr_t *qr, int mask)
{
    int x, y;
    for (y = 0; y < qr->size; y++)
        for (x = 0; x < qr->size; x++)
            if (!bm_get(qr->reserved, x, y) && mask_bit(mask, x, y))
                bm_xor(qr->modules, x, y);
}

// ------------------------------------------------------------------ penalty
//
// ISO/IEC 18004 table 11, all four rules.
//
// Rule 3 is the one implementations disagree about, so state the reading used
// here: each occurrence of the seven-module 1:1:3:1:1 pattern scores N3 ONCE,
// when it is preceded OR followed by four light modules. Not twice when both
// sides are light, and overlapping occurrences each count. Modules outside the
// symbol read light, because the quiet zone is light and a scanner sees it that
// way, so a pattern flush against an edge always qualifies.
//
// Rule 3 is also the only rule that can change which mask wins, and the mask is
// a print-quality heuristic: every mask produces a symbol that decodes. Two
// widely used encoders disagree with this reading and with each other; that
// costs contrast margin on a bad print, never correctness.

static uint32_t penalty_runs(const eos_qr_t *qr)
{
    const int n = qr->size;
    uint32_t pen = 0;
    int a, b;

    for (a = 0; a < n; a++) {
        int run_h = 1, run_v = 1;
        for (b = 1; b < n; b++) {
            if (bm_get(qr->modules, b, a) == bm_get(qr->modules, b - 1, a)) {
                run_h++;
            } else {
                if (run_h >= 5) pen += PEN_N1 + (uint32_t)(run_h - 5);
                run_h = 1;
            }
            if (bm_get(qr->modules, a, b) == bm_get(qr->modules, a, b - 1)) {
                run_v++;
            } else {
                if (run_v >= 5) pen += PEN_N1 + (uint32_t)(run_v - 5);
                run_v = 1;
            }
        }
        if (run_h >= 5) pen += PEN_N1 + (uint32_t)(run_h - 5);
        if (run_v >= 5) pen += PEN_N1 + (uint32_t)(run_v - 5);
    }
    return pen;
}

static uint32_t penalty_blocks(const eos_qr_t *qr)
{
    const int n = qr->size;
    uint32_t pen = 0;
    int x, y;

    for (y = 0; y + 1 < n; y++)
        for (x = 0; x + 1 < n; x++) {
            bool v = bm_get(qr->modules, x, y);
            if (v == bm_get(qr->modules, x + 1, y) &&
                v == bm_get(qr->modules, x, y + 1) &&
                v == bm_get(qr->modules, x + 1, y + 1))
                pen += PEN_N2;
        }
    return pen;
}

// Reads the module at (x,y) with everything outside the symbol light.
static bool quiet_get(const eos_qr_t *qr, int x, int y)
{
    if (x < 0 || y < 0 || x >= qr->size || y >= qr->size) return false;
    return bm_get(qr->modules, x, y);
}

static uint32_t penalty_finderlike(const eos_qr_t *qr)
{
    const int n = qr->size;
    // 1:1:3:1:1 is dark light dark dark dark light dark.
    static const uint8_t PAT[7] = { 1, 0, 1, 1, 1, 0, 1 };
    uint32_t pen = 0;
    int a, i, k;

    for (a = 0; a < n; a++) {
        for (i = 0; i + 7 <= n; i++) {
            int horiz = 1, vert = 1;
            for (k = 0; k < 7; k++) {
                if (quiet_get(qr, i + k, a) != (PAT[k] != 0)) horiz = 0;
                if (quiet_get(qr, a, i + k) != (PAT[k] != 0)) vert = 0;
            }
            if (!horiz && !vert) continue;

            // Preceded OR followed by four light modules. Off-symbol reads
            // light, so a pattern flush against either edge always qualifies.
            {
                int before_h = 1, after_h = 1, before_v = 1, after_v = 1;
                for (k = 1; k <= 4; k++) {
                    if (quiet_get(qr, i - k, a))     before_h = 0;
                    if (quiet_get(qr, i + 6 + k, a)) after_h  = 0;
                    if (quiet_get(qr, a, i - k))     before_v = 0;
                    if (quiet_get(qr, a, i + 6 + k)) after_v  = 0;
                }
                if (horiz && (before_h || after_h)) pen += PEN_N3;
                if (vert  && (before_v || after_v)) pen += PEN_N3;
            }
        }
    }
    return pen;
}

static uint32_t penalty_balance(const eos_qr_t *qr)
{
    const int n = qr->size;
    int total = n * n, dark = 0, x, y, dev;

    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++)
            if (bm_get(qr->modules, x, y)) dark++;

    // |percent - 50| in whole percent, floored, then in 5% steps. Done in
    // integers: |dark*200 - total*100| / total is 2*|percent-50|.
    dev = dark * 200 - total * 100;
    if (dev < 0) dev = -dev;
    return (uint32_t)((dev / total) / 2 / 5) * PEN_N4;
}

static uint32_t penalty(const eos_qr_t *qr)
{
    return penalty_runs(qr) + penalty_blocks(qr) +
           penalty_finderlike(qr) + penalty_balance(qr);
}

// ------------------------------------------------------------------- encode

eos_qr_err_t eos_qr_encode_version(eos_qr_t *qr, const uint8_t *data,
                                   size_t len, int version)
{
    int m, best = 0;
    uint32_t best_pen = 0;

    if (!qr) return EOS_QR_ERR_NULL;
    if (!data && len) return EOS_QR_ERR_NULL;
    if (len == 0) return EOS_QR_ERR_EMPTY;
    if (version < EOS_QR_MIN_VERSION || version > EOS_QR_MAX_VERSION)
        return EOS_QR_ERR_VERSION;
    if (len > (size_t)VER[version].capacity) return EOS_QR_ERR_TOO_LONG;

    memset(qr, 0, sizeof *qr);
    qr->version  = (uint8_t)version;
    qr->size     = VER[version].size;
    qr->data_cw  = VER[version].data_cw;
    qr->ecc_cw   = VER[version].ecc_cw;
    qr->total_cw = VER[version].total_cw;
    qr->blocks   = VER[version].blocks;
    qr->data_len = (uint8_t)len;

    encode_payload(qr, data, (int)len);
    append_ecc(qr);

    draw_function_patterns(qr);
    draw_data(qr);

    // Every mask is applied, formatted, scored and undone. The scores are kept
    // so a caller — or the test suite — can see the whole comparison and not
    // just the winner.
    for (m = 0; m < EOS_QR_MASKS; m++) {
        apply_mask(qr, m);
        draw_format(qr, m);
        qr->penalty[m] = penalty(qr);
        apply_mask(qr, m);
        if (m == 0 || qr->penalty[m] < best_pen) {
            best_pen = qr->penalty[m];
            best = m;
        }
    }

    qr->mask = (uint8_t)best;
    apply_mask(qr, best);
    draw_format(qr, best);
    return EOS_QR_OK;
}

eos_qr_err_t eos_qr_encode_bytes(eos_qr_t *qr, const uint8_t *data, size_t len)
{
    int v;

    if (!qr) return EOS_QR_ERR_NULL;
    if (!data && len) return EOS_QR_ERR_NULL;
    if (len == 0) return EOS_QR_ERR_EMPTY;

    v = eos_qr_version_for(len);
    if (v == 0) return EOS_QR_ERR_TOO_LONG;
    return eos_qr_encode_version(qr, data, len, v);
}

eos_qr_err_t eos_qr_encode(eos_qr_t *qr, const char *text)
{
    if (!qr || !text) return EOS_QR_ERR_NULL;
    return eos_qr_encode_bytes(qr, (const uint8_t *)text, strlen(text));
}

// ------------------------------------------------------------------- render

size_t eos_qr_render_bytes(const eos_qr_t *qr, int scale, int quiet)
{
    int mod, px, stride;

    if (!qr || qr->size == 0 || scale < 1 || quiet < 0) return 0;
    mod = qr->size + 2 * quiet;
    px = mod * scale;
    stride = (px + 7) / 8;
    return (size_t)stride * (size_t)px;
}

bool eos_qr_render(const eos_qr_t *qr, int scale, int quiet,
                   uint8_t *out, size_t out_len, int *out_w, int *out_h)
{
    size_t need = eos_qr_render_bytes(qr, scale, quiet);
    int px, stride, py;

    if (need == 0 || !out || out_len < need) return false;

    px = (qr->size + 2 * quiet) * scale;
    stride = (px + 7) / 8;
    memset(out, 0, need);

    for (py = 0; py < px; py++) {
        int my = py / scale - quiet;
        uint8_t *row;
        int pxx;
        if (my < 0 || my >= qr->size) continue;      // quiet zone row, all light
        row = out + (size_t)py * (size_t)stride;
        for (pxx = 0; pxx < px; pxx++) {
            int mx = pxx / scale - quiet;
            if (mx < 0 || mx >= qr->size) continue;
            if (bm_get(qr->modules, mx, my))
                row[pxx >> 3] |= (uint8_t)(0x80u >> (pxx & 7));
        }
    }

    if (out_w) *out_w = px;
    if (out_h) *out_h = px;
    return true;
}
