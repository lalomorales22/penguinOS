// eos_vox — MagicaVoxel .vox reader for the penguinOS avatar.
//
// Reads the RIFF-ish chunk stream (MAIN / SIZE / XYZI / RGBA) into a caller
// supplied voxel array. No allocation, no seeking, no scene graph: the first
// SIZE+XYZI pair wins and every other chunk is stepped over, because the
// buddy is one model and nothing on a 4MB board wants a scene.
//
// The file arrives off a microSD card, so every offset and length inside it
// is hostile until proven otherwise. Sizes are checked against what actually
// remains in the buffer and a chunk that overruns is a hard error, never a
// clamp — a clamp is how you turn a bad file into a heap corruption.
//
// The one non-obvious thing here is eos_vox_finish(): it sorts the voxels
// into (z,y,x) order, records which of the six faces of each voxel is not
// buried against a neighbour, and then DELETES every voxel whose faces are
// all buried. On a solid 11x9x15 buddy that is over half the voxels, and it
// is the single biggest win in the whole renderer because the cost of a
// frame is faces drawn, not voxels stored.

#ifndef EOS_VOX_H
#define EOS_VOX_H

#include <stdint.h>
#include <stdbool.h>

// Caps. Anything larger is rejected rather than truncated. 32^3 is already
// far past what a 320x240 panel can resolve at a readable voxel size.
#define EOS_VOX_MAX_DIM     32
#define EOS_VOX_MAX_VOXELS  4096

// Exposed-face bits, in MagicaVoxel axes: x right, y depth, z up.
#define EOS_VOX_FACE_XP 0x01
#define EOS_VOX_FACE_XN 0x02
#define EOS_VOX_FACE_YP 0x04
#define EOS_VOX_FACE_YN 0x08
#define EOS_VOX_FACE_ZP 0x10
#define EOS_VOX_FACE_ZN 0x20
#define EOS_VOX_FACE_ALL 0x3F

typedef struct {
    uint8_t x, y, z;   // 0 .. dim-1
    uint8_t ci;        // palette index 1..255; 0 is "empty" and never stored
    uint8_t faces;     // EOS_VOX_FACE_* still exposed; 0 only before finish()
} eos_voxel_t;

// 768 bytes. Separate from the model so a tier 0 build that renders through
// an index shade table can pass NULL and never spend the RAM.
typedef struct { uint8_t rgb[256][3]; } eos_vox_pal_t;

typedef struct {
    eos_voxel_t         *v;      // caller-provided pool
    uint16_t             cap;
    uint16_t             count;
    uint8_t              sx, sy, sz;
    bool                 sorted; // v is in ascending (z,y,x) key order
    bool                 culled; // finish() has run
    const eos_vox_pal_t *pal;    // may be NULL
} eos_vox_model_t;

typedef enum {
    EOS_VOX_OK = 0,
    EOS_VOX_ERR_ARG,        // NULL pointer or zero-length pool
    EOS_VOX_ERR_MAGIC,      // not "VOX "
    EOS_VOX_ERR_VERSION,    // version field we refuse to guess at
    EOS_VOX_ERR_TRUNCATED,  // a read would have run off the end of the buffer
    EOS_VOX_ERR_CHUNK,      // a chunk header is self-contradictory
    EOS_VOX_ERR_DIM,        // SIZE is zero or larger than EOS_VOX_MAX_DIM
    EOS_VOX_ERR_COUNT,      // XYZI declares more voxels than the cap allows
    EOS_VOX_ERR_POOL,       // more voxels than the caller's pool holds
    EOS_VOX_ERR_NO_MODEL,   // no SIZE+XYZI pair anywhere in the file
    EOS_VOX_ERR_RANGE       // a voxel sits outside the declared SIZE
} eos_vox_err_t;

const char *eos_vox_strerror(eos_vox_err_t e);

// Fills `p` with the stock MagicaVoxel palette. Used automatically when a
// file carries no RGBA chunk, which most hand-made models do not.
void eos_vox_default_palette(eos_vox_pal_t *p);

// Parses `data`. `pool` receives the voxels and `out` is filled to point at
// it. `pal` may be NULL to discard colour. On success the model has been run
// through eos_vox_finish() already.
eos_vox_err_t eos_vox_parse(const uint8_t *data, uint32_t len,
                            eos_voxel_t *pool, uint16_t pool_cap,
                            eos_vox_pal_t *pal, eos_vox_model_t *out);

// Building a model in code, for compiled-in buddies and for tests.
void eos_vox_model_init(eos_vox_model_t *m, eos_voxel_t *pool, uint16_t cap,
                        uint8_t sx, uint8_t sy, uint8_t sz,
                        const eos_vox_pal_t *pal);
bool eos_vox_set(eos_vox_model_t *m, uint8_t x, uint8_t y, uint8_t z, uint8_t ci);

// Sorts, drops exact duplicates, computes face masks, removes fully buried
// voxels. Returns how many voxels it removed. Idempotent.
uint16_t eos_vox_finish(eos_vox_model_t *m);

// Binary search on the (z,y,x) key. Only meaningful while m->sorted is true;
// eos_buddy_render() reorders the pool by depth and clears that flag, so ask
// before you render, or call eos_vox_finish() again to restore the order.
bool eos_vox_occupied(const eos_vox_model_t *m, int x, int y, int z);

#endif
