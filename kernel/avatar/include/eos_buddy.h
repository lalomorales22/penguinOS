// eos_buddy — the voxel avatar: software cube rasteriser plus the little
// personality that drives it.
//
// The buddy is the face of the OS, so it has to look good on a panel whose
// board has about 20KB of free heap. That rules out a 3D pipeline. What is
// here instead: the model is blocky and axis aligned, so each voxel projects
// to at most three visible parallelograms (a top and two sides), the camera
// sits at one of 32 fixed yaw steps so sin/cos come out of a 64-byte table,
// and occlusion is a painter sort along the view axis with no depth buffer.
// Three brightness levels for the three face orientations is what makes it
// read as 3D; it is the cheapest lighting model that works.
//
// The non-obvious constraint: eos_buddy_render() REORDERS the model's voxel
// array in place, far to near. That is deliberate — it is how the painter
// sort costs zero extra RAM, and because the yaw only moves a step or two
// between frames the insertion sort is near linear on every frame after the
// first. The price is that one model belongs to exactly one buddy, and that
// eos_vox_occupied() stops answering until eos_vox_finish() runs again.

#ifndef EOS_BUDDY_H
#define EOS_BUDDY_H

#include <stdint.h>
#include <stdbool.h>
#include "eos_vox.h"

// Yaw is quantised to this many steps around the vertical axis. A power of
// two so the wrap is a mask, and 32 is fine enough that a turn reads as
// smooth at 20fps.
#define EOS_BUDDY_YAW_STEPS 32

typedef enum {
    EOS_BUDDY_IDLE = 0,
    EOS_BUDDY_THINKING,
    EOS_BUDDY_TALKING,
    EOS_BUDDY_LISTENING,
    EOS_BUDDY_SLEEPING,
    EOS_BUDDY_HAPPY,
    EOS_BUDDY_CONFUSED,
    EOS_BUDDY_STATE_COUNT
} eos_buddy_state_t;

// The megabrain request lifecycle, which is the only reason the state machine
// exists. Feed these in from the HTTP client and the buddy perks up and turns
// to face the user the moment the first chunk of a reply lands.
typedef enum {
    EOS_BUDDY_EV_USER_TYPING = 0, // -> LISTENING
    EOS_BUDDY_EV_REQUEST_SENT,    // -> THINKING, head turns away
    EOS_BUDDY_EV_STREAM_FIRST,    // -> TALKING, snaps back to face the user
    EOS_BUDDY_EV_STREAM_CHUNK,    // stays TALKING, tops up the mouth energy
    EOS_BUDDY_EV_STREAM_DONE,     // -> HAPPY, then falls back to IDLE
    EOS_BUDDY_EV_ERROR,           // -> CONFUSED, then falls back to IDLE
    EOS_BUDDY_EV_IDLE_TIMEOUT     // -> SLEEPING
} eos_buddy_event_t;

typedef enum {
    EOS_BUDDY_PIX_I8 = 0,         // 8-bit indexed, tier 0's compositor format
    EOS_BUDDY_PIX_RGB565 = 1
} eos_buddy_pix_t;

typedef struct {
    // Caller-provided, w*h of the chosen format. An RGB565 buffer is written
    // as uint16_t, so it must be 2-byte aligned; a byte buffer offset by one
    // is undefined behaviour on the host and a load/store exception on xtensa.
    void    *pixels;
    uint16_t w, h;
    uint8_t  fmt;                 // eos_buddy_pix_t
    bool     clear;               // fill with the background first
    uint8_t  bg_i8;
    uint16_t bg_565;

    // Painter audit. Leave audit_depth NULL on hardware; it costs 4 bytes a
    // pixel and exists so the host test can prove that no pixel is ever
    // overwritten by something further away.
    int32_t *audit_depth;
    uint32_t audit_violations;
    uint32_t audit_pixels;
} eos_buddy_target_t;

typedef struct {
    uint8_t  home_yaw;            // the step the buddy returns to, 0..31
    uint8_t  eye_ci;              // palette index of the open eyes, 0 = none
    uint8_t  eye_shut_ci;         // index drawn in its place while blinking
    uint8_t  shade[3];            // Q8 brightness for top / y-face / x-face

    // I8 targets cannot shade by arithmetic, so they need a 3*256 remap of
    // (face level, model index) -> display index. NULL means no shading, and
    // the buddy goes flat. eos_buddy_build_shade_lut() makes one; run it on
    // the host and paste the result as a const array to spend no RAM at all.
    const uint8_t *shade_lut;

    uint16_t scale_q8;            // voxel edge in Q8 pixels; 0 fits the target
    uint16_t flat_565;            // colour used for RGB565 when there is no palette
    uint32_t idle_sleep_ms;       // IDLE this long -> SLEEPING; 0 disables
    uint32_t seed;                // blink jitter; fixed so tests repeat
} eos_buddy_cfg_t;

typedef struct {
    eos_vox_model_t *model;
    eos_buddy_cfg_t  cfg;

    uint8_t  state, prev_state;
    uint32_t state_ms, life_ms, since_chunk_ms;
    uint32_t rng;

    int32_t  yaw_q8, yaw_target_q8;      // Q8 yaw steps, wraps at 32<<8
    uint16_t bob_phase, sway_phase, shear_phase, squash_phase;
    int16_t  bob_q8;                     // vertical offset, Q8 voxel units
    int16_t  shear_q8;                   // lean at the top, Q8 voxel units
    int16_t  squash_q8;                  // +squat / -stretch, Q8
    int16_t  energy_q8;                  // how hard the reply is streaming

    uint16_t blink_in_ms, blink_left_ms;
    uint8_t  blink_again;
    uint16_t pop_ms;

    uint16_t fit_w, fit_h, fit_scale_q8; // cached auto-fit
    uint32_t faces_drawn;                // last frame, for perf work
} eos_buddy_t;

void eos_buddy_default_cfg(eos_buddy_cfg_t *cfg);
void eos_buddy_init(eos_buddy_t *b, eos_vox_model_t *m, const eos_buddy_cfg_t *cfg);

void eos_buddy_set_state(eos_buddy_t *b, eos_buddy_state_t s);
void eos_buddy_event(eos_buddy_t *b, eos_buddy_event_t ev);
eos_buddy_state_t eos_buddy_state(const eos_buddy_t *b);
const char *eos_buddy_state_name(eos_buddy_state_t s);

// Advances the animation. Everything procedural — bob, yaw ease, blink,
// squash — happens here so render() is pure drawing.
void eos_buddy_tick(eos_buddy_t *b, uint32_t dt_ms);

bool eos_buddy_blinking(const eos_buddy_t *b);
uint8_t eos_buddy_yaw_step(const eos_buddy_t *b);

// Draws the buddy into `t`. Returns the number of faces drawn, or -1 on a
// bad argument. Never writes outside t->pixels.
int eos_buddy_render(eos_buddy_t *b, eos_buddy_target_t *t);

// Builds the (level, index) -> index table an I8 target needs, by finding the
// nearest colour in the display palette to each model colour scaled by each
// of the three face brightnesses. `disp_rgb` is 3*disp_n bytes.
//
// Pass disp_n = eos_display_caps_t.palette_len (255), NOT 256. Slot 255 is
// EOS_COLOR_NONE, the display HAL's transparency sentinel: a face that shades
// onto it does not draw, and the buddy comes out with holes in exactly the
// brightest places. This function has no way to know that on its own, because
// it deliberately takes a plain byte array rather than depending on the HAL.
void eos_buddy_build_shade_lut(const eos_vox_pal_t *vox_pal,
                               const uint8_t *disp_rgb, int disp_n,
                               const uint8_t shade[3], uint8_t out[768]);

#endif
