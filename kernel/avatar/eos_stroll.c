#include "eos_stroll.h"
#include <string.h>

// The gait, the wander and the play, in that order of how much they matter.
//
// The one number to keep in mind reading this file is 100: that is the
// milliseconds between two frames when the buddy is on screen. A waddle cycle
// is nine or ten of them, so the lean is sampled about every 36 degrees and
// there is no room for a second oscillator that has to stay in step with the
// first. It does not have one. `gait_phase` is the whole animation: the lean
// is its sine, the rise is its rectified sine, and the forward step is scaled
// by that same rectified sine, so the two lean extremes and the two push-offs
// per cycle are the same two events and cannot drift apart.

#define YAW_STEPS   EOS_BUDDY_YAW_STEPS
#define YAW_MASK    (EOS_BUDDY_YAW_STEPS - 1)
#define FULL_TURN   (EOS_BUDDY_YAW_STEPS << 8)

#define TURN_CAP_MS   2500   // a turn that has not settled by now is settled
#define ARRIVE_Q8      420   // within 1.6 px of the spot is standing on it
#define STALL_MS       450   // pressed against the edge this long: arrived
#define GAIT_IN_MS     260   // the rock ramps in over about two frames and a half
#define GAIT_OUT_MS    380

// Q8 voxel units. The rasteriser has no roll, so the lean is a horizontal
// shear: the top of the body slides over the base, which is what a body over
// a stance foot actually does. 300 puts the head about 1.2 voxels off centre
// on a 15-voxel model, which is a visible rock and not a fall.
#define HOP_RISE       270
#define HOP_MS         450   // one arch, not one sine: see run_act()
#define FLAP_LEAN      210
#define FLAP_MS        500
#define STRETCH_RISE   150

// Every one of these is a multiple of four or five frames at 10 Hz, and that
// is not a coincidence. An act built on a 200 ms oscillation aliases against
// a 100 ms frame into a two-frame flicker: the buddy does not flap, he
// strobes. Nothing here swings faster than about two frames a half-cycle.

typedef struct {
    uint16_t rest_min, rest_span;
    uint16_t look_min, look_span;   // look_min 0 means he does not look about
    uint16_t walk_cap_ms;
    uint16_t gait_ms;               // one full waddle: two steps
    int16_t  lean_q8;               // roll amplitude
    int16_t  rise_q8;               // hip rise over the stance leg
    uint16_t speed_q8;              // Q8 px/s at the push-off peak; mean is 0.64 of it
    uint16_t reach;                 // Q8 fraction of the stage he will aim at
                                    // 256 is the whole of it
    uint16_t play_min, play_span;   // play_min 0 means he never plays
} preset_t;

// Each row is one value of buddy.json's idle.behaviour. Rests and play gaps
// are min-plus-jitter rather than fixed for the obvious reason: a penguin who
// hops every nine seconds exactly is a metronome, and the eye finds that in
// about three repeats.
static const preset_t PRESET[EOS_STROLL_PRESET_COUNT] = {
/* STILL   */ {  2000, 3000,    0,    0,     0,    0,   0,   0,     0,   0,     0,     0 },
/* WANDER  */ {  1900, 3400,  800,  900,  5000, 1100, 250,  70,  1500, 160, 19000, 22000 },
/* CURIOUS */ {   900, 1800,  700, 1000,  5000,  900, 300,  90,  2600, 256,  8000, 13000 },
/* SLEEPY  */ {  4200, 5200,  900, 1100,  4000, 1500, 190,  55,   900, 110,     0,     0 },
/* ROAM    */ {   700, 1300,  600,  800,  7000,  950, 300,  90,  2900, 256, 13000, 17000 },
/* PLAY    */ {   600, 1200,  600,  900,  6000,  900, 330, 100,  3000, 256,  4000,  8000 }
};

static const char *const PRESET_NAME[EOS_STROLL_PRESET_COUNT] = {
    "still", "wander", "curious", "sleepy", "roam", "play"
};

static const char *const PHASE_NAME[EOS_STROLL_PHASE_COUNT] = {
    "rest", "turn", "walk", "look", "act", "held", "settled"
};

static const char *const ACT_NAME[EOS_STROLL_ACT_COUNT] = {
    "none", "hop", "spin", "flap", "stretch"
};

const char *eos_stroll_preset_name(eos_stroll_preset_t p)
{
    return ((unsigned)p < EOS_STROLL_PRESET_COUNT) ? PRESET_NAME[p] : "wander";
}

const char *eos_stroll_phase_name(eos_stroll_phase_t p)
{
    return ((unsigned)p < EOS_STROLL_PHASE_COUNT) ? PHASE_NAME[p] : "?";
}

const char *eos_stroll_act_name(eos_stroll_act_t a)
{
    return ((unsigned)a < EOS_STROLL_ACT_COUNT) ? ACT_NAME[a] : "none";
}

eos_stroll_preset_t eos_stroll_preset_from_name(const char *name)
{
    unsigned i, j;
    if (!name) return EOS_STROLL_WANDER;
    for (i = 0; i < EOS_STROLL_PRESET_COUNT; i++) {
        const char *n = PRESET_NAME[i];
        for (j = 0; n[j] && name[j] == n[j]; j++) { }
        if (n[j] == '\0' && name[j] == '\0') return (eos_stroll_preset_t)i;
    }
    return EOS_STROLL_WANDER;    // web/README.md: unknown falls back to wander
}

// A fifth of him is the trade that makes an 80x80 tile a stage: he drops to
// three quarters and gets about a third of the box to move over, which is far
// enough that a walk reads as a walk and near enough that he is still the
// biggest thing in the tile. SLEEPY takes less because it barely moves.
uint8_t eos_stroll_roam_q8(eos_stroll_preset_t preset)
{
    switch (preset) {
    case EOS_STROLL_STILL:  return 0;
    case EOS_STROLL_SLEEPY: return 32;
    case EOS_STROLL_WANDER: return 41;
    default:                return 51;
    }
}

static uint32_t rnd(eos_stroll_t *s)
{
    uint32_t x = s->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s->rng = x ? x : 0x9E3779B9u;
    return s->rng;
}

static uint32_t jitter(eos_stroll_t *s, uint16_t min, uint16_t span)
{
    return (uint32_t)min + (span ? (rnd(s) % span) : 0u);
}

static int32_t yaw_sin(int step)
{
    return eos_buddy_sin_q12((uint16_t)(((uint32_t)(step & YAW_MASK)) << 11));
}

static int32_t yaw_cos(int step) { return yaw_sin(step + YAW_STEPS / 4); }

// Which yaw step points at a delta given in GROUND space — that is, with the
// screen's vertical already un-foreshortened. Thirty-two dot products once
// per target chosen, which is nothing next to a frame; a table inverse would
// need an atan2 and would still be a search at the wrap.
//
// Forward for the buddy is model -y, which the projection puts at screen
// (-sin yaw, +cos yaw * sin phi). Strip the sin phi and the ground heading is
// just (-sin, +cos), which is what this matches against.
static uint8_t heading(int32_t dx, int32_t gy)
{
    int best = 0, k;
    int64_t bd = 0;
    for (k = 0; k < YAW_STEPS; k++) {
        int64_t d = -(int64_t)dx * yaw_sin(k) + (int64_t)gy * yaw_cos(k);
        if (k == 0 || d > bd) { bd = d; best = k; }
    }
    return (uint8_t)best;
}

// Asks the buddy to face `step`, expressed as an offset from home so the mood
// machine keeps ownership of home_yaw and of its own per-mood offset.
static void aim(eos_stroll_t *s, int step)
{
    int32_t off = ((int32_t)(step & YAW_MASK) - (int32_t)s->b->cfg.home_yaw) << 8;
    while (off >  FULL_TURN / 2) off -= FULL_TURN;
    while (off < -FULL_TURN / 2) off += FULL_TURN;
    eos_buddy_face(s->b, off);
}

static void enter(eos_stroll_t *s, eos_stroll_phase_t ph, uint32_t for_ms)
{
    s->phase        = (uint8_t)ph;
    s->phase_ms     = 0;
    s->phase_for_ms = for_ms;
    if (ph != EOS_STROLL_ACT) s->act = EOS_STROLL_ACT_NONE;
}

static void schedule_play(eos_stroll_t *s)
{
    const preset_t *p = &PRESET[s->preset];
    s->play_in_ms = p->play_min ? jitter(s, p->play_min, p->play_span) : 0xFFFFFFFFu;
}

static void pick_target(eos_stroll_t *s)
{
    const preset_t *p = &PRESET[s->preset];
    int32_t hx, hy, x, y;

    eos_buddy_stage(s->b, &hx, &hy);
    hx = hx * (int32_t)p->reach / 256;
    hy = hy * (int32_t)p->reach / 256;
    s->tx_q8 = hx ? (int32_t)(rnd(s) % (uint32_t)(2 * hx + 1)) - hx : 0;
    s->ty_q8 = hy ? (int32_t)(rnd(s) % (uint32_t)(2 * hy + 1)) - hy : 0;

    eos_buddy_pos(s->b, &x, &y);
    s->face = heading(s->tx_q8 - x, (s->ty_q8 - y) * 2);
    aim(s, s->face);

    // A tenth either side of the preset's cycle, so two walks in a row are
    // not the same walk. Wide enough to notice, narrow enough that it is
    // still the same animal.
    s->gait_ms = (uint16_t)(p->gait_ms - p->gait_ms / 10 + (rnd(s) % (p->gait_ms / 5 + 1)));
    s->stall_ms = 0;
}

// The lean and the rise, from the one oscillator, scaled by the ramp. Called
// every tick that is not an act, so that stopping eases out instead of
// freezing him mid-rock.
static void gait_write(eos_stroll_t *s)
{
    const preset_t *p = &PRESET[s->preset];
    int32_t sn = eos_buddy_sin_q12(s->gait_phase);
    int32_t as = sn < 0 ? -sn : sn;
    int32_t lean = (int32_t)p->lean_q8 * sn / 4096;
    int32_t rise = (int32_t)p->rise_q8 * as / 4096;
    lean = lean * (int32_t)s->gait_amp_q8 / 256;
    rise = rise * (int32_t)s->gait_amp_q8 / 256;
    eos_buddy_set_gait(s->b, (int16_t)lean, (int16_t)rise);
}

static void gait_ramp(eos_stroll_t *s, uint32_t dt_ms, bool up)
{
    int32_t a = s->gait_amp_q8;
    if (up) {
        a += (int32_t)(dt_ms * 256u / GAIT_IN_MS);
        if (a > 256) a = 256;
        s->gait_phase = (uint16_t)(s->gait_phase +
                        (uint16_t)((dt_ms * 65536u / (s->gait_ms ? s->gait_ms : 1000u)) & 0xFFFFu));
    } else {
        a -= (int32_t)(dt_ms * 256u / GAIT_OUT_MS);
        if (a < 0) a = 0;
        // Keep the phase running while it fades, so he settles out of the
        // rock rather than being switched off in the middle of one.
        if (a > 0)
            s->gait_phase = (uint16_t)(s->gait_phase +
                            (uint16_t)((dt_ms * 65536u / (s->gait_ms ? s->gait_ms : 1000u)) & 0xFFFFu));
        else
            s->gait_phase = 0;
    }
    s->gait_amp_q8 = (uint16_t)a;
    gait_write(s);
}

void eos_stroll_init(eos_stroll_t *s, eos_buddy_t *b,
                     eos_stroll_preset_t preset, uint32_t seed)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->b      = b;
    s->preset = (preset < EOS_STROLL_PRESET_COUNT) ? (uint8_t)preset : EOS_STROLL_WANDER;
    s->rng    = seed ? seed : 0x5EED1E55u;
    s->gait_ms = PRESET[s->preset].gait_ms ? PRESET[s->preset].gait_ms : 1000;
    enter(s, EOS_STROLL_REST, jitter(s, PRESET[s->preset].rest_min,
                                        PRESET[s->preset].rest_span));
    schedule_play(s);
    if (b) { eos_buddy_set_gait(b, 0, 0); eos_buddy_face(b, 0); }
}

void eos_stroll_set_preset(eos_stroll_t *s, eos_stroll_preset_t preset)
{
    if (!s || preset >= EOS_STROLL_PRESET_COUNT) return;
    if (s->preset == (uint8_t)preset) return;
    s->preset = (uint8_t)preset;
    schedule_play(s);
    // Do not touch the phase. Changing the preset mid-stride should change
    // where he goes next, not drop him on the spot with a foot in the air.
}

eos_stroll_preset_t eos_stroll_preset(const eos_stroll_t *s)
{
    return s ? (eos_stroll_preset_t)s->preset : EOS_STROLL_WANDER;
}

eos_stroll_phase_t eos_stroll_phase(const eos_stroll_t *s)
{
    return s ? (eos_stroll_phase_t)s->phase : EOS_STROLL_REST;
}

eos_stroll_act_t eos_stroll_act(const eos_stroll_t *s)
{
    return s ? (eos_stroll_act_t)s->act : EOS_STROLL_ACT_NONE;
}

bool eos_stroll_moved(const eos_stroll_t *s) { return s && s->moved; }

int16_t eos_stroll_lean(const eos_stroll_t *s)
{
    return (s && s->b) ? s->b->gait_lean_q8 : 0;
}

int32_t eos_stroll_stride(const eos_stroll_t *s)
{
    int32_t sn;
    if (!s) return 0;
    sn = eos_buddy_sin_q12(s->gait_phase);
    return sn < 0 ? -sn : sn;
}

// ------------------------------------------------------------------- phases

static void begin_walk_or_look(eos_stroll_t *s)
{
    const preset_t *p = &PRESET[s->preset];
    if (p->reach == 0) {
        if (p->look_min) enter(s, EOS_STROLL_LOOK, jitter(s, p->look_min, p->look_span));
        else             enter(s, EOS_STROLL_REST, jitter(s, p->rest_min, p->rest_span));
        return;
    }
    pick_target(s);
    enter(s, EOS_STROLL_TURN, TURN_CAP_MS);
}

static void begin_act(eos_stroll_t *s)
{
    static const uint16_t DUR[EOS_STROLL_ACT_COUNT] = { 0, 1350, 1300, 1500, 1100 };
    uint8_t a = (uint8_t)(1 + (rnd(s) % (EOS_STROLL_ACT_COUNT - 1)));
    enter(s, EOS_STROLL_ACT, DUR[a]);
    s->act          = a;
    s->gait_amp_q8  = 0;
    s->gait_phase   = 0;
    s->spin_q8      = 0;
    eos_buddy_set_gait(s->b, 0, 0);
    schedule_play(s);
}

// Every act is written through the same two numbers the waddle uses, plus the
// yaw offset for the spin. That is the whole reason there are only four of
// them and adding a fifth is a case label: the buddy has no other joints.
static void run_act(eos_stroll_t *s, uint32_t dt_ms)
{
    int32_t t = (int32_t)s->phase_ms, dur = (int32_t)s->phase_for_ms, sn, as;
    (void)dt_ms;
    if (dur <= 0) dur = 1;

    switch (s->act) {
    case EOS_STROLL_ACT_HOP: {
        // HALF a turn per hop, then rectified. That makes each hop one clean
        // arch of length HOP_MS — up, over, down — rather than the two humps
        // a rectified full sine would give, which at four frames a hop is the
        // difference between a bounce and a stutter.
        uint16_t ph = (uint16_t)(((uint32_t)t * 32768u / HOP_MS) & 0xFFFFu);
        sn = eos_buddy_sin_q12(ph);
        as = sn < 0 ? -sn : sn;                 // rectified: he rests on the floor
        eos_buddy_set_gait(s->b, 0, (int16_t)(HOP_RISE * as / 4096));
        break;
    }
    case EOS_STROLL_ACT_SPIN: {
        // A whole turn wound onto the yaw target across the act. The ease in
        // eos_buddy_tick() lags it by about six steps at this rate, which is
        // what stops it reading as a teleport, and the wind is exactly one
        // full turn so he ends up facing where he started.
        // The wind is exactly one turn, so the offset the act ends on and the
        // zero it is reset to are the same yaw. The ease is six or seven
        // steps behind by then and finishes the turn the way it was already
        // going, rather than reversing out of it.
        int32_t off = (int32_t)(((int64_t)FULL_TURN * t) / dur);
        s->spin_q8 = off;
        eos_buddy_face(s->b, off);
        eos_buddy_set_gait(s->b, (int16_t)(60), 0);
        break;
    }
    case EOS_STROLL_ACT_FLAP: {
        uint16_t ph = (uint16_t)(((uint32_t)t * 65536u / FLAP_MS) & 0xFFFFu);
        sn = eos_buddy_sin_q12(ph);
        as = sn < 0 ? -sn : sn;
        eos_buddy_set_gait(s->b, (int16_t)(FLAP_LEAN * sn / 4096),
                                 (int16_t)(30 * as / 4096));
        break;
    }
    case EOS_STROLL_ACT_STRETCH: {
        // One half turn of sine over the whole act: up, hold, down.
        uint16_t ph = (uint16_t)(((uint32_t)t * 32768u / (uint32_t)dur) & 0xFFFFu);
        sn = eos_buddy_sin_q12(ph);
        if (sn < 0) sn = 0;
        eos_buddy_set_gait(s->b, (int16_t)(90 * sn / 4096),
                                 (int16_t)(STRETCH_RISE * sn / 4096));
        break;
    }
    default:
        eos_buddy_set_gait(s->b, 0, 0);
        break;
    }
}

static void run_walk(eos_stroll_t *s, uint32_t dt_ms)
{
    const preset_t *p = &PRESET[s->preset];
    int32_t x, y, rx, gy, v, sn, cs, dx, dy, nx, ny;
    int64_t fwd;

    gait_ramp(s, dt_ms, true);

    eos_buddy_pos(s->b, &x, &y);
    rx = s->tx_q8 - x;
    gy = (s->ty_q8 - y) * 2;               // ground space: undo the foreshortening

    // The step. Its size is the SAME rectified sine the rise came from, so
    // the two surges per cycle land on the two lean extremes: he is over the
    // stance foot, the far leg swings through, and that is the push-off.
    v = (int32_t)p->speed_q8 * (int32_t)dt_ms / 1000;
    v = v * eos_stroll_stride(s) / 4096;

    sn = yaw_sin(s->face);
    cs = yaw_cos(s->face);
    dx = -(v * sn) / 4096;
    // The vertical is squashed by the camera's elevation, which is what makes
    // walking away from you cover less glass than walking across. Nothing
    // scales the model, so this is the only depth cue there is.
    dy = ((v * cs) / 4096) * EOS_BUDDY_SIN_PHI / 4096;

    eos_buddy_move_by(s->b, dx, dy);
    eos_buddy_pos(s->b, &nx, &ny);
    if (nx != x || ny != y) {
        int32_t mx = nx > x ? nx - x : x - nx;
        int32_t my = ny > y ? ny - y : y - ny;
        s->walked_q8 += (uint32_t)(mx + my);
        s->moved = 1;
        if (mx + my < 4 && v > 8) s->stall_ms = (uint16_t)(s->stall_ms + dt_ms);
        else                      s->stall_ms = 0;
    } else if (v > 8) {
        s->stall_ms = (uint16_t)(s->stall_ms + dt_ms);
    }

    // Arrived: close enough, or the target is now behind him, or the clamp
    // has been holding him for long enough that it never will be closer.
    fwd = -(int64_t)rx * sn + (int64_t)gy * cs;
    if ((rx < 0 ? -rx : rx) + (gy < 0 ? -gy : gy) < ARRIVE_Q8 ||
        fwd <= 0 || s->stall_ms >= STALL_MS ||
        s->phase_ms >= s->phase_for_ms) {
        enter(s, EOS_STROLL_LOOK, jitter(s, p->look_min ? p->look_min : 500,
                                            p->look_span));
    }
}

static void run_look(eos_stroll_t *s, uint32_t dt_ms)
{
    int32_t half = (int32_t)s->phase_for_ms / 2;
    int nudge = 2 + (int)(s->rng & 1u);
    gait_ramp(s, dt_ms, false);
    if (half > 0)
        aim(s, (int)s->face + ((int32_t)s->phase_ms < half ? nudge : -nudge));
}

// ---------------------------------------------------------------- the tick

void eos_stroll_tick(eos_stroll_t *s, uint32_t dt_ms)
{
    const preset_t *p;
    eos_buddy_state_t st;

    if (!s || !s->b || dt_ms == 0) return;
    if (dt_ms > 1000) dt_ms = 1000;      // a stall must not teleport him either

    p  = &PRESET[s->preset];
    st = eos_buddy_state(s->b);
    s->moved = 0;

    // --- the mood gate ---------------------------------------------------
    // He walks while IDLE and at no other time. SLEEPING settles him; the
    // four moods that mean the owner is at the panel hold him where he is,
    // because a buddy who wanders off mid-answer looks broken rather than
    // alive. HAPPY holds too: it lasts 1.4 seconds and already has a hop of
    // its own, and stacking a waddle under that is just noise.
    if (st == EOS_BUDDY_SLEEPING) {
        if (s->phase != EOS_STROLL_SETTLED) {
            enter(s, EOS_STROLL_SETTLED, 0);
            eos_buddy_face(s->b, 0);
        }
    } else if (st != EOS_BUDDY_IDLE) {
        if (s->phase != EOS_STROLL_HELD) {
            enter(s, EOS_STROLL_HELD, 0);
            eos_buddy_face(s->b, 0);
        }
    } else if (s->phase == EOS_STROLL_HELD || s->phase == EOS_STROLL_SETTLED) {
        enter(s, EOS_STROLL_REST, jitter(s, p->rest_min, p->rest_span));
    }

    s->phase_ms += dt_ms;
    if (s->play_in_ms != 0xFFFFFFFFu)
        s->play_in_ms = (s->play_in_ms > dt_ms) ? s->play_in_ms - dt_ms : 0;

    switch (s->phase) {
    case EOS_STROLL_SETTLED:
    case EOS_STROLL_HELD:
        gait_ramp(s, dt_ms, false);
        break;

    case EOS_STROLL_REST:
        gait_ramp(s, dt_ms, false);
        aim(s, (int)s->b->cfg.home_yaw);
        if (s->phase_ms >= s->phase_for_ms) {
            if (s->play_in_ms == 0 && p->play_min) begin_act(s);
            else                                   begin_walk_or_look(s);
        }
        break;

    case EOS_STROLL_TURN: {
        int d;
        gait_ramp(s, dt_ms, false);
        aim(s, s->face);
        d = (int)eos_buddy_yaw_step(s->b) - (int)s->face;
        while (d >  YAW_STEPS / 2) d -= YAW_STEPS;
        while (d < -YAW_STEPS / 2) d += YAW_STEPS;
        if (d < 0) d = -d;
        // One step of slack, because IDLE's sway is nearly a whole step wide
        // and waiting for exact would wait forever. The cap is the promise
        // that he can never be stranded facing the wrong way.
        if (d <= 1 || s->phase_ms >= s->phase_for_ms)
            enter(s, EOS_STROLL_WALK, p->walk_cap_ms);
        break;
    }

    case EOS_STROLL_WALK:
        aim(s, s->face);
        run_walk(s, dt_ms);
        break;

    case EOS_STROLL_LOOK:
        run_look(s, dt_ms);
        if (s->phase_ms >= s->phase_for_ms) {
            aim(s, (int)s->b->cfg.home_yaw);
            enter(s, EOS_STROLL_REST, jitter(s, p->rest_min, p->rest_span));
        }
        break;

    case EOS_STROLL_ACT:
        run_act(s, dt_ms);
        if (s->phase_ms >= s->phase_for_ms) {
            eos_buddy_set_gait(s->b, 0, 0);
            eos_buddy_face(s->b, 0);
            s->spin_q8 = 0;
            enter(s, EOS_STROLL_REST, jitter(s, p->rest_min, p->rest_span));
        }
        break;

    default:
        enter(s, EOS_STROLL_REST, jitter(s, p->rest_min, p->rest_span));
        break;
    }
}
