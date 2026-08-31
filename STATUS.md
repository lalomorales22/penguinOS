# penguinos — status

Last integration pass: 2026-08-30.

**One sentence: penguinOS boots on a Waveshare ESP32-C6-LCD-1.3, tiles five windows
with the buddy in one of them, joins the house WiFi, shows its own address on
the panel, and serves a companion app whose entire API the firmware now
answers.** The owner sets a new board up by pointing a phone camera at a QR code
on the glass. Everything through the first flash is verified on hardware;
everything added since — LittleFS, the settings store, the files/console/apps
endpoints, the megabrain client and the avatar window — is verified on the host
and has not been flashed.

What is still missing is `apps/`. There is no terminal, no file browser and no
arcade on the panel; the five windows are the shell's own, and `sys.autostart`
picks which of them has the focus rather than launching anything.

## Component table

| Component | Files | Built | Host-verified | Runs on hardware | Notes |
|---|---|---|---|---|---|
| `kernel/wm` | 2 + test | yes | **268 checks** | yes | Finished and frozen. Not modified since it was written. |
| `kernel/theme` | 2 + 7 themes + test | yes | **213 checks** | yes | Parser is genuinely hardened: survives 200k random buffers, 7,030 single-byte mutations, exhaustive short inputs, all under ASan. |
| `kernel/avatar` | 4 + test | yes | **109 checks** | **not yet flashed** | Renderer proven per-pixel by a painter-order audit — and, since this pass, **drawn**. `firmware/main/eos_shell_draw.c` renders it into the `buddy` window and `firmware/main/eos_buddy_model.c` supplies a compiled-in 372-voxel model so the tile is never empty. |
| `kernel/shell` | 4 + test | yes | **2,201 checks** | yes | Model only — emits segments and actions, draws nothing. |
| `kernel/svc` | 7 + test | yes | **182 + 2,118 + 334 + 368 checks** | partly | `eos_brain`, `eos_httpd`, `eos_net`, `eos_ble`, `eos_radio`, and now `eos_settings` (the 12-key store) and `eos_apps` (files, console, buddy, apps — 14 routes). Only the net/ble/httpd half has run on silicon. |
| `kernel/font` | 2 + data + test | yes | **418 checks** | yes | All four faces: tiny 4x6, small 6x8, med 8x13, big 12x20. 6,597 B of flash. |
| `kernel/test` | 1 | yes | **635 checks** | n/a | Cross-component contracts, plus every generated board header held against the registry. |
| `kernel/hal` | 4 headers + ST7789 backend + storage backend + tests | yes | **109 + 276 checks** | display yes, storage no | The banded RGB565 ST7789 backend has run on the panel. The LittleFS storage backend is implemented and cross-compiles; `EOS_FS_FAT` is not implemented, because no board has verified card pins. |
| `boards/` | schema + 6 profiles | yes | validator + generator | n/a | All 6 validate; all 6 generate headers that compile, link and run against the real HAL, and regeneration is byte-reproducible. |
| `tools/` | flasher, detector, prober, generator, `host_tests.sh` | yes | `--list` / `--identify` exercised | partially | `tools/host_tests.sh` runs all seventeen suites in one command; before this pass every build line lived only in a comment. |
| `firmware/main` | 9 + 2 tests | yes | **24 + 42 checks** | partly | The boot glue. `test_shell_draw.c` renders the whole desktop on the host and writes it as a PPM, which is what `eos_shell_draw.h` had always promised and nothing had done. |
| `web/` | 5 served files | yes | `node --check`, mock firmware | **the app itself, yes** | The contract is implemented end to end. All 31 endpoints resolve; the page has been driven against a mock but not against the board. |
| `apps/` | — | **empty** | — | — | Terminal, files, settings, arcade, CP/M. None exist. |

**32,223 checks across seventeen host suites, all passing**, clean under
`-Wall -Wextra` and `-fsanitize=address,undefined`, and the whole kernel
compiles clean for xtensa and riscv32 at `-Wall -Wextra -Werror -Os`.

```
wm 268   theme 213   avatar 109   brain 182   shell 2201   font 418
integ 635   display 109   qr 23080   net 1276   ble 570   httpd 2118
storage 276   settings 334   apps 368   draw 42   setup 24
```

```bash
./tools/host_tests.sh
```

## Verified this pass

Three agents wrote storage, settings, megabrain and the apps component in
parallel, each guessing at the others' seams. This pass joined them up and
checked the joins.

- **All 31 endpoints in `web/README.md` resolve**, and an unwired board answers
  `501 unsupported` to every one of them rather than 404 or 500.
  `test_httpd.c`'s `test_every_endpoint()` holds the contract's list against the
  one route table and then drives all 31 through the real dispatch.
- **The whole desktop renders on the host**, band by band, through the real
  `eos_shell_draw.c` and the real ST7789 compositor with `ESP_PLATFORM` unset.
  Every pixel of a full-screen frame is composited exactly once; two frames from
  identical state are byte-for-byte identical, which is the check that would
  catch the avatar's in-place voxel sort not being idempotent.
- **The buddy is on the desktop.** `eos_buddy.c` passed 109 checks for a whole
  release without ever being drawn. It now has a window, a compiled-in
  372-voxel model, a shade table built against the live display palette, and
  seven visibly different moods driven by the megabrain request lifecycle.
- The five `brain.*` keys are live in both directions: `app_main` feeds the
  settings store to `eos_brain_bridge_from_settings()` at boot, and
  `eos_settings_bind.c`'s apply hook calls the same function on every patch.
  Before this pass they were stored, reported as live, and only read at boot.
- `/api/system`'s `limits` block now asks `eos_apps` for its numbers instead of
  restating them. A hardcoded `chunk_max` of 4,096 against a board that refuses
  anything over 512 would have turned every upload into a 413 that looked like a
  network fault.
- `tools/host_tests.sh` runs all seventeen suites in one command. Every build
  line used to live only in a comment at the top of its test file.

## Defects found and fixed this pass

| # | Defect | Fix |
|---|---|---|
| 1 | **Palette index 255 was claimed twice.** `eos_display.h` reserved it as `EOS_COLOR_NONE`; `eos_theme.h`'s RGB cube also owned it. `eos_theme_cube_index()` returned exactly 255 for pure white, so on tier SOFT every white pixel silently did not draw. 945 of 636,056 sampled colours landed on it. | The cube gives the cell up. White steps back one *green* level to 251 (36 counts of error) rather than one blue level (85). `eos_theme_index()` now searches 255 entries, not 256. Regression-pinned in both the theme suite and the new integration test. |
| 2 | **Nothing in the tree supplied `min_tile_w` / `min_tile_h`.** The WM needs them, the theme explicitly disclaims them ("they belong to the panel"), and the board registry did not carry them. Only test files had numbers. `eos_wm_cfg_t` was unconstructible. | Added `render.min_tile_w` / `min_tile_h` to the schema, all five profiles (marked `unverified`), the generator's validator and both emitters, and `eos_board_render_t`. |
| 3 | **The web app used `PATCH`** for settings while its own README said every mutation is `POST` "because a small HTTP server's method table is one more thing to get wrong". A firmware author following the prose would have shipped a GET+POST server and settings would 404. | `POST /api/settings`, carrying the same changed-keys-only body. Verb removed from `app.js` and the docs. |
| 4 | **Every shipped theme named a font that does not exist.** All seven said `mono10`/`mono12`; the HAL's only font authority is `eos_font_id_t` = tiny/small/med/big. Every theme silently fell back. | Renamed to the HAL's own identifiers (`mono10`→`small`, `mono12`→`med`), default changed to `small`, and the naming rule written into `eos_theme.h`. |
| 5 | **`eos_buddy_build_shade_lut()` would produce an invisible buddy.** Passing `disp_n = 256`, the obvious thing to do, lets a face shade onto slot 255 and not draw — holes in the brightest places. | Documented at the declaration: pass 255. The function stays HAL-independent by design, so it cannot know this itself. **Closed structurally this pass**: `eos_shell_buddy_shade()` resolves through `eos_display_match()`, which searches 0..254 and cannot return the sentinel, and a check asserts no entry of the built table is `EOS_COLOR_NONE`. |
| 6 | `web/preview.html` was a design mock with baked-in theme data and Google Fonts links, sitting in the directory that gets flashed. | **Moved to `design/preview.html`** and made regenerable: `design/build_preview.py` rebuilds it from the live kernel, so it can no longer go stale. `web/` now holds only the five files that ship. |
| 7 | **`eos_httpd`'s route scan returned 405 on the first path match with a wrong method.** `/api/settings` is the only path in the contract that answers two methods, so a reordered table would have lost its POST. | The scan remembers the method mismatch and keeps looking. Benign for every other row, and pinned by two checks in `test_every_endpoint()`. |
| 8 | **The buddy lost its feet the first time it was ever drawn on a panel.** Its feet and its mouth shared one near-black palette entry. That reads on a teal head and it does not read on the tile's `#1b1b22` surface: at the x-face shade the foot came out within a step or two of the background. Every previous render of this model was ASCII on black, which is why nine host suites never saw it. | The feet get their own mid-slate index. The mouth keeps the near-black, because it is on the head. |
| 9 | **`/api/system` restated `limits.chunk_max` as 512 and `list_max` as 32** rather than asking the component that enforces them. Two copies of a number the client changes behaviour on. | All five limits now come from `eos_apps_chunk_max()` and its four siblings. |
| 10 | **`web/README.md` contradicted itself on one status**: the prose said a path over 95 bytes is `too_big` **(400)**, the error table said **413**. | Prose corrected to 413, which is what `eos_httpd_err_status()` implements. Both are 4xx, so no client retried either way — the sort of disagreement only found by implementing both halves. |

## Defects found and NOT fixed

| Defect | Why not | Who should |
|---|---|---|
| **`render.heap_budget_bytes` is still a guess on five of the six boards.** Only `waveshare-c6-lcd-13` is measured (425,648, radios down). The wavvy figure (64 KB) contradicts the owner's own measurement — `../tft-videos/CLAUDE.md` records free heap at 48–55 KB on that exact firmware. `eos_display_init()` sizes off this number, so a wrong value is an OOM at boot, not a degradation. | Needs a real heap dump on real silicon. Cannot be derived. | First bring-up. |
| **The C5's `render.band_height` (40) disagrees with its own BSP.** `../esp32-c5/bsp/Kconfig` defaults the draw buffer to 20 rows at 172 wide, not 40 at 320. The profile's `reason` text describes the post-rotation logical frame, not the buffer the BSP allocates. | It is a Kconfig knob penguinOS may legitimately set, so the value is not necessarily wrong — but the prose and the BSP have to be reconciled by whoever writes the build. | Build wiring. |
| **`eos_board_fb_bytes()` lies on LVGL boards.** It returns `w*h`, assuming one palette index per pixel, so on the C5 it returns 55,040 — a number with no meaning, since LVGL owns the buffers and the profile correctly derives `EOS_FB_BYTES = 0`. | It is a one-line guard on `render.compositor == EOS_COMP_LVGL`, but changing a HAL function's return contract while no backend exists yet is guessing at which caller is right. | Whoever writes the first LVGL backend. |
| **The buddy has no 1bpp path.** `eos_buddy_pix_t` is I8 or RGB565. `wavvy-oled-c5` is tier SOFT with the `mono1` compositor, so the buddy cannot be drawn on it at all. | A real feature, not a mismatch. Also possibly the right answer — a 2.5D voxel character on 128x64 mono may not be worth it. | Design call. |
| **`eos_buddy_pix_t` and `eos_display_pixfmt_t` are two enums for the same idea, differently numbered.** I8 is 0 in both, but RGB565 is 1 in one and 3 in the other. Anything that casts between them is wrong. | Merging them would couple the avatar to the HAL, which the avatar author deliberately avoided. | The shell glue, which must translate explicitly. |
| **The megabrain `POST` path has never been tested against the real server.** `build_head` sends the raw prompt as `text/plain`. Whether `/ask` accepts that rather than a form encoding is unknown. Only `GET` was verified live. | Needs the server. If it wants a form, `METHOD_AUTO`'s long-prompt fallback fails in production while every host test still passes. | Anyone with the mini up. |
| **The mDNS name `megabrain` is a guess.** Nothing confirms what the mini advertises; the old Arduino sketch only ever used the literal IP. | One string (`cfg.mdns_name`). If wrong, discovery silently falls through to `192.168.0.139`, which works — making the mDNS candidate dead weight rather than broken. | Anyone with the mini up. |
| **App id 0 = terminal is assumed**, taken from the WM test's `APPNAME[]`. `super+return` spawns app 0 on faith. | There is no app registry to check against. | Whoever writes `apps/`. |
| **macOS ASan cannot do leak detection**, so "no leaks" is unproven everywhere. Near-harmless — the kernel allocates nothing by design — but it should not be repeated as verified. | Platform limitation. | A Linux CI run. |
| **Endianness is still untested.** The whole kernel now compiles clean under `xtensa-esp32-elf-gcc -mlongcalls` and `riscv32-esp-elf-gcc -march=rv32imac_zicsr_zifencei` at `-Wall -Wextra -Werror -Os`, so the 32-bit assumption is checked. Both targets are little-endian, so the endian-agnosticism claim is still only a claim. | Needs a big-endian host build. | Anyone adding CI. |
| **`EOS_MAX_DIRS` is 2 while `eos_httpd` has 4 workers.** The `#error` guard only covers `EOS_MAX_FILES >= 4`. | Not reachable today: `eos_httpd` serialises dispatch behind one mutex, so only one directory scan is ever open. It becomes real the moment anything walks a tree off the OS loop. | Whoever makes dispatch concurrent. |
| **`eos_dirent_t` has no `mtime`, but `/api/fs/list` returns one per entry.** `readdir` already `stat`s each entry for its size, so the fs handler `stat`s a second time. | A 4-byte additive field would remove the second `stat`, but it changes a public HAL header for a performance win nobody has measured. | Whoever profiles the Files tab. |
| **A bad `.vox` upload leaves the board with no model.** The parse fills the one voxel pool, so the previous model does not survive it. | A second pool to make it survivable is 5,120 B for a case the editor already prevents. `/api/buddy` says so rather than describing a model that is now half of two. **Softened this pass**: the panel falls back to the compiled-in buddy, so the tile is never empty even then. | — |
| **The buddy's 10 fps costs about 5.3% of the SPI bus, continuously, whenever its tile is visible.** 264 KB/s against 56 KB/s for an idle board with the buddy behind a tab. | It is the only thing on the board that animates, and 4 fps reads as a stutter. `BUDDY_TICK_MS` in `main.c` is the dial and nothing else depends on it. | Anyone measuring power. |

## Margins as built

| Measure | esp32c6 (C6-LCD-1.3) |
|---|---|
| `penguinos.bin` | 1,640,048 B (0x190670) |
| `factory` free | 1,505,680 B of 3 MB (48%) |
| DIRAM static | 242,428 of 452,112 B; **209,684 B remaining at link** |
| free heap after boot, **last measured on hardware** | 173,100 B, largest block 155,648 — and that was before storage, settings, megabrain, `eos_apps` and the avatar, which have added about 39 KB of static DIRAM since |
| `int` | 960 KB, empty |

The heap number to expect on the next flash is in the low 130s. The boot log
says it step by step — `heap after display / storage / shell / ble / wifi /
brain / httpd / buddy` — and if it disagrees with this table, the log is right.

## What has to happen next, in order

Steps 1 to 5 of the old list have landed. `firmware/` builds for
`waveshare-c6-lcd-13` (esp32c6) and cross-builds for `cyd-2432s024n` (esp32);
the whole kernel compiles clean for both cross-compilers at
`-Wall -Wextra -Werror -Os`. The storage backend mounts `/int`, the settings
store applies theme, brightness, hostname, timezone and autostart at boot, the
megabrain client runs and feeds the status bar, the avatar is in a window, and
every endpoint in `web/README.md` is answered.

**The last flash predates all of that.** The next thing that happens is a human
with eyes on the panel, and the six numbers to read off the boot log are the
`heap` lines.

What is left, in order:

1. **Flash it, and read the heap log.** Everything below is sized against a
   number nobody has measured since this run started.

2. **Deploy `web/` onto `/int`.** The five gzipped files are 50,349 B and the
   partition is 960 KB. `eos_apps_bind_files()` already prefers a real file, so
   this is a deploy step and not a code change — and it takes 50 KB of the
   image back out of flash the moment `EMBED_FILES` can be dropped.

3. **One app, and make it the terminal.** `super+return` already spawns app 0,
   `eos_brain` is running, and the buddy's mood events are already driven from
   it. At this point the board does something a person would want.

4. **The tier-0 indexed backend for the CYD.** The ST7789 backend is RGB565
   banded, which is what the C6 wanted. The CYD is the tighter board and the
   one that proves the indexed path; it holds a full framebuffer at 76,800
   bytes. Its `render.heap_budget` is still a guess.

5. **An SNTP client, or an RTC.** `time.synced` is false on every board, the
   clock window shows uptime, and the first file written on a fresh board
   carries a 1970 mtime for ever.

Things worth doing alongside, cheaply:

- Draw the buddy on other boards. `wavvy-oled-c5` is `mono1` and the avatar has
  no 1bpp path, so the `buddy` window on that board would show its fallback
  text. The window degrades honestly; the renderer does not exist.
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
