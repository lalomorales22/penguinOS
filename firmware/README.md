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
| `main/main.c` | placeholder `app_main`: logs the descriptor, probes, checks, returns |
| `components/eos_kernel/` | the kernel as an IDF component |
| `components/eos_kernel/eos_board_active.c` | `eos_board_get()` and `eos_board_probe()` |

`build/`, `sdkconfig`, and `managed_components/` are gitignored by the root
`.gitignore`. `dependencies.lock` is not, and should be committed.

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

| Measure | esp32c6 (C6-LCD-1.3) | esp32 (CYD) |
|---|---|---|
| `esp-os.bin` | 136,832 B | 152,688 B |
| `factory` free | 3,008,896 B (96%) | 2,993,040 B (95%) |
| bootloader | 22,176 B | 26,080 B |
| bootloader headroom to 0x8000 | 10,592 B (32%) | **2,592 B (9%)** |
| `int` free | 983,040 B (nothing writes it yet) | 983,040 B |

The esp32 bootloader margin is the tight one. Nothing needs it today, but
turning on secure boot, flash encryption, or a verbose bootloader log on a CYD
will overflow into the partition table. The fix when that happens is
`CONFIG_BOOTLOADER_LOG_LEVEL_WARN` in a `sdkconfig.defaults.esp32`, not moving
`CONFIG_PARTITION_TABLE_OFFSET`, which would shift every partition above.

## What is deliberately not configured

| Not here | Why |
|---|---|
| LVGL | no agent in this run adds it; a declared-but-absent component breaks the build for everyone. The C6 is tier LEAN and will want it. |
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
