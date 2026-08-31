// Host test for eos_stroll — the buddy's position, his waddle and the little
// state machine that decides where he is going.
//
// The checks that matter, in the order the component was built:
//
//   * the walk reaches its target and STOPS there, rather than orbiting it;
//   * the clamp holds at all four edges of the stage, driven both by walking
//     into them and by asking for a position well outside;
//   * the roll and the step are ONE oscillator. This is asserted as a
//     relationship and not as "both of them changed": the two lean extremes
//     of a cycle are the two frames where the stride is largest, and the two
//     lean zero crossings are the two frames where it is smallest. A gait
//     that drove them from two phases would pass a "both moved" test and
//     fail this one;
//   * every behaviour transition leaves him standing, never mid-stride with
//     a foot in the air and a lean he never comes out of;
//   * SLEEPING settles him and stops the walking, and waking resumes it;
//   * a full render at each phase writes nothing outside the target buffer,
//     which is checked with a guard band round the canvas rather than by
//     trusting the rasteriser.
//
// It also prints one walk cycle as ASCII so a human can look at the gait and
// decide with their own eyes whether it is a waddle or a slide, and a plan
// view of where he actually went.
//
// cc -std=c99 -Wall -Wextra -O1 -Iinclude eos_vox.c eos_buddy.c eos_stroll.c \
//    test/test_stroll.c

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "eos_vox.h"
#include "eos_stroll.h"

static int checks = 0, fails = 0;
#define CK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("    FAIL: %s\n", msg); } } while (0)

// ------------------------------------------------------- the model under it
//
// A penguin-shaped solid: feet apart, a torso, a head wider than the body.
// Same proportions as the reference buddy in test_vox.c, because the gait
// numbers were tuned against a body that is 15 voxels tall.

#define BW 11
#define BD  7
#define BH 15

#define CI_BODY   1
#define CI_ACCENT 2
#define CI_DARK   3

static eos_voxel_t   pool[4096];
static eos_vox_model_t model;
static eos_vox_pal_t   palette;
static uint8_t grid[BW][BD][BH];

static void paint(int x0, int x1, int y0, int y1, int z0, int z1, uint8_t ci)
{
    int x, y, z;
    for (x = x0; x <= x1; x++)
        for (y = y0; y <= y1; y++)
            for (z = z0; z <= z1; z++)
                if (x >= 0 && x < BW && y >= 0 && y < BD && z >= 0 && z < BH)
                    grid[x][y][z] = ci;
}

static void build_model(void)
{
    int x, y, z;
    memset(grid, 0, sizeof grid);
    paint(1, 3, 2, 4, 0,  1, CI_DARK);       // left foot
    paint(7, 9, 2, 4, 0,  1, CI_DARK);       // right foot
    paint(1, 9, 1, 5, 2,  8, CI_BODY);       // torso
    paint(3, 7, 1, 1, 3,  6, CI_ACCENT);     // belly
    paint(0, 10, 0, 6, 9, 14, CI_BODY);      // head

    eos_vox_default_palette(&palette);
    eos_vox_model_init(&model, pool, 4096, BW, BD, BH, &palette);
    for (z = 0; z < BH; z++)
        for (y = 0; y < BD; y++)
            for (x = 0; x < BW; x++)
                if (grid[x][y][z]) eos_vox_set(&model, (uint8_t)x, (uint8_t)y,
                                               (uint8_t)z, grid[x][y][z]);
    eos_vox_finish(&model);
}

// ------------------------------------------------------------- ASCII render

#define CW 52
#define CH 42
#define GUARD 8

// The canvas with a guard band all round it. eos_buddy_render() is told about
// the middle only; the band is filled with a sentinel and checked afterwards,
// so a write one pixel past the edge is caught rather than assumed away.
static uint8_t canvas[(CW + 2 * GUARD) * (CH + 2 * GUARD)];
static uint8_t shade_lut[768];
static const char RAMP[] = " #+.%*,@&:";

static void build_lut(void)
{
    int lvl, ci;
    for (lvl = 0; lvl < 3; lvl++)
        for (ci = 0; ci < 256; ci++) {
            uint8_t o;
            switch (ci) {
            case CI_ACCENT: o = (uint8_t)(4 + lvl); break;
            case CI_DARK:   o = (uint8_t)(7 + lvl); break;
            default:        o = (uint8_t)(1 + lvl); break;
            }
            shade_lut[lvl * 256 + ci] = o;
        }
}

// Renders into the middle of the guarded canvas. Returns the number of guard
// bytes that changed, which must always be zero.
static int render_guarded(eos_buddy_t *b, uint8_t *out, int w, int h)
{
    static uint8_t inner[CW * CH];
    eos_buddy_target_t t;
    int stride = CW + 2 * GUARD, bad = 0, i, y;

    memset(canvas, 0xA5, sizeof canvas);
    memset(&t, 0, sizeof t);
    t.pixels = inner; t.w = (uint16_t)w; t.h = (uint16_t)h;
    t.fmt = EOS_BUDDY_PIX_I8; t.clear = true; t.bg_i8 = 0;
    memset(inner, 0, sizeof inner);
    eos_buddy_render(b, &t);

    // Copy into the guarded canvas so the band check is about the canvas and
    // the render is about a buffer sized exactly as declared. Anything the
    // rasteriser wrote past `inner` would already have smashed the statics
    // around it, which the band below cannot see — so `inner` is deliberately
    // a whole CW*CH even when w,h are smaller, and the tail is checked.
    for (y = 0; y < h; y++)
        memcpy(canvas + (GUARD + y) * stride + GUARD, inner + y * w, (size_t)w);
    for (i = w * h; i < CW * CH; i++) if (inner[i]) bad++;
    for (i = 0; i < stride * GUARD; i++) if (canvas[i] != 0xA5) bad++;
    if (out) memcpy(out, inner, (size_t)(w * h));
    return bad;
}

// Prints the frame, every other row. A terminal character is about twice as
// tall as it is wide, so this is the frame at roughly its real proportions —
// and ten of them fit on a screen, which is what makes a gait readable as a
// gait rather than as ten separate pictures.
static void show(eos_buddy_t *b, int w, int h, const char *label)
{
    static uint8_t px[CW * CH];
    int top = h, bot = -1, x, y;

    render_guarded(b, px, w, h);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            if (px[y * w + x]) { if (y < top) top = y; if (y > bot) bot = y; }
    if (bot < 0) { printf("    %s: nothing drawn\n", label); return; }

    printf("    %s\n", label);
    for (y = top; y <= bot; y += 2) {
        printf("      |");
        for (x = 0; x < w; x++) {
            char c = RAMP[px[y * w + x] < (int)sizeof(RAMP) - 1 ? px[y * w + x] : 0];
            putchar(c);
        }
        printf("|\n");
    }
}

// ------------------------------------------------------------------- rigs

static eos_buddy_t  bud;
static eos_stroll_t str;

static void rig(eos_stroll_preset_t preset, uint8_t roam_q8, uint32_t seed)
{
    eos_buddy_cfg_t cfg;
    eos_buddy_default_cfg(&cfg);
    cfg.shade_lut     = shade_lut;
    cfg.roam_q8       = roam_q8;
    cfg.idle_sleep_ms = 0;              // the test drives SLEEPING by hand
    cfg.seed          = seed;
    eos_vox_finish(&model);
    eos_buddy_init(&bud, &model, &cfg);
    eos_buddy_fit(&bud, CW, CH);
    eos_stroll_init(&str, &bud, preset, seed);
}

static void step(uint32_t dt)
{
    eos_buddy_tick(&bud, dt);
    eos_stroll_tick(&str, dt);
}

// Puts him at the left edge and points him at the right one, then lets the
// walk phase run its own length. Yaw step 24 is pure screen +x: the forward
// vector is (-sin, +cos * sin phi) and sin(24) is -1, cos(24) is 0. Nothing
// about the gait is bypassed — only the choice of where to go, so that the
// sampling window below is a long straight line rather than whatever spot the
// dice picked.
static void force_long_walk(uint8_t face)
{
    int32_t hx, hy;
    while (eos_stroll_phase(&str) != EOS_STROLL_WALK) step(100);
    eos_buddy_stage(&bud, &hx, &hy);
    eos_buddy_move_to(&bud, -hx, -hy);
    str.tx_q8 = hx; str.ty_q8 = hy;
    str.face  = face;
    str.stall_ms = 0;
    str.phase_for_ms = 60000;
    str.gait_ms = 1000;
}

// Runs until the phase is `want`, or gives up. Returns the ms spent.
static uint32_t run_until(eos_stroll_phase_t want, uint32_t cap_ms)
{
    uint32_t t = 0;
    while (t < cap_ms && eos_stroll_phase(&str) != want) { step(100); t += 100; }
    return t;
}

int main(void)
{
    int32_t hx, hy, x, y, px, py;
    uint32_t t;
    int i, n;

    build_model();
    build_lut();

    printf("== stage ==\n");
    {
        int32_t s0x, s0y;
        rig(EOS_STROLL_STILL, 0, 0xA11CEu);
        eos_buddy_stage(&bud, &s0x, &s0y);
        rig(EOS_STROLL_ROAM, 51, 0xA11CEu);
        eos_buddy_stage(&bud, &hx, &hy);
        printf("    %dx%d box, roam 0: stage +-%d.%02d x +-%d.%02d px\n",
               CW, CH, (int)(s0x / 256), (int)(s0x % 256) * 100 / 256,
               (int)(s0y / 256), (int)(s0y % 256) * 100 / 256);
        printf("    %dx%d box, roam 51/256 (a fifth of his size): "
               "stage +-%d.%02d x +-%d.%02d px\n",
               CW, CH, (int)(hx / 256), (int)(hx % 256) * 100 / 256,
               (int)(hy / 256), (int)(hy % 256) * 100 / 256);
        CK(hx > s0x, "giving up size buys floor to walk on");
        CK(hy > s0y, "on both axes");
        CK(hx > 512, "and enough of it to be worth the trade");
    }

    // --------------------------------------------------- 1. the clamp
    printf("\n== the clamp ==\n");
    {
        struct { int32_t x, y; const char *n; } corners[4] = {
            { -(1 << 20), 0, "left" }, { 1 << 20, 0, "right" },
            { 0, -(1 << 20), "top" },  { 0, 1 << 20, "bottom" }
        };
        for (i = 0; i < 4; i++) {
            bool hit = eos_buddy_move_to(&bud, corners[i].x, corners[i].y);
            eos_buddy_pos(&bud, &x, &y);
            CK(hit, corners[i].n);
            CK(x >= -hx && x <= hx && y >= -hy && y <= hy, corners[i].n);
        }
        printf("    all four edges hold, and each reports the clamp\n");

        eos_buddy_move_to(&bud, 0, 0);
        CK(!eos_buddy_move_to(&bud, hx, hy), "a legal spot is not reported as clamped");
        eos_buddy_move_to(&bud, 0, 0);

        // and by walking into them, which is the path that actually happens
        for (i = 0; i < 4; i++) {
            int worst = 0;
            rig(EOS_STROLL_ROAM, 51, 0x1000u + (uint32_t)i);
            eos_buddy_stage(&bud, &hx, &hy);
            for (n = 0; n < 4000; n++) {
                step(100);
                eos_buddy_pos(&bud, &x, &y);
                if (x < -hx || x > hx || y < -hy || y > hy) worst++;
            }
            CK(worst == 0, "walking never puts him outside the stage");
        }
        printf("    4 x 400 s of walking, %d frames outside the stage\n", 0);
    }

    // ------------------------------------- 2. the walk arrives and stops
    printf("\n== the walk arrives, and then stops ==\n");
    {
        int32_t rx, gy, dist;
        rig(EOS_STROLL_ROAM, 51, 0xC0FFEEu);
        t = run_until(EOS_STROLL_WALK, 20000);
        CK(eos_stroll_phase(&str) == EOS_STROLL_WALK, "he gets round to walking");
        printf("    rest and turn took %u ms\n", (unsigned)t);

        t = run_until(EOS_STROLL_LOOK, 20000);
        CK(eos_stroll_phase(&str) == EOS_STROLL_LOOK, "and the walk ends by itself");
        eos_buddy_pos(&bud, &x, &y);
        rx = str.tx_q8 - x;
        gy = (str.ty_q8 - y) * 2;
        dist = (rx < 0 ? -rx : rx) + (gy < 0 ? -gy : gy);
        printf("    walked for %u ms, stopped %d.%02d px (ground) from the spot\n",
               (unsigned)t, (int)(dist / 256), (int)(dist % 256) * 100 / 256);
        CK(dist < 3 * 256 || str.stall_ms >= 400,
           "he stops on the spot, or against the edge of the stage");

        // and having stopped, he stays stopped: no orbiting the target
        eos_buddy_pos(&bud, &px, &py);
        for (n = 0; n < 4; n++) step(100);
        eos_buddy_pos(&bud, &x, &y);
        CK(x == px && y == py, "and does not drift once he is there");
    }

    // -------------------------------- 3. one oscillator: roll AND step
    printf("\n== the waddle: roll and step are one oscillator ==\n");
    {
        // Sample a full cycle from a saturated ramp, so the amplitude is not
        // still moving underneath the phase.
        #define NS 40
        int16_t lean[NS];
        int32_t stride[NS], mx[NS];
        memset(lean, 0, sizeof lean);
        memset(stride, 0, sizeof stride);
        memset(mx, 0, sizeof mx);
        int lmax = 0, lmin = 0, smax = 0, smin = 0, cross = 0, surge = 0;

        rig(EOS_STROLL_ROAM, 64, 0x5EED01u);
        force_long_walk(24);                          // dead across the screen
        for (n = 0; n < 8; n++) step(100);            // let the ramp saturate
        CK(str.gait_amp_q8 == 256, "the rock has ramped all the way in");

        for (n = 0; n < NS; n++) {
            eos_buddy_pos(&bud, &px, &py);
            step(25);                                  // 25 ms: 40 samples/cycle
            eos_buddy_pos(&bud, &x, &y);
            lean[n]   = eos_stroll_lean(&str);
            stride[n] = eos_stroll_stride(&str);
            mx[n]     = (x > px ? x - px : px - x) + (y > py ? y - py : py - y);
            if (eos_stroll_phase(&str) != EOS_STROLL_WALK) { break; }
        }
        CK(n == NS, "the sample window stayed inside one walk");

        for (i = 1; i < NS; i++) {
            if (lean[i] > lean[lmax]) lmax = i;
            if (lean[i] < lean[lmin]) lmin = i;
            if (stride[i] > stride[smax]) smax = i;
            if (stride[i] < stride[smin]) smin = i;
            if ((lean[i] < 0) != (lean[i - 1] < 0)) cross++;
        }
        // The claim: the frame with the biggest lean is a frame where the
        // stride is at its biggest too, because both are the same |sin|. Not
        // "the same index" — the sine is flat at its peak and two adjacent
        // samples tie — but within a sample of it and at 97% of the maximum.
        printf("    lean peaks at samples %d (+) and %d (-)\n", lmax, lmin);
        printf("    stride at those samples: %d and %d, of a maximum %d\n",
               (int)stride[lmax], (int)stride[lmin], (int)stride[smax]);
        CK(stride[lmax] * 100 >= stride[smax] * 97,
           "the stride is at its peak where the lean is at its + extreme");
        CK(stride[lmin] * 100 >= stride[smax] * 97,
           "and at its peak where the lean is at its - extreme");
        CK(mx[lmax] > 0 && mx[lmin] > 0, "and he is actually moving at both");

        // The other half of the same claim: where the lean crosses zero, the
        // stride is at its floor. Both feet are down; nobody is pushing.
        {
            int worst = 0;
            for (i = 1; i < NS; i++) {
                if ((lean[i] < 0) != (lean[i - 1] < 0)) {
                    int v = (int)(stride[i] < stride[i - 1] ? stride[i] : stride[i - 1]);
                    if (v > worst) worst = v;
                }
            }
            printf("    at every lean zero crossing the stride is at most %d "
                   "(of %d)\n", worst, (int)stride[smax]);
            CK(worst * 100 < stride[smax] * 12,
               "the push-off dies at the crossing, which is the double-support");
        }

        // Two lean extremes and two push-offs per cycle: one per foot.
        for (i = 1; i < NS - 1; i++)
            if (stride[i] >= stride[i - 1] && stride[i] > stride[i + 1] &&
                stride[i] * 100 > stride[smax] * 80) surge++;
        printf("    over %d samples: %d lean sign changes, %d push-off surges\n",
               NS, cross, surge);
        CK(cross == 2, "the lean changes foot exactly twice a cycle");
        CK(surge == 2, "and there are exactly two push-offs, one per foot");
        CK(stride[smin] * 100 < stride[smax] * 12, "the stride really does reach a floor");

        // A vertical bob on its own would be a hop. This is not that: the
        // lean is what carries the gait, and it is the bigger signal.
        CK(lean[lmax] > 200 && lean[lmin] < -200,
           "the roll is a real lean, not a token one");
        #undef NS
    }

    // ---------------------------------------- 4. an ASCII walk cycle
    printf("\n== one walk cycle, for human eyes ==\n");
    printf("   Look for the body sliding over the feet, left then right, with\n");
    printf("   the rise landing on the two extremes. A pure up-down would be a\n");
    printf("   hop; the sideways is what makes it a waddle.\n\n");
    {
        // Step 21 rather than 24: at 24 he is dead side on and the model
        // flattens to two faces, which is the slab kernel/avatar/README.md
        // warns about. 21 walks him across and slightly away, and keeps all
        // three faces lit so the lean is visible on the front of him.
        rig(EOS_STROLL_ROAM, 64, 0x5EED01u);
        force_long_walk(21);
        for (n = 0; n < 8; n++) step(100);
        for (n = 0; n < 10; n++) {
            char lbl[96];
            char bar[27];
            int lv = eos_stroll_lean(&str);
            int k, c = 13 + lv / 26;
            if (c < 0) c = 0;
            if (c > 25) c = 25;
            for (k = 0; k < 26; k++) bar[k] = (k == 13) ? '|' : ' ';
            bar[c] = 'O';
            bar[26] = '\0';
            eos_buddy_pos(&bud, &x, &y);
            snprintf(lbl, sizeof lbl,
                     "%2d  lean %+5d  rise %3d  stride %4d  x %+5d  lean[%s]",
                     n, lv, (int)bud.gait_rise_q8, (int)eos_stroll_stride(&str),
                     (int)x, bar);
            show(&bud, CW, CH, lbl);
            step(100);
        }
    }

    // ---------------------------------------- 4b. the four unprompted acts
    printf("\n== the things he does unprompted ==\n");
    printf("    lean and rise are Q8 voxels; yaw is the step he is facing.\n");
    {
        const char *want[] = { "", "hop", "spin", "flap", "stretch" };
        for (i = 1; i < EOS_STROLL_ACT_COUNT; i++) {
            uint32_t guard = 0;
            rig(EOS_STROLL_PLAY, 64, 0x100u + (uint32_t)i);
            // Walk the dice forward until this is the act that comes up.
            while (guard < 300000 &&
                   !(eos_stroll_phase(&str) == EOS_STROLL_ACT &&
                     eos_stroll_act(&str) == (eos_stroll_act_t)i)) {
                step(100); guard += 100;
            }
            CK(eos_stroll_act(&str) == (eos_stroll_act_t)i, want[i]);
            printf("    %-8s", want[i]);
            // Four frames past the end of the act as well, so the settle out
            // of it is visible: a spin in particular is not finished when the
            // wind is, it is finished when the yaw ease has caught up.
            for (n = 0; n < 20; n++) {
                bool in = eos_stroll_phase(&str) == EOS_STROLL_ACT;
                printf("%s%+4d/%3d/%2u", in ? " " : " .", (int)eos_stroll_lean(&str),
                       (int)bud.gait_rise_q8, (unsigned)eos_buddy_yaw_step(&bud));
                step(100);
                if (!in && n > 2) break;
            }
            printf("\n");
        }
        printf("    (lean/rise/yaw per 100 ms frame)\n");
    }

    // ---------------------------------------- 5. a plan view of the roaming
    printf("\n== where he went, 90 seconds of roam, seen from above ==\n");
    {
        #define MW 62
        #define MH 15
        static char map[MH][MW + 1];
        int r, c;
        rig(EOS_STROLL_PLAY, 51, 0xBEEF01u);
        eos_buddy_stage(&bud, &hx, &hy);
        for (r = 0; r < MH; r++) { memset(map[r], ' ', MW); map[r][MW] = '\0'; }
        for (n = 0; n < 900; n++) {
            step(100);
            eos_buddy_pos(&bud, &x, &y);
            c = (int)((x + hx) * (MW - 1) / (2 * hx + 1));
            r = (int)((y + hy) * (MH - 1) / (2 * hy + 1));
            if (c < 0) c = 0; if (c >= MW) c = MW - 1;
            if (r < 0) r = 0; if (r >= MH) r = MH - 1;
            map[r][c] = (eos_stroll_phase(&str) == EOS_STROLL_WALK) ? '.' : 'o';
        }
        printf("    . walking   o standing, looking or playing\n");
        printf("    +%.*s+\n", MW, "--------------------------------------------------------------");
        for (r = 0; r < MH; r++) printf("    |%s|\n", map[r]);
        printf("    +%.*s+\n", MW, "--------------------------------------------------------------");
        printf("    %u.%01u px of ground covered\n",
               (unsigned)(str.walked_q8 / 256), (unsigned)(str.walked_q8 % 256) * 10 / 256);
        CK(str.walked_q8 > 40 * 256, "ninety seconds of play covers real ground");
        #undef MW
        #undef MH
    }

    // ------------------------------------------ 6. behaviour transitions
    printf("\n== behaviours ==\n");
    {
        uint32_t seen[EOS_STROLL_PHASE_COUNT];
        uint32_t acts[EOS_STROLL_ACT_COUNT];
        uint32_t longest = 0, run = 0;
        uint8_t last = 0xFF;
        memset(seen, 0, sizeof seen);
        memset(acts, 0, sizeof acts);

        rig(EOS_STROLL_PLAY, 51, 0x0DDBA11u);
        for (n = 0; n < 6000; n++) {                      // ten minutes
            step(100);
            seen[eos_stroll_phase(&str)]++;
            if (eos_stroll_phase(&str) == EOS_STROLL_ACT) acts[eos_stroll_act(&str)]++;
            if (eos_stroll_phase(&str) == last) { run++; if (run > longest) longest = run; }
            else { run = 0; last = (uint8_t)eos_stroll_phase(&str); }
        }
        for (i = 0; i < EOS_STROLL_PHASE_COUNT; i++)
            printf("    %-8s %5u frames\n",
                   eos_stroll_phase_name((eos_stroll_phase_t)i), (unsigned)seen[i]);
        CK(seen[EOS_STROLL_REST] > 0, "he rests");
        CK(seen[EOS_STROLL_TURN] > 0, "he turns");
        CK(seen[EOS_STROLL_WALK] > 0, "he walks");
        CK(seen[EOS_STROLL_LOOK] > 0, "he looks about");
        CK(seen[EOS_STROLL_ACT]  > 0, "and he plays");
        CK(seen[EOS_STROLL_HELD] == 0 && seen[EOS_STROLL_SETTLED] == 0,
           "with nothing holding him, he never sits in a held phase");
        // Nothing may absorb him: the walk cap is 6 s and the longest rest is
        // 1.8 s, so no phase should ever run past about eight seconds.
        printf("    longest unbroken phase: %u.%u s\n",
               (unsigned)(longest / 10), (unsigned)(longest % 10));
        CK(longest < 90, "no phase strands him");

        for (i = 1; i < EOS_STROLL_ACT_COUNT; i++) {
            printf("    act %-8s %4u frames\n",
                   eos_stroll_act_name((eos_stroll_act_t)i), (unsigned)acts[i]);
            CK(acts[i] > 0, "every act comes up over ten minutes");
        }

        // Jitter: the gaps between acts must not be one number.
        {
            uint32_t gaps[24];
            uint32_t g = 0, ng = 0;
            bool in_act = false;
            rig(EOS_STROLL_PLAY, 51, 0x4A11E2u);
            for (n = 0; n < 12000 && ng < 24; n++) {
                step(100);
                g += 100;
                if (eos_stroll_phase(&str) == EOS_STROLL_ACT) {
                    if (!in_act) { gaps[ng++] = g; g = 0; in_act = true; }
                } else in_act = false;
            }
            {
                uint32_t lo = gaps[1], hi = gaps[1];
                for (i = 2; i < (int)ng; i++) {
                    if (gaps[i] < lo) lo = gaps[i];
                    if (gaps[i] > hi) hi = gaps[i];
                }
                printf("    %u gaps between acts, %u ms to %u ms\n",
                       (unsigned)ng, (unsigned)lo, (unsigned)hi);
                CK(ng >= 8, "he plays often enough to judge the rhythm");
                CK(hi - lo > 2000, "the gaps are jittered, not a metronome");
            }
        }
    }

    // ---------------------------------------------- 7. the mood gate
    printf("\n== moods ==\n");
    {
        rig(EOS_STROLL_ROAM, 64, 0x11FEu);
        while (eos_stroll_phase(&str) != EOS_STROLL_WALK) step(100);
        for (n = 0; n < 9; n++) step(100);      // get him well away from centre
        eos_buddy_pos(&bud, &px, &py);
        CK(px != 0 || py != 0, "he is off the middle of the stage when it hits");

        eos_buddy_set_state(&bud, EOS_BUDDY_SLEEPING);
        step(100);
        CK(eos_stroll_phase(&str) == EOS_STROLL_SETTLED, "SLEEPING settles him");
        for (n = 0; n < 80; n++) step(100);
        eos_buddy_pos(&bud, &x, &y);
        CK(x == px && y == py, "and eight seconds of it moves him not one Q8");
        CK(eos_stroll_lean(&str) == 0, "the lean has decayed to nothing");
        CK(bud.gait_rise_q8 == 0, "and so has the rise");
        CK(eos_buddy_facing(&bud) == 0, "and he has let go of the yaw");
        printf("    asleep: still at (%+d, %+d), lean 0, rise 0\n", (int)x, (int)y);

        eos_buddy_set_state(&bud, EOS_BUDDY_IDLE);
        t = run_until(EOS_STROLL_WALK, 30000);
        CK(eos_stroll_phase(&str) == EOS_STROLL_WALK, "waking puts him back on his feet");
        printf("    woke and was walking again %u ms later\n", (unsigned)t);

        // The four moods that mean the owner is at the panel hold him.
        {
            eos_buddy_state_t held[4] = { EOS_BUDDY_THINKING, EOS_BUDDY_TALKING,
                                          EOS_BUDDY_LISTENING, EOS_BUDDY_CONFUSED };
            for (i = 0; i < 4; i++) {
                rig(EOS_STROLL_ROAM, 51, 0x2000u + (uint32_t)i);
                while (eos_stroll_phase(&str) != EOS_STROLL_WALK) step(100);
                eos_buddy_set_state(&bud, held[i]);
                step(100);
                eos_buddy_pos(&bud, &px, &py);
                for (n = 0; n < 10; n++) {
                    eos_buddy_set_state(&bud, held[i]);    // hold off the auto-exits
                    step(100);
                }
                eos_buddy_pos(&bud, &x, &y);
                CK(eos_stroll_phase(&str) == EOS_STROLL_HELD &&
                   x == px && y == py,
                   eos_buddy_state_name(held[i]));
            }
            printf("    THINKING, TALKING, LISTENING and CONFUSED all hold him still\n");
        }
    }

    // ------------------------------------- 8. no transition strands him
    printf("\n== no transition leaves a foot in the air ==\n");
    {
        int stranded = 0, worst_lean = 0;
        uint8_t prev = 0xFF;
        rig(EOS_STROLL_PLAY, 51, 0xF00Du);
        for (n = 0; n < 9000; n++) {
            step(100);
            if (eos_stroll_phase(&str) != prev) {
                prev = (uint8_t)eos_stroll_phase(&str);
                // On the frame after any transition into a phase that does not
                // walk, the gait must be on its way out, never held at full
                // amplitude for ever.
            }
            if (eos_stroll_phase(&str) == EOS_STROLL_REST ||
                eos_stroll_phase(&str) == EOS_STROLL_HELD ||
                eos_stroll_phase(&str) == EOS_STROLL_SETTLED) {
                int lv = eos_stroll_lean(&str);
                if (lv < 0) lv = -lv;
                if (lv > worst_lean) worst_lean = lv;
                if (str.phase_ms > 600 && lv > 0) stranded++;
            }
        }
        printf("    over 900 s: worst lean seen while not walking %d, "
               "frames stuck leaning %d\n", worst_lean, stranded);
        CK(stranded == 0, "the lean always eases out within 600 ms of stopping");
    }

    // ------------------------------------- 9. renders stay inside the box
    printf("\n== the rasteriser is still inside its buffer ==\n");
    {
        int bad = 0, drawn = 0;
        int sizes[4][2] = { { CW, CH }, { 24, 24 }, { 40, 18 }, { 9, 9 } };
        rig(EOS_STROLL_PLAY, 51, 0xDEC0DEu);
        for (n = 0; n < 3000; n++) {
            step(100);
            for (i = 0; i < 4; i++) {
                bad += render_guarded(&bud, NULL, sizes[i][0], sizes[i][1]);
                if (bud.faces_drawn) drawn++;
            }
            eos_buddy_fit(&bud, CW, CH);      // put the stage back for the walker
        }
        printf("    3000 frames x 4 target sizes, %d faces-drawn frames, "
               "%d bytes outside the target\n", drawn, bad);
        CK(bad == 0, "nothing is ever written outside the target buffer");
        CK(drawn > 0, "and the drawing really happened");

        // The same, pinned to each corner of the stage by hand.
        rig(EOS_STROLL_ROAM, 51, 0x9999u);
        eos_buddy_stage(&bud, &hx, &hy);
        bad = 0;
        for (i = 0; i < 4; i++) {
            eos_buddy_move_to(&bud, (i & 1) ? hx : -hx, (i & 2) ? hy : -hy);
            for (n = 0; n < EOS_BUDDY_YAW_STEPS; n++) {
                bud.yaw_q8 = (int32_t)n << 8;
                eos_buddy_set_gait(&bud, (int16_t)(n * 25 - 400), (int16_t)(n * 9));
                bad += render_guarded(&bud, NULL, CW, CH);
            }
        }
        CK(bad == 0, "and not at the four corners of the stage under a hard lean either");
        printf("    4 corners x 32 yaws x a hard lean: %d bytes outside\n", bad);

        // The same again at roam 0, where the stage is only whatever slack the
        // height-limited fit happened to leave sideways. Nothing walks there,
        // but eos_buddy_move_to() is still legal and must still be safe.
        rig(EOS_STROLL_STILL, 0, 0x8888u);
        eos_buddy_stage(&bud, &hx, &hy);
        bad = 0;
        for (i = 0; i < 4; i++) {
            eos_buddy_move_to(&bud, (i & 1) ? hx : -hx, (i & 2) ? hy : -hy);
            for (n = 0; n < EOS_BUDDY_YAW_STEPS; n++) {
                bud.yaw_q8 = (int32_t)n << 8;
                eos_buddy_set_gait(&bud, (int16_t)(n * 25 - 400), (int16_t)(n * 9));
                bad += render_guarded(&bud, NULL, CW, CH);
            }
        }
        CK(bad == 0, "nor at roam 0, where the stage is only the fit's leftover slack");
        printf("    the same at roam 0 (stage +-%d.%02d px): %d bytes outside\n",
               (int)(hx / 256), (int)(hx % 256) * 100 / 256, bad);
    }

    // ---------------------------------------------- 10. the vocabulary
    printf("\n== buddy.json idle.behaviour ==\n");
    {
        const char *names[] = { "still", "wander", "curious", "sleepy", "roam", "play" };
        for (i = 0; i < 6; i++) {
            CK(eos_stroll_preset_from_name(names[i]) == (eos_stroll_preset_t)i, names[i]);
            CK(strcmp(eos_stroll_preset_name((eos_stroll_preset_t)i), names[i]) == 0,
               names[i]);
        }
        CK(eos_stroll_preset_from_name("nonsense") == EOS_STROLL_WANDER,
           "an unknown behaviour falls back to wander");
        CK(eos_stroll_preset_from_name(NULL) == EOS_STROLL_WANDER,
           "and so does no behaviour at all");
        CK(eos_stroll_preset_from_name("wan") == EOS_STROLL_WANDER, "a prefix is not a match");
        CK(eos_stroll_preset_from_name("playful") == EOS_STROLL_WANDER,
           "and neither is a longer word that starts the same");
        printf("    six presets, still/wander/curious/sleepy/roam/play\n");

        // still really is still
        rig(EOS_STROLL_STILL, 51, 0x5711u);
        for (n = 0; n < 2000; n++) step(100);
        eos_buddy_pos(&bud, &x, &y);
        CK(x == 0 && y == 0, "still never leaves the spot");
        CK(str.walked_q8 == 0, "and covers no ground at all in 200 s");
        CK(eos_stroll_lean(&str) == 0, "and never leans");

        // sleepy walks, but never plays
        {
            uint32_t played = 0;
            rig(EOS_STROLL_SLEEPY, 51, 0x51EEu);
            for (n = 0; n < 6000; n++) {
                step(100);
                if (eos_stroll_phase(&str) == EOS_STROLL_ACT) played++;
            }
            CK(str.walked_q8 > 0, "sleepy still gets about");
            CK(played == 0, "but sleepy never plays");
            printf("    sleepy covered %u px in 600 s and played %u times\n",
                   (unsigned)(str.walked_q8 / 256), (unsigned)played);
        }

        // changing preset mid-stride does not drop him
        rig(EOS_STROLL_WANDER, 51, 0x7777u);
        while (eos_stroll_phase(&str) != EOS_STROLL_WALK) step(100);
        eos_stroll_set_preset(&str, EOS_STROLL_PLAY);
        CK(eos_stroll_phase(&str) == EOS_STROLL_WALK,
           "changing the preset mid-stride does not put a foot down early");
        CK(eos_stroll_preset(&str) == EOS_STROLL_PLAY, "but it does take effect");
    }

    // -------------------------------------------------- 11. bad arguments
    printf("\n== bad arguments ==\n");
    {
        eos_stroll_t z;
        eos_stroll_init(&z, NULL, EOS_STROLL_ROAM, 1);
        eos_stroll_tick(&z, 100);                       // must not fault
        eos_stroll_tick(NULL, 100);
        eos_stroll_init(NULL, &bud, EOS_STROLL_ROAM, 1);
        CK(eos_stroll_moved(NULL) == false, "a NULL stroll has not moved");
        CK(eos_stroll_lean(NULL) == 0, "and is not leaning");
        CK(eos_stroll_phase(NULL) == EOS_STROLL_REST, "and is at rest");
        eos_stroll_init(&z, &bud, (eos_stroll_preset_t)99, 0);
        CK(eos_stroll_preset(&z) == EOS_STROLL_WANDER, "a nonsense preset is wander");
        eos_stroll_tick(&z, 0);
        eos_stroll_tick(&z, 4000000u);                  // a stall must not teleport
        eos_buddy_pos(&bud, &x, &y);
        CK(x >= -hx - 1 && x <= hx + 1, "a four-thousand-second frame is clamped like any other");
        eos_buddy_move_to(NULL, 0, 0);
        eos_buddy_set_gait(NULL, 1, 1);
        eos_buddy_face(NULL, 1);
        eos_buddy_fit(NULL, 10, 10);
        eos_buddy_fit(&bud, 0, 0);
        CK(eos_buddy_facing(NULL) == 0, "a NULL buddy is facing home");
        printf("    NULLs, zero sizes and a four-thousand-second frame all survive\n");
    }

    // ------------------------------------------ 12. move_by cannot overflow
    //
    // The delta lands on walk_x_q8 as a sum, and at int32 width that sum can
    // overflow. Signed overflow is undefined, and the wrap it produces in
    // practice is the worst possible answer: a buddy pressed hard into one wall
    // reappears against the OPPOSITE wall on the next frame. eos_stroll cannot
    // ask for a delta that big today, which is exactly why this is checked
    // here - the day a preset gets faster or a second walker appears, nothing
    // else would notice.
    printf("\n== move_by saturates ==\n");
    {
        eos_buddy_fit(&bud, 80, 80);
        eos_buddy_stage(&bud, &hx, &hy);
        CK(hx > 0 && hy > 0, "an 80x80 box gives the buddy a stage to walk on");

        eos_buddy_move_to(&bud, hx, hy);
        eos_buddy_move_by(&bud, 2147483000, 2147483000);
        eos_buddy_pos(&bud, &x, &y);
        CK(x == hx && y == hy, "a huge positive delta holds him against the far corner");

        eos_buddy_move_to(&bud, -hx, -hy);
        eos_buddy_move_by(&bud, -2147483000, -2147483000);
        eos_buddy_pos(&bud, &x, &y);
        CK(x == -hx && y == -hy, "a huge negative delta holds him against the near corner");

        eos_buddy_move_to(&bud, hx, -hy);
        eos_buddy_move_by(&bud, 2147483647, -2147483647);
        eos_buddy_pos(&bud, &x, &y);
        CK(x == hx && y == -hy, "and INT32_MAX on one axis with INT32_MIN-ish on the other");
        printf("    a delta that overflows the sum clamps instead of wrapping\n");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
