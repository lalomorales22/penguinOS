# assets/buddy — the shipped buddies

The models penguinOS ships with. Each is a real MagicaVoxel file, parsed by the
same `eos_vox` reader that was fuzzed over 8,000 mutated inputs, and rendered by
the same painter-order rasteriser that runs on the board.

A gallery with one model in it is not a gallery, so there are four. They are
meant to be told apart in a list of thumbnails and at fifteen voxels tall on a
240-pixel panel, which is why the silhouettes are a bird, a tufted bird, a
sitting cat and a box with an antenna, rather than four rounded blobs.

| Model | Who | Size | Solid voxels | After culling | Pool | File | Palette |
|---|---|---|---|---|---|---|---|
| `penguin.vox` | Pip | 15 x 13 x 22 | 1,280 | 572 (55% gone) | 2,860 B | 6,216 B | black · white · orange · eye · eye-shut |
| `owl.vox` | Hoot | 15 x 11 x 20 | 1,190 | 522 (56% gone) | 2,610 B | 5,856 B | brown · cream · orange · eye · eye-shut · tan |
| `cat.vox` | Mochi | 15 x 13 x 19 | 1,021 | 469 (54% gone) | 2,345 B | 5,180 B | grey · cream · pink · eye · eye-shut · dark |
| `robot.vox` | Bolt | 15 x 11 x 20 | 887 | 498 (44% gone) | 2,490 B | 4,644 B | panel · metal · cyan · eye · eye-shut |

Every one is inside `EOS_APPS_VOX_VOXELS` (1536) and `EOS_APPS_VOX_BYTES`
(7264), so any of them can go back up through the board's own upload path.
Bolt culls least because a box has less interior per voxel than an egg does.

Faces drawn per frame, which is what a frame actually costs:

| Model | Face on | In profile | Worst yaw |
|---|---|---|---|
| Pip | 367 | 319 | 537 |
| Hoot | 319 | 283 | 479 |
| Mochi | 296 | 296 | 459 |
| Bolt | 278 | 260 | 415 |

Each `.vox` has a `.json` beside it holding the name, the accent colour, the
idle behaviour and the eye pair, in the `schema_version: 1` shape `eos_apps`
reads. Pip's is `buddy.json` for historical reasons; the other three are named
after their model.

Slots 4 and 5 are the blink pair in all four: `eye_shut_ci` holds the **same
RGB** as whatever surrounds the eye, so closing one is a palette swap rather
than geometry. What "surrounds" means is per model — cream face on the owl,
grey fur on the cat, the dark visor on the robot. Match the wrong one and a
blink punches two bright holes in the face.

## Regenerating them

None of these is modelled by hand:

```bash
python3 assets/buddy/make_penguin.py    # penguin.vox, and firmware/main/eos_pip_data.inc
python3 assets/buddy/make_owl.py        # owl.vox
python3 assets/buddy/make_cat.py        # cat.vox
python3 assets/buddy/make_robot.py      # robot.vox
```

The shape of each is a profile table — one `(rx, ry)` per Z level — so tuning a
buddy means editing a table and re-rendering, and the rules in
`kernel/avatar/README.md` stay enforced rather than remembered. `voxbuild.py`
holds the parts they share: the grid, the surface painters, the appendage
grower, the `.vox` writer, and an overhang audit. `make_penguin.py` predates it
and stands alone on purpose — it is the one model the board boots with, and
rewriting it to prove a refactor is not worth the risk.

## Checking them

A generator can only prove it wrote the voxels it meant to. Whether a buddy
*looks* like anything is decided by the culler and the rasteriser, so there is
a checker that runs both, unmodified:

```bash
cd assets/buddy
cc -std=c99 -Wall -Wextra -O1 -I../../kernel/avatar/include \
   ../../kernel/avatar/eos_vox.c ../../kernel/avatar/eos_buddy.c \
   check_vox.c -o check_vox -lm
./check_vox owl.vox 4 5 44        # file, eye index, lid index, canvas
```

It prints eight yaw steps and one blinking frame as ASCII, each material in its
own three-character family so both the colour and which way a face points are
readable. Two numbers per frame are the ones to watch: **enclosed gaps** counts
unlit pixels walled in on all four sides and must be zero, and **painter
violations** is `eos_buddy`'s own audit that nothing further away overwrote
something nearer. All four models are at zero for both, at every yaw.

The blinking frame is not decoration. It is the only thing that catches a
mismatched `eye_shut_ci`, because a wrong one still renders — just wrongly.

## Three things that were got wrong first, and are worth not repeating

**Markings must be painted onto the front SURFACE, not tested in 3-D.** The
first penguin selected white with an ellipse in x/y, which at the flanks starts
the white *behind* the front face — so the frontmost voxel stayed black and the
belly rendered with two black gaps down it. `paint_front()` walks each column to
its frontmost solid voxel instead.

**Attach appendages to the body's actual edge.** Flippers placed at a fixed x
float free the moment the profile narrows above or below them, leaving a
one-voxel line of background between flipper and body. `grow_from_edge()` finds
the solid extent at each level and grows outward from it. The cat's tail is
traced the same way, along the haunches' own outline.

Both are the same mistake in different clothes: reasoning about the model in
model space when what matters is the surface the camera sees.

**Sit the model on the floor.** The cat's first draft started its lowest body
level at z = 1 with paws at z = 0, which left seventy-odd voxels of haunch
hovering over air. Nothing shows at 30 degrees of elevation, but the model
carries the weight and the shape is a lie. Filling z = 0 cost 51 voxels and
removed 51 overhangs. For reference, the number of voxels with nothing directly
beneath them: Pip 101, Hoot 83, Bolt 75, Mochi 55. A limb costs a few; a
floating body costs dozens.

## Swapping the buddy, and getting Pip back

**This used to be a recipe. It is now a click.** The board keeps a gallery at
`/int/buddy/gallery/`, one `<slug>.vox` and `<slug>.json` per model, and a
one-line `/int/buddy/active` naming which of them is live. Importing writes a
new slug beside the others instead of over `buddy.vox`, so it cannot cost you
the buddy you had — which is what it used to do, and how Pip was lost for an
evening.

Open the Buddy tab and click a card. The panel changes while you watch, no
reboot. Pip is always one of the cards, because the seeder puts every shipped
model back on **every** boot rather than only the first, so a delete or a bad
import cannot leave the board with no penguin.

From the shell, the same three things:

```bash
curl 'http://<board>/api/buddy/gallery'                     # what is on the board
curl -X POST -d '{"slug":"pip"}' 'http://<board>/api/buddy/gallery/select'
curl -X POST -d '{"slug":"mine"}' 'http://<board>/api/buddy/gallery/remove'
```

`select` takes the slug in a **JSON body**, not a query parameter, and it parses
the model before it moves the pointer: a `.vox` this board refuses is a `400`
and the buddy you were wearing stays on the panel. `remove` refuses the live
entry (`busy`) and refuses the last one (`state`) — select something else first.

The older `/api/fs/*` calls still take their path as a **query parameter**, not
a JSON body, and sending JSON gets you `{"error":"bad_argument"}`, which reads
like a bad path rather than a bad call shape:

```bash
curl -o mine.vox 'http://<board>/api/fs/read?path=/int/buddy/gallery/mine.vox'
```

A board updated from before the gallery keeps what it was wearing: the seeder
moves its `/int/buddy/buddy.vox` into the gallery — renamed, not copied — and
makes that model the active one. If those bytes are the penguin this image
ships it files as `pip`; anything else keeps the name `buddy`. The boot log says
which, on the `buddy  active is ...` line.

`user/` holds models imported onto this board and pulled back off it. They are
not seeded and nothing reads them; it is a shelf.
