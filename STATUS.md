# penguinos — status

Last integration pass: 2026-08-30.

**One sentence: penguinOS boots on a Waveshare ESP32-C6-LCD-1.3, tiles ten
windows the owner can open from a super+space app list, close with super+q or
with the x in a tile's header, and drive with a keyboard and a trackpad through
one event queue and one dispatcher.** It joins the house WiFi, shows its own
address on the panel, wears a voxel penguin called Pip, answers megabrain
questions on the glass, and serves a companion app whose entire API the
firmware answers. A new board is set up by pointing a phone camera at a QR code
on the glass.

Everything through the first flash is verified on hardware. **Everything in
this pass is verified on the host only** — the BLE handle-latch fix that makes
the keyboard work at all landed on 2026-08-29 and has not been confirmed on
silicon, and every input path below depends on it.

`apps/` is still empty as a directory, but the thing it was a placeholder for
now exists: `firmware/main/eos_app_registry.c` is a table of ten windows with a
draw function in each row, and the launcher, `/api/apps`, `sys.autostart` and
the tab labels all read that one table.

## Component table

| Component | Files | Built | Host-verified | Runs on hardware | Notes |
|---|---|---|---|---|---|
| `kernel/wm` | 2 + test | yes | **268 checks** | yes | Finished and frozen. Not modified since it was written. |
| `kernel/theme` | 2 + 7 themes + test | yes | **213 checks** | yes | Parser is genuinely hardened: survives 200k random buffers, 7,030 single-byte mutations, exhaustive short inputs, all under ASan. |
| `kernel/avatar` | 4 + test | yes | **109 checks** | **not yet flashed** | Renderer proven per-pixel by a painter-order audit, and drawn into the `buddy` window by `firmware/main/eos_shell_draw.c`. |
| `kernel/shell` | 6 + tests | yes | **2,201 + 204 + 604 checks** | partly | Model only — emits segments, actions, rows and rectangles, and draws nothing. `eos_launcher.c` (the app list) and `eos_pointer.c` (the cursor, the hit test and the close box) are new. |
| `kernel/svc` | 7 + test | yes | **215 + 2,118 + 334 + 368 + 570 checks** | partly | `eos_brain`, `eos_httpd`, `eos_net`, `eos_ble`, `eos_radio`, `eos_settings`, `eos_apps`. `eos_ble.c` now subscribes to every notifying input report and latches the handle that delivers keyboard-shaped ones and the handle that delivers 3-byte mouse ones. |
| `kernel/font` | 2 + data + test | yes | **418 checks** | yes | All four faces: tiny 4x6, small 6x8, med 8x13, big 12x20. 6,597 B of flash. |
| `kernel/test` | 1 | yes | **635 checks** | n/a | Cross-component contracts, plus every generated board header held against the registry. |
| `kernel/hal` | 4 headers + ST7789 backend + storage backend + tests | yes | **109 + 276 checks** | display yes, storage no | The event ring now carries pointer events beside keys. `EOS_FS_FAT` is still not implemented, because no board has verified card pins. |
| `boards/` | schema + 6 profiles | yes | validator + generator | n/a | All 6 validate, generate headers that compile and link, and regeneration is byte-reproducible. |
| `tools/` | flasher, detector, prober, generator, `host_tests.sh` | yes | `--list` / `--identify` exercised | partially | `tools/host_tests.sh` runs all **21** suites in one command. |
| `firmware/main` | 17 + 4 tests | yes | **24 + 55 + 396 + 67 checks** | partly | The boot glue, the scene, the dispatcher, the app table and seven app bodies. `test_shell_draw.c` renders the whole desktop on the host and writes it as a PPM; `test_dispatch.c` drives the input ladder end to end. |
| `web/` | 5 served files | yes | `node --check`, mock firmware | **the app itself, yes** | The contract is implemented end to end. All 31 endpoints resolve; the Buddy tab now loads and saves Pip against the board's own `/api/buddy` limits. |
| `apps/` | — | **empty** | — | — | Superseded by `firmware/main/eos_app_registry.c`. There is no separate process model and nothing plans one. |

**33,546 checks across twenty-one host suites, all passing**, clean under
`-Wall -Wextra` and `-fsanitize=address,undefined`, and the whole kernel
compiles clean for xtensa and riscv32 at `-Wall -Wextra -Werror -Os`.

```
wm 268    theme 213   avatar 109   brain 215   shell 2201  launcher 204
pointer 604  font 418  integ 635   display 109 qr 23080    net 1282
ble 570   httpd 2118  storage 276  settings 334  apps 368  draw 55
appsui 396   dispatch 67   setup 24
```

```bash
./tools/host_tests.sh
```

## The input pipeline, written down

Every key, character and trackpad click on this board arrives in kernel/hal's
32-event ring — from the NimBLE HID host, the GPIO buttons, the phone page or
the serial console — and leaves it in exactly one place:
`eos_shell_input_pump()`. `eos_shell_input_next()` has one caller and there is
no second dispatch path.

What the pump does with each event, in order:

| Rung | Who | What is left |
|---|---|---|
| 1 | **Inactive** — the setup or passkey screen owns the panel | every event is drained and dropped |
| 2 | **Locked** | only the keymap sees a key; the pointer and printable text are dropped |
| 3 | **The launcher**, while it is open | it takes its own keys, and every hover and click; it hands back SUPER chords and keys it does not bind |
| 4 | **The keymap** — the global chords | a chord a bind *claimed* is consumed here whether or not it moved anything |
| 5 | **The focused window** | printable characters, and the keys no bind claimed |
| 6 | dropped | |

Rule 4 beating rule 5 is the one that is not negotiable: an app that decided it
wanted the letter `q` would otherwise stop `super+q` from closing anything.

`test_dispatch.c` drives all six rungs through the real ring, keymap, window
manager, launcher and app table. Nothing in it is mocked, because every bug it
exists to catch is a bug of agreement between two of those.

## Verified this pass

Five agents wrote the pointer, the launcher, the app table, the web Buddy tab
and the megabrain end-of-stream rule in parallel, each guessing at the others'
seams. This pass joined them into one system and checked the joins.

- **One input pipeline.** The frame loop used to drain the ring itself on every
  pass where the desktop was not the visible scene, so one queue had two
  consumers — and the order between them was wrong. See defect 11.
- **One app registry.** `sys.autostart` resolved its stored value against the
  registry's `name` column (the tab label) while `/api/apps` publishes and the
  web picker sends the `id` column. See defect 12.
- **A window can be closed with the trackpad.** Every visible tile draws an x at
  the right of its header, `eos_pointer_close_box()` computes that rectangle,
  and both the painter and the hit test call it — so the x on the glass and the
  x a click lands on are the same rectangle by construction. `test_shell_draw.c`
  proves it by reading the composited panel back and counting ink inside the box
  the dispatcher would test against.
- **The cursor does not fight the banded backend.** A moved arrow declares two
  rects — the hole it left and the place it went — and never the screen. The
  draw suite measures it: a cursor move comes back in strictly fewer bands than
  a full frame and changes fewer than four hundred of the panel's 57,600
  pixels.
- **`super+q` works from inside the launcher**, and closing the last window
  stops changing anything rather than misbehaving.

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
| 11 | **The event ring had two consumers and they disagreed.** `main.c` called `eos_shell_input_pump()` inside the desktop branch and ran its own `while (eos_shell_input_next(&drop)) { }` at the FOOT of the loop for every other screen. The discard therefore ran *after* the desktop branch, so the first pass that reached the desktop dispatched the entire backlog the setup screen had collected — the exact `super+q`-behind-a-screen-you-cannot-see case the discard was written to prevent. | The rule moved inside the dispatcher as `eos_shell_input_set_active()`, one rung of a ladder written down at the top of `eos_shell_input.h`. `main.c` now calls the pump unconditionally on every pass and there is one consumer of the ring in the image. `test_dispatch.c:test_inactive()` pushes four events at an inactive pump and checks the ring is empty afterwards AND that the next active pass has nothing to replay. |
| 12 | **`sys.autostart` was matched against the wrong column.** `apply_autostart()` compared the stored string to `eos_shell_app_names()`, which is the registry's `name` column — the tab label. `/api/apps` publishes `id`, and `web/app.js` sends `a.id` as the picker's value. The two columns are spelled identically for all ten rows today, so nothing was visibly broken; a row that ever named itself differently would have silently stopped autostarting. | `eos_app_index_of(id)`, which matches the `id` column the contract actually names. Pinned in `test_dispatch.c`. |
| 13 | **A locked board could be typed into and clicked through.** `eos_keys_feed()` enforced the lock, but the two paths that never reach it — printable text and every pointer event — went straight to the focused window and the window manager. So `super+escape` locked the desktop and a trackpad still focused tiles, and after this pass would have closed windows. | The lock is enforced once, in the pump, for all three paths. Rung 2. |
| 14 | **A global chord that fired and found nothing to do leaked to the focused app.** The pump tested `r.changed` to decide whether the keymap had consumed a key. `super+left` at the left edge moves no focus, so it came back `handled` and `!changed` — and was handed on to the chat window as a bare left arrow. | Test `r.handled` for consumption and `r.changed` for redraw; they answer different questions and the header now says which. Pinned by binding `super+pgup` to focus-up at the top of the layout, `pgup` being a key chat always consumes and no default bind claims. |
| 15 | **The close box's first version shrank to fit.** A tile too narrow for a full-width box got a smaller one — and `draw_close_x()` refuses to paint anything under five pixels, so the result was a rectangle that closed a window with nothing on the glass to say so. | The box is `close_w` wide or it does not exist. `EOS_POINTER_CLOSE_MIN` is shared by the painter and the geometry, and the pointer suite checks the exact tile width where it switches off. |

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
| **macOS ASan cannot do leak detection**, so "no leaks" is unproven everywhere. Near-harmless — the kernel allocates nothing by design — but it should not be repeated as verified. | Platform limitation. | A Linux CI run. |
| **Endianness is still untested.** The whole kernel now compiles clean under `xtensa-esp32-elf-gcc -mlongcalls` and `riscv32-esp-elf-gcc -march=rv32imac_zicsr_zifencei` at `-Wall -Wextra -Werror -Os`, so the 32-bit assumption is checked. Both targets are little-endian, so the endian-agnosticism claim is still only a claim. | Needs a big-endian host build. | Anyone adding CI. |
| **`EOS_MAX_DIRS` is 2 while `eos_httpd` has 4 workers.** The `#error` guard only covers `EOS_MAX_FILES >= 4`. | Not reachable today: `eos_httpd` serialises dispatch behind one mutex, so only one directory scan is ever open. It becomes real the moment anything walks a tree off the OS loop. | Whoever makes dispatch concurrent. |
| **`eos_dirent_t` has no `mtime`, but `/api/fs/list` returns one per entry.** `readdir` already `stat`s each entry for its size, so the fs handler `stat`s a second time. | A 4-byte additive field would remove the second `stat`, but it changes a public HAL header for a performance win nobody has measured. | Whoever profiles the Files tab. |
| **A bad `.vox` upload leaves the board with no model.** The parse fills the one voxel pool, so the previous model does not survive it. | A second pool to make it survivable is 5,120 B for a case the editor already prevents. `/api/buddy` says so rather than describing a model that is now half of two. **Softened this pass**: the panel falls back to the compiled-in buddy, so the tile is never empty even then. | — |
| **The buddy's 10 fps costs about 5.3% of the SPI bus, continuously, whenever its tile is visible.** 264 KB/s against 56 KB/s for an idle board with the buddy behind a tab. | It is the only thing on the board that animates, and 4 fps reads as a stutter. `BUDDY_TICK_MS` in `main.c` is the dial and nothing else depends on it. | Anyone measuring power. |

## Margins as built

Measured from this pass's build, `idf.py build && idf.py size`, target esp32c6.

| Measure | esp32c6 (C6-LCD-1.3) |
|---|---|
| `penguinos.bin` | 1,713,552 B (0x1a2390) |
| `factory` free | 1,432,688 B of 3 MB (**46%**) |
| DIRAM static | 252,494 of 452,112 B; **199,618 B remaining at link** |
| .bss | 94,032 B |
| free heap after boot, **last measured on hardware** | 114 KB free, largest block 94 KB — measured before this pass |
| `int` | 960 KB |

What this pass cost, and it is all static:

| Thing | Flash | Static RAM | Heap |
|---|---|---|---|
| the close box: geometry, painter, chrome accessor | ~430 B | 0 | 0 |
| the dispatcher's new rungs and the chrome it holds | ~180 B | 8 B (`eos_shell_input_t` 28 -> 36) | 0 |
| the boot log lines that name the above | ~430 B rodata | 0 | 0 |
| **total against the previous build** | **+1,104 B** | **+8 B** | **0** |

Nothing added this pass allocates. The dispatcher, the launcher (316 B), the
cursor (52 B) and the app bodies (2,423 B) are all file statics claimed before
`app_main` runs, so they never appear as a heap step — which is why the boot log
now prints them by name:

```
shell  launcher 10 of 10 apps, N rows of N px, panel NxN
shell  input 36 B, launcher 316 B, cursor 52 B of static RAM; close box 11x10 px, border 1
apps   10 windows, 2423 B of static RAM, led ...
heap   after shell   ...
```

The heap number to expect on the next flash is unchanged from the last one:
about 114 KB. If the boot log disagrees with this table, the log is right.

## What has to happen next, in order

**Nothing in this pass has been flashed, and the one thing everything depends
on is unconfirmed.** The BLE fix that subscribes to every notifying input
report — rather than only the boot-keyboard handle the K809 advertises and then
ignores — landed on 2026-08-29 and no human has watched a panel since.

1. **Flash it and press a key.** In order, cheapest first:
   - `super+q` closes the focused window. If this does nothing, the keyboard is
     still not delivering and every other check below is blocked. The boot log
     line to read is `eos_ble: keyboard reports on handle N`.
   - `super+space` opens a single column of apps with Chat, Buddy, Settings,
     Files, Media and Party in it. Down/up move the highlight and it wraps;
     enter opens; escape closes; a second `super+space` closes it too — those
     are two different code paths.
   - Move the trackpad. Watch for `eos_ble: pointer reports on handle 59`. The
     arrow should follow your finger in both axes; either axis inverted means
     the sign extension is not reaching the cursor.
   - Click the **x** at the right of a tile's header. That window should close.
     Click a tile's body: it should take the focus. Click a tab: it should
     raise.
2. **Read the heap log.** Six `heap` lines plus the three static-RAM lines
   above. If free heap after boot is materially under 100 KB, the thing to cut
   is named in the table above it.
3. **Calibrate the cursor.** `eos_pointer_accel_defaults()` was written against
   no hardware: nobody knows the K809 pad's CPI or its report rate. Sluggish
   means raise `gain_q4` / `max_q4`; skating means lower them. `unity` is the
   precision knob, not the speed one.
4. **The first-move lag.** The loop idles at 250 ms and drops to 33 ms while
   the cursor is visible, but nothing wakes it on a BLE notification — so the
   first report after a quiet spell waits out whatever tick was already
   running. If that reads as lag, the fix is a task notification from the
   notify callback, not a faster idle tick.
5. **`firmware/main/eos_brain_bridge.c`.** Chat on the panel and in the browser
   both end with `! megabrain went quiet` after a 30-second stall, and the
   `brainend` work proved the stall is mid-reply and upstream of
   `eos_brain`'s parser: `eos_brain_bridge.c:518`'s back-pressure `break` skips
   `eos_brain_pump()` entirely, and every timeout in `eos_brain` lives inside
   `pump`. A serial log of the brain task during a stream would settle it.
6. **Deploy `web/` onto `/int` gzipped.** The five files are now 191 KB
   uncompressed and every byte is a byte of flash, because
   `firmware/main/eos_web_embed.c` `EMBED_FILES`es them plain.
   `eos_httpd` already probes `<path>.gz` first; embedding the gzipped assets
   takes 191 KB to about 57 KB. That is the single largest flash win available
   and it is a build change, not a code change.
7. **The tier-0 indexed backend for the CYD.** The ST7789 backend is RGB565
   banded, which is what the C6 wanted. The CYD is the tighter board and the
   one that proves the indexed path; it holds a full framebuffer at 76,800
   bytes. Its `render.heap_budget` is still a guess.
8. **An SNTP client, or an RTC.** `time.synced` is false on every board, the
   clock window shows uptime, and the first file written on a fresh board
   carries a 1970 mtime for ever.

Things worth doing alongside, cheaply:

- **A text filter in the launcher.** `eos_keys.h` promises plain keys go to
  "the launcher's own text field". There isn't one, and `j`/`k` are bound as
  list movement — whoever adds a filter must give it precedence over those two.
- **Scroll.** penguinOS deliberately has none: a 4-byte HID report is a scroll
  wheel on most devices and is *this* keyboard's consumer-control page on
  handle 67, and HOGP notifications carry no report id to tell them apart.
  Decoding four bytes would turn a volume key into a middle-button click.
- Draw the buddy on other boards. `wavvy-oled-c5` is `mono1` and the avatar has
  no 1bpp path, so the `buddy` window on that board shows its fallback text.
- Move `kernel/hal`'s 220-check author harness into `kernel/hal/test/`. It
  stubs all 58 externs and fuzzes the geometry and path helpers, and it is the
  only thing standing between the HAL headers and silent rot. It currently
  exists only in a scratch directory.
- Run `python3 tools/gen_board_header.py --all` in CI before the integration
  suite.
- Decide `wavvy-oled-c5`'s tier. It is a tier-1-class C5 running the tier-0
  `mono1` compositor. Currently declared tier 0, which is defensible, but it is
  the one place the registry departs from "C5 class = tier 1".
