# esp-os — status

Last integration pass: 2026-08-30.

**One sentence: the kernel is written, heavily host-tested, and now builds into
a real firmware image, and not one line of it has ever run on a
microcontroller.** There is an ESP-IDF project under `firmware/`, an ST7789
display backend, four font faces and the boot glue; there is still no app, and
nothing has been flashed. The gap between here and a board drawing its first
pixel is now one `idf.py flash` and a human watching the panel.

## Component table

| Component | Files | Built | Host-verified | Runs on hardware | Notes |
|---|---|---|---|---|---|
| `kernel/wm` | 2 + test | yes | **268 checks** | never | Finished and frozen. Untouched by this pass. |
| `kernel/theme` | 2 + 7 themes + test | yes | **213 checks** | never | Parser is genuinely hardened: survives 200k random buffers, 7,030 single-byte mutations, exhaustive short inputs, all under ASan. |
| `kernel/avatar` | 4 + test | yes | **109 checks** | never | Renderer proven per-pixel by a painter-order audit. 6,737 B flash cross-compiled for xtensa. **No model asset exists.** |
| `kernel/shell` | 4 + test | yes | **2,201 checks** | never | Model only — emits segments and actions, draws nothing. |
| `kernel/svc` | 2 + test | yes | **182 checks** | never | Parser verified against the live Mac mini over real sockets. The ESP-IDF half has never been compiled. |
| `kernel/font` | 2 + data + test | yes | **418 checks** | never | All four faces: tiny 4x6, small 6x8, med 8x13, big 12x20. 6,597 B of flash. |
| `kernel/test` | 1 | yes | **635 checks** | n/a | Cross-component contracts, plus every generated board header held against the registry. |
| `kernel/hal` | 4 headers + ST7789 backend + test | yes | **109 checks** | never | The banded RGB565 ST7789 backend is implemented and cross-compiles. Storage and input are still 0 of their declared functions. |
| `boards/` | schema + 6 profiles | yes | validator + generator | n/a | All 6 validate; all 6 generate headers that compile, link and run against the real HAL, and regeneration is byte-reproducible. |
| `tools/` | flasher, detector, prober, generator | yes | `--list` / `--identify` exercised | partially | Identification works today. There is now an image to flash; the flash itself has not been run. |
| `web/` | 4 served files | yes | `node --check` only | never | Contract is documented and self-consistent. It has never spoken to a board. |
| `apps/` | — | **empty** | — | — | Terminal, buddy, files, settings, arcade, CP/M. None exist. |

**3,500 component checks + 635 integration checks, 4,135 in all, all passing**,
clean under `-Wall -Wextra` and `-fsanitize=address,undefined`, and the whole
kernel compiles clean for xtensa and riscv32 at `-Wall -Wextra -Werror -Os`.

## Verified this pass

Things that were claimed but had never actually been run together:

- All eleven kernel headers compose in one translation unit, in forward and
  reverse order, doubly included, clean under `-pedantic`.
- All seven kernel `.c` files compile and **link** together — no duplicate
  symbols, no clashing include guards, no type redefined in two places.
- All five board profiles generate HAL headers that **compile, link and run**
  against the real `eos_board.h`, drive `eos_wm` to a correct layout and produce
  a fitted status bar. Behaviour matches the design intent exactly: the 128x64
  OLED tabs all six windows, the 320x480 wavvy tabs the fewest.
- All seven theme files parse with complete role and ANSI sets.

## Defects found and fixed this pass

| # | Defect | Fix |
|---|---|---|
| 1 | **Palette index 255 was claimed twice.** `eos_display.h` reserved it as `EOS_COLOR_NONE`; `eos_theme.h`'s RGB cube also owned it. `eos_theme_cube_index()` returned exactly 255 for pure white, so on tier SOFT every white pixel silently did not draw. 945 of 636,056 sampled colours landed on it. | The cube gives the cell up. White steps back one *green* level to 251 (36 counts of error) rather than one blue level (85). `eos_theme_index()` now searches 255 entries, not 256. Regression-pinned in both the theme suite and the new integration test. |
| 2 | **Nothing in the tree supplied `min_tile_w` / `min_tile_h`.** The WM needs them, the theme explicitly disclaims them ("they belong to the panel"), and the board registry did not carry them. Only test files had numbers. `eos_wm_cfg_t` was unconstructible. | Added `render.min_tile_w` / `min_tile_h` to the schema, all five profiles (marked `unverified`), the generator's validator and both emitters, and `eos_board_render_t`. |
| 3 | **The web app used `PATCH`** for settings while its own README said every mutation is `POST` "because a small HTTP server's method table is one more thing to get wrong". A firmware author following the prose would have shipped a GET+POST server and settings would 404. | `POST /api/settings`, carrying the same changed-keys-only body. Verb removed from `app.js` and the docs. |
| 4 | **Every shipped theme named a font that does not exist.** All seven said `mono10`/`mono12`; the HAL's only font authority is `eos_font_id_t` = tiny/small/med/big. Every theme silently fell back. | Renamed to the HAL's own identifiers (`mono10`→`small`, `mono12`→`med`), default changed to `small`, and the naming rule written into `eos_theme.h`. |
| 5 | **`eos_buddy_build_shade_lut()` would produce an invisible buddy.** Passing `disp_n = 256`, the obvious thing to do, lets a face shade onto slot 255 and not draw — holes in the brightest places. | Documented at the declaration: pass 255. The function stays HAL-independent by design, so it cannot know this itself. |
| 6 | `web/preview.html` was a design mock with baked-in theme data and Google Fonts links, sitting in the directory that gets flashed. | **Moved to `design/preview.html`** and made regenerable: `design/build_preview.py` rebuilds it from the live kernel, so it can no longer go stale. `web/` now holds only the five files that ship. |

## Defects found and NOT fixed

| Defect | Why not | Who should |
|---|---|---|
| **`render.heap_budget_bytes` is still a guess on five of the six boards.** Only `waveshare-c6-lcd-13` is measured (425,648, radios down). The wavvy figure (64 KB) contradicts the owner's own measurement — `../tft-videos/CLAUDE.md` records free heap at 48–55 KB on that exact firmware. `eos_display_init()` sizes off this number, so a wrong value is an OOM at boot, not a degradation. | Needs a real heap dump on real silicon. Cannot be derived. | First bring-up. |
| **The C5's `render.band_height` (40) disagrees with its own BSP.** `../esp32-c5/bsp/Kconfig` defaults the draw buffer to 20 rows at 172 wide, not 40 at 320. The profile's `reason` text describes the post-rotation logical frame, not the buffer the BSP allocates. | It is a Kconfig knob ESP-OS may legitimately set, so the value is not necessarily wrong — but the prose and the BSP have to be reconciled by whoever writes the build. | Build wiring. |
| **`eos_board_fb_bytes()` lies on LVGL boards.** It returns `w*h`, assuming one palette index per pixel, so on the C5 it returns 55,040 — a number with no meaning, since LVGL owns the buffers and the profile correctly derives `EOS_FB_BYTES = 0`. | It is a one-line guard on `render.compositor == EOS_COMP_LVGL`, but changing a HAL function's return contract while no backend exists yet is guessing at which caller is right. | Whoever writes the first LVGL backend. |
| **The buddy has no 1bpp path.** `eos_buddy_pix_t` is I8 or RGB565. `wavvy-oled-c5` is tier SOFT with the `mono1` compositor, so the buddy cannot be drawn on it at all. | A real feature, not a mismatch. Also possibly the right answer — a 2.5D voxel character on 128x64 mono may not be worth it. | Design call. |
| **`eos_buddy_pix_t` and `eos_display_pixfmt_t` are two enums for the same idea, differently numbered.** I8 is 0 in both, but RGB565 is 1 in one and 3 in the other. Anything that casts between them is wrong. | Merging them would couple the avatar to the HAL, which the avatar author deliberately avoided. | The shell glue, which must translate explicitly. |
| **The megabrain `POST` path has never been tested against the real server.** `build_head` sends the raw prompt as `text/plain`. Whether `/ask` accepts that rather than a form encoding is unknown. Only `GET` was verified live. | Needs the server. If it wants a form, `METHOD_AUTO`'s long-prompt fallback fails in production while every host test still passes. | Anyone with the mini up. |
| **The mDNS name `megabrain` is a guess.** Nothing confirms what the mini advertises; the old Arduino sketch only ever used the literal IP. | One string (`cfg.mdns_name`). If wrong, discovery silently falls through to `192.168.0.139`, which works — making the mDNS candidate dead weight rather than broken. | Anyone with the mini up. |
| **App id 0 = terminal is assumed**, taken from the WM test's `APPNAME[]`. `super+return` spawns app 0 on faith. | There is no app registry to check against. | Whoever writes `apps/`. |
| **macOS ASan cannot do leak detection**, so "no leaks" is unproven everywhere. Near-harmless — the kernel allocates nothing by design — but it should not be repeated as verified. | Platform limitation. | A Linux CI run. |
| **Endianness is still untested.** The whole kernel now compiles clean under `xtensa-esp32-elf-gcc -mlongcalls` and `riscv32-esp-elf-gcc -march=rv32imac_zicsr_zifencei` at `-Wall -Wextra -Werror -Os`, so the 32-bit assumption is checked. Both targets are little-endian, so the endian-agnosticism claim is still only a claim. | Needs a big-endian host build. | Anyone adding CI. |
| **Only `kernel/wm/test` has a Makefile.** Every other component's build line lives in a comment at the top of its test file, so it is not discoverable from a directory listing and nothing runs it. | Not a correctness defect, but the asymmetry will bite. | Step 1 below. |

## What has to happen next, in order

Steps 1 to 4 have landed. `firmware/` is an ESP-IDF project that builds for
`waveshare-c6-lcd-13` (esp32c6, 220,400 B) and cross-builds for
`cyd-2432s024n` (esp32, 237,568 B); the whole kernel compiles clean for both
cross-compilers at `-Wall -Wextra -Werror -Os`. `kernel/font/` ships all four
faces. `kernel/hal/backend/esp_lcd/` is a banded RGB565 ST7789 backend on the
C6's hardware-verified pins, and `render.heap_budget` is now the measured
425,648 rather than a guess. `firmware/main/` is the boot glue and the frame
loop. **None of it has been flashed** — that is the next thing that happens,
and it is a human with eyes on the panel.

What is left, in order:

1. **The tier-0 indexed backend for the CYD.** The ST7789 backend is RGB565
   banded, which is what the C6 wanted. The CYD is the tighter board and the
   one that proves the indexed path; it holds a full framebuffer at 76,800
   bytes. Its `render.heap_budget` is still a guess.

2. **Storage and input backends.** Storage (22 functions) before input, because
   the theme and the buddy both live on the card. Then the NimBLE HID host —
   port the working one out of `../ESP-OS-CY24/arcade024`, do not rewrite it.

3. **One app, and make it the terminal.** `super+return` already spawns app 0.
   Wire `eos_brain` to it and to the buddy's mood events — the two enums map
   one-for-one and the adapter is about a dozen lines. At this point the board
   does something a person would want.

4. **Author the buddy.** A real `.vox` in MagicaVoxel, plus the shade LUT
   generated on the host and pasted in as `const`. Until then the buddy window
   has nothing to draw.

5. **The HTTP server behind `web/`.** Twenty endpoints, documented precisely
   enough to implement. Chunked uploads with the board setting `chunk_max`. Do
   this last: it is the largest surface and the least load-bearing.

Things worth doing alongside, cheaply:

- Move `kernel/hal`'s 220-check author harness into `kernel/hal/test/`. It
  stubs all 58 externs and fuzzes the geometry and path helpers, and it is the
  only thing standing between the HAL headers and silent rot. It currently
  exists only in a scratch directory.
- Run `python3 tools/gen_board_header.py --all` in CI before the integration
  suite. `kernel/test/test_integration.c` now includes all six generated headers
  alongside `eos_board.h` and holds each descriptor against the registry, but it
  reaches them through `__has_include` and fails with one loud check if they
  were never generated.
- Decide `wavvy-oled-c5`'s tier. It is a tier-1-class C5 running the tier-0
  `mono1` compositor because a 1024-byte page in the SSD1306's own layout does
  not want LVGL. Currently declared tier 0. Defensible, but it is the one place
  the registry departs from "C5 class = tier 1".
