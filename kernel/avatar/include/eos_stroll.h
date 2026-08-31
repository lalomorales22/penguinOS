// eos_stroll — the little state machine that gives the buddy somewhere to be.
//
// eos_buddy owns seven moods and knows how to draw one frame of one of them.
// It has no idea where it is standing or where it would like to be, and that
// is the right split: a mood is what the megabrain is doing to him, and a
// stroll is what he does with the rest of his day. So this sits ON TOP of the
// mood machine and never inside it — it picks a spot, turns him, waddles him
// there, stops, looks about, and every so often does something silly. When a
// mood that means the owner is present takes over (THINKING, TALKING,
// LISTENING, CONFUSED) it stands down and holds position, because a penguin
// who wanders off mid-answer is a bug and not a feature. SLEEPING settles him
// completely.
//
// The whole surface it touches on the buddy is four numbers: a position, a
// lean, a rise, and a yaw offset. Everything else about the frame still comes
// from the mood table.
//
// The non-obvious constraint is the one the gait is built around: the frame
// loop runs at 10 Hz when the buddy is visible, so a waddle gets about ten
// samples per cycle and anything that needs sub-frame timing to read will
// just look like noise. That is why the roll and the step are the SAME
// oscillator rather than two that have to be kept in step — he leans onto the
// foot he is about to push off, so the lean peaks and the forward surges are
// the same event sampled once, and there is no phase to drift.
//
// What it is honestly not: there is no roll in the rasteriser and no separate
// legs in the model, so the lean is a horizontal shear of the whole body and
// the rise is a vertical offset. A shear reads as a lean on a blocky model
// because the top slides over the base, which is what a body over a stance
// foot does. A yaw wobble was the other candidate and is not used: one yaw
// step is 11.25 degrees, so the smallest wobble available snaps rather than
// rocks at ten frames a second.

#ifndef EOS_STROLL_H
#define EOS_STROLL_H

#include <stdint.h>
#include <stdbool.h>
#include "eos_buddy.h"

// The vocabulary of buddy.json's idle.behaviour, in the order eos_apps_idle_t
// spells it. The first four are the presets web/README.md already named; the
// last two are new and are what the owner asked for. Names, not numbers, are
// the contract between the two files — use eos_stroll_preset_from_name().
typedef enum {
    EOS_STROLL_STILL = 0,   // holds home_yaw, blinks, never leaves the spot
    EOS_STROLL_WANDER,      // the fallback: long rests, the odd short walk
    EOS_STROLL_CURIOUS,     // quick turns, looks about, covers the stage
    EOS_STROLL_SLEEPY,      // slow, short walks, very long rests, no play
    EOS_STROLL_ROAM,        // walks most of the time, whole stage, rarely idle
    EOS_STROLL_PLAY,        // roams and plays: hops, spins, flaps, stretches
    EOS_STROLL_PRESET_COUNT
} eos_stroll_preset_t;

typedef enum {
    EOS_STROLL_REST = 0,    // standing, waiting out a timer
    EOS_STROLL_TURN,        // yaw easing round to face the chosen spot
    EOS_STROLL_WALK,        // waddling toward it
    EOS_STROLL_LOOK,        // arrived; head turns one way then the other
    EOS_STROLL_ACT,         // doing one silly thing
    EOS_STROLL_HELD,        // a mood that means the owner is here; hold still
    EOS_STROLL_SETTLED,     // SLEEPING; everything decays to nothing
    EOS_STROLL_PHASE_COUNT
} eos_stroll_phase_t;

typedef enum {
    EOS_STROLL_ACT_NONE = 0,
    EOS_STROLL_ACT_HOP,
    EOS_STROLL_ACT_SPIN,
    EOS_STROLL_ACT_FLAP,
    EOS_STROLL_ACT_STRETCH,
    EOS_STROLL_ACT_COUNT
} eos_stroll_act_t;

typedef struct {
    eos_buddy_t *b;              // not owned; must outlive this

    uint8_t  preset;             // eos_stroll_preset_t
    uint8_t  phase;              // eos_stroll_phase_t
    uint8_t  act;                // eos_stroll_act_t
    uint8_t  moved;              // the position changed on the last tick

    uint32_t rng;
    uint32_t phase_ms;           // time spent in the current phase
    uint32_t phase_for_ms;       // how long this phase wants, 0 = until done

    // THE oscillator. One uint16 turn is one full waddle: two steps, two
    // lean extremes, two push-offs. Everything the gait does is a function
    // of eos_buddy_sin_q12() of this and nothing else.
    uint16_t gait_phase;
    uint16_t gait_ms;            // period of it, jittered per walk
    uint16_t gait_amp_q8;        // 0..256 inclusive, so not a byte; ramps in
                                 // and out so he does not snap into and out
                                 // of the rock mid-lean

    int32_t  tx_q8, ty_q8;       // where he is headed, Q8 px from stage centre
    uint8_t  face;               // the yaw step that points at it
    int32_t  spin_q8;            // yaw still owed by a spin, Q8 steps

    uint32_t play_in_ms;         // countdown to the next unprompted act
    uint16_t stall_ms;           // how long the walk has been getting nowhere
    uint32_t walked_q8;          // total ground covered, for the tests
} eos_stroll_t;

// `b` must already be initialised. The seed is the jitter: two buddies on two
// boards with the same seed do the same thing, which is what makes the tests
// repeatable and is why it is not taken from a clock.
void eos_stroll_init(eos_stroll_t *s, eos_buddy_t *b,
                     eos_stroll_preset_t preset, uint32_t seed);

// Changes what he does without moving him or dropping him mid-stride.
void eos_stroll_set_preset(eos_stroll_t *s, eos_stroll_preset_t preset);
eos_stroll_preset_t eos_stroll_preset(const eos_stroll_t *s);

// Call once per frame, AFTER eos_buddy_tick(), with the same dt. Ticking it
// before would drive the gait off a mood the buddy has not adopted yet, and
// on the frame a mood changes that is a lean applied to the wrong animation.
void eos_stroll_tick(eos_stroll_t *s, uint32_t dt_ms);

eos_stroll_phase_t eos_stroll_phase(const eos_stroll_t *s);
eos_stroll_act_t   eos_stroll_act(const eos_stroll_t *s);
const char *eos_stroll_phase_name(eos_stroll_phase_t p);
const char *eos_stroll_act_name(eos_stroll_act_t a);
const char *eos_stroll_preset_name(eos_stroll_preset_t p);

// buddy.json's idle.behaviour string. Anything unrecognised is WANDER, which
// is the rule web/README.md already set for the four presets that predate
// this file. NULL is WANDER too.
eos_stroll_preset_t eos_stroll_preset_from_name(const char *name);

// How much of his own size this preset wants traded for floor, as the value
// to put in eos_buddy_cfg_t.roam_q8 before eos_buddy_init(). STILL asks for
// nothing, so a board configured "still" draws exactly the buddy it drew
// before this file existed, at exactly the size it drew him. The policy lives
// here rather than in the caller because it is a property of the behaviour.
uint8_t eos_stroll_roam_q8(eos_stroll_preset_t preset);

// True when the last tick actually moved him, so a caller that tracks damage
// per tile knows this frame is worth repainting. It stays inside the buddy's
// own box either way — see the motion notes in eos_buddy.h — so this is a
// hint about whether to bother, never about how much to dirty.
bool eos_stroll_moved(const eos_stroll_t *s);

// The lean the gait is asking for right now, Q8 voxel units, signed: negative
// is a lean to screen-left. Exposed so a test can assert the relationship
// between the roll and the step rather than just that both of them move.
int16_t eos_stroll_lean(const eos_stroll_t *s);

// The forward speed the SAME oscillator is asking for, as a Q12 fraction of
// the preset's peak. It is |sin| of the gait phase: fastest at the two lean
// extremes, where the stance leg is straight and the other foot swings
// through, and zero at the two crossings, where both feet are down.
int32_t eos_stroll_stride(const eos_stroll_t *s);

#endif
