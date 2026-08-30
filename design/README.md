# design/

Design artifacts. **Nothing in this directory is ever flashed or served from a
board.** `web/` is what ships; this is what the OS is *designed* in.

`preview.html` renders every board profile at true pixel dimensions, under every
theme, with a live voxel buddy. It exists because the alternative — waiting for
a display backend before anyone can see a design decision — is how you end up
discovering at flash time that a 128x64 panel cannot hold a status bar.

## It is not a mockup

`build_preview.py` compiles `dump_layout.c` against the **real** `eos_wm`,
`eos_bar` and `eos_theme`, runs all five board geometries through them for every
theme on disk, and injects the result into `preview.tmpl.html`. Tile rects, bar
segments, tab groups, palettes and metrics are the kernel's own output.

```bash
python3 design/build_preview.py
```

Rerun it after touching the tiling rule, the bar fitter, a theme, or a board
profile. If you do not, the page starts lying — which is the one failure mode
a design tool cannot have.

## Why it must stay out of web/

- It links Google Fonts. A board serving pages offline cannot reach them.
- It is 60 KB, against a whole web app budgeted to be small enough for an
  ESP32 to serve out of a microSD.
- It carries a snapshot of theme data. On the board that data has one home,
  `/sd/themes/`, and a second stale copy is a bug waiting to be believed.

## Files

| File | What |
|---|---|
| `preview.tmpl.html` | The page. Contains `/*__DATA__*/`, which the builder replaces. |
| `dump_layout.c` | Links against the kernel; emits real geometry as JSON. Its `THEMES[]` must list every theme in `kernel/theme/themes/`. |
| `build_preview.py` | Compile, run, merge, inject. Stdlib only. |
| `preview.html` | Generated. Do not hand-edit — `build_preview.py` overwrites it. |

The browser buddy in `preview.tmpl.html` is a port of the painter's-algorithm
cube rasteriser in `kernel/avatar/eos_buddy.c`: same interior-voxel culling,
same three face shades, arbitrary yaw instead of the board's fixed steps.
It is a second implementation, so it can drift — it is illustrative, and
`kernel/avatar` is the authority.
