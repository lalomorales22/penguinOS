# avatar — the voxel buddy

The little 3D character that is the face of penguinOS. Two pieces:

| File | What |
|---|---|
| `eos_vox.[ch]` | MagicaVoxel `.vox` reader, interior culling, face masks |
| `eos_buddy.[ch]` | software cube rasteriser + the personality state machine |
| `eos_stroll.[ch]` | where he stands, the waddle, and what he does unprompted |

No allocation, no float, no LVGL. It draws into a buffer you hand it, in
either 8-bit indexed or RGB565, so the same code runs on the CYD's software
compositor and on an LVGL canvas.

## How it draws

The model is blocky and axis aligned, so there is no 3D pipeline. Each voxel
is a unit cube; at most three of its faces can face the camera (the top, one
x side, one y side); each of those is a parallelogram on screen; occlusion is
a painter sort along the view axis with no depth buffer.

| Decision | Why |
|---|---|
| Camera fixed at 30 degrees elevation | The top face is always visible, the bottom never is. Kills a branch and half the geometry. |
| Yaw quantised to 32 steps | `sin`/`cos` come out of a 64-byte `int16` table. No float trig anywhere in the hot loop. |
| Three brightness levels, one per face orientation | The cheapest lighting that reads as 3D. Top brightest, y faces mid, x faces dark. |
| Painter sort, no z-buffer | A 64x64 depth buffer is 16KB. The whole board has 20KB. |
| The sort reorders the model in place | Zero extra RAM. Yaw moves a step or two per frame, so insertion sort is near linear after the first frame. |
| Interior culling at load | The single biggest win. See below. |

Default home yaw is step 4 (45 degrees) so three faces are always visible.
Face on, at step 0, you only ever see two and the buddy flattens into a slab.

### Interior culling

`eos_vox_finish()` records which of the six faces of each voxel is not buried
against a neighbour, then deletes every voxel whose faces are all buried. On
the reference buddy that is **half the model**:

| Model | Solid voxels | Buried | Kept | Pool bytes | Faces drawn per frame |
|---|---|---|---|---|---|
| mini buddy 8x5x11 | 244 | 86 | 158 | 790 | ~148 |
| stock buddy 11x7x15 | 733 | 361 | 372 | 1860 | 212–300 |
| solid 9x9x9 box | 729 | 343 | 386 | 1930 | 162–243 |

Order matters and is not negotiable: **compute the face masks against the
full set, then remove.** Doing it the other way round, or running the pass
twice, finds empty space where the deleted interior used to be and lights up
faces that point into the middle of a solid model. `eos_vox_finish()` is
idempotent for exactly that reason — a second call only restores the spatial
sort order and returns 0.

## Memory

| Thing | Bytes | Notes |
|---|---|---|
| `eos_voxel_t` | 5 | x, y, z, palette index, face mask |
| `eos_vox_model_t` | 32 | points at your pool |
| `eos_vox_pal_t` | 768 | optional — pass NULL and never spend it |
| `eos_buddy_t` | 116 | riscv32; was 92 before it could move |
| `eos_buddy_cfg_t` | 24 | `roam_q8` landed in existing padding and cost nothing |
| `eos_stroll_t` | 56 | one per buddy; the caller owns it |
| `eos_buddy_target_t` | 40 | |
| shade LUT (I8 only) | 768 | `const`, put it in flash, costs no RAM |
| code | 6737 B flash | vox + buddy, `xtensa-esp32-elf-gcc -std=c99 -Os -mlongcalls`; 2308 vox + 4429 buddy |
| motion | 3781 B flash | `riscv32-esp-elf-gcc -Os`: 3267 for `eos_stroll.o`, 514 added to `eos_buddy.o` |
| writable statics | **0** | no `.data`, no `.bss` in either object; every table is `const` |
| peak stack, render at 64x64 | ~320 B | xtensa `-Os`: `eos_buddy_render` 288 + `ceil_q8` 32, no recursion, no VLA |
| peak stack, parse + finish | ~240 B | `eos_vox_parse` 80 + `eos_vox_finish` 48 + `sort_spatial` 80 + `voxkey` 32 |

Caps are `EOS_VOX_MAX_DIM` 32 and `EOS_VOX_MAX_VOXELS` 4096. A 4096-voxel
model is a 20KB pool — that is a tier 2 number, not a tier 0 one.

### Per tier

| Tier | Model | Format | Size | Buddy RAM | Notes |
|---|---|---|---|---|---|
| 0 (CYD, no PSRAM, ~20K free heap) | mini 8x5x11 | I8 | 64x64 | **~970 B** | no palette, shade LUT `const` in flash |
| 1 (C5 class) | stock 11x7x15 | RGB565 | 128x128 | ~2.8 KB | palette in RAM for real shading |
| 2 (PSRAM) | up to the caps | RGB565 | 128x128+ | up to 21 KB | double buffer, animation on |

The render target is **not** counted above: it is the window's own pixel
buffer, which the compositor already owns. 64x64 I8 is 4KB; 128x128 RGB565
is 32KB and only exists on a board that can hold it.

On tier 0, generate the shade LUT on the host with
`eos_buddy_build_shade_lut()`, print it, and paste it in as a `const uint8_t
[768]`. It lands in flash and the buddy costs zero RAM for shading.

## Personality

`eos_buddy_tick(dt_ms)` drives everything procedurally on top of one static
model: a vertical bob, a yaw ease toward a target, a lean (implemented as a
screen-space shear, which maps parallelograms to parallelograms and so costs
the rasteriser nothing), a squash-stretch, and a blink that swaps the eye
palette index for the lid index.

| State | Reads as |
|---|---|
| `IDLE` | slow bob, lazy sway, blinks every 2.4–5.2s |
| `THINKING` | head turned 56 degrees away, quick shallow bob, fast blinks |
| `TALKING` | faces you, squash-stretch pulse scaled by how hard the reply is streaming |
| `LISTENING` | faces you, leans in, blinks often |
| `SLEEPING` | sunk down, slow breathing bob, tilted, eyes shut |
| `HAPPY` | hop (bob rectified so it rests on the floor), wiggle, lean |
| `CONFUSED` | head tilt, looks left and right |

The whole personality is one `static const` table in `eos_buddy.c`. Adding a
mood is a row of data, not code.

### Megabrain lifecycle

The states exist to track a megabrain request. Feed the HTTP client's
progress in as events and the buddy perks up and turns to face the user the
moment the first chunk of a reply lands:

| Event | Result |
|---|---|
| `EV_USER_TYPING` | LISTENING |
| `EV_REQUEST_SENT` | THINKING, head turns away |
| `EV_STREAM_FIRST` | TALKING, yaw snaps back to home, energy spikes, one-shot stretch pop |
| `EV_STREAM_CHUNK` | tops the energy up; drives the mouth. Stop feeding these and TALKING lapses to IDLE after 900 ms |

Entering `TALKING` through `eos_buddy_set_state()` rather than through the
events counts as a chunk arriving, so the 900 ms lapse timer starts from that
moment. It has to: otherwise a buddy told to talk after a quiet spell inherits
a stale gap and drops back to IDLE on the next tick.
| `EV_STREAM_DONE` | HAPPY for 1.4s, then IDLE |
| `EV_ERROR` | CONFUSED for 1.9s, then IDLE |
| `EV_IDLE_TIMEOUT` | SLEEPING (or set `cfg.idle_sleep_ms` and it happens on its own) |

## Motion

`eos_buddy` draws one frame of one mood. It has no idea where it is standing.
`eos_stroll` is the layer over the top that decides that: pick a spot, turn to
face it, waddle there, stop, look about, and every so often do something silly.
It is a separate object because a mood arrives from the megabrain and a stroll
does not, and folding the second into the first would put a walk cycle inside
the thing the HTTP client drives.

The entire surface it touches is four numbers on the buddy:

| It writes | Through | In |
|---|---|---|
| where he stands | `eos_buddy_move_to/by()` | Q8 pixels from the centre of the target |
| the lean | `eos_buddy_set_gait()` | Q8 voxels of shear at the top of the model |
| the rise | `eos_buddy_set_gait()` | Q8 voxels of lift |
| which way he faces | `eos_buddy_face()` | Q8 yaw steps, offset from `cfg.home_yaw` |

### The stage

Position is an offset **inside the render target**, not a move of where the
target is blitted. That is the whole reason a step is free: the buddy's box is
already the unit of damage on the panel — rendered once into its own buffer,
blitted at a fixed spot — so walking repaints exactly the rectangle a bob
repaints. Moving the blit would dirty two boxes a frame and drag the tile
behind it.

`cfg.roam_q8` is how much of his own size he gives up to get floor to walk on.
0 is exactly the old behaviour, to the pixel: he is fitted as large as the box
allows and there is nowhere to go. The stage is then whatever the box has left
over after his footprint, minus an eighth of his own size on each axis held
back for the bob and the lean — so a hop at the top of the stage cannot put his
head through the edge of the tile.

Measured, with the reference 11x7x15 buddy. The 80x80 box is
`EOS_SHELL_BUDDY_PX`, which is what the tile gets; 240x240 is the Buddy app
full screen.

| `roam_q8` | Drawn at | Stage in 80x80 | Stage in 240x240 |
|---|---|---|---|
| 0 (`still`) | full fit | +-9.0 x +-0.0 px | +-26.9 x +-0.0 px |
| 32 (`sleepy`) | 87% | +-12.9 x +-2.5 px | +-38.6 x +-7.2 px |
| 41 (`wander`) | 84% | +-14.0 x +-4.0 px | +-41.8 x +-11.8 px |
| 51 (`curious`, `roam`, `play`) | 80% | +-15.2 x +-5.6 px | +-45.5 x +-16.8 px |

A step at 10 Hz is between a third and two thirds of a pixel, and a crossing of
the 80x80 tile takes four or five seconds. Two pixels is visible on this panel,
so that is a walk and not a twitch.

The horizontal stage is about three times the vertical, and that is correct
rather than a compromise: the camera sits 30 degrees up, so a square of floor
projects to a 2:1 letterbox. Vertical travel is foreshortened by
`EOS_BUDDY_SIN_PHI` for the same reason, which makes up-screen read as
*further away* rather than *airborne*, and is the only depth cue there is —
nothing rescales the model.

### The waddle

A penguin walking is not a slide, it is a rock from foot to foot, and the
give-away is that the roll and the step are **the same oscillator**: he leans
onto the foot he is about to push off. So there is exactly one phase in the
whole gait, and everything is a function of its sine:

| | Is | Peaks |
|---|---|---|
| lean | `amp * sin(phase)` | twice a cycle, opposite signs — one per foot |
| rise | `amp * abs(sin(phase))` | at both lean extremes: the hip over a straight stance leg |
| step | `speed * abs(sin(phase))` | at both lean extremes: that is the push-off |

Two feet, two lean extremes, two push-offs, one cycle. They cannot drift apart
because there is nothing to drift against. The host test asserts the
*relationship* and not merely that both of them move: at the frame where the
lean is extreme the stride is within 3% of its maximum, and at every lean zero
crossing — both feet down, nobody pushing — it is under 12% of it.

**What the lean actually is, and what it cost.** The rasteriser has no roll.
The three things it could have been:

| Option | Verdict |
|---|---|
| horizontal shear | **chosen.** `shear_q8` already exists, `fill_quad()` already handles it (a shear maps parallelograms to parallelograms), and on a blocky model the top sliding over the base reads as a body over a stance foot. Costs nothing new. |
| one-voxel vertical offset per side | rejected: the model has no left/right halves to offset. Splitting it would mean two draws and a seam. |
| yaw wobble | rejected: one yaw step is 11.25 degrees, so the smallest wobble available *snaps* rather than rocks at 10 frames a second. |

The cost of the shear is that it is a whole-body lean, not a hip: his feet
lean with him. At three pixels per voxel nobody can see the feet well enough
for that to read as wrong, and it is the difference between a waddle and a
slide.

The rise on its own would be a hop, not a waddle. It is not on its own — the
lean is the larger signal and is what carries the gait. `EOS_STROLL_ACT_HOP`
is the one that really is a hop, and is named that.

### Behaviour

A small state machine over the moods, never inside them:

    REST -> TURN -> WALK -> LOOK -> REST
      \-> ACT -> REST

| Phase | What |
|---|---|
| `REST` | standing at home yaw, waiting out a jittered timer |
| `TURN` | yaw easing round to face the chosen spot; capped at 2.5 s so he cannot be stranded facing the wrong way |
| `WALK` | waddling toward it; ends on arrival, on overshoot, on 450 ms of getting nowhere against the clamp, or on the preset's cap |
| `LOOK` | arrived; head goes two or three steps one way then the other |
| `ACT` | one unprompted thing: hop, spin, flap or stretch |
| `HELD` | THINKING, TALKING, LISTENING, CONFUSED or HAPPY: hold position |
| `SETTLED` | SLEEPING: the lean and rise decay to nothing and he stays put |

**He walks only while IDLE.** A buddy who wanders off mid-answer looks broken
rather than alive, so the four moods that mean the owner is at the panel hold
him where he is, and SLEEPING settles him completely. Waking puts him back on
his feet.

The four acts are all written through the same two numbers the waddle uses,
plus the yaw offset for the spin, because the buddy has no other joints:

| Act | Expressed as | Length |
|---|---|---|
| hop | rise, half a sine per bounce, rectified so he rests on the floor | 1350 ms, 3 bounces |
| spin | exactly one turn wound onto the yaw target | 1300 ms |
| flap | lean oscillating at 500 ms — a body shimmy, there are no flippers to move | 1500 ms |
| stretch | one half sine of rise over the whole act, with a small lean | 1100 ms |

Every one of those periods is four or five frames at 10 Hz, and that is not a
coincidence. An act built on a 200 ms oscillation aliases against a 100 ms
frame into a two-frame flicker: the buddy does not flap, he strobes. The first
cut of `flap` was 220 ms and did exactly that.

Rest lengths and the gap between acts are `min + jitter`, and the gait period
is jittered a tenth either side per walk. A penguin who hops every nine seconds
exactly is a metronome and the eye finds that in about three repeats.

### `idle.behaviour`

`buddy.json` already carried this field, so the vocabulary is extended rather
than replaced. `eos_stroll_preset_from_name()` maps the string; anything
unrecognised is `wander`, which is the rule `web/README.md` already set.

| Value | What he does | `roam_q8` |
|---|---|---|
| `still` | holds home yaw, blinks. Never moves, never leans, and is drawn at exactly the size he was before this file existed | 0 |
| `wander` | the fallback: long rests, the odd short walk, plays every 20–40 s | 41 |
| `curious` | quick turns, looks about, covers the stage, plays every 8–21 s | 51 |
| `sleepy` | slow and short, very long rests, never plays; still halves `idle_sleep_ms` | 32 |
| `roam` | walks most of the time, whole stage, plays every 13–30 s | 51 |
| `play` | roams and plays every 4–12 s | 51 |

`eos_apps_idle_t` in `kernel/svc/include/eos_apps.h` has the same six in the
same order, but the two are joined **by name and not by cast**, so neither
header has to include the other. That enum is append-only: a `buddy.json` in
the wild carries the name, so a new value is free, but reordering would change
what an already-stored behaviour means.

## The .vox format, as we read it

RIFF-ish: 4-byte id, `int32` content bytes, `int32` children bytes, then the
content, then the children. `"VOX "` + version, then one `MAIN` chunk.

| Chunk | What we do |
|---|---|
| `MAIN` | required; we walk its direct children |
| `SIZE` | first one wins; 1..32 per axis, anything else is rejected |
| `XYZI` | first one wins; capped at 4096 voxels |
| `RGBA` | 256 entries; **file entry `j` is palette index `j+1`** |
| `PACK`, `nTRN`, `nGRP`, `nSHP`, `MATL`, `LAYR`, anything else | stepped over whole, children included |

Axes are MagicaVoxel's: **x right, y depth, z up**. At yaw 0 the camera looks
along +y, so the buddy's face is the **y = 0** slice.

Hard-won bits of the format:

- **Palette index 0 means empty** and never appears in a model. That is why
  the `RGBA` chunk's 256 entries map to indices 1..255 and the last one has no
  index at all. Reading it straight into `pal[0..255]` shifts every colour by
  one and is the classic way to get a buddy in the wrong colours.
- The stock palette is **generative, not a table**: indices 1..215 are a 6x6x6
  cube over `{ff,cc,99,66,33,00}` with blue varying fastest, minus the final
  black; 216..255 are four 10-step ramps (red, green, blue, grey). We build it
  in 30 lines rather than carrying 1KB of flash.
- Most hand-made files carry no `RGBA` chunk at all, so the stock palette is a
  fallback that actually gets used.

Files come off a microSD card, so nothing in one is trusted. Every chunk
length is checked against what remains in the buffer; a chunk that overruns
is a hard error, never a clamp. Errors are typed — see `eos_vox_strerror()`.

## Authoring a buddy

1. Keep it inside 32x32x32 and a few thousand voxels. Readable at **2 pixels
   per voxel** is the tier 0 bar: at 64x64 a 15-tall model gets about that.
2. Z is up. Put the face on the **y = 0** slice.
3. **Model it solid.** Do not hollow it out to save space — the culler does
   that for you, and a hand-hollowed model is worse, because its inner surface
   is exposed and gets drawn.
4. **No overhangs with visible undersides.** The bottom face of a voxel is
   never camera-facing and never drawn, so anything sticking out over empty
   space shows the background through it. Fill underneath.
5. Give the eyes their own palette slot, and a second slot holding the same
   RGB as the surrounding skin. Those are `cfg.eye_ci` and `cfg.eye_shut_ci`;
   the blink is a swap between them.
6. One model belongs to one buddy. `eos_buddy_render()` reorders the pool.

## Test

```bash
cc -std=c99 -Wall -Wextra -O1 -Iinclude eos_vox.c eos_buddy.c test/test_vox.c -o test_vox
./test_vox                  # 109 checks

cc -std=c99 -Wall -Wextra -O1 -Iinclude eos_vox.c eos_buddy.c eos_stroll.c \
   test/test_stroll.c -o test_stroll -lm
./test_stroll               # 92 checks, and prints the gait
./test_vox buddy.vox        # also drops a real .vox you can open in MagicaVoxel

# and the one that matters, because the parser eats untrusted card data:
cc -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
   -Iinclude eos_vox.c eos_buddy.c test/test_vox.c -o test_vox_san && ./test_vox_san
```

The test builds the buddy in code, writes a real `.vox` file byte for byte
and parses it back, then renders the buddy to ASCII at eight yaw steps and in
five moods so you can see it turn. Beyond the obvious, it checks:

- culling removes **exactly** the buried voxels, verified against an
  independent neighbour scan of a reference grid;
- the painter sort never lets a further-away face overwrite a nearer pixel —
  audited per pixel, at all 32 yaws, under extreme lean and squash, in both
  pixel formats, on both the cold and warm sort paths;
- a solid box projects with **no interior holes** on any scanline, over 2080
  combinations of yaw, lean, squash and scale;
- nothing is written outside the target buffer, across 10 canvas sizes
  (including 1x1) x 2 formats x 11 yaws, with guard bytes on both sides;
- malformed `.vox` input is rejected and not trusted: 17 hand-built corrupt
  files, every truncated prefix of a good file, and 8000 randomly mutated
  files, with pool guard bytes checked after every one;
- the same truncation and mutation sweeps again with the input in an **exactly
  sized** heap block and the pool sized to match, so that under
  `-fsanitize=address` a read one byte past the declared length is a hard
  failure. Fed a big static buffer, an over-read is invisible; that is the only
  reason this test allocates.

`test_stroll` prints two things a human should actually look at: one walk
cycle as ASCII, ten frames of it, with the lean and the stride beside each
frame — you can watch the body slide over the feet and come back — and a plan
view of ninety seconds of `play` seen from above, so the roaming looks like
roaming and not like a pendulum. Beyond that it checks:

- the walk reaches its target and **stops**, and does not drift afterwards;
- the clamp holds at all four edges, asked for directly and walked into, over
  4 x 400 s of continuous roaming;
- the roll and the step are one oscillator, asserted as the relationship
  above rather than as "both of them moved";
- exactly two lean sign changes and exactly two push-off surges per cycle;
- no phase strands him: over ten minutes of `play`, nothing runs past 3.2 s,
  every one of the four acts comes up, and the gaps between them span 8.6 s;
- the lean always eases out within 600 ms of stopping — over 900 s there is
  no frame where he is standing still and still leaning;
- SLEEPING settles him: eight seconds of it does not move him one Q8 unit,
  the lean and rise reach exactly zero, and he lets go of the yaw;
- 3000 frames x 4 target sizes, and all four corners of the stage x 32 yaws
  under a hard lean, write **nothing** outside the target buffer — checked
  with a guard band, not by trusting the rasteriser.

### The one that bites

Two faces sharing an edge must agree, to the Q8 unit, on where that edge is.
Interpolating along the edge in traversal order does **not** give that: the
two faces meet the edge from opposite ends, integer division truncates toward
zero, and the two answers land one unit apart. Most of the time that is
sub-pixel and invisible. Occasionally it straddles a pixel centre and opens a
one-pixel hole in the middle of a flat wall. `fill_quad()` walks every edge
from its low end for exactly this reason, and the watertight test above is
there to keep it that way.
