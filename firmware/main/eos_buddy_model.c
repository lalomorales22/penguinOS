// The compiled-in buddy: Pip, the penguin.
//
// A board with no filesystem, or with an empty one, still has to have a face.
// This is that face, linked into the image so it cannot fail to load. A model
// uploaded through the Buddy tab replaces it at runtime; a rejected one falls
// back here.
//
// The shape itself is generated - assets/buddy/make_penguin.py writes both
// assets/buddy/penguin.vox and the eos_pip_data.inc included below, from one
// radius profile, so the file on the card and the one in the image are the
// same penguin and cannot drift.
//
// The one non-obvious constraint, and the reason PIP_SOLID exists at all:
// only the SHELL is stored, because a buried voxel draws nothing at any yaw
// and on this shape that is more than half the model. But eos_vox_finish()
// derives its face masks from the voxels it can see, and a shell has air
// behind every inward face, so it comes back with all of them exposed -
// which would draw a second surface on the inside of the penguin. PIP_SOLID
// is one bit per cell of the solid shape and is the only thing that still
// knows better, so the masks are corrected against it afterwards.

#include "eos_buddy_model.h"

#include <string.h>

#include "eos_pip_data.inc"

// Headroom on purpose. The suite asserts count < cap, so a change to the
// shape that grows the shell fails a check here instead of silently losing
// voxels to eos_vox_set() refusing past the end.
#define POOL_N (PIP_SHELL_N + 16)

static bool pip_solid(int x, int y, int z)
{
    int i;
    if (x < 0 || y < 0 || z < 0 || x >= PIP_W || y >= PIP_D || z >= PIP_H) return false;
    i = (z * PIP_D + y) * PIP_W + x;
    return (PIP_SOLID[i >> 3] >> (i & 7)) & 1u;
}

static uint8_t faces_of(int x, int y, int z)
{
    uint8_t f = 0;
    if (!pip_solid(x + 1, y, z)) f |= EOS_VOX_FACE_XP;
    if (!pip_solid(x - 1, y, z)) f |= EOS_VOX_FACE_XN;
    if (!pip_solid(x, y + 1, z)) f |= EOS_VOX_FACE_YP;
    if (!pip_solid(x, y - 1, z)) f |= EOS_VOX_FACE_YN;
    if (!pip_solid(x, y, z + 1)) f |= EOS_VOX_FACE_ZP;
    if (!pip_solid(x, y, z - 1)) f |= EOS_VOX_FACE_ZN;
    return f;
}

static eos_voxel_t     s_pool[POOL_N];
static eos_vox_model_t s_model;
static bool            s_built;

static void build(void)
{
    int i;

    eos_vox_model_init(&s_model, s_pool, POOL_N, PIP_W, PIP_D, PIP_H, &PIP_PAL);

    for (i = 0; i < PIP_SHELL_N; i++)
        eos_vox_set(&s_model,
                    PIP_SHELL[i][0], PIP_SHELL[i][1], PIP_SHELL[i][2], PIP_SHELL[i][3]);

    // Sorts into (z,y,x) order. It removes nothing: the shell has no buried
    // voxel left to remove.
    eos_vox_finish(&s_model);

    // And then the masks are corrected against the solid shape. See the note
    // at the top of this file for why finish() cannot get these right alone.
    for (i = 0; i < (int)s_model.count; i++)
        s_model.v[i].faces = faces_of(s_model.v[i].x, s_model.v[i].y, s_model.v[i].z);

    s_built = true;
}

eos_vox_model_t *eos_buddy_model_default(void)
{
    if (!s_built) build();
    return &s_model;
}

const eos_vox_pal_t *eos_buddy_model_default_palette(void)
{
    return &PIP_PAL;
}

uint16_t eos_buddy_model_default_count(void)
{
    if (!s_built) build();
    return s_model.count;
}

void eos_buddy_model_default_cfg(eos_buddy_cfg_t *cfg)
{
    if (!cfg) return;
    eos_buddy_default_cfg(cfg);
    // The two the state machine cannot know, because they are indices into
    // THIS model's palette. PIP_CI_LID holds the same RGB as the white face
    // around the eye, so a blink is a palette swap and not geometry.
    cfg->eye_ci      = PIP_CI_EYE;
    cfg->eye_shut_ci = PIP_CI_LID;
}
