# esp-os — a tiling window OS for ESP32 panels

One firmware, one API, five very different screens. ESP-OS puts an Omarchy-style
tiling window manager, a themeable shell, a voxel buddy and a megabrain client on
boards ranging from a 128x64 mono OLED to a 320x480 ILI9488 — including a Cheap
Yellow Display with **20KB of free heap** once WiFi and BLE are up.

Built on **ESP-IDF**, not Arduino. C99, no `malloc` anywhere in kernel code.

**Status: the kernel is written and host-verified. Nothing boots yet.** There is
no ESP-IDF build, no display backend and no app layer. See `STATUS.md` for the
honest table and the ordered list of what has to happen next.

---

## The tier model

A tier is a property of the **board**, decided once in the registry. It is not a
runtime negotiation and not a quality slider the user can raise. It selects which
display backend gets linked, and nothing else in the OS branches on it.

| Tier | Name | Condition | Renderer | Boards |
|---|---|---|---|---|
| 0 | `EOS_TIER_SOFT` | No PSRAM, tight heap | Software compositor: 8-bit indexed, or 1bpp on the OLED. **No LVGL.** | CYD, both wavvy panels, wavvy OLED |
| 1 | `EOS_TIER_LEAN` | Enough SRAM for LVGL 9 + a partial draw buffer, still no PSRAM | LVGL 9, banded partial buffers, no animation budget | ESP32-C5-LCD-1.47 |
| 2 | `EOS_TIER_RICH` | PSRAM present | LVGL 9, double buffered, animations on | none yet |

The reason the tier exists at all: a 320x240 indexed framebuffer is 76,800 bytes
and fits on the CYD; LVGL next to WiFi and NimBLE on that board does not. Rather
than ship a worse OS everywhere or two codebases, every drawing call goes through
one `eos_display` API and the backend behind it changes.

Two consequences shape the whole display API, and they are not obvious:

- **Frames are banded, not one-shot.** A 320x480 indexed buffer is 153,600 bytes
  and does not fit next to WiFi, so the wavvy boards render in 16-row strips. The
  scene-drawing code is a re-runnable callback: `frame_begin` / `while
  (frame_band(&band))` / `frame_end`. Damage is declared **before** the frame
  opens, never during it, because a banded backend has to know its bands up front.
- **Colour is always a palette index**, one `eos_color_t` byte. Tier SOFT writes
  it into the framebuffer; tiers LEAN and RICH resolve it through a 256-entry
  RGB565 LUT; the SSD1306 thresholds it to one bit. No caller ever branches on
  the panel.

## Supported boards

| Profile | SoC | Panel | Tier | Compositor | Bus clock | Upload baud |
|---|---|---|---|---|---|---|
| `cyd-2432s024n` | esp32 | ILI9341 320x240 | 0 | indexed8 | 40 MHz | 460800 |
| `waveshare-c5-lcd-147` | esp32c5 | ST7789 320x172 | 1 | lvgl | 40 MHz | 460800 |
| `wavvy-ili9488-40` | esp32 | ILI9488 320x480 | 0 | indexed8 | 80 MHz | 230400 |
| `wavvy-ili9488-35` | esp32 | ILI9488 320x480 | 0 | indexed8 | **40 MHz** | 230400 |
| `wavvy-oled-c5` | esp32c5 | SSD1306 128x64 | 0 | mono1 | 400 kHz I2C | 460800 |

Every profile is one JSON file under `boards/`. `tools/gen_board_header.py` turns
it into a C header; the flasher reads the same file to decide what to write. A
profile that disagrees with itself does not generate — the validator checks GPIO
ranges per target, input-only pins bound to outputs, double-booked pins, DAC on a
chip with no DAC, Bluedroid on a no-PSRAM board, tier against PSRAM and LVGL and
compositor, and about twenty other things, and reports every problem at once.

**The two ILI9488 profiles are indistinguishable to `esptool`.** Same chip, same
flash, same USB bridge. Picking the wrong one does not error — it renders
structured block corruption that reads as a dead panel. That is why identification
ends in a human confirmation, not a probe.

## Architecture

```
boards/           the board registry: one JSON per board, plus schema.json
kernel/wm/        tiling window manager — BSP tree, one per workspace. FINISHED.
kernel/hal/       the hardware interface. HEADERS ONLY — no backend exists yet.
kernel/theme/     JSON theme parser, 256-entry palette builder, three resolvers
kernel/avatar/    the voxel buddy: .vox parser and a 2.5D painter renderer
kernel/shell/     keybind table and the status bar MODEL (no drawing)
kernel/svc/       megabrain HTTP client: chunked streaming, UTF-8 safe
kernel/test/      cross-component integration test: all 11 headers in one TU
tools/            flasher, board detector, panel prober, board header generator
web/              the phone/desktop console served off the SD card
design/           the visual preview. Never flashed, never served. Regenerated
                  from the live kernel by design/build_preview.py.
apps/             EMPTY. The terminal, buddy, files, settings and arcade go here.
```

| File | One line |
|---|---|
| `kernel/wm/include/eos_wm.h` | The BSP tree API. A split that cannot give both children the minimum tile size collapses into a tab group — that one rule is what makes tiling usable on a 2.4" panel. |
| `kernel/hal/include/eos_board.h` | `eos_board_t`, mirroring `boards/*.json` field for field. Also the tier and SoC/panel/bus enums, defined here and nowhere else. |
| `kernel/hal/include/eos_display.h` | The whole drawing surface: banded frames, fills, blits, one text run. 16 functions a backend must implement. |
| `kernel/hal/include/eos_input.h` | HID usages straight off the wire, a 32-slot event ring, and the modifier bit layout the shell matches against. |
| `kernel/hal/include/eos_storage.h` | `/sd` and `/int` behind one open/read/close. Fixed handle pools, no malloc. |
| `kernel/theme/eos_theme.c` | Parses a theme off the card. A bad file can never stop the board booting — every failure path restores the compiled-in default. |
| `kernel/avatar/eos_vox.c` | MagicaVoxel `.vox` reader, interior culling, no allocation. |
| `kernel/avatar/eos_buddy.c` | Painter-order 2.5D renderer plus the megabrain-driven mood machine. 6,737 B of flash, zero writable statics. |
| `kernel/shell/eos_keys.c` | 45 default binds, `keys.json` overrides applied atomically. |
| `kernel/shell/eos_bar.c` | Fits the status bar to any width from 480px down to 64px by dropping segments in priority order. |
| `kernel/svc/eos_brain.c` | Talks to megabrain. Holds back at most 3 bytes so a UTF-8 character is never split across two callbacks. |
| `tools/flash.sh` | Identify, confirm, build, flash. Writes nothing without a yes. |
| `tools/detect.py` | esptool facts to registry candidates. Only chip, flash size and PSRAM may reject a profile. |
| `tools/gen_board_header.py` | Validates a profile and emits its C header: macros and one `eos_board_t` initialiser, filling the type `eos_board.h` declares. It emits no types of its own. |

### What depends on what

```
wm  <-- hal  <-- shell        theme -- (standalone)
        hal  <-- (backends, not written)
                 avatar -- (standalone, no hal/theme dependency by design)
                 svc    -- (standalone)
```

`eos_rect_t` is defined once, in `eos_wm.h`, and the HAL includes that header
rather than declaring an identical second one. `kernel/shell` includes
`kernel/hal/include/eos_input.h` for the `EOS_KEY_*` usages, so **the shell can
no longer be built against the WM alone** — `-Ikernel/hal/include` is mandatory.

## Keybinds

Omarchy/Hyprland muscle memory. `super` is the GUI key on the K809. 45 binds,
dumped from the compiled-in table:

| Chord | Action |
|---|---|
| `super+return` | spawn the terminal (app id 0) |
| `super+q` | close the focused window |
| `super+h` `j` `k` `l` | focus left / down / up / right (arrows work too) |
| `super+shift+h` `j` `k` `l` | move the window in that direction (arrows too) |
| `super+ctrl+h` / `super+ctrl+v` | force the next split to columns / rows |
| `super+1`..`super+9` | go to workspace |
| `super+shift+1`..`9` | move the window to that workspace |
| `super+tab` | next tab in a collapsed group |
| `super+minus` / `super+equal` | shrink / grow the focused tile by 50 permille |
| `super+space` | launcher |
| `super+b` | toggle the status bar |
| `super+t` | cycle theme |
| `super+escape` | lock |

**`super+h` is focus-left, not split-horizontal.** The brief asked for both and
they collide; focus wins because it is the key pressed a hundred times an hour.
Two lines of `keys.json` restore the i3 spelling.

Overrides live in `/sd/keys.json` and merge on top of the defaults, so the file
lists only what changes:

```json
{ "binds": [ { "keys": "super+shift+l", "action": "move_right" },
             { "keys": "super+f",       "action": "spawn", "arg": 2 },
             { "keys": "super+b",       "action": "none" } ] }
```

The whole file parses into a private copy and is committed only if all of it
parses, so a truncated card can never leave you with a keymap you cannot type
your way out of.

## Building and flashing

**Nothing builds for a board yet.** There is no ESP-IDF project, no
`CMakeLists.txt` and no partition CSV. `tools/flash.sh` autodetects a project in
`<repo>`, `<repo>/firmware` or `<repo>/app` and tells you what is missing rather
than inventing one. What follows is the intended flow, and the identify half of
it works today.

```bash
tools/flash.sh --list        # attached boards + the whole registry. Works now.
tools/flash.sh --identify    # narrow to profiles, write nothing. Works now.
tools/flash.sh --probe       # flash the panel prober, decide by eye
tools/flash.sh               # identify, confirm, build, flash
```

Identification is two-tier and only the tier esptool can prove is allowed to
reject a profile. Chip target, flash size and PSRAM-present are hard signals;
chip description, USB bridge and serial number rank and explain but never
eliminate. **Detection always connects at 115200**, never the profile's upload
baud — the wavvy CP2102 cable fails at 460800 even for plain reads, and a
detector using the upload baud would fail to identify the one board whose upload
baud matters most.

`--yes` authorises writing, not guessing. With two candidates still standing it
still stops and asks.

### Host tests

Every kernel component is plain C that builds with a single `cc` line and prints
`N checks, M failed`.

```bash
cc -std=c99 -Wall -Wextra -O1 -Ikernel/wm/include \
   kernel/wm/eos_wm.c kernel/wm/test/test_wm.c -o t && ./t

cc -std=c99 -Wall -Wextra -O1 -Ikernel/theme/include \
   kernel/theme/eos_theme.c kernel/theme/test/test_theme.c -o t && ./t

cc -std=c99 -Wall -Wextra -O1 -Ikernel/avatar/include \
   kernel/avatar/eos_vox.c kernel/avatar/eos_buddy.c \
   kernel/avatar/test/test_vox.c -lm -o t && ./t

cc -std=c99 -Wall -Wextra -O1 -Ikernel/wm/include -Ikernel/hal/include \
   -Ikernel/shell/include kernel/wm/eos_wm.c kernel/shell/eos_keys.c \
   kernel/shell/eos_bar.c kernel/shell/test/test_shell.c -o t && ./t

cc -std=c99 -Wall -Wextra -O1 -Ikernel/svc/include \
   kernel/svc/eos_brain.c kernel/svc/test/test_brain.c -o t && ./t
```

2,973 checks total, all passing, all clean under `-fsanitize=address,undefined`.

## Theming

A theme is one JSON file on the microSD. It carries all 16 colour roles, all 16
ANSI colours, and a few scalars. Seven ship in `kernel/theme/themes/`: `carbon`,
`catppuccin-mocha`, `cyd-amber`, `ember`, `goldleaf`, `gruvbox`, `tokyonight`.

```json
{
  "name": "tokyonight",
  "font": "med",
  "metrics": { "gap": 4, "border": 1, "bar_h": 14, "tab_h": 12, "radius": 3 },
  "colors": { "bg": "#1a1b26", "text": "#c0caf5", "accent": "#7aa2f7", "...": "" },
  "ansi":   { "black": "#15161e", "red": "#f7768e", "...": "" }
}
```

- **All 32 colours are required.** A file missing one is rejected outright and the
  default is kept, because a half-theme renders as something visibly broken, which
  is worse than something plain.
- **Scalars clamp, colours fail.** A gap of 99999 clamps to 32. A colour that will
  not parse is a real error.
- `gap`, `bar_h` and `tab_h` are copied straight into `eos_wm_cfg_t`.
  `min_tile_w`/`min_tile_h` deliberately are **not** in the theme — they belong to
  the panel and come from the board registry.
- `font` names an `eos_font_id_t`, lowercased: `tiny` (4x6), `small` (6x8, the
  workhorse), `med` (8x13), `big` (12x20). **The font tables are not written yet**,
  so every name currently resolves to whatever the renderer defaults to.
- `//` and `/* */` comments, trailing commas and a UTF-8 BOM are all tolerated,
  because these files get edited on an SD card in whatever editor is to hand.

The palette layout is fixed so drawing code reaches a shade without searching:

| Index | Contents |
|---|---|
| 0..15 | the colour roles, exact |
| 16..31 | the sixteen ANSI colours, exact |
| 32..47 | 16-step ramp, bg to text |
| 48..63 | 16-step ramp, bg to accent |
| 64..255 | 6x8x4 RGB cube for arbitrary content — **minus slot 255** |

Slot 255 is `EOS_COLOR_NONE`, the display HAL's transparency sentinel, so the cube
gives that cell up: pure white quantises to 251, one green step down. Green is
quantised in eight levels and blue in four, so stepping off green costs 36 counts
where stepping off blue would have cost 85.

On the SSD1306 the lit/unlit threshold is a luminance distance from **the theme's
own background**, not an absolute mid-grey. An absolute 128 drops gruvbox's `err`
(luma 124) and its ANSI blue (114) into the background, and inverts a light theme
outright.

## Making a buddy

The buddy is a MagicaVoxel model rendered as axis-aligned cubes: camera fixed at
30 degrees elevation, yaw quantised to 32 steps, at most three faces visible per
voxel, painter order with no depth buffer. `web/voxel-editor.js` renders exactly
the same way in the browser, so what you draw is what the panel can draw.

1. Model it in MagicaVoxel, or draw it in the web editor at `/` on the board.
2. Keep it small. The stock 11x7x15 buddy is 372 surface voxels — about 2.0 KB of
   pool at 5 bytes each. A mini 8x5x11 is under 1 KB.
3. Interior voxels are culled on load (~50% of a solid model), so hollowing it
   yourself buys nothing.
4. Save to `/sd/buddy/buddy.vox`. The web editor uploads through the same chunked
   `/api/fs/write` as everything else, then calls `/api/buddy/reload`.
5. For an indexed (tier SOFT) target the buddy needs a 3x256 shade LUT.
   `eos_buddy_build_shade_lut()` builds one; run it on the host and paste the
   result as a `const` array so it lands in flash instead of RAM. **Pass
   `disp_n = 255`, not 256** — a face that shades onto slot 255 does not draw, and
   the buddy comes out with holes in its brightest places.

Moods follow the megabrain request lifecycle one-for-one: typing to LISTENING,
request sent to THINKING with the head turned away, first token to TALKING
snapping back to face you, done to HAPPY, error to CONFUSED.

**There is no buddy asset in the repo.** The reference model is built by paint()
calls inside `kernel/avatar/test/test_vox.c`. Somebody has to author the real one.

---

## Verification

Everything is host-tested on a Mac; nothing has been flashed. Run the suites from
the repo root — each prints its own `N checks, M failed` line.

| Suite | Checks | What it covers |
|---|---|---|
| `kernel/wm/test` | 268 | tiling, tab collapse, workspaces, focus, node-pool reclaim |
| `kernel/theme/test` | 213 | parser, palette, resolvers, and corrupt files under ASan |
| `kernel/avatar/test` | 109 | `.vox` parsing fuzzed over 8,000 mutated files |
| `kernel/svc/test` | 182 | chunked framing split at every pathological boundary |
| `kernel/shell/test` | 2,201 | bind dispatch against a live WM, bar fitting at four widths |
| `kernel/test` | 71 | all 11 headers in one TU; proves the seven `.c` files link |
| **total** | **3,044** | 0 failed |

The kernel also compiles clean under `-Wall -Wextra -Werror -Os` with the real
ESP-IDF cross-compilers for **both** instruction sets — xtensa (`esp32`) and
RISC-V (`esp32c5`) — not just the host clang:

```bash
. ~/esp/esp-idf/export.sh
XT=$(ls -d ~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin | head -1)
$XT/xtensa-esp32-elf-gcc -mlongcalls -std=c99 -Wall -Wextra -Werror -Os -c \
  -Ikernel/wm/include -Ikernel/hal/include -Ikernel/theme/include \
  -Ikernel/shell/include -Ikernel/svc/include -Ikernel/avatar/include \
  kernel/wm/eos_wm.c -o /tmp/x.o
```

This matters more than it sounds: gcc caught two `snprintf` truncation warnings
in `eos_bar.c` that clang did not, and an IDF build with `-Werror` would have
refused them.

## Hard-won facts

Ground truth, paid for on the bench. Do not re-derive these and do not contradict
them.

### ESP32-2432S024N — the "Cheap Yellow Display", 2.4"

| Fact | Detail |
|---|---|
| Silicon | ESP32-D0WD-V3, 4MB flash, **no PSRAM** |
| Panel | ILI9341 320x240 on HSPI: SCK14 MOSI13 MISO12 DC2 CS15, backlight GPIO27 |
| Touch | **There is none.** It is the N variant and looks exactly like a touchscreen. Both boards were probed — I2C and XPT2046 on every candidate pin, 60s hold test. Nothing answered. |
| Heap | ~20KB free at steady state with WiFi + BLE up. This is the number the whole tier-0 design exists for. |
| Bluetooth | **NimBLE (19KB), never Bluedroid (83KB = instant OOM).** Release classic-BT memory and init BLE *before* WiFi. |
| Extras | RGB LED R4/G16/B17, **active LOW**. Speaker on DAC GPIO26. Light sensor GPIO34. |
| Port | `/dev/cu.usbserial-10`, CH340 |
| Clock | 40 MHz. MISO is bound (GPIO12) because the module wires it, and a bound MISO caps what the ESP32 SPI driver will honour. The CYD gets away with 40 only because HSPI pins 14/13/12/15 are the IOMUX pins, so the GPIO-matrix ceiling does not apply. |

### wavvy 4.0" and 3.5" — plain ESP32 WROOM, 4MB, no PSRAM

| Fact | Detail |
|---|---|
| Panel | ILI9488 320x480 **portrait** (rotation 0) on VSPI: DC2 CS5 RST4 SCK18 MOSI23. MISO19 exists but the display does not use it. |
| Pixels | ILI9488 has **no 16-bit SPI pixel mode**. Every pixel costs 3 bytes. That is the hard ceiling on frame rate, and there is no way around it. |
| 4.0" clock | 80 MHz |
| 3.5" clock | **40 MHz.** It is not stable at 80. It renders structured block corruption — blocks and lines, not speckle, because whole bytes are lost mid-block rather than flipped at random. Do not re-diagnose this as a wrong-driver problem: the panel *is* an ILI9488, verified with a visual prober after the corruption initially looked like an ST7796. |
| Upload baud | **230400.** 921600 and 460800 both fail on that CP2102 cable with `Invalid head of packet (0xFF)`, including for esptool reads. |
| Rotation | Tracks how the panel is physically **mounted**, not its size. `0`/`2` are portrait 320x480, `1`/`3` landscape. |

### Waveshare ESP32-C5-LCD-1.47

| Fact | Detail |
|---|---|
| Panel | ST7789 320x172 IPS on FSPI: CS23 CLK7 MOSI6 DC24 RST26 BL10 |
| RGB LED | GPIO8, WS2812 |
| Driver | The Espressif BSP component `waveshare/esp32_c5_lcd_1_47` v1.0.0 owns panel init. It depends on `esp_lvgl_port ^2` and `lvgl >=8,<10`. |
| Offsets | The BSP calls `esp_lcd_panel_set_gap(panel, 34, 0)` and `esp_lcd_panel_invert_color(panel, true)`. 34 = (240-172)/2 — the 172-column panel centred in the ST7789's 240-column frame memory. The profile mirrors both. If this board ever moves off the BSP, those are the numbers. |

### wavvy OLED

SSD1306 128x64 mono on an ESP32-C5. I2C, address 0x3C, SDA GPIO23 SCL GPIO24,
400 kHz. 1bpp, bytes pack vertically, so a 1bpp band must be a multiple of 8 rows.

### Identification

**Panel controllers are not reliably auto-detectable.** The ILI9488 answers
register `0xD3` with `00 7F DF`, matching no known part. The Waveshare C5 does not
wire MISO to the panel at all. `esptool` can tell you the chip type, the flash
size, whether PSRAM answered, and the MAC. That is the entire list.

So board identity comes from the registry plus **one human confirmation**, cached
by MAC afterwards. `tools/probe/probe.ino` exists for that confirmation: it builds
every candidate panel configuration at runtime and draws a labelled test card
under each, so one binary compares them without reflashing. The two wavvy boards
differ only in clock, so their discriminator is a **1px stripe field** — the
densest pattern the bus carries, where a dropped byte shifts phase into visible
rectangles. Solid fills and gradients hide exactly the failure being tested.

Anything claiming to autodetect a panel is lying.

### megabrain

Local models on the Mac mini at `http://192.168.0.139`, port 80.
`GET /ask?stream=1&max=<int>&system=<urlencoded>&q=<urlencoded>`. The response is
HTTP chunked streaming plain text with **no Content-Length**, `X-Accel-Buffering:
no`, and the chunk framing has to be parsed. `/health` answers JSON *with* a
Content-Length, so both body framings are live code, not speculation.

Models: `qwen3.5:2b` (fast default), `gemma4:12b-it-qat` (the clean one),
`ornith:9b`.
