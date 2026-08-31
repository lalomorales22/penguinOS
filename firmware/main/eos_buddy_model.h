// eos_buddy_model — the buddy the board wears when there is no buddy.vox on
// flash, which is every board that has never been given one.
//
// It exists so the avatar tile is never empty. /int is 960 KB of blank
// LittleFS on a fresh board and the Buddy tab's whole first-run flow is "there
// is no model yet, build one" — a desktop that showed a hole until somebody
// did that would read as a broken window rather than as an empty card. So the
// shape is compiled in, and a real upload simply replaces it.
//
// The shape is the one kernel/avatar/test/test_vox.c has been building in code
// and rendering to ASCII since the component was written: 11 x 7 x 15, feet
// apart, bevelled torso, a head wider than the body, and eyes on the -y face,
// which is the face the camera is looking at when yaw is home. Same buddy,
// finally on glass.
//
// The one non-obvious constraint: this file writes the voxels' FACE MASKS
// itself, after eos_vox_finish() has run. It stores only the shell — the pool
// is a third of the size that way — and a shell handed to eos_vox_finish()
// has no interior left to hide the inward faces behind, so finish() would mark
// every one of them exposed and the renderer would draw a second, invisible
// surface on the inside. The masks are recomputed from the SOLID shape the
// voxels were sampled from, which is the only place that information still
// exists once the interior is gone.

#ifndef EOS_BUDDY_MODEL_H
#define EOS_BUDDY_MODEL_H

#include "eos_buddy.h"
#include "eos_vox.h"

// The compiled-in buddy, built on the first call and cached. Never NULL.
//
// Not const: eos_buddy_render() reorders the voxel array in place, which is
// how the painter sort costs no RAM. One model belongs to one buddy.
eos_vox_model_t *eos_buddy_model_default(void);

// Its palette. Lives in flash, not in RAM: nothing ever writes to it, and the
// 768 bytes an eos_vox_pal_t costs are worth more as .rodata than as .bss.
const eos_vox_pal_t *eos_buddy_model_default_palette(void);

// eos_buddy_default_cfg() plus the three fields that are properties of THIS
// model and not of the state machine: which palette index the eyes are, which
// one closes over them when it blinks, and how far round it stands.
void eos_buddy_model_default_cfg(eos_buddy_cfg_t *cfg);

// How many voxels the built model kept. For the boot log and for the tests.
uint16_t eos_buddy_model_default_count(void);

#endif // EOS_BUDDY_MODEL_H
