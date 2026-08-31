# assets/buddy — Pip

The penguin the OS ships with. `penguin.vox` is a real MagicaVoxel file, parsed
by the same `eos_vox` reader that was fuzzed over 8,000 mutated inputs, and
rendered by the same painter-order rasteriser that runs on the board.

| | |
|---|---|
| Size | 15 x 13 x 22, inside the 32³ cap |
| Voxels | 1,280 solid, **572 after interior culling** (55% thrown away) |
| Faces drawn | 367 front-on, 319 in profile |
| File | 6,216 bytes |
| Palette | 1 black · 2 white · 3 orange · 4 eye · 5 eye-shut |

Slots 4 and 5 are the blink pair: `eye_shut_ci` holds the **same RGB** as the
white face around it, so closing an eye is a palette swap rather than geometry.

## Regenerating it

`penguin.vox` is generated, not modelled by hand:

```bash
python3 assets/buddy/make_penguin.py
```

The shape is a radius profile — one `(rx, ry)` per Z level in `PROFILE` — so
tuning the penguin means editing a table and re-rendering, and the rules in
`kernel/avatar/README.md` stay enforced rather than remembered.

## Two things that were got wrong first, and are worth not repeating

**Markings must be painted onto the front SURFACE, not tested in 3-D.** The
first pass selected white with an ellipse in x/y, which at the flanks starts
the white *behind* the front face — so the frontmost voxel stayed black and the
belly rendered with two black gaps down it. `paint_front()` walks each column
to its frontmost solid voxel instead.

**Attach appendages to the body's actual edge.** Flippers placed at a fixed x
float free the moment the profile narrows above or below them, leaving a
one-voxel line of background between flipper and body. The generator finds the
solid extent at each level and grows outward from it.

Both are the same mistake in different clothes: reasoning about the model in
model space when what matters is the surface the camera sees.

## Swapping the buddy, and getting Pip back

The seeder writes `penguin.vox` onto `/int` only when nothing is there. That is
deliberate — a reboot must not overwrite a buddy you made — but it also means
importing your own model is a one-way door until the file is removed.

To restore the shipped penguin:

```bash
curl -X POST 'http://<board>/api/fs/remove?path=/int/buddy/buddy.vox'
# then reboot; the seeder writes Pip back on the next boot
```

**Back the old one up first** if you want it, because remove is remove:

```bash
curl -o mine.vox 'http://<board>/api/fs/read?path=/int/buddy/buddy.vox'
```

Note the path is a **query parameter**, not a JSON body. `/api/fs/remove`,
`/api/fs/read` and `/api/fs/write` all take it that way, and sending JSON gets
you `{"error":"bad_argument"}`, which reads like a bad path rather than a bad
call shape.

`user/` holds models imported onto this board and pulled back off it. They are
not seeded and nothing reads them; it is a shelf.
