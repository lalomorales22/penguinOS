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
| `sdkconfig.defaults` | defaults true of all six boards |
| `sdkconfig.defaults.esp32c6` | C6-only overrides; IDF appends it automatically |
| `partitions.csv` | the partition table, with the 4 MB arithmetic |
| `main/main.c` | `app_main`: the boot path, the mode choice, and the frame loop |
| `main/eos_boot_theme.c` | theme search — sd, internal fs, embedded, default |
| `main/eos_shell_draw.c` | the scene: tiles, tab strips, status bar. No IDF calls |
| `main/eos_shell_input.c` | keybind dispatch, over the HAL's event ring |
| `main/eos_setup_screen.c` | the three full-screen scenes: setup, passkey, message. No IDF calls |
| `main/test/test_setup_screen.c` | host test: renders those scenes and checks the QR module for module |
| `components/eos_kernel/` | the kernel as an IDF component |
| `components/eos_kernel/eos_board_active.c` | `eos_board_get()` and `eos_board_probe()` |

`build/`, `sdkconfig`, and `managed_components/` are gitignored by the root
`.gitignore`. `dependencies.lock` is not, and should be committed.

## The boot path

`app_main` does nine things, in an order that is load-bearing rather than
alphabetical.

| # | Step | Why it is where it is |
|---|---|---|
| 1 | `eos_board_get()`, `eos_board_probe()`, `eos_board_check()` | board identity is not probeable; the three facts that are get checked before anything is driven |
| 2 | `eos_display_init()` | the only call in the image that takes heap and keeps it. Seeds its own LUT from `eos_theme_default()`, so a board that finds no theme still draws in real colours |
| 3 | `eos_boot_theme_load()` + `eos_boot_theme_upload()` | after the display, because the upload is an update to a LUT that already holds something usable |
| 4 | `eos_wm_init()`, `eos_keys_defaults()`, five `eos_wm_open()` calls | after the theme, because `eos_wm_cfg_t` wants `min_tile_w`/`min_tile_h` from the BOARD and `gap`/`bar_h`/`tab_h` from the THEME at the same moment. The windows are opened here even when SETUP is what gets drawn: a banded backend fixes its bands when the frame opens |
| 5 | `eos_setup_screen_message()` | **before anything slow.** Step 7 blocks for up to fifteen seconds and a black panel for that long reads as a dead board |
| 6 | `eos_ble_on_passkey()` + `eos_input_init()` | brings up the NimBLE HID host. Before WiFi: the controller wants a large contiguous block and the WiFi stack fragments the heap |
| 7 | `eos_net_idf_defaults()`, `eos_net_init()`, `eos_net_start()` | the three-state boot in `docs/provisioning.md`. Returns OK in both landing states — reaching SETUP because a join failed is an outcome, not an error |
| 8 | `eos_httpd_init()`, `eos_httpd_idf_bind()`, `eos_httpd_start()` | last, because it binds to both radios and reads `eos_ble_status()` to decide whether the four BLE endpoints exist |
| 9 | frame loop | picks one of three screens per pass, and pumps input, net and httpd |

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
| payload | `WIFI:S:esp-os-f048;T:WPA;P:<12 chars>;;` — 41 bytes |
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
heap   after wifi     ...
heap   after httpd    ...
heap   boot cost <n> B of the <n> free at app_main; <n> left, largest block <n>
```

### Theme search order

`kernel/theme` guarantees that a missing or corrupt theme cannot stop the boot.
The search honours that at every step — each failure is a log line.

| Order | Source | Status on this board today |
|---|---|---|
| 1 | `<storage.sd_point>/theme.json` | skipped; the C6-LCD-1.3 declares no card slot |
| 2 | `<storage.int_point>/theme.json` — `/int/theme.json` | `fopen` fails; nothing mounts the `int` partition yet |
| 3 | the copy linked into the image (`EMBED_TXTFILES`) | **this is what runs**: `kernel/theme/themes/cyd-amber.json`, ~1.7 KB of rodata |
| 4 | `eos_theme_default()` | unreachable unless the embedded copy is corrupt |

Step 3 exists so the board comes up looking like ESP-OS rather than like the
neutral slate fallback, and it is the only thing in the image that runs
`eos_theme_parse()` on RISC-V — 213 host checks say the parser is right, and
none of them ran on target.

### What is on the screen

Five windows: four on workspace 1, one on workspace 2 so the bar's pips have
something to show. With `min_tile_w` 80 in a 117 px tile, the third split cannot
give both children the minimum and **collapses into a tab group**, which is the
one window-manager rule this board exists to demonstrate.

| Window | Face | Content |
|---|---|---|
| `clock` | 12x20 | uptime. **Not wall time** — this board has no RTC and nothing sets the clock from the network yet |
| `board` | 8x13 + 6x8 | soc, flash, panel, bus, straight off the descriptor |
| `heap` | 8x13 + 6x8 | free and largest-block, live. Behind a tab |
| `keys` | 6x8 | four binds formatted out of the real keymap by `eos_keys_format()` |

The status bar is `eos_bar_build()` output, not hand-placed text: workspace
pips, focused title, mood, heap, brain, wifi and clock, fitted to 236 px in the
bar model's own priority order. The window ids in each tile's top-right corner
are the 4x6 face, so all four shipped faces are on the glass and a bad glyph
table cannot hide.

### Redraw discipline

| Event | Damage declared |
|---|---|
| boot | `eos_display_damage_all()` |
| a keybind moved something | `eos_display_damage_all()` — a move can change every tile and the pips |
| the second ticked | the bar rect and the `clock` tile's rect, nothing else |
| a net event fired | the same two rects on the desktop; a whole frame on the setup screen |
| the screen changed | `eos_display_damage_all()` |
| nothing | none; the loop sleeps 250 ms |

An idle board therefore pushes 58,176 B a second — the 240x12 bar plus the
117x224 clock tile — instead of the 115,200 B a full frame costs. About 12 ms
of the 40 MHz bus per second.

## Provisioning

The whole of `docs/provisioning.md` is implemented in this image. The short
version: a board that has never seen your network brings up a **closed** WPA2
SoftAP called `esp-os-<last 4 of MAC>`, prints its name, its password and a QR of
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

## What is not here

Nothing in the boot path is stubbed. Two capabilities are absent because the
code that would provide them does not exist yet, and both are absent honestly —
they answer "no" rather than answering wrongly.

**A storage backend.** `kernel/hal/include/eos_storage.h` declares 22 functions
and nothing implements them, so the `int` LittleFS partition is declared in the
table and never mounted. `eos_httpd_idf_bind()` therefore leaves its three file
ports NULL, which that header already defines as "no filesystem": every static
route answers 404 and SETUP serves `eos_httpd`'s own built-in page, which needs
no filesystem to work. The web app in `web/` is not on the board. Four lines in
`app_main`, after the bind, turn static serving on the day a backend lands.

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

Measured after this run, with WiFi, NimBLE and the HTTP server all in the image.

| Measure | esp32c6 (C6-LCD-1.3) |
|---|---|
| `esp-os.bin` | 1,363,280 B (0x14CF50) |
| `factory` free | 1,782,448 B (57%) |
| bootloader | 22,176 B |
| bootloader headroom to 0x8000 | 10,592 B (32%) |
| `int` free | 983,040 B (nothing writes it yet) |
| DIRAM static | 203,650 of 452,112 B; **248,462 B remaining** |

The image grew by 695,488 B in this run and the static DIRAM by 133,118 B. Both
are the radios arriving, not the boot glue: before it, `eos_net.c` and
`eos_httpd.c` were compiled into the archive and never pulled out of it, because
nothing referenced them. The static DIRAM split, by archive:

| Archive | DIRAM | What |
|---|---|---|
| `libpp.a` | 47,134 | WiFi PHY/MAC layer |
| `libble_app.a` | 23,011 | the NimBLE controller |
| `libnet80211.a` | 19,392 | the 802.11 MAC |
| `libmain.a` | 17,820 | the boot glue: `eos_httpd_t` 4,752, the QR pixel buffer 5,330, `eos_net_t` 1,144, `eos_wm_t` ~900, the rest |
| `libfreertos.a` | 14,372 | |
| `libhw_support.a` | 12,553 | |
| `libeos_kernel.a` | 7,040 | |
| `libphy.a` | 5,629 | |

**248 KB of DRAM is what the heap starts from, not the 425,648 B the tier
decisions were made against.** Out of it the display takes 38,400 B for its DMA
strips at `eos_display_init()`, and then WiFi, NimBLE and `esp_http_server` take
their dynamic buffers. On the evidence above it fits with room, but nobody has
measured it on silicon yet — the `heap` lines in the boot log are what settle
it, and they are the first thing to read on the next flash.

If it turns out not to fit, the levers in order, none of which is a quiet buffer
shrink:

1. `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` and `..._TX_BUFFER_NUM` are at IDF's
   defaults (32 each) and are the largest single dynamic cost. Halving them
   costs throughput this board does not use.
2. `EOS_HTTPD_RESP_MAX` (4,096) and `EOS_HTTPD_SCAN_MAX` (16) are tunables in
   `eos_httpd.h` and cost only the length of the network list.
3. `hcfg.workers` is 4. A phone is one client.
4. Sequence it, as `docs/provisioning.md` already proposes for the CYD:
   provisioning becomes a reboot-into mode with the WM and the avatar down,
   rather than a service running alongside them.

The esp32/xtensa cross-build of the CYD still configures and links; its
bootloader margin was 2,592 B (9%) when last measured and is the tight number
in this project.

The esp32 bootloader margin is the tight one. Nothing needs it today, but
turning on secure boot, flash encryption, or a verbose bootloader log on a CYD
will overflow into the partition table. The fix when that happens is
`CONFIG_BOOTLOADER_LOG_LEVEL_WARN` in a `sdkconfig.defaults.esp32`, not moving
`CONFIG_PARTITION_TABLE_OFFSET`, which would shift every partition above.

## What is deliberately not configured

| Not here | Why |
|---|---|
| LVGL | the C6 registry row says `compositor: lvgl`, and the backend that draws this image is not LVGL — it is `kernel/hal/backend/esp_lcd`, a banded RGB565 compositor. Either that row or the tier-to-backend table wants reconciling; the code reads `render.band_h` and ignores `render.compositor`. |
| `esp_littlefs` | `int` is declared in the table but nothing mounts it. The component is `joltwallet/littlefs` and belongs with the code in `eos_storage.h` that will use it. Until then `web/` is not served from the board and SETUP uses `eos_httpd`'s built-in page. |
| TLS | the SoftAP is WPA2 and its password is on the panel; that is the boundary. Over a joined network the server is plain HTTP on the LAN. |
| a `sdkconfig.defaults.esp32` / `.esp32c5` | nothing target-specific is known to be needed there yet. The C6 file exists because the console genuinely moves. |
| OTA | `ota_slots` is 0 in all six profiles. |

## The one managed component

`espressif/mdns`, pinned in `components/eos_kernel/idf_component.yml`. It left
IDF core in v5.0, and `eos_brain.c`'s `ESP_PLATFORM` section calls
`mdns_query_a()` to find the host running the model, so it is a hard requirement
of the kernel rather than an option. It is fetched into `managed_components/` on
first configure and pins nothing else.
