#include "eos_vox.h"
#include <string.h>

// ------------------------------------------------------------ default palette
//
// MagicaVoxel's stock palette is not an arbitrary table: indices 1..215 are a
// 6x6x6 colour cube over the levels {ff,cc,99,66,33,00} with blue varying
// fastest, minus the final all-black entry (index 0 already means empty).
// Indices 216..255 are four 10-step ramps: red, green, blue, grey. Generating
// it costs 30 lines instead of 1KB of flash for a literal table, and there is
// no transcription to get wrong.

void eos_vox_default_palette(eos_vox_pal_t *p)
{
    static const uint8_t L[6]  = { 0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00 };
    static const uint8_t R[10] = { 0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11 };
    if (!p) return;

    p->rgb[0][0] = p->rgb[0][1] = p->rgb[0][2] = 0;

    for (int ri = 0; ri < 6; ri++)
        for (int gi = 0; gi < 6; gi++)
            for (int bi = 0; bi < 6; bi++) {
                int idx = 1 + ri * 36 + gi * 6 + bi;
                if (idx > 215) continue;              // the dropped black
                p->rgb[idx][0] = L[ri];
                p->rgb[idx][1] = L[gi];
                p->rgb[idx][2] = L[bi];
            }

    for (int i = 0; i < 10; i++) {
        p->rgb[216 + i][0] = R[i]; p->rgb[216 + i][1] = 0;    p->rgb[216 + i][2] = 0;
        p->rgb[226 + i][0] = 0;    p->rgb[226 + i][1] = R[i]; p->rgb[226 + i][2] = 0;
        p->rgb[236 + i][0] = 0;    p->rgb[236 + i][1] = 0;    p->rgb[236 + i][2] = R[i];
        p->rgb[246 + i][0] = R[i]; p->rgb[246 + i][1] = R[i]; p->rgb[246 + i][2] = R[i];
    }
}

const char *eos_vox_strerror(eos_vox_err_t e)
{
    switch (e) {
    case EOS_VOX_OK:            return "ok";
    case EOS_VOX_ERR_ARG:       return "bad argument";
    case EOS_VOX_ERR_MAGIC:     return "not a .vox file";
    case EOS_VOX_ERR_VERSION:   return "unsupported .vox version";
    case EOS_VOX_ERR_TRUNCATED: return "file ends inside a chunk";
    case EOS_VOX_ERR_CHUNK:     return "chunk length overruns the file";
    case EOS_VOX_ERR_DIM:       return "model dimensions out of range";
    case EOS_VOX_ERR_COUNT:     return "too many voxels in the model";
    case EOS_VOX_ERR_POOL:      return "voxel pool too small";
    case EOS_VOX_ERR_NO_MODEL:  return "no SIZE+XYZI pair in the file";
    case EOS_VOX_ERR_RANGE:     return "voxel outside the declared size";
    }
    return "unknown error";
}

// ------------------------------------------------------------------ ordering

static uint32_t vkey(const eos_vox_model_t *m, uint32_t x, uint32_t y, uint32_t z)
{
    return (z * (uint32_t)m->sy + y) * (uint32_t)m->sx + x;
}

static uint32_t voxkey(const eos_vox_model_t *m, const eos_voxel_t *v)
{
    return vkey(m, v->x, v->y, v->z);
}

// Shell sort: no recursion, no scratch, and the gap sequence keeps it near
// n*log(n) on the few thousand voxels we allow. Runs once, at load.
static void sort_spatial(eos_vox_model_t *m)
{
    static const uint16_t GAPS[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
    eos_voxel_t *v = m->v;
    int n = (int)m->count;

    for (unsigned g = 0; g < sizeof(GAPS) / sizeof(GAPS[0]); g++) {
        int gap = GAPS[g];
        if (gap >= n) continue;
        for (int i = gap; i < n; i++) {
            eos_voxel_t t = v[i];
            uint32_t tk = voxkey(m, &t);
            int j = i;
            while (j >= gap && voxkey(m, &v[j - gap]) > tk) { v[j] = v[j - gap]; j -= gap; }
            v[j] = t;
        }
    }
    m->sorted = true;
}

bool eos_vox_occupied(const eos_vox_model_t *m, int x, int y, int z)
{
    if (!m || !m->sorted || m->count == 0) return false;
    if (x < 0 || y < 0 || z < 0) return false;
    if (x >= m->sx || y >= m->sy || z >= m->sz) return false;

    uint32_t want = vkey(m, (uint32_t)x, (uint32_t)y, (uint32_t)z);
    int lo = 0, hi = (int)m->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t k = voxkey(m, &m->v[mid]);
        if (k == want) return true;
        if (k < want) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

uint16_t eos_vox_finish(eos_vox_model_t *m)
{
    if (!m || !m->v || m->count == 0) return 0;

    // Already culled: only put the pool back in (z,y,x) order, which is what
    // a caller wants after a render has reshuffled it into depth order. Doing
    // the face pass again would be WRONG — the buried voxels are gone now, so
    // their neighbours would find empty space and light up faces that point
    // into the middle of a solid model.
    if (m->culled) { sort_spatial(m); return 0; }

    sort_spatial(m);

    // Exact duplicates are legal in the wild (a model edited over itself) and
    // would each draw their own faces. Drop them while the order makes them
    // adjacent.
    uint16_t w = 1;
    for (uint16_t i = 1; i < m->count; i++)
        if (voxkey(m, &m->v[i]) != voxkey(m, &m->v[w - 1])) m->v[w++] = m->v[i];
    m->count = w;

    // Face masks first, against the FULL set. Doing this before any removal
    // is what makes the removal safe: a voxel that loses every face was
    // already invisible, so nothing that survives gains a face by its going.
    for (uint16_t i = 0; i < m->count; i++) {
        eos_voxel_t *v = &m->v[i];
        uint8_t f = 0;
        if (!eos_vox_occupied(m, v->x + 1, v->y, v->z)) f |= EOS_VOX_FACE_XP;
        if (!eos_vox_occupied(m, v->x - 1, v->y, v->z)) f |= EOS_VOX_FACE_XN;
        if (!eos_vox_occupied(m, v->x, v->y + 1, v->z)) f |= EOS_VOX_FACE_YP;
        if (!eos_vox_occupied(m, v->x, v->y - 1, v->z)) f |= EOS_VOX_FACE_YN;
        if (!eos_vox_occupied(m, v->x, v->y, v->z + 1)) f |= EOS_VOX_FACE_ZP;
        if (!eos_vox_occupied(m, v->x, v->y, v->z - 1)) f |= EOS_VOX_FACE_ZN;
        v->faces = f;
    }

    uint16_t before = m->count;
    w = 0;
    for (uint16_t i = 0; i < m->count; i++)
        if (m->v[i].faces) m->v[w++] = m->v[i];
    m->count = w;
    m->culled = true;
    m->sorted = true;      // compaction preserves the ordering
    return (uint16_t)(before - w);
}

// -------------------------------------------------------------- construction

void eos_vox_model_init(eos_vox_model_t *m, eos_voxel_t *pool, uint16_t cap,
                        uint8_t sx, uint8_t sy, uint8_t sz,
                        const eos_vox_pal_t *pal)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->v   = pool;
    m->cap = pool ? cap : 0;
    m->sx  = sx;
    m->sy  = sy;
    m->sz  = sz;
    m->pal = pal;
}

bool eos_vox_set(eos_vox_model_t *m, uint8_t x, uint8_t y, uint8_t z, uint8_t ci)
{
    if (!m || !m->v || m->count >= m->cap) return false;
    if (ci == 0) return false;
    if (x >= m->sx || y >= m->sy || z >= m->sz) return false;

    eos_voxel_t *v = &m->v[m->count++];
    v->x = x; v->y = y; v->z = z; v->ci = ci; v->faces = EOS_VOX_FACE_ALL;
    m->sorted = false;
    m->culled = false;
    return true;
}

// ------------------------------------------------------------------- parsing
//
// Every helper below takes the remaining length and refuses to read past it.
// There is no seek and no back-reference in this format, so a single forward
// pass with a bounds check on each chunk header is the whole safety story.

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

eos_vox_err_t eos_vox_parse(const uint8_t *data, uint32_t len,
                            eos_voxel_t *pool, uint16_t pool_cap,
                            eos_vox_pal_t *pal, eos_vox_model_t *out)
{
    if (!data || !pool || !out || pool_cap == 0) return EOS_VOX_ERR_ARG;

    memset(out, 0, sizeof(*out));
    out->v   = pool;
    out->cap = pool_cap;
    out->pal = pal;

    if (len < 8) return EOS_VOX_ERR_TRUNCATED;
    if (memcmp(data, "VOX ", 4) != 0) return EOS_VOX_ERR_MAGIC;

    uint32_t ver = rd32(data + 4);
    if (ver < 100 || ver > 250) return EOS_VOX_ERR_VERSION;

    // The outermost chunk must be MAIN, and everything we want is one of its
    // direct children. Anything else (PACK, nTRN, nGRP, nSHP, MATL, LAYR) is
    // stepped over whole, children included.
    if (len < 8 + 12) return EOS_VOX_ERR_TRUNCATED;
    if (memcmp(data + 8, "MAIN", 4) != 0) return EOS_VOX_ERR_CHUNK;

    uint32_t main_content  = rd32(data + 12);
    uint32_t main_children = rd32(data + 16);
    uint32_t body = 8 + 12;
    if (main_content > len - body) return EOS_VOX_ERR_CHUNK;
    if (main_children > len - body - main_content) return EOS_VOX_ERR_CHUNK;

    uint32_t pos = body + main_content;
    uint32_t end = pos + main_children;

    bool have_size = false, have_xyzi = false, have_rgba = false;

    while (pos + 12 <= end) {
        const uint8_t *id = data + pos;
        uint32_t clen = rd32(data + pos + 4);
        uint32_t klen = rd32(data + pos + 8);
        uint32_t cbody = pos + 12;

        if (clen > end - cbody)                return EOS_VOX_ERR_CHUNK;
        if (klen > end - cbody - clen)         return EOS_VOX_ERR_CHUNK;
        const uint8_t *c = data + cbody;

        if (!have_xyzi && memcmp(id, "SIZE", 4) == 0) {
            if (clen < 12) return EOS_VOX_ERR_TRUNCATED;
            uint32_t sx = rd32(c), sy = rd32(c + 4), sz = rd32(c + 8);
            if (sx == 0 || sy == 0 || sz == 0)  return EOS_VOX_ERR_DIM;
            if (sx > EOS_VOX_MAX_DIM || sy > EOS_VOX_MAX_DIM || sz > EOS_VOX_MAX_DIM)
                return EOS_VOX_ERR_DIM;
            out->sx = (uint8_t)sx;
            out->sy = (uint8_t)sy;
            out->sz = (uint8_t)sz;
            have_size = true;

        } else if (!have_xyzi && memcmp(id, "XYZI", 4) == 0) {
            if (!have_size) return EOS_VOX_ERR_NO_MODEL;
            if (clen < 4)   return EOS_VOX_ERR_TRUNCATED;
            uint32_t n = rd32(c);
            if (n > EOS_VOX_MAX_VOXELS)         return EOS_VOX_ERR_COUNT;
            if (n > (clen - 4) / 4)             return EOS_VOX_ERR_TRUNCATED;
            if (n > pool_cap)                   return EOS_VOX_ERR_POOL;

            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *q = c + 4 + i * 4;
                if (q[0] >= out->sx || q[1] >= out->sy || q[2] >= out->sz)
                    return EOS_VOX_ERR_RANGE;
                if (q[3] == 0) continue;        // index 0 is empty by definition
                eos_voxel_t *v = &pool[out->count++];
                v->x = q[0]; v->y = q[1]; v->z = q[2];
                v->ci = q[3]; v->faces = EOS_VOX_FACE_ALL;
            }
            have_xyzi = true;

        } else if (pal && !have_rgba && memcmp(id, "RGBA", 4) == 0) {
            if (clen < 1024) return EOS_VOX_ERR_TRUNCATED;
            // File entry j is palette index j+1; the last one has no index.
            pal->rgb[0][0] = pal->rgb[0][1] = pal->rgb[0][2] = 0;
            for (int j = 0; j < 255; j++) {
                pal->rgb[j + 1][0] = c[j * 4 + 0];
                pal->rgb[j + 1][1] = c[j * 4 + 1];
                pal->rgb[j + 1][2] = c[j * 4 + 2];
            }
            have_rgba = true;
        }

        pos = cbody + clen + klen;
    }

    if (!have_size || !have_xyzi) return EOS_VOX_ERR_NO_MODEL;
    if (pal && !have_rgba) eos_vox_default_palette(pal);

    eos_vox_finish(out);
    return EOS_VOX_OK;
}
