#include "eos_buddy.h"
#include <string.h>

// The camera sits 30 degrees above the horizon and orbits on the yaw ring.
// Everything downstream is integer: Q12 for trig, Q8 for screen pixels.
#define SIN_PHI EOS_BUDDY_SIN_PHI   // sin(30) * 4096
#define COS_PHI 3547                // cos(30) * 4096

// sin(step * 360/32) * 4096. cos(i) is sin(i+8), so one table does both.
static const int16_t SIN_Q12[EOS_BUDDY_YAW_STEPS] = {
       0,  799, 1567, 2276, 2896, 3406, 3784, 4017,
    4096, 4017, 3784, 3406, 2896, 2276, 1567,  799,
       0, -799,-1567,-2276,-2896,-3406,-3784,-4017,
   -4096,-4017,-3784,-3406,-2896,-2276,-1567, -799
};

#define YAW_FULL  (EOS_BUDDY_YAW_STEPS << 8)
#define YAW_TAU   230      // ms to close most of a turn
#define BLINK_MS  120
#define POP_MS    260
#define TALK_GAP  900      // no chunk for this long and TALKING lapses

static int32_t mq12(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) / 4096); }
static int32_t mq8 (int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) / 256); }

// Interpolated sine over the same table. Angle is a full uint16 turn, so the
// animation phases can just wrap on overflow.
static int32_t sin16(uint16_t a)
{
    uint32_t i = a >> 11, f = a & 0x7FF;
    int32_t s0 = SIN_Q12[i], s1 = SIN_Q12[(i + 1) & (EOS_BUDDY_YAW_STEPS - 1)];
    return s0 + (int32_t)(((int64_t)(s1 - s0) * (int32_t)f) / 2048);
}

int32_t eos_buddy_sin_q12(uint16_t turn) { return sin16(turn); }

static uint32_t rnd(eos_buddy_t *b)
{
    uint32_t x = b->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    b->rng = x ? x : 0x9E3779B9u;
    return b->rng;
}

// ------------------------------------------------------------- personality
//
// One row per state. Amplitudes are Q8: 256 is one voxel of travel, or one
// yaw step of sway. This table IS the personality — nothing else about the
// states is hand-coded, which is why adding a mood is a line of data.

typedef struct {
    uint16_t bob_ms;   int16_t bob_amp;  uint8_t bob_bounce;
    int16_t  lift;
    int16_t  yaw_off;
    uint16_t sway_ms;  int16_t sway_amp;
    uint16_t shear_ms; int16_t shear_amp;
    uint16_t squash_ms;int16_t squash_amp;
    uint16_t blink_min, blink_max;
    uint8_t  eyes_shut;
    uint16_t hold_ms;  uint8_t next;
} anim_t;

static const anim_t ANIM[EOS_BUDDY_STATE_COUNT] = {
/* IDLE      */ { 2600,  56, 0,    0,     0, 7000, 230,    0,    0,    0,   0, 2400, 5200, 0,    0, EOS_BUDDY_IDLE },
/* THINKING  */ {  900,  26, 0,   38,  1280, 2300, 410, 2900,  128,    0,   0,  900, 1800, 0,    0, EOS_BUDDY_IDLE },
/* TALKING   */ {  700,  36, 0,    0,     0, 1500, 128,    0,    0,  210,  77, 1800, 3600, 0,    0, EOS_BUDDY_IDLE },
/* LISTENING */ { 1800,  26, 0,   77,     0, 4000, 100, 3300,   46,    0,   0, 1200, 2600, 0,    0, EOS_BUDDY_IDLE },
/* SLEEPING  */ { 4200,  72, 0, -154,  -768,    0,   0,    0, -180,    0,   0, 3000, 6000, 1,    0, EOS_BUDDY_IDLE },
/* HAPPY     */ {  420, 140, 1,    0,     0,  380, 560,  380,  205,  420,  50,  600, 1100, 0, 1400, EOS_BUDDY_IDLE },
/* CONFUSED  */ { 1500,  26, 0,    0,     0, 1900, 770, 1900,  358,    0,   0, 1400, 2400, 0, 1900, EOS_BUDDY_IDLE }
};

static const char *STATE_NAME[EOS_BUDDY_STATE_COUNT] = {
    "IDLE", "THINKING", "TALKING", "LISTENING", "SLEEPING", "HAPPY", "CONFUSED"
};

const char *eos_buddy_state_name(eos_buddy_state_t s)
{
    // Cast before the compare: an enum whose enumerators are all non-negative
    // may be signed or unsigned at the compiler's option, and a signed -1 here
    // would index off the front of the table.
    return ((unsigned)s < EOS_BUDDY_STATE_COUNT) ? STATE_NAME[s] : "?";
}

void eos_buddy_default_cfg(eos_buddy_cfg_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    // 45 degrees off axis, so three faces are always visible and the buddy
    // never flattens into a two-tone slab.
    cfg->home_yaw      = EOS_BUDDY_YAW_STEPS / 8;
    cfg->shade[0]      = 255;   // top
    cfg->shade[1]      = 200;   // +-y faces
    cfg->shade[2]      = 148;   // +-x faces
    cfg->flat_565      = 0xFDA0;
    cfg->idle_sleep_ms = 90000;
    cfg->seed          = 0xB0DDBEEFu;
}

static void reschedule_blink(eos_buddy_t *b)
{
    const anim_t *a = &ANIM[b->state];
    uint16_t span = (a->blink_max > a->blink_min) ? (uint16_t)(a->blink_max - a->blink_min) : 1;
    b->blink_in_ms = (uint16_t)(a->blink_min + (rnd(b) % span));
}

void eos_buddy_init(eos_buddy_t *b, eos_vox_model_t *m, const eos_buddy_cfg_t *cfg)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->model = m;
    if (cfg) b->cfg = *cfg; else eos_buddy_default_cfg(&b->cfg);
    b->cfg.home_yaw &= (EOS_BUDDY_YAW_STEPS - 1);
    b->rng = b->cfg.seed ? b->cfg.seed : 0xB0DDBEEFu;
    b->state = b->prev_state = EOS_BUDDY_IDLE;
    b->yaw_q8 = b->yaw_target_q8 = (int32_t)b->cfg.home_yaw << 8;
    reschedule_blink(b);
}

eos_buddy_state_t eos_buddy_state(const eos_buddy_t *b)
{
    return b ? (eos_buddy_state_t)b->state : EOS_BUDDY_IDLE;
}

bool eos_buddy_blinking(const eos_buddy_t *b)
{
    if (!b) return false;
    return b->blink_left_ms > 0 || ANIM[b->state].eyes_shut != 0;
}

uint8_t eos_buddy_yaw_step(const eos_buddy_t *b)
{
    if (!b) return 0;
    return (uint8_t)(((b->yaw_q8 + 128) >> 8) & (EOS_BUDDY_YAW_STEPS - 1));
}

void eos_buddy_set_state(eos_buddy_t *b, eos_buddy_state_t s)
{
    if (!b || s >= EOS_BUDDY_STATE_COUNT) return;
    if (b->state == s) { b->state_ms = 0; return; }
    b->prev_state   = b->state;
    b->state        = (uint8_t)s;
    b->state_ms     = 0;
    b->squash_phase = 0;
    b->blink_left_ms = 0;
    b->blink_again   = 0;
    // Entering TALKING IS a chunk arriving, whichever door it came through.
    // Without this a caller who drives the state directly rather than through
    // the stream events inherits however long it has been since the last
    // chunk, and tick() lapses the buddy straight back to IDLE on the very
    // next frame.
    if (s == EOS_BUDDY_TALKING) b->since_chunk_ms = 0;
    reschedule_blink(b);
}

void eos_buddy_event(eos_buddy_t *b, eos_buddy_event_t ev)
{
    if (!b) return;
    switch (ev) {
    case EOS_BUDDY_EV_USER_TYPING:
        eos_buddy_set_state(b, EOS_BUDDY_LISTENING);
        break;
    case EOS_BUDDY_EV_REQUEST_SENT:
        eos_buddy_set_state(b, EOS_BUDDY_THINKING);
        break;
    case EOS_BUDDY_EV_STREAM_FIRST:
        // The perk-up: snap the turn target back to the user, spike the
        // energy so the first syllable is the biggest one, and pop.
        eos_buddy_set_state(b, EOS_BUDDY_TALKING);
        b->yaw_target_q8  = (int32_t)b->cfg.home_yaw << 8;
        b->energy_q8      = 256;
        b->pop_ms         = POP_MS;
        b->since_chunk_ms = 0;
        break;
    case EOS_BUDDY_EV_STREAM_CHUNK:
        if (b->state != EOS_BUDDY_TALKING) eos_buddy_set_state(b, EOS_BUDDY_TALKING);
        b->energy_q8 = (int16_t)(b->energy_q8 + 90 > 256 ? 256 : b->energy_q8 + 90);
        b->since_chunk_ms = 0;
        break;
    case EOS_BUDDY_EV_STREAM_DONE:
        eos_buddy_set_state(b, EOS_BUDDY_HAPPY);
        break;
    case EOS_BUDDY_EV_ERROR:
        eos_buddy_set_state(b, EOS_BUDDY_CONFUSED);
        break;
    case EOS_BUDDY_EV_IDLE_TIMEOUT:
        eos_buddy_set_state(b, EOS_BUDDY_SLEEPING);
        break;
    }
}

static uint16_t phase_step(uint32_t dt_ms, uint16_t period_ms)
{
    if (!period_ms) return 0;
    return (uint16_t)((dt_ms * 65536u / period_ms) & 0xFFFFu);
}

void eos_buddy_tick(eos_buddy_t *b, uint32_t dt_ms)
{
    if (!b || dt_ms == 0) return;
    if (dt_ms > 1000) dt_ms = 1000;      // a stall must not teleport the buddy

    b->life_ms       += dt_ms;
    b->state_ms      += dt_ms;
    b->since_chunk_ms += dt_ms;

    const anim_t *a = &ANIM[b->state];

    // --- automatic exits -------------------------------------------------
    if (a->hold_ms && b->state_ms >= a->hold_ms) {
        eos_buddy_set_state(b, (eos_buddy_state_t)a->next);
        a = &ANIM[b->state];
    } else if (b->state == EOS_BUDDY_TALKING && b->since_chunk_ms > TALK_GAP) {
        eos_buddy_set_state(b, EOS_BUDDY_IDLE);
        a = &ANIM[b->state];
    } else if (b->state == EOS_BUDDY_IDLE && b->cfg.idle_sleep_ms &&
               b->state_ms >= b->cfg.idle_sleep_ms) {
        eos_buddy_set_state(b, EOS_BUDDY_SLEEPING);
        a = &ANIM[b->state];
    }

    // --- energy, which is how hard the reply is streaming ----------------
    if (b->energy_q8 > 0) {
        int32_t d = (int32_t)(dt_ms * 256u / 500u);
        b->energy_q8 = (int16_t)(b->energy_q8 > d ? b->energy_q8 - d : 0);
    }
    if (b->pop_ms) b->pop_ms = (uint16_t)(b->pop_ms > dt_ms ? b->pop_ms - dt_ms : 0);

    // --- phases ----------------------------------------------------------
    b->bob_phase    = (uint16_t)(b->bob_phase    + phase_step(dt_ms, a->bob_ms));
    b->sway_phase   = (uint16_t)(b->sway_phase   + phase_step(dt_ms, a->sway_ms));
    b->shear_phase  = (uint16_t)(b->shear_phase  + phase_step(dt_ms, a->shear_ms));
    b->squash_phase = (uint16_t)(b->squash_phase + phase_step(dt_ms, a->squash_ms));

    // --- vertical bob ----------------------------------------------------
    int32_t s = sin16(b->bob_phase);
    if (a->bob_bounce && s < 0) s = -s;              // a hop rests on the floor
    b->bob_q8 = (int16_t)(a->lift + (a->bob_amp * s) / 4096);

    // --- lean ------------------------------------------------------------
    if (a->shear_ms) b->shear_q8 = (int16_t)((a->shear_amp * sin16(b->shear_phase)) / 4096);
    else             b->shear_q8 = a->shear_amp;

    // --- squash and stretch, scaled by how much is being said ------------
    int32_t sq = 0;
    if (a->squash_ms && a->squash_amp) {
        sq = (a->squash_amp * sin16(b->squash_phase)) / 4096;
        sq = sq * b->energy_q8 / 256;
    }
    if (b->pop_ms) sq -= (int32_t)b->pop_ms * 90 / POP_MS;   // negative = stretch tall
    b->squash_q8 = (int16_t)sq;

    // --- yaw: target is home plus the state's offset plus the sway -------
    int32_t target = ((int32_t)b->cfg.home_yaw << 8) + a->yaw_off + b->face_off_q8;
    if (a->sway_ms) target += (a->sway_amp * sin16(b->sway_phase)) / 4096;
    target %= YAW_FULL;
    if (target < 0) target += YAW_FULL;
    b->yaw_target_q8 = target;

    int32_t d = b->yaw_target_q8 - b->yaw_q8;
    while (d >  YAW_FULL / 2) d -= YAW_FULL;
    while (d < -YAW_FULL / 2) d += YAW_FULL;
    int32_t step = (int32_t)(((int64_t)d * (int32_t)dt_ms) / YAW_TAU);
    if (step == 0 && d != 0) step = (d > 0) ? 1 : -1;
    if ((d > 0 && step > d) || (d < 0 && step < d)) step = d;
    b->yaw_q8 += step;
    b->yaw_q8 %= YAW_FULL;
    if (b->yaw_q8 < 0) b->yaw_q8 += YAW_FULL;

    // --- blink -----------------------------------------------------------
    if (b->blink_left_ms) {
        b->blink_left_ms = (uint16_t)(b->blink_left_ms > dt_ms ? b->blink_left_ms - dt_ms : 0);
        if (b->blink_left_ms == 0 && b->blink_again) { b->blink_again = 0; b->blink_in_ms = 90; }
    } else if (b->blink_in_ms > dt_ms) {
        b->blink_in_ms = (uint16_t)(b->blink_in_ms - dt_ms);
    } else {
        b->blink_left_ms = BLINK_MS;
        reschedule_blink(b);
        if ((rnd(b) & 3) == 0) b->blink_again = 1;   // the odd double blink
    }
}

// ---------------------------------------------------------------- rasteriser

static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

// How much screen the model needs at scale 1: `rad` across (the diagonal, so
// it holds at every yaw) and `uh` tall once the 30 degree camera has squashed
// the depth into the height. Both in voxels.
static void footprint(const eos_vox_model_t *m, uint32_t *rad, uint32_t *uh)
{
    uint32_t sx = m->sx, sy = m->sy, sz = m->sz;
    uint32_t r = isqrt32(sx * sx + sy * sy);          // widest footprint, any yaw
    uint32_t u;
    if (r == 0) r = 1;
    u = (r * SIN_PHI) / 4096 + (sz * COS_PHI) / 4096;
    if (u == 0) u = 1;
    *rad = r;
    *uh  = u;
}

static uint16_t fit_scale(const eos_vox_model_t *m, uint16_t w, uint16_t h)
{
    uint32_t rad, uh, a, c, s;
    footprint(m, &rad, &uh);
    a = ((uint32_t)w << 8) / rad;
    c = ((uint32_t)h << 8) / uh;
    s = (a < c ? a : c) * 220 / 256;                  // room for bob, lean, squash
    if (s < 256)  s = 256;
    if (s > 8192) s = 8192;
    return (uint16_t)s;
}

// ------------------------------------------------------------------- motion

static void clamp_pos(eos_buddy_t *b)
{
    if (b->walk_x_q8 >  b->stage_x_q8) b->walk_x_q8 =  b->stage_x_q8;
    if (b->walk_x_q8 < -b->stage_x_q8) b->walk_x_q8 = -b->stage_x_q8;
    if (b->walk_y_q8 >  b->stage_y_q8) b->walk_y_q8 =  b->stage_y_q8;
    if (b->walk_y_q8 < -b->stage_y_q8) b->walk_y_q8 = -b->stage_y_q8;
}

// The drawn scale, and the stage that is left over once he is standing on it.
// Both come out of the same division of the target, which is why they are
// computed together and why cfg.roam_q8 belongs to the buddy rather than to
// the walker: whoever decides how big he is has already decided how far he
// can go.
static int32_t plan_stage(eos_buddy_t *b, uint16_t w, uint16_t h)
{
    const eos_vox_model_t *m = b->model;
    uint32_t rad, uh;
    int32_t S, fx, fy, sx, sy;

    if (!m || m->sx == 0 || m->sy == 0 || m->sz == 0) return 256;

    if (b->cfg.scale_q8) {
        S = b->cfg.scale_q8;
    } else {
        if (b->fit_scale_q8 == 0 || b->fit_w != w || b->fit_h != h) {
            b->fit_scale_q8 = fit_scale(m, w, h);
            b->fit_w = w;
            b->fit_h = h;
        }
        S = b->fit_scale_q8;
    }
    if (b->cfg.roam_q8) S = mq8(S, 256 - (int32_t)b->cfg.roam_q8);
    if (S < 1) S = 1;

    footprint(m, &rad, &uh);
    fx = (int32_t)rad * S;
    fy = (int32_t)uh  * S;

    // Everything the box has left over, minus an eighth of his own size on
    // each axis. That eighth is the bob-and-lean margin: without it a hop at
    // the top of the stage would put his head through the edge of the tile.
    sx = ((int32_t)w * 256 - fx) / 2 - fx / 8;
    sy = ((int32_t)h * 256 - fy) / 2 - fy / 8;
    b->stage_x_q8 = sx > 0 ? sx : 0;
    b->stage_y_q8 = sy > 0 ? sy : 0;
    clamp_pos(b);
    return S;
}

void eos_buddy_fit(eos_buddy_t *b, uint16_t w, uint16_t h)
{
    if (!b || !b->model || w == 0 || h == 0) return;
    (void)plan_stage(b, w, h);
}

void eos_buddy_stage(const eos_buddy_t *b, int32_t *hx_q8, int32_t *hy_q8)
{
    if (hx_q8) *hx_q8 = b ? b->stage_x_q8 : 0;
    if (hy_q8) *hy_q8 = b ? b->stage_y_q8 : 0;
}

void eos_buddy_pos(const eos_buddy_t *b, int32_t *x_q8, int32_t *y_q8)
{
    if (x_q8) *x_q8 = b ? b->walk_x_q8 : 0;
    if (y_q8) *y_q8 = b ? b->walk_y_q8 : 0;
}

bool eos_buddy_move_to(eos_buddy_t *b, int32_t x_q8, int32_t y_q8)
{
    if (!b) return false;
    b->walk_x_q8 = x_q8;
    b->walk_y_q8 = y_q8;
    clamp_pos(b);
    return b->walk_x_q8 != x_q8 || b->walk_y_q8 != y_q8;
}

// The sum is formed in 64 bits and saturated back, which is not defensive
// padding: at int32 width `walk + delta` can overflow, and signed overflow does
// not merely wrap - it is undefined, and the wrap it happens to produce turns a
// hard push against one edge into a landing on the OPPOSITE one. A buddy
// pressed into the right-hand wall would appear at the left-hand wall on the
// next frame. eos_stroll.c cannot reach it today, because it clamps dt to a
// second and its fastest preset asks for about 3,000 Q8 units a frame, but that
// is an argument about one caller and this is a public entry point.
bool eos_buddy_move_by(eos_buddy_t *b, int32_t dx_q8, int32_t dy_q8)
{
    int64_t x, y;

    if (!b) return false;
    x = (int64_t)b->walk_x_q8 + (int64_t)dx_q8;
    y = (int64_t)b->walk_y_q8 + (int64_t)dy_q8;
    if (x >  INT32_MAX) x =  INT32_MAX;
    if (x <  INT32_MIN) x =  INT32_MIN;
    if (y >  INT32_MAX) y =  INT32_MAX;
    if (y <  INT32_MIN) y =  INT32_MIN;
    return eos_buddy_move_to(b, (int32_t)x, (int32_t)y);
}

void eos_buddy_set_gait(eos_buddy_t *b, int16_t lean_q8, int16_t rise_q8)
{
    if (!b) return;
    b->gait_lean_q8 = lean_q8;
    b->gait_rise_q8 = rise_q8;
}

void eos_buddy_face(eos_buddy_t *b, int32_t off_q8)
{
    if (b) b->face_off_q8 = off_q8;
}

int32_t eos_buddy_facing(const eos_buddy_t *b)
{
    return b ? b->face_off_q8 : 0;
}

// Sorts the pool far to near. The cold pass runs the whole shell-sort gap
// sequence because the pool arrives in (z,y,x) order, which is nowhere near
// depth order; every later frame runs gap 1 only, which on a yaw that moved
// one step is very close to linear.
static void depth_sort(eos_vox_model_t *m, int32_t dx, int32_t dy, int32_t dz, bool cold)
{
    static const uint16_t GAPS[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
    const unsigned NG = sizeof(GAPS) / sizeof(GAPS[0]);
    eos_voxel_t *v = m->v;
    int n = (int)m->count;

    for (unsigned g = cold ? 0 : NG - 1; g < NG; g++) {
        int gap = GAPS[g];
        if (gap >= n) continue;
        for (int i = gap; i < n; i++) {
            eos_voxel_t t = v[i];
            int32_t td = dx * t.x + dy * t.y + dz * t.z;
            int j = i;
            while (j >= gap) {
                const eos_voxel_t *u = &v[j - gap];
                if (dx * u->x + dy * u->y + dz * u->z >= td) break;
                v[j] = v[j - gap];
                j -= gap;
            }
            v[j] = t;
        }
    }
    m->sorted = false;
}

// ceil(a / 256) for signed a, without relying on the sign of a shift.
static int ceil_q8(int32_t a)
{
    return (a >= 0) ? (int)((a + 255) / 256) : -(int)((-a) / 256);
}

typedef struct {
    eos_buddy_target_t *t;
    uint8_t  *p8;
    uint16_t *p16;
    bool      rgb;
} raster_t;

static void put(raster_t *r, int x, int y, uint32_t col, int32_t depth)
{
    uint32_t idx = (uint32_t)y * r->t->w + (uint32_t)x;
    if (r->t->audit_depth) {
        if (depth > r->t->audit_depth[idx]) r->t->audit_violations++;
        r->t->audit_depth[idx] = depth;
        r->t->audit_pixels++;
    }
    if (r->rgb) r->p16[idx] = (uint16_t)col;
    else        r->p8[idx]  = (uint8_t)col;
}

// A voxel face is always a parallelogram, even under the lean, because a
// shear maps parallelograms to parallelograms. Sampling at pixel centres with
// a half-open span means two faces that share an edge share no pixel and
// leave no gap, so adjacent cubes tile exactly.
static void fill_quad(raster_t *r, const int32_t *qx, const int32_t *qy,
                      uint32_t col, int32_t depth)
{
    int32_t ymin = qy[0], ymax = qy[0];
    for (int i = 1; i < 4; i++) {
        if (qy[i] < ymin) ymin = qy[i];
        if (qy[i] > ymax) ymax = qy[i];
    }

    int py0 = ceil_q8(ymin - 128);
    int py1 = ceil_q8(ymax - 128) - 1;
    if (py0 < 0) py0 = 0;
    if (py1 > (int)r->t->h - 1) py1 = (int)r->t->h - 1;

    for (int py = py0; py <= py1; py++) {
        int32_t yc = (int32_t)py * 256 + 128;
        int32_t xlo = 0, xhi = 0;
        int hits = 0;

        for (int i = 0; i < 4; i++) {
            int j = (i + 1) & 3;
            int32_t ax = qx[i], ay = qy[i], bx = qx[j], by = qy[j];
            if (ay == by) continue;
            // Walk the edge from its low end every time. Two faces that share
            // an edge meet it from opposite directions, and integer division
            // truncates toward zero, so interpolating in the traversal order
            // gives the two of them answers one Q8 unit apart. That is how a
            // one pixel hole opens up in the middle of a flat wall.
            if (ay > by) { int32_t t;
                t = ax; ax = bx; bx = t;
                t = ay; ay = by; by = t; }
            if (yc < ay || yc >= by) continue;
            int32_t x = ax + (int32_t)(((int64_t)(bx - ax) * (yc - ay)) / (by - ay));
            if (!hits) { xlo = xhi = x; }
            else { if (x < xlo) xlo = x; if (x > xhi) xhi = x; }
            hits++;
        }
        if (hits < 2) continue;

        int px0 = ceil_q8(xlo - 128);
        int px1 = ceil_q8(xhi - 128) - 1;
        if (px0 < 0) px0 = 0;
        if (px1 > (int)r->t->w - 1) px1 = (int)r->t->w - 1;
        for (int px = px0; px <= px1; px++) put(r, px, py, col, depth);
    }
}

static uint32_t face_colour(const eos_buddy_t *b, const eos_vox_model_t *m,
                            uint8_t ci, int level, bool rgb)
{
    if (!rgb) {
        const uint8_t *lut = b->cfg.shade_lut;
        return lut ? lut[level * 256 + ci] : ci;
    }
    int32_t k = b->cfg.shade[level];
    int32_t rr, gg, bb;
    if (m->pal) {
        rr = m->pal->rgb[ci][0]; gg = m->pal->rgb[ci][1]; bb = m->pal->rgb[ci][2];
    } else {
        uint16_t f = b->cfg.flat_565;
        rr = ((f >> 11) & 0x1F) << 3; gg = ((f >> 5) & 0x3F) << 2; bb = (f & 0x1F) << 3;
    }
    rr = rr * k / 256; gg = gg * k / 256; bb = bb * k / 256;
    return (uint32_t)(((rr & 0xF8) << 8) | ((gg & 0xFC) << 3) | (bb >> 3));
}

// Corner k of a unit cube is (k&1, k>>1&1, k>>2&1) in model x,y,z.
static const uint8_t F_ZP[4] = { 4, 5, 7, 6 };
static const uint8_t F_XP[4] = { 1, 3, 7, 5 };
static const uint8_t F_XN[4] = { 0, 4, 6, 2 };
static const uint8_t F_YP[4] = { 2, 3, 7, 6 };
static const uint8_t F_YN[4] = { 0, 1, 5, 4 };

int eos_buddy_render(eos_buddy_t *b, eos_buddy_target_t *t)
{
    if (!b || !t || !t->pixels || !b->model || t->w == 0 || t->h == 0) return -1;
    eos_vox_model_t *m = b->model;

    raster_t ras;
    ras.t   = t;
    ras.rgb = (t->fmt == EOS_BUDDY_PIX_RGB565);
    ras.p8  = (uint8_t *)t->pixels;
    ras.p16 = (uint16_t *)t->pixels;

    uint32_t npx = (uint32_t)t->w * (uint32_t)t->h;
    t->audit_violations = 0;
    t->audit_pixels     = 0;
    if (t->audit_depth) for (uint32_t i = 0; i < npx; i++) t->audit_depth[i] = INT32_MAX;
    if (t->clear) {
        if (ras.rgb) for (uint32_t i = 0; i < npx; i++) ras.p16[i] = t->bg_565;
        else         memset(ras.p8, t->bg_i8, npx);
    }

    b->faces_drawn = 0;
    if (m->count == 0 || m->sx == 0 || m->sy == 0 || m->sz == 0) return 0;

    // Scale and stage together, and in that order: the space he does not fill
    // is the space he gets to walk in, so the clamp cannot be decided until
    // the fit is.
    int32_t S = plan_stage(b, t->w, t->h);

    int yi = (int)(((b->yaw_q8 + 128) >> 8) & (EOS_BUDDY_YAW_STEPS - 1));
    int32_t sn = SIN_Q12[yi];
    int32_t cs = SIN_Q12[(yi + EOS_BUDDY_YAW_STEPS / 4) & (EOS_BUDDY_YAW_STEPS - 1)];

    // The mood's lean and bob, plus the gait's. One oscillator drives the two
    // gait terms and they arrive here already summed with nothing else, so
    // adding them is the whole of how a waddle reaches the rasteriser.
    int32_t shear = (int32_t)b->shear_q8 + (int32_t)b->gait_lean_q8;
    int32_t lift  = (int32_t)b->bob_q8   + (int32_t)b->gait_rise_q8;

    int32_t kxy = 256 + b->squash_q8 / 2;   // squash widens as it flattens
    int32_t kz  = 256 - b->squash_q8;
    if (kxy < 64) kxy = 64;
    if (kz  < 64) kz  = 64;
    int32_t Sxy = mq8((int32_t)S, kxy);
    int32_t Sz  = mq8((int32_t)S, kz);

    // Screen delta per +1 along each model axis, in Q8 pixels.
    int32_t ex_x = mq12(Sxy, cs);
    int32_t ex_y = mq12(mq12(Sxy, sn), SIN_PHI);
    int32_t ey_x = mq12(Sxy, sn);
    int32_t ey_y = -mq12(mq12(Sxy, cs), SIN_PHI);
    int32_t ez_y = -mq12(Sz, COS_PHI);
    int32_t ez_x = (int32_t)(((int64_t)Sxy * shear) / (256 * (int32_t)m->sz));

    // Depth along the view axis. Larger is further from the camera.
    int32_t dpx = -mq12(sn, COS_PHI);
    int32_t dpy =  mq12(cs, COS_PHI);
    int32_t dpz = -SIN_PHI;

    int32_t cx8 = ((int32_t)m->sx * 256) / 2;
    int32_t cy8 = ((int32_t)m->sy * 256) / 2;
    int32_t cz8 = ((int32_t)m->sz * 256) / 2;
    int32_t bob = (int32_t)(((int64_t)lift * S) / 256);

    int32_t ox = (int32_t)t->w * 128 - mq8(cx8, ex_x) - mq8(cy8, ey_x) - mq8(cz8, ez_x)
                 + b->walk_x_q8;
    int32_t oy = (int32_t)t->h * 128 - mq8(cx8, ex_y) - mq8(cy8, ey_y) - mq8(cz8, ez_y)
                 - bob + b->walk_y_q8;

    int32_t cofx[8], cofy[8];
    for (int k = 0; k < 8; k++) {
        int a = k & 1, c = (k >> 1) & 1, e = (k >> 2) & 1;
        cofx[k] = a * ex_x + c * ey_x + e * ez_x;
        cofy[k] = a * ex_y + c * ey_y + e * ez_y;
    }

    // At most three faces of any voxel can face the camera: the top, one x
    // side and one y side. Which ones is a property of the yaw, not of the
    // voxel, so it is decided once per frame.
    struct { uint8_t bit; uint8_t level; const uint8_t *c; } vis[3];
    int nvis = 0;
    vis[nvis].bit = EOS_VOX_FACE_ZP; vis[nvis].level = 0; vis[nvis].c = F_ZP; nvis++;
    if (dpy < 0)      { vis[nvis].bit = EOS_VOX_FACE_YP; vis[nvis].level = 1; vis[nvis].c = F_YP; nvis++; }
    else if (dpy > 0) { vis[nvis].bit = EOS_VOX_FACE_YN; vis[nvis].level = 1; vis[nvis].c = F_YN; nvis++; }
    if (dpx < 0)      { vis[nvis].bit = EOS_VOX_FACE_XP; vis[nvis].level = 2; vis[nvis].c = F_XP; nvis++; }
    else if (dpx > 0) { vis[nvis].bit = EOS_VOX_FACE_XN; vis[nvis].level = 2; vis[nvis].c = F_XN; nvis++; }

    depth_sort(m, dpx, dpy, dpz, m->sorted);

    bool shut = eos_buddy_blinking(b);
    uint8_t eye = b->cfg.eye_ci, lid = b->cfg.eye_shut_ci;

    for (int i = 0; i < (int)m->count; i++) {
        const eos_voxel_t *v = &m->v[i];
        int32_t X = v->x, Y = v->y, Z = v->z;
        int32_t depth = dpx * X + dpy * Y + dpz * Z;
        int32_t px = ox + X * ex_x + Y * ey_x + Z * ez_x;
        int32_t py = oy + X * ex_y + Y * ey_y + Z * ez_y;

        uint8_t ci = v->ci;
        if (shut && eye && ci == eye) ci = lid;

        for (int f = 0; f < nvis; f++) {
            if (!(v->faces & vis[f].bit)) continue;
            const uint8_t *cc = vis[f].c;
            int32_t qx[4], qy[4];
            for (int k = 0; k < 4; k++) {
                qx[k] = px + cofx[cc[k]];
                qy[k] = py + cofy[cc[k]];
            }
            fill_quad(&ras, qx, qy, face_colour(b, m, ci, vis[f].level, ras.rgb), depth);
            b->faces_drawn++;
        }
    }
    return (int)b->faces_drawn;
}

void eos_buddy_build_shade_lut(const eos_vox_pal_t *vox_pal,
                               const uint8_t *disp_rgb, int disp_n,
                               const uint8_t shade[3], uint8_t out[768])
{
    if (!vox_pal || !disp_rgb || disp_n <= 0 || !shade || !out) return;
    if (disp_n > 256) disp_n = 256;

    for (int lvl = 0; lvl < 3; lvl++) {
        int32_t k = shade[lvl];
        for (int ci = 0; ci < 256; ci++) {
            int32_t r = vox_pal->rgb[ci][0] * k / 256;
            int32_t g = vox_pal->rgb[ci][1] * k / 256;
            int32_t b = vox_pal->rgb[ci][2] * k / 256;
            int32_t best = 0; int32_t bestd = 0;
            for (int j = 0; j < disp_n; j++) {
                int32_t dr = r - disp_rgb[j * 3 + 0];
                int32_t dg = g - disp_rgb[j * 3 + 1];
                int32_t db = b - disp_rgb[j * 3 + 2];
                int32_t d = dr * dr + dg * dg + db * db;
                if (j == 0 || d < bestd) { bestd = d; best = j; }
            }
            out[lvl * 256 + ci] = (uint8_t)best;
        }
    }
}
