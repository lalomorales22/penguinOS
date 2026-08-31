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

// sin of the camera's elevation, Q12. Exported because anything that wants to
// move the buddy along the floor rather than across the glass has to squash
// its vertical travel by exactly this, and a second copy of the number is a
// second thing to keep in step with the projection.
#define EOS_BUDDY_SIN_PHI 2048

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

    // How much of his own size he gives up so that the box is a stage rather
    // than a frame, Q8. 0 is exactly what this file has always done: he is
    // fitted as large as the target allows and there is nowhere to walk. 51
    // (a fifth) drops an 80x80 tile's buddy to three quarters and hands the
    // freed pixels to eos_buddy_move_to(). It sits here rather than in the
    // walker because the scale and the stage are the same division of the
    // target, and only one of them can be decided first.
    uint8_t  roam_q8;

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

    int32_t  face_off_q8;                // extra yaw a walker asks for, Q8 steps

    int32_t  walk_x_q8, walk_y_q8;       // where he stands, Q8 px from centre
    int32_t  stage_x_q8, stage_y_q8;     // half-extents he is clamped inside
    int16_t  gait_lean_q8;               // waddle roll, added to shear_q8
    int16_t  gait_rise_q8;               // waddle lift, added to bob_q8

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

// ------------------------------------------------------------------- motion
//
// Position is an offset from the CENTRE of the render target, in Q8 pixels,
// and it lives inside the target on purpose. The buddy's box is already the
// unit of damage on the panel — it is rendered once into its own buffer and
// blitted at a fixed spot — so a step repaints exactly the rectangle a bob
// repaints and costs the compositor nothing extra. Moving the blit instead
// would dirty two boxes a frame and drag the whole tile behind it.
//
// The stage is a half-extent: the legal box is -stage_x..+stage_x by
// -stage_y..+stage_y, computed from the target size, the model's footprint
// and cfg.roam_q8, with an eighth of his own size held back on each axis for
// the bob and the lean. It is zero until the first render or the first
// eos_buddy_fit(), because until then nothing knows how big the box is.
//
// Vertical travel should be foreshortened by EOS_BUDDY_SIN_PHI by whatever is
// driving this, so that up-screen reads as further away rather than airborne.
//
// One buddy is one stage. Every render recomputes the stage for the target it
// was handed and clamps the position into it, so rendering the same buddy into
// two different sized targets in the same frame lets the smaller one pull him
// toward the middle. That is the correct answer to a stage that shrank — it is
// only wrong if you wanted two independent views of one buddy, and this file
// has never supported that anyway: the painter sort reorders the shared model.
void eos_buddy_fit(eos_buddy_t *b, uint16_t w, uint16_t h);
void eos_buddy_stage(const eos_buddy_t *b, int32_t *hx_q8, int32_t *hy_q8);
void eos_buddy_pos(const eos_buddy_t *b, int32_t *x_q8, int32_t *y_q8);

// Both return true when the clamp bit, which is how a walker learns it has
// reached the edge of the stage and should stop rather than grind against it.
bool eos_buddy_move_to(eos_buddy_t *b, int32_t x_q8, int32_t y_q8);
bool eos_buddy_move_by(eos_buddy_t *b, int32_t dx_q8, int32_t dy_q8);

// The waddle, in Q8 VOXEL units, added on top of whatever the mood is doing.
// `lean` shears the top of the model sideways, which is the only roll this
// rasteriser has; `rise` lifts him. Both are one oscillator's business — see
// eos_stroll.c, which is the only thing that should be calling this.
void eos_buddy_set_gait(eos_buddy_t *b, int16_t lean_q8, int16_t rise_q8);

// An extra yaw offset in Q8 yaw steps, added to the target the mood picks.
// This is how something outside the mood machine turns him without editing
// cfg.home_yaw out from under it. It is not wrapped: a walker may wind it a
// whole turn to spin, and eos_buddy_tick() does the modulo.
void eos_buddy_face(eos_buddy_t *b, int32_t off_q8);
int32_t eos_buddy_facing(const eos_buddy_t *b);

// The buddy's own sine over a full uint16 turn, Q12. Exported because a gait
// outside this file has to stay in phase with the animation inside it, and
// two sine tables is two things to keep in step. Step k of the yaw ring is
// eos_buddy_sin_q12((uint16_t)(k << 11)).
int32_t eos_buddy_sin_q12(uint16_t turn);

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
