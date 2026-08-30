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
| `main/main.c` | `app_main`: the boot path, and the frame loop |
| `main/eos_boot_theme.c` | theme search — sd, internal fs, embedded, default |
| `main/eos_shell_draw.c` | the scene: tiles, tab strips, status bar. No IDF calls |
| `main/eos_shell_input.c` | keybind dispatch, and the one input stub |
| `components/eos_kernel/` | the kernel as an IDF component |
| `components/eos_kernel/eos_board_active.c` | `eos_board_get()` and `eos_board_probe()` |

`build/`, `sdkconfig`, and `managed_components/` are gitignored by the root
`.gitignore`. `dependencies.lock` is not, and should be committed.

## The boot path

`app_main` does six things, in an order that is load-bearing rather than
alphabetical.

| # | Step | Why it is where it is |
|---|---|---|
| 1 | `eos_board_get()`, `eos_board_probe()`, `eos_board_check()` | board identity is not probeable; the three facts that are get checked before anything is driven |
| 2 | `eos_display_init()` | the only call in the image that takes heap and keeps it. Seeds its own LUT from `eos_theme_default()`, so a board that finds no theme still draws in real colours |
| 3 | `eos_boot_theme_load()` + `eos_boot_theme_upload()` | after the display, because the upload is an update to a LUT that already holds something usable |
| 4 | `eos_wm_init()` | after the theme, because `eos_wm_cfg_t` wants `min_tile_w`/`min_tile_h` from the BOARD and `gap`/`bar_h`/`tab_h` from the THEME at the same moment |
| 5 | five `eos_wm_open()` calls | before the first damage: a banded backend fixes its bands when the frame opens |
| 6 | frame loop | full frame once, then damage-driven |

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
| `clock` | 12x20 | uptime. **Not wall time** — no RTC, no NTP, no radios in this image |
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
| nothing | none; the loop sleeps 250 ms |

An idle board therefore pushes 58,176 B a second — the 240x12 bar plus the
117x224 clock tile — instead of the 115,200 B a full frame costs. About 12 ms
of the 40 MHz bus per second.

## What is stubbed

**Input.** `kernel/hal/include/eos_input.h` declares a queue that nothing
implements — no NimBLE HID host, no button poll, no touch — so there is no way
to press a key on this image. Everything downstream of the event is written and
reachable: `eos_shell_input_pump()` calls `eos_keys_feed()` with the real
compiled-in keymap against the real `eos_wm_t`, and `EOS_ACT_TOGGLE_BAR` really
takes the bar out of `eos_wm_cfg_t`. The single stub is
`eos_shell_input_next()` in `main/eos_shell_input.c`, which always reports an
empty queue. When the input HAL lands its whole body becomes `return
eos_input_poll(out);` and nothing else in ESP-OS changes.

Nothing else in the boot path is stubbed.

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

| Measure | esp32c6 (C6-LCD-1.3) |
|---|---|
| `esp-os.bin` | 220,400 B (0x35CF0) |
| `factory` free | 2,924,304 B (93%) |
| bootloader | 22,176 B |
| bootloader headroom to 0x8000 | 10,592 B (32%) |
| `int` free | 983,040 B (nothing writes it yet) |
| DIRAM static | 70,532 of 452,112 B; 381,580 B remaining |

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
| Wi-Fi, NimBLE | no radio is brought up in this run, and the 425,648 B free-heap figure the tier decisions rest on was measured with the radios down. Enabling them costs tens of KB and has to be paired with re-measuring. |
| `esp_littlefs` | `int` is declared in the table but nothing mounts it. The component is `joltwallet/littlefs` and belongs with the code in `eos_storage.h` that will use it. |
| a `sdkconfig.defaults.esp32` / `.esp32c5` | nothing target-specific is known to be needed there yet. The C6 file exists because the console genuinely moves. |
| OTA | `ota_slots` is 0 in all six profiles. |

## The one managed component

`espressif/mdns`, pinned in `components/eos_kernel/idf_component.yml`. It left
IDF core in v5.0, and `eos_brain.c`'s `ESP_PLATFORM` section calls
`mdns_query_a()` to find the host running the model, so it is a hard requirement
of the kernel rather than an option. It is fetched into `managed_components/` on
first configure and pins nothing else.
