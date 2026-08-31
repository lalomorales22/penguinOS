# firmware/ — the ESP-IDF project

One IDF project builds one image for one board. Which board is a build-time
argument, never a source edit: `-DEOS_BOARD_ID=<id>` picks the
`boards/generated/<id>.h` the kernel compiles against, and the IDF target is
read back out of that header so the two cannot drift.

The kernel sources are not in this directory. They stay in `kernel/` and are
listed by relative path from `components/eos_kernel/CMakeLists.txt`, because the
same files are compiled by six host suites with a plain `cc` and a copy under
`firmware/` would rot the first time someone fixed a bug in the wrong one.

## Layout

| Path | What it is |
|---|---|
| `CMakeLists.txt` | project, board selection, board/target guard |
| `sdkconfig.defaults` | defaults true of every board, whatever its silicon |
| `sdkconfig.defaults.esp32` | ESP32 only: IRAM is 668 bytes short with Wi-Fi and NimBLE resident, and the default Wi-Fi buffer pools cost 58KB on a 148KB board |
| `sdkconfig.defaults.esp32c6` | C6 only: native-USB console, 160MHz |
| `sdkconfig.defaults.esp32s3` | S3 only: octal PSRAM, 240MHz, and the 16MB partition table |
| `partitions.csv` | the 4MB layout, used by seven of the eight boards |
| `partitions-16mb.csv` | the S3's layout: 4MB app, 11.9MB filesystem |

## Building for a specific board

The board is a build-time choice and never a source edit. Two things must both
be right, and getting either wrong fails in a way that does not name itself:

```bash
idf.py -B build/<board-id> \
       -DEOS_BOARD_ID=<board-id> \
       -DSDKCONFIG=build/<board-id>/sdkconfig \
       set-target <target> && \
idf.py -B build/<board-id> \
       -DEOS_BOARD_ID=<board-id> \
       -DSDKCONFIG=build/<board-id>/sdkconfig build
```

`EOS_BOARD_ID`, not `EOS_BOARD_PROFILE`. Nothing reads the latter, so passing it
silently builds the DEFAULT board instead of the one you asked for. Across
different targets that trips the mismatch guard below; across the SAME target it
succeeds and flashes one board's panel geometry onto another, reporting success.
`tools/flash.sh` did exactly this for a long time. CMakeLists.txt now hard-fails
on the old spelling rather than ignoring it.

`SDKCONFIG` per build directory. Every board sharing one `firmware/sdkconfig`
was harmless while the whole fleet was `esp32c6`, and breaks the moment two
TARGETS exist: `set-target` rewrites the shared file, and the next board's build
stops with

    Project sdkconfig was generated for target 'esp32s3', but CMakeCache.txt
    contains 'esp32c6'

Separate `-B` directories are NOT enough on their own. `tools/flash.sh` handles
both of these for you; the incantation above is what it runs.

The target must match what the profile says. `CMakeLists.txt` cross-checks
`chip.target` from the board profile against the build tree's target and refuses
to continue if they differ - the one guard that catches a wrong-board build.
| `sdkconfig.defaults.esp32c6` | C6-only overrides; IDF appends it automatically |
| `partitions.csv` | the partition table, with the 4 MB arithmetic |
| `main/main.c` | `app_main`: the boot path, the mode choice, and the frame loop |
| `main/eos_boot_theme.c` | theme search — sd, internal fs, embedded, default |
| `main/eos_brain_bridge.c` | the one task that owns `eos_brain`, and the ring the HTTP workers drain |
| `main/eos_settings_bind.c` | `/api/settings`, `/api/system`, `/api/themes`: the board's answers |
| `main/eos_buddy_model.c` | the buddy compiled into the image, for a board with no `buddy.vox` |
| `main/eos_shell_draw.c` | the scene: tiles, tab strips, close boxes, status bar, the launcher overlay, the avatar, the cursor. No IDF calls |
| `main/eos_shell_input.c` | **the one dispatcher.** Every event leaves the HAL's ring here; see *The input pipeline* below |
| `main/eos_app_registry.c` | **the one table of windows**, with a draw function in each row |
| `main/eos_app_basic.c` | the clock, board, heap, keys and settings bodies |
| `main/eos_app_chat.c` | megabrain on the panel, through `httpd.ports.brain_*` |
| `main/eos_app_files.c` | a read-only browser over `/int` |
| `main/eos_app_media.c` | the WS2812: colour, brightness, effects. Light, not audio |
| `main/eos_app_party.c` | the demo: Pip dancing, the LED cycling |
| `main/eos_led.c` | the WS2812 on GPIO8, over RMT, and the effect engine |
| `main/eos_setup_screen.c` | the three full-screen scenes: setup, passkey, message. No IDF calls |
| `main/test/test_setup_screen.c` | host test: renders those scenes and checks the QR module for module |
| `main/test/test_shell_draw.c` | host test: renders the desktop, writes it as a PPM, and reads the close boxes back out of the composited panel |
| `main/test/test_apps_ui.c` | host test: every app body, at every size, with no clip, twice |
| `main/test/test_dispatch.c` | host test: the input ladder, end to end, nothing mocked |
| `components/eos_kernel/` | the kernel as an IDF component |
| `components/eos_kernel/eos_board_active.c` | `eos_board_get()` and `eos_board_probe()` |

`build/`, `sdkconfig`, and `managed_components/` are gitignored by the root
`.gitignore`. `dependencies.lock` is not, and should be committed.

## The boot path

`app_main` does twelve things, in an order that is load-bearing rather than
alphabetical.

| # | Step | Why it is where it is |
|---|---|---|
| 0 | `eos_apps_init()` + `eos_apps_log_install()` | **before anything logs.** It is BSS and touches no hardware, and installing the log hook here is the difference between the Console tab showing this whole boot and showing only what was typed into it afterwards. It also registers the files, console, buddy and apps routes |
| 1 | `eos_board_get()`, `eos_board_probe()`, `eos_board_check()` | board identity is not probeable; the three facts that are get checked before anything is driven |
| 2 | `eos_display_init()` | the only call in the image that takes heap and keeps it. Seeds its own LUT from `eos_theme_default()`, so a board that finds no theme still draws in real colours |
| 3 | `eos_storage_init()` + `eos_apps_buddy_reload()` | **after the panel and before everything that reads a file.** A first boot formats a blank 960 KB partition and that is a stall the glass should be lit for. A mount that fails is a log line naming the partition, never a stop: every caller above storage falls back to something compiled in |
| 4 | `eos_settings_load()`, then TZ and backlight | between storage and theme because it names the theme, and before the network because it names the mDNS host. A truncated, garbled, empty or missing file leaves the store holding defaults and the board booting |
| 5 | `eos_boot_theme_prefer()` + `eos_boot_theme_load()` + `eos_boot_theme_upload()` | after the display, because the upload is an update to a LUT that already holds something usable; after settings, because `ui.theme` is what it goes looking for |
| 6 | `eos_wm_init()`, `eos_keys_defaults()`, five `eos_wm_open()` calls | after the theme, because `eos_wm_cfg_t` wants `min_tile_w`/`min_tile_h` from the BOARD and `gap`/`bar_h`/`tab_h` from the THEME at the same moment. The windows are opened here even when SETUP is what gets drawn: a banded backend fixes its bands when the frame opens |
| 7 | `eos_setup_screen_message()` | **before anything slow.** Step 9 blocks for up to fifteen seconds and a black panel for that long reads as a dead board |
| 8 | `eos_ble_on_passkey()` + `eos_input_init()` | brings up the NimBLE HID host. Before WiFi: the controller wants a large contiguous block and the WiFi stack fragments the heap |
| 9 | `eos_net_idf_defaults()`, `eos_net_init()`, `eos_net_start()` | the three-state boot in `docs/provisioning.md`. Returns OK in both landing states — reaching SETUP because a join failed is an outcome, not an error. `net.host` from the store becomes `ncfg.hostname`, which is also the mDNS name |
| 10 | `eos_httpd_init()`, `eos_httpd_idf_bind()`, `eos_web_embed_bind()`, `eos_apps_bind_files()`, `eos_settings_bind()`, `eos_brain_bridge_start()`, `eos_httpd_start()` | in that order and no other. `eos_httpd_idf_bind()` assigns the whole port table **by value** and wipes anything set before it, so every other binder comes after it — and all of them come before `start()`, so the first request cannot arrive at a half-wired table |
| 11 | `buddy_adopt()`, `apply_autostart()` | the avatar's shade table maps model colours onto DISPLAY palette indices, so it can only be built once the theme's palette is the loaded one. Autostart moves the focus, which changes which tab of the collapsed group is visible, so it happens before the first frame rather than after it |
| 12 | frame loop | picks one of three screens per pass, and pumps input, net, httpd, apps, settings and the avatar |

### The three screens

The loop chooses per pass. The passkey screen outranks the other two: it is the
only one with a human waiting on it, it lasts seconds, and it can arrive on a
board that is already at the desktop.

| Screen | When | What is on it |
|---|---|---|
| passkey | `eos_ble_status().passkey_shown` | six digits at 3x the 12x20 face (36x60 per digit), the peer name, and `eos_ble_pair_warning()` wrapped underneath |
| setup | `eos_net_mode() == EOS_NET_SETUP` | the QR, the AP name, the AP password, `http://192.168.4.1`, and one status line |
| desktop | otherwise | the five tiled windows and the status bar, exactly as before |

Only the setup screen redraws on a change rather than on a clock: a full frame
is 115,200 B of SPI and nothing on it moves except the status line.

### The setup screen, measured

`main/eos_setup_screen.c` calls no IDF function, so it renders on the host. On a
240x240 panel with the real payload:

| Fact | Value |
|---|---|
| payload | `WIFI:S:penguinos-f048;T:WPA;P:<12 chars>;;` — 41 bytes |
| symbol | version 3, 29x29 modules, ECC level L |
| scale | 4 px per module, chosen at runtime to fit the box left over after the text |
| drawn | 148x148 px at (46,21), quiet zone 16 px on all four sides |
| verified | every one of the 841 modules, all 16 pixels of each, matches `eos_qr_module()` |
| foot | AP name in 8x13, password in 12x20, URL and status in 6x8; last ink at row 236 of 239 |

### Running that check

`main/eos_setup_screen.c` calls no IDF function, so the check above is a `cc`
line. It links a minimal `eos_display` backend of its own, because the ST7789
host build composites forty rows at a time and `eos_display_host_band()` is only
valid while the frame is open — there is no way to read all six bands back from
outside the band loop.

```bash
cc -std=c99 -Wall -Wextra -Werror -Wpedantic -O1 \
   -Ikernel/hal/include -Ikernel/wm/include -Ikernel/theme/include \
   -Ikernel/font/include -Ikernel/qr/include -Ifirmware/main \
   firmware/main/test/test_setup_screen.c firmware/main/eos_setup_screen.c \
   kernel/qr/eos_qr.c kernel/font/eos_font.c kernel/theme/eos_theme.c \
   -lm -o /tmp/tss && /tmp/tss
```

24 checks, 0 failed, and clean under `-fsanitize=address,undefined`. Panel size
is a compile-time argument: add `-DW=128 -DH=64` and it runs the OLED's
text-only path instead and reports 14. `DUMP=1` in the environment prints each
screen as ASCII, two rows per line, which is how the layout was tuned.

A 60-byte payload moves the symbol to version 4 (33 modules) and the scale to 3,
and still fits. A panel that cannot give the symbol **two** pixels per module and
still hold the four text lines gets the text-only layout instead — that is the
128x64 OLED, by design, and it is what `docs/provisioning.md` asks for. The QR is
drawn black on white and ignores the theme completely: an amber-on-black symbol
in a dark theme is a decoration, and its failure is silent.

### Heap, logged at every step

Every init step prints what it cost, because the first flash is the only place
the real numbers exist and the pre-radio figure this project's tier decisions
were made against no longer holds.

```
heap   at app_main    <n> free, <n> largest
heap   after display  <n> free, <n> largest, this step took <n>
heap   after shell    ...
heap   after ble      ...
heap   after storage  ...
heap   after wifi     ...
heap   after brain    ...
heap   after httpd    ...
heap   after buddy    ...
heap   boot cost <n> B of the <n> free at app_main; <n> left, largest block <n>
```

`heap after buddy` reports about zero, and that is the point: the avatar's
offscreen box, its shade table and the compiled-in model are all BSS, claimed
before `app_main` ran. The line above it names the number instead:

```
buddy  compiled-in model, 372 voxels, 80x80 px box, 7184 B of BSS
```

### Theme search order

`kernel/theme` guarantees that a missing or corrupt theme cannot stop the boot.
The search honours that at every step — each failure is a log line.

| Order | Source | Status on this board today |
|---|---|---|
| 1 | `<sd_point>/themes/<ui.theme>.json` | skipped; the C6-LCD-1.3 declares no card slot |
| 2 | `<int_point>/themes/<ui.theme>.json` — `/int/themes/gruvbox.json` | a real open on a mounted LittleFS. Empty on a board nobody has written a theme to |
| 3 | `<sd_point>/theme.json` | skipped, same reason as 1 |
| 4 | `<int_point>/theme.json` | the older convention, kept so a board provisioned before `ui.theme` existed still works |
| 5 | the copy linked into the image (`EMBED_TXTFILES`) | **this is what runs today**: `kernel/theme/themes/cyd-amber.json`, ~1.7 KB of rodata |
| 6 | `eos_theme_default()` | unreachable unless the embedded copy is corrupt |

Steps 1 and 2 are what `ui.theme` in the settings store selects; the name is the
file **stem**, not the `name` field inside the file, because the stem is what
the loader builds a path from and a file whose two names disagree would give a
picker entry that vanishes the moment it is chosen. A theme switch through
`POST /api/settings` re-runs the same search and reprograms the CLUT live —
colours follow immediately, `gap`/`bar_h`/`tab_h` do not and come back in
`reboot_required`.

Step 3 exists so the board comes up looking like penguinOS rather than like the
neutral slate fallback, and it is the only thing in the image that runs
`eos_theme_parse()` on RISC-V — 213 host checks say the parser is right, and
none of them ran on target.

### The input pipeline

There is **one** event queue and **one** dispatcher. Keys from the BLE HID host,
characters from the phone page, bytes from the serial console, GPIO buttons and
the trackpad's clicks all arrive in kernel/hal's 32-event ring as the same
`eos_event_t`, and every one of them leaves that ring in
`eos_shell_input_pump()`. `eos_shell_input_next()` has exactly one caller.

The frame loop calls the pump on **every** pass, whatever is on the glass. It
used to call it only inside the desktop branch and run a discard loop of its own
at the foot of the loop for the other two screens — one queue, two consumers,
and the discard ran *after* the desktop branch, so the first pass that arrived
at the desktop dispatched the whole backlog the setup screen had collected.
`eos_shell_input_set_active()` moved that rule inside the dispatcher, where it is
rung 1 of a ladder rather than a second code path.

The ladder, in order, and the full argument for it is at the top of
`main/eos_shell_input.h`:

| Rung | Who | What it takes |
|---|---|---|
| 1 | **inactive** — setup or passkey owns the panel | everything, dropped |
| 2 | **locked** | everything; only the keymap sees a key, and the pointer and text are dropped |
| 3 | **the launcher**, while open | its own keys, and every hover and click. It hands back SUPER chords and keys it does not bind |
| 4 | **the keymap** | the global chords. A chord a bind *claimed* is consumed whether or not it moved anything |
| 5 | **the focused window** | printable characters, and keys no bind claimed |
| 6 | dropped | |

Rung 4 beating rung 5 is not negotiable: an app that decided it wanted `q` would
otherwise stop `super+q` closing anything.

An app's answer never sets `moved`. `moved` means the **layout** changed and the
whole screen has to be repainted, which is the expensive answer on a banded
backend; an app that took an arrow key changed one tile and says so through its
own dirty flag, which `eos_app_damage()` turns into a single tile-sized rect.

`main/test/test_dispatch.c` drives all six rungs through the real ring, the real
keymap, the real window manager, the real launcher and the real app table. 67
checks, nothing mocked — every bug it exists to catch is a bug of agreement
between two of those.

### Closing a window

Two ways, one call. `super+q` is `EOS_ACT_CLOSE`, and every visible tile draws
an **x** at the right of its header that a click closes it with. Both end in
`eos_wm_close()`, so a window shut with the trackpad and a window shut with the
keyboard leave the tree in the same state — `eos_wm` owns which sibling absorbs
the space and where the focus lands, and neither decision is repeated anywhere
else.

The box is 11 px wide and as tall as the border plus the header (10 px on this
board), growing **up** through the border to the tile's own top edge so the
target is bigger than the glyph without the glyph moving out of the title row.
`eos_pointer_close_box()` computes it, and both the painter in
`eos_shell_draw.c` and the hit test in `eos_pointer.c` call that one function:
the x on the glass and the x a click lands on are the same rectangle by
construction, not by agreement. `test_shell_draw.c` proves it from the other
end, by reading the composited panel back and counting ink inside the rectangle
the dispatcher would test against.

A box is `close_w` wide or it does not exist. A tile that cannot spare it and
still keep half its header for the window's name gets none — a shrunken box
would be a rectangle that closes a window with nothing drawn on it.

### The app launcher

`super+space`. A single column over the whole desktop, arrow keys or `j`/`k` to
move, enter to open, escape or a second `super+space` to close, and a click
outside the panel closes it too. The list is `eos_app_count()` /
`eos_app_at()` — the registry, unfiltered and in registry order — so a window
added to the table appears in the overlay without anyone remembering to add it
twice.

Picking an app that already has a window on the current workspace **focuses**
that window rather than opening a second copy of it. Five windows already fill a
240x240 panel and a launcher that answers "buddy" with a sixth buddy tile makes
the desktop worse every time it is used.

Its geometry is computed by the scene, which knows the panel and the theme's UI
face, stored once, and read back by both the renderer and the hit test — the
same pattern the close box uses, for the same reason. It is recomputed on a live
theme change, because a theme may name a different face and the row height is
derived from the face's height.

### The cursor

Three-byte HID boot-mouse reports on the K809's trackpad handle. Byte 0 is
buttons, bytes 1 and 2 are signed dx and dy. Exactly three bytes and never
four: four bytes is a scroll wheel on most devices and is *this* keyboard's
consumer-control page on another handle, and HOGP notifications carry no report
id to tell them apart — so penguinOS has no scroll, deliberately.

`kernel/shell/eos_pointer.c` owns where the cursor is. The HAL takes absolute
pixels because it has no idea how big the screen is; the radio hands over signed
relative counts; the acceleration, the sub-pixel accumulation and the clamp in
between are all integer, because this chip has no FPU.

On a tiling window manager a click is deliberately only three things: focus the
tile under it, raise the tab under it, or close the window whose x it landed on.
There is no drag and no edge-resize — a tiling layout is computed from a tree,
so a dragged window has nowhere to land.

### What is on the screen

Ten windows in the table; six open at boot — five on workspace 1, one on
workspace 2 so the bar's pips have something to show. The other five are opened
from the launcher. With `min_tile_w` 80 in a 117 px tile, the third split cannot
give both children the minimum and **collapses into a tab group**, which is the
one window-manager rule this board exists to demonstrate.

| Window | Rect | Face | Content |
|---|---|---|---|
| `clock` | 4,18 114x218 | 12x20 | uptime. **Not wall time** — this board has no RTC and no SNTP client |
| `board` | 122,18 114x107 | 8x13 + 6x8 | its address once it has joined, otherwise soc, flash, panel, bus |
| `heap` | tab 0 of 3 | 8x13 + 6x8 | free and largest-block, live. Behind a tab |
| `keys` | tab 1 of 3 | 6x8 | four binds formatted out of the real keymap by `eos_keys_format()` |
| `buddy` | tab 2 of 3, 122,145 114x91 | — | the voxel avatar, and the mood megabrain has put it in |

The buddy is opened **last** and that is deliberate on both counts. Last means
it lands in that tab group as the visible tab, so the avatar is on the glass
from the first frame without anybody pressing anything; and it means the four
windows that were verified on hardware keep the rects they had, because a fifth
window here only lengthens the tab strip.

The status bar is `eos_bar_build()` output, not hand-placed text: workspace
pips, focused title, mood, heap, brain, wifi and clock, fitted to 236 px in the
bar model's own priority order. The window ids in each tile's top-right corner
are the 4x6 face, so all four shipped faces are on the glass and a bad glyph
table cannot hide.

### The buddy window

`kernel/avatar/eos_buddy.c` is a painter-order voxel renderer with a mood state
machine. It passed 109 host checks for a whole release and had never once been
drawn on a screen. It is now a window.

**Why it is rendered once and blitted, rather than drawn.** The scene in
`eos_shell_draw.c` is replayed **once per band** — six times on a 240x240 panel
with a different 40-row clip installed each time — and every replay must produce
identical pixels. `eos_buddy_render()` cannot satisfy that: it writes whole
pixels into a buffer of its own, and it REORDERS the model's voxel array in
place, far to near, which is how the painter sort costs no RAM. So it runs once
per frame, before `eos_display_frame_begin()`, into an offscreen 8-bit indexed
box; the per-band job is one clipped `eos_display_blit()` of a picture that is
already finished. `test_shell_draw.c` asserts that two frames drawn from
identical state come out byte for byte identical, which is the only check that
would catch a sort that was not idempotent.

**The box is a fixed size, not the tile's.** `EOS_SHELL_BUDDY_PX` is 80, so the
box is 6,400 bytes and the render is fitted inside `min(body, 80)` and centred
in the rest. Sizing it to the tile would mean a theme with no bar and no tab
strip could ask for 230x220 — 50 KB of BSS for a window that might be behind a
tab. On this board the body is 110x76, the mood line takes 10 of that, and the
buddy renders at 80x66.

**The shade table.** An 8-bit indexed target cannot shade by arithmetic, so it
needs a 3x256 map of (face orientation, model palette index) to display index.
`eos_shell_buddy_shade()` builds it through `eos_display_match()` rather than
through `eos_buddy_build_shade_lut()`, for two reasons: the HAL hands out no
copy of its loaded palette, and a second copy taken from the theme would be a
second copy to go stale; and `eos_display_match()` searches 0..254 and can never
return `EOS_COLOR_NONE`, which removes the hazard `eos_buddy.h` warns about —
defect 5 in `STATUS.md` — structurally rather than by remembering. It is rebuilt
at boot, at every model reload, and **after every live theme switch**, because a
table of indices built against the old palette would leave the buddy wearing the
previous theme.

**The model.** `/int/buddy/buddy.vox` wins if it is there. It is not there on any
board that exists, so `main/eos_buddy_model.c` builds the compiled-in one: the
same 11x7x15 shape — feet apart, bevelled torso, head wider than the body, eyes
on the -y face — that `kernel/avatar/test/test_vox.c` has been rendering to
ASCII since the component was written. 372 voxels, and it stores only the shell:
`eos_vox_finish()` would mark every inward face of a shell exposed, so the file
writes the face masks itself from the solid shape the voxels were sampled from.
That is a third of the pool and half the faces drawn.

The feet are a mid slate and the mouth is near-black, and that difference is a
bug fix rather than a style choice. Every previous render of this buddy was
ASCII on black; on the panel the tile's surface is `#1b1b22` and a near-black
foot at the x-face shade came out within a step or two of it. The buddy lost its
feet in the first frame this window ever drew.

**Where the mood comes from.** `eos_brain_bridge` posts the megabrain request
lifecycle into an event queue; the frame loop drains it into
`eos_buddy_event()`. The word under the avatar is `eos_buddy_state_name()`
lowercased, which is the same string `/api/buddy` reports. The bar's mood glyph
is the same machine — `eos_bar_mood_t` is `eos_buddy_state_t` by another name.

### Rendering the desktop without a board

`main/eos_shell_draw.c` calls no IDF function, so the whole scene renders on the
host — and since this run something takes the header up on that. The suite
composites every band through the real ST7789 backend with `ESP_PLATFORM`
unset, checks what it drew, and with a path argument writes the frame out as a
PPM.

```bash
cc -std=c99 -Wall -Wextra -Werror -O1 \
   -Ikernel/hal/include -Ikernel/wm/include -Ikernel/theme/include \
   -Ikernel/shell/include -Ikernel/font/include -Ikernel/avatar/include \
   -Iboards/generated -Ifirmware/main \
   firmware/main/test/test_shell_draw.c firmware/main/eos_buddy_model.c \
   kernel/hal/backend/esp_lcd/eos_display_st7789.c kernel/wm/eos_wm.c \
   kernel/theme/eos_theme.c kernel/shell/eos_bar.c kernel/shell/eos_keys.c \
   kernel/font/eos_font.c kernel/avatar/eos_vox.c kernel/avatar/eos_buddy.c \
   -lm -o /tmp/tdraw

/tmp/tdraw                          # 42 checks, 0 failed
/tmp/tdraw /tmp/desktop.ppm         # and write the frame
/tmp/tdraw /tmp/think.ppm thinking  # in any of the seven moods
```

It `#include`s `eos_shell_draw.c` rather than linking it, because
`eos_display_host_band()` only answers while a frame is open and the band loop
is inside `eos_shell_draw_frame()`. Including the translation unit lets the test
re-run the identical three-step frame with one line added, instead of a hook
being cut into production code that only a test would ever use.

What the 42 checks are for, beyond "it drew something": the compiled-in model is
372 voxels with no buried voxel and no empty one; the shade table never resolves
to `EOS_COLOR_NONE`; every pixel of a full-screen frame is composited exactly
once; two frames from identical state are identical byte for byte; a tile-only
damage rect redraws the tile identically; a NULL buddy, an empty model and a
buddy behind a tab each skip the render and say so; and all seven moods draw.

### Redraw discipline

| Event | Damage declared |
|---|---|
| boot | `eos_display_damage_all()` |
| a keybind moved something | `eos_display_damage_all()` — a move can change every tile and the pips |
| a live theme switch | `eos_display_damage_all()`, and the shade table is rebuilt first |
| the second ticked | the bar rect and the `clock` tile's rect, nothing else |
| a net event fired | the same two rects on the desktop; a whole frame on the setup screen |
| the buddy tile is visible | that tile's rect, **every pass** |
| an app changed its picture | that app's tile rect, one per app, via `eos_app_damage()` |
| the cursor moved | **two rects and never the screen**: the hole it left and the place it went |
| the screen changed | `eos_display_damage_all()` |
| nothing | none; the loop sleeps 250 ms, 100 ms while the buddy or an animating app is on screen, 33 ms while the cursor is visible |

The cursor is the one thing on the glass that can move every frame, and it is
the reason the damage rule above is a rule. A trackpad reports about fifty times
a second; a moved arrow that damaged the screen would ask a banded backend with
no framebuffer for 115,200 B per report. Two 7x11 boxes are 154 pixels. The
draw suite measures it rather than asserting it: a cursor move comes back in
strictly fewer bands than a full frame and changes fewer than four hundred of
the panel's 57,600 pixels.

`eos_pointer_commit()` runs **after** the draw and never before. The damage is
the difference between where the arrow was and where it is; committing early
collapses that difference to nothing and leaves the old arrow on the glass.

The avatar is the only thing on this board that animates, so it is the only
thing that earns a faster loop. It bobs, blinks and eases its yaw; at 4 fps that
reads as a stutter and at 10 fps it does not. The band engine sizes its strips
from the damage rect rather than from a fixed full-width band, so a buddy frame
costs the tile's own 114x91 and nothing else.

| State | Pushed per second | Share of the 40 MHz bus |
|---|---|---|
| idle, buddy behind a tab | 56,424 B — the 240x14 bar plus the 114x218 clock tile | ~1.1% |
| idle, buddy on screen | 263,904 B — the above plus 114x91 ten times | ~5.3% |
| a full frame | 115,200 B | — |

`BUDDY_TICK_MS` in `main.c` is the dial. Nothing else in the image is affected
by it: off the desktop, and with the buddy behind a tab, the loop is back at
`IDLE_TICK_MS`.

## The API

`web/README.md` is the contract and it is implemented. Thirty-one endpoints
across five owners, one route table, and one `eos_httpd_dispatch()`.

| Endpoints | Answered by | Bound in `app_main` by |
|---|---|---|
| `/api/net/status`, `/api/wifi/*`, `/api/ble/*` | `kernel/svc/eos_httpd.c` | `eos_httpd_idf_bind()` |
| `/api/settings` (GET, POST), `/api/system`, `/api/system/health`, `/api/system/reboot`, `/api/themes` | `main/eos_settings_bind.c` | `eos_settings_bind()` |
| `/api/fs/*`, `/api/console/*`, `/api/buddy`, `/api/buddy/reload`, `/api/apps` | `kernel/svc/eos_apps.c` | `eos_apps_init()`, through `eos_httpd_set_api()` |
| `/api/brain/{status,ask,cancel}` | `main/eos_brain_bridge.c` | `eos_brain_bridge_bind()` |
| `/`, `/style.css`, `/app.js`, `/setup.js`, `/voxel-editor.js` | `main/eos_web_embed.c`, wrapped by `eos_apps_bind_files()` | both, in that order |

Three agents added rows to that one route table in parallel and each guessed at
the others'. `test_httpd.c`'s `test_every_endpoint()` is the reconciliation
written down: it holds the complete list from `web/README.md` against the real
route scan, and then drives every one through the real dispatch on a server with
**nothing bound**. Two properties, and the second is the one that matters — every
path must resolve, and an unwired endpoint must answer `501 unsupported`, never
404 and never 500. `web/README.md`'s error table defines unsupported as "valid
call, not available on this tier", which is the honest thing for a board whose
ports were never assigned to say.

Three details a client will notice:

- **`limits.chunk_max` is 512 and every upload is bounded by it.** `/api/system`
  reports it by calling `eos_apps_chunk_max()`, not by restating the number.
  `eos_apps.c` is what enforces it, and a `chunk_max` reported as 4,096 by a
  board that refuses anything over 512 turns every upload into a 413 that looks
  like a network fault.
- **`GET /api/buddy` 404s on this board even though a buddy is on the panel.**
  That endpoint reports `/int/buddy/buddy.json`, and the avatar you can see is
  compiled into the image. `web/README.md` already treats the 404 as normal on a
  fresh card, which is exactly what this is.
- **`POST /api/system/reboot` arms, it does not restart.** It answers
  `{"ok":true,"in_ms":500}` and the OS loop flushes settings and calls
  `esp_restart()` on the next tick. Restarting inside the handler drops the
  socket mid-response and the page reports a network failure for a reboot that
  worked.

## Provisioning

The whole of `docs/provisioning.md` is implemented in this image. The short
version: a board that has never seen your network brings up a **closed** WPA2
SoftAP called `penguinos-<last 4 of MAC>`, prints its name, its password and a QR of
`WIFI:S:...;T:WPA;P:...;;` on the panel, answers every DNS question with
192.168.4.1 so the phone opens the page by itself, and serves this:

| Method | Path | Answer |
|---|---|---|
| GET | `/api/wifi/scan` | the cached scan; `?rescan=1` queues a fresh one and returns 202 |
| POST | `/api/wifi/connect` | 202 and `{"state":"trying"}`. The client polls `/api/net/status` |
| POST | `/api/wifi/forget` | clears NVS and drops back to SETUP |
| GET | `/api/net/status` | mode, IP, RSSI, hostname, `ap.*`, `join.{state,reason}` |
| GET | `/api/ble/scan` | HID peripherals; 501 when the board declares no keyboard |
| POST | `/api/ble/pair` | 202. The passkey appears on the panel and in `/api/ble/status` |
| GET | `/api/ble/status` | bonded device, connected, battery, live passkey |
| POST | `/api/ble/forget` | drops the bond |

Two rules the code enforces rather than documents:

**Credentials reach flash only after a join succeeds.** There is exactly one
call to `eos_net_commit()` in the whole tree — in `eos_httpd_pump()`, immediately
after an `eos_net_try()` that returned OK. `eos_net_commit()` itself refuses
unless the last try landed, and consumes that success so it cannot be replayed
after a later failure. Save-then-try is how one typo turns into a board that
needs a serial cable, and `test_net.c` walks the try-good/try-bad/commit sequence
and asserts the store's write counter is still zero.

**No handler waits for a radio.** A scan is ~3 s, a join up to 15, and
`esp_http_server` has four workers while a phone gives up in about ten — and the
join takes the radio away from the SoftAP the request arrived on. Every slow
operation is queued and run from `eos_httpd_pump()` on the main task, which is
also why that pump must never move onto an HTTP worker.

## Megabrain

`kernel/svc/eos_brain.c` was complete and tested for a whole release and nothing
started it, which is the entire reason the status bar said "no brain".
`main/eos_brain_bridge.c` starts it, on a task of its own.

**Why a task and not the frame loop.** `eos_brain_pump()` blocks: `connect()` to
a mini that is switched off costs up to three seconds and `mdns_query_a()`
costs two more. On the loop that owns the panel, the first ask on a board whose
NVS cache is cold would freeze the glass for five seconds. 4,096 bytes of stack
buys that back.

**Why one task and not four workers.** `eos_brain_t` holds a socket, a parser
and a request in one struct with no lock anywhere in it. Exactly one task calls
it — never an HTTP worker, never a callback. Everybody else posts intent under a
mutex held for microseconds, and the reply comes back through a 1 KB ring.

**Back-pressure, not dropped text.** The task stops pumping when fewer than 256
bytes of the ring are free. One `eos_brain_pump(b, 0)` is one `recv()` of at
most `EOS_BRAIN_RX_MAX`, so that headroom is a bound rather than a guess, and a
browser that stops reading becomes a full TCP window on the megabrain socket
instead of a lost token. A bigger ring would not have promised that.

**The status bar and the avatar.** `bar.brain_up` and `bar.brain_model` are read
from the binding, so "no brain" becomes the model name when the mini answers.
The megabrain request lifecycle drives `eos_buddy_t` through an event queue the
frame loop drains, and that machine is now on the glass twice: as the bar's mood
glyph, and as the avatar in the `buddy` window. `eos_bar_mood_t` is
`eos_buddy_state_t` by another name, so neither is a decoration.

**Where the settings come in.** The five `brain.*` keys are marked "live" in
`web/README.md` and they are, in both directions. `app_main` hands the store to
`eos_brain_bridge_from_settings()` at boot, so a configured mini survives a
reboot; `eos_settings_bind.c`'s apply hook calls the same function on every
`POST /api/settings` that touches one of them. All five go over together rather
than one at a time, because `eos_brain_bridge_configure()` takes a whole config
and a patch that changed the host and the model would otherwise apply the new
host against the old model for the length of one call. The bridge applies it
between requests, never mid-stream.

## Serving the web app

`web/README.md`'s rule is: prefer a real file, fall back to the embedded copy,
and never leave the board with nothing to serve. All three binds are in
`app_main` and the order is the rule.

```
eos_httpd_idf_bind(&httpd, ...)   // assigns the port table BY VALUE
eos_web_embed_bind(&httpd)        // the five files, linked in with EMBED_FILES
eos_apps_bind_files(&httpd)       // wraps them: a real file on /int wins
```

`eos_apps_bind_files()` keeps whatever was bound before it and calls it when its
own open misses, so `/int/web/app.js.gz` is served the moment it exists and the
copy in flash answers when it does not. Nothing deploys the five files onto
`/int` yet, so today the embedded copy is what every board serves — which is
what was verified on hardware and is unchanged by this run.

`/api/fs/read` goes through the same ports with the fallback **off**: a GET for
a file that is not there is a 404 and never the contents of a same-named asset
in the image.

## What is not here

Nothing in the boot path is stubbed.

**`EOS_FS_FAT`, and therefore `/sd`.** The microSD slot on this board exists
physically and its pins are not known, so the profile says `sdcard.present
false`. `eos_storage` declares the mount, routes it, and answers `EOS_ERR_NODEV`
immediately without touching a bus. `/api/system`'s `fs` array reports the card
declared and absent, which is what the panel says. Writing an SDSPI mount
against guessed pins would be untestable code.

**A clock.** `time.epoch` comes from `time()` and `synced` is false before 2020,
because there is no SNTP client. The `clock` window shows uptime and says so.
The first file written on a fresh board carries a 1970 mtime for ever.

**`sys.autostart` does not launch anything.** Six windows are open from boot, so
the honest meaning of the key on this board is which one has the focus when the
desktop appears — and that is what `apply_autostart()` does. There is no process
to start. It resolves the stored string through `eos_app_index_of()`, which
matches the registry's `id` column: the same column `/api/apps` publishes and
the same one the web picker sends back.

**A distinct join failure reason.** `eos_net_last_error()` collapses every
association failure into `EOS_NET_ERR_JOIN`, so `/api/net/status` reports
`join.reason` as `failed` and never `bad_auth` or `no_ap`. `web/setup.js` and
`eos_httpd.c` both implement the three-way split already and it lights up the
moment `eos_net` distinguishes them. Until then the phone says a join failed and
cannot say why; the panel says the same thing, and both of them say that nothing
was saved, which is the part that matters.

## Build

```sh
. ~/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32c6
idf.py build
```

`set-target` is only needed the first time, or after changing board. With no
`sdkconfig` present and no explicit `IDF_TARGET`, the project defaults the target
to whatever the selected board's header names, so a fresh tree can go straight to
`idf.py build`.

## Selecting a board

```sh
idf.py fullclean
idf.py -DEOS_BOARD_ID=cyd-2432s024n set-target esp32
idf.py -DEOS_BOARD_ID=cyd-2432s024n build
```

`EOS_BOARD_ID` is a CMake cache variable, so it sticks in a configured build tree
and only has to be passed on the command that configures it. The default is
`waveshare-c6-lcd-13`.

| `EOS_BOARD_ID` | Board | Target | Bridge | Upload baud | Port hint |
|---|---|---|---|---|---|
| `waveshare-c6-lcd-13` | Waveshare ESP32-C6-LCD-1.3 | esp32c6 | usb_serial_jtag | 460800 | /dev/cu.usbmodem101 |
| `waveshare-c5-lcd-147` | Waveshare ESP32-C5-LCD-1.47 | esp32c5 | usb_serial_jtag | 460800 | none |
| `cyd-2432s024n` | ESP32-2432S024N (Cheap Yellow Display 2.4in, N variant) | esp32 | CH340 | 460800 | /dev/cu.usbserial-10 |
| `wavvy-ili9488-35` | wavvy 3.5in ILI9488 | esp32 | CP2102 | 230400 | /dev/cu.usbserial-0001 |
| `wavvy-ili9488-40` | wavvy 4.0in ILI9488 | esp32 | CP2102 | 230400 | /dev/cu.usbserial-0001 |
| `wavvy-oled-c5` | wavvy OLED (ESP32-C5 + SSD1306) | esp32c5 | usb_serial_jtag | 460800 | none |

The headers those ids name are derived from `boards/*.json` and are gitignored.
A fresh clone has none, and the build says so by name:

```sh
python3 tools/gen_board_header.py --all
```

Two failures are caught at configure time rather than on the bench:

| Mistake | What happens |
|---|---|
| `EOS_BOARD_ID` with no generated header | fatal, lists the ids that do exist |
| board needs `esp32`, build tree is `esp32c6` | fatal, prints the `fullclean` + `set-target` line |

The second one matters because flashing an image built against the wrong pinout
produces a panel that never leaves reset and no other symptom.

## Flash

**A human flashes. Not an agent, and not CI.** Every flash on this project is run
by someone with their eyes on the panel.

```sh
idf.py -p /dev/cu.usbmodem101 -b 460800 flash monitor
```

The C6 board has no UART bridge chip: `/dev/cu.usbmodem101` is the SoC's own USB
peripheral, which is why `sdkconfig.defaults.esp32c6` moves the console to USB
Serial JTAG. Without that the board boots correctly and looks dead.

On macOS a native-USB board can enumerate and still produce no `/dev/cu.*` node.
That is System Settings > Privacy & Security > Allow accessories to connect, not
a dead board. See the `macos-accessory-approval` gotcha in the profile.

The wavvy boards' CP2102 cable fails at 460800 and 921600 with "Invalid head of
packet"; 230400 is the working rate and is what the table above lists.

## Restore

The C6's as-shipped image is backed up, with its SHA-256 beside it:

```sh
esptool --port /dev/cu.usbmodem101 write-flash 0x0 \
    boards/waveshare-c6-lcd-13/backup/factory-4MB.bin
```

Back up any board's flash before the first overwrite. The JTAG pin-recovery
technique in `boards/waveshare-c6-lcd-13/README.md` only works while the factory
firmware is still there to be read.

## Partition table

4 MB on every board, `ota_slots` 0 on every board, so the flash is spent once.

| Partition | Type | Offset | Size | Notes |
|---|---|---|---|---|
| — | bootloader | 0x000000 | up to 0x8000 | |
| — | partition table | 0x008000 | 4 KB | |
| `nvs` | data/nvs | 0x009000 | 24 KB | `eos_brain` caches the discovered host here |
| `phy_init` | data/phy | 0x00F000 | 4 KB | |
| `factory` | app/factory | 0x010000 | 3,072 KB | `flashing.app_partition_kb` in every profile |
| `int` | data/littlefs | 0x310000 | 960 KB | `peripherals.internal_fs.partition_label` |

The label `int` is load-bearing. It is what `eos_board_storage_t.int_label`
carries into the firmware; renaming it here fails at mount time with no compile
error anywhere.

The table is IDF's stock `huge_app` — which is what the profiles name in
`flashing.partition_scheme` — plus a filesystem in the 960 KB `huge_app` leaves
dead at the end of flash. The app slot costs nothing for it.

### Margins as built

Measured from this build (`idf.py build && idf.py size && idf.py size-components`),
with WiFi, NimBLE, LittleFS, the HTTP server, the settings store, the megabrain
client, the avatar, the app table, the launcher and the cursor all in the image.

| Measure | esp32c6 (C6-LCD-1.3) |
|---|---|
| `penguinos.bin` | 1,713,552 B (0x1a2390) |
| `factory` free | 1,432,688 B (**46%**) |
| bootloader | 22,176 B |
| bootloader headroom to 0x8000 | 10,592 B (32%) |
| `int` free | 983,040 B less whatever LittleFS costs on format |
| DIRAM static | 252,494 of 452,112 B; **199,618 B remaining** |
| of which `.bss` | 94,032 B |

The static DIRAM split, by archive:

| Archive | DIRAM | What |
|---|---|---|
| `libpp.a` | 47,190 | WiFi PHY/MAC layer |
| `libmain.a` | 39,394 | the boot glue and the scene: `eos_httpd_t` 5,324, the QR pixel buffer 5,330, the avatar's box and shade table 7,184, the compiled-in buddy 1,941, the app bodies 2,423 (chat 1,294, files 1,068), `eos_settings_bind` 1,580, `eos_brain_bridge` 4,620, `eos_net_t` 1,068, `launcher` 316, `input` 36, the rest |
| `libeos_kernel.a` | 30,019 | mostly `eos_apps.c`: a 6,144 B `.vox` staging buffer, a 1,024-voxel pool, the console ring. The cursor is 52 B of it |
| `libble_app.a` | 23,011 | the NimBLE controller |
| `libnet80211.a` | 19,392 | the 802.11 MAC |
| `libfreertos.a` | 14,750 | |
| `libesp_hw_support.a` | 12,553 | |
| `libhal.a` | 10,361 | |
| `libspi_flash.a` | 10,234 | |
| `libphy.a` | 5,629 | |
| `libjoltwallet__littlefs.a` | 136 | the mount's own statics; its pools are heap |

**199 KB of DRAM is what the heap starts from, not the 425,648 B the tier
decisions were made against.** Out of it the display takes 38,400 B for its DMA
strips at `eos_display_init()`, LittleFS takes 1,424 B for the mount plus 648 B
per open file, `eos_brain_bridge` takes about 4.6 KB for a task stack and a
mutex, `eos_led`'s RMT channel and byte encoder take a few hundred, and then
WiFi, NimBLE and `esp_http_server` take their dynamic buffers.

The last measurement on real hardware was **114 KB free after boot, largest
block 94 KB**, with everything up. Nothing added since allocates: the launcher,
the cursor, the dispatcher and every app body are file statics claimed before
`app_main` runs, so they never show as a heap step and the boot log names them
instead:

```
shell  launcher 10 of 10 apps, N rows of N px, panel NxN
shell  input 36 B, launcher 316 B, cursor 52 B of static RAM; close box 11x10 px, border 1
apps   10 windows, 2423 B of static RAM, led up on GPIO8
```

If the boot log's `heap` lines disagree with this table, the log is right.

## What is deliberately not configured

| Not here | Why |
|---|---|
| LVGL | the C6 registry row says `compositor: lvgl`, and the backend that draws this image is not LVGL — it is `kernel/hal/backend/esp_lcd`, a banded RGB565 compositor. Either that row or the tier-to-backend table wants reconciling; the code reads `render.band_h` and ignores `render.compositor`. |
| a second `.vox` pool | a bad buddy upload fills the one pool, so the previous model does not survive it and `/api/buddy` says so. A pool to make it survivable is 5,120 B for a case the editor already prevents. |
| TLS | the SoftAP is WPA2 and its password is on the panel; that is the boundary. Over a joined network the server is plain HTTP on the LAN. |
| a `sdkconfig.defaults.esp32` / `.esp32c5` | nothing target-specific is known to be needed there yet. The C6 file exists because the console genuinely moves. |
| OTA | `ota_slots` is 0 in all six profiles. |

## The two managed components

Both are pinned in `components/eos_kernel/idf_component.yml` and locked by hash
in `dependencies.lock`, which is committed.

`espressif/mdns` left IDF core in v5.0, and `eos_brain.c`'s `ESP_PLATFORM`
section calls `mdns_query_a()` to find the host running the model, so it is a
hard requirement of the kernel rather than an option.

`joltwallet/littlefs` is what `eos_storage_idf.c` mounts `/int` with. It is the
only non-Espressif code in the image: 28,766 B of flash and 136 B of static RAM.
`sdkconfig.defaults` pins `LITTLEFS_CACHE_SIZE`, `LOOKAHEAD_SIZE` and
`USE_MTIME` at their current values so the published heap figures cannot drift
under a component update.
