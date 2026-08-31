# tools — the penguinOS universal flasher

Plug in any board in `boards/`, run one script, get penguinOS.

```bash
tools/flash.sh
```

That is the whole interface. Everything below is what it does and what to do
when it does not.

| File | What |
|---|---|
| `flash.sh` | The orchestrator. Identify, resolve, confirm, build, write, remember. |
| `detect.py` | Steps 1–2 and the MAC-keyed profile cache. python3, stdlib only. |
| `probe/probe.ino` | The panel prober. Draws a labelled test card under each candidate. |
| `gen_board_header.py` | Not part of the flasher; turns a profile into the C header. |

## Why this is not just `esptool write_flash`

`esptool` reports the chip, the flash size, embedded PSRAM and the MAC. It does
not report what is on the other end of the SPI bus, and `boards/README.md`
records why nothing here pretends otherwise: the ILI9488 answers register `0xD3`
with `00 7F DF`, matching no known part, and the Waveshare C5 does not wire MISO
to the panel at all.

So the registry contains **two pairs of boards that esptool cannot separate**:

| Pair | Identical in | Differs only in |
|---|---|---|
| `wavvy-ili9488-40` / `wavvy-ili9488-35` | chip, flash, controller, wiring, CP2102 bridge reporting USB serial `0001` | the SPI clock the panel survives — 80MHz vs 40MHz |
| `waveshare-c5-lcd-147` / `wavvy-oled-c5` | ESP32-C5, 4MB, native USB | what is soldered on — a 320x172 SPI LCD vs a 128x64 I2C OLED |

Getting the first pair wrong does not fail. It renders structured block
corruption that looks like a dead panel. That is the reason this is a flasher
with an identification phase rather than a one-liner, and the reason `--yes`
means "write it" and never "guess which board".

## The flow

1. **Enumerate.** Every `/dev/cu.*` that is not Bluetooth or an internal debug
   console (`/dev/ttyUSB*` and `/dev/ttyACM*` on Linux).
2. **Identify.** One `esptool flash-id` per port — a single connection gives the
   chip description, the features, the crystal, the MAC and the flash size.
   In parallel, `ioreg` (macOS) or sysfs (Linux) gives the USB bridge, its
   VID:PID, its serial number and its `locationID`.
3. **Narrow.** Every profile in `boards/` is scored against those facts.
   *Hard* signals reject a profile: chip target, flash size, and PSRAM in the one
   direction esptool can prove. *Soft* signals rank and explain: the chip
   description recorded in `identification.esptool_reports.chip`, the
   `flashing.usb_bridge`, and `identification.usb_serial`. The outcome is one of
   four:

   | Decision | Meaning | What happens |
   |---|---|---|
   | `pinned` | this MAC is in a profile's `mac_allowlist`, or in the local cache | proceeds, asks nothing |
   | `unique` | exactly one profile survives | asks the profile's `confirm_prompt` once |
   | `ambiguous` | more than one survives | you pick, or you probe |
   | `none` | nothing matches | stops; add a profile |

4. **Resolve.** For `ambiguous`, either pick from the menu, pass `--profile`, or
   pass `--probe` to flash `probe/probe.ino` and read the screen.
5. **Remember.** The answer is written to the MAC cache *before* the image is,
   so a failed flash never costs you the identification you just did.
6. **Build.** `gen_board_header.py` emits `boards/generated/<id>.h`, then
   `idf.py build` into `build/<id>/`.
7. **Write.** Offsets, files and `write_flash` arguments come from the build's
   own `flasher_args.json`, so the flasher never duplicates a decision the build
   already made. The upload baud comes from the profile.
8. **Stamp.** The chosen profile goes into the board's NVS, so a later run can
   read back what is on it. Optional; skipped cleanly when `nvs_partition_gen.py`
   is not reachable.

## Prerequisites

| Tool | Needed for | Install |
|---|---|---|
| python3 ≥ 3.7 | everything | preinstalled on macOS, or `brew install python3` |
| esptool ≥ 4 | identification and writing | `pip3 install esptool` or `brew install esptool` |
| ESP-IDF | building the real image | `. $IDF_PATH/export.sh` before running |
| arduino-cli + esp32 core ≥ 3.3 | `--probe` only | `brew install arduino-cli && arduino-cli core install esp32:esp32` |
| GFX Library for Arduino | `--probe` only | `arduino-cli lib install "GFX Library for Arduino"` |

`--list`, `--identify` and `--probe` do not need ESP-IDF. `tools/flash.sh --list`
prints which of these it can find.

esptool 5 renamed every subcommand to hyphens (`flash-id`, `write-flash`);
esptool 4 only knows the underscores. Both are handled — the version is read
once and the spelling picked from it.

## Commands

```bash
tools/flash.sh                      # identify, confirm, build, flash
tools/flash.sh --list               # attached boards, the registry, the toolchain
tools/flash.sh --identify           # identify only, write nothing
tools/flash.sh --probe              # decide the panel by eye, then flash
tools/flash.sh --profile cyd-2432s024n --yes
tools/flash.sh --dry-run            # print every write instead of doing it
tools/flash.sh --provision-sd       # serve microSD contents over HTTP
```

| Flag | Effect |
|---|---|
| `--port PORT` | which serial device; required when more than one board is attached |
| `--profile ID` | skip identification entirely |
| `--list` | boards, registry and toolchain, then stop |
| `--identify` | identify and stop |
| `--probe` | build and upload the panel prober, then ask which pass rendered |
| `--provision-sd [DIR]` | serve `DIR` (default `<repo>/sdcard`) on port 8765 |
| `--dry-run` | every writing command is printed, not run |
| `--yes` | do not ask before writing. Does **not** authorise guessing a board |
| `--erase` | erase the whole flash first. Never implied |
| `--monitor` | open a serial monitor afterwards |
| `--baud N` | override the profile's upload baud; warns if the profile says it fails |
| `--build-dir DIR` | default `<repo>/build/<profile>` |
| `--project DIR` | the ESP-IDF project; autodetected from `CMakeLists.txt` |
| `--no-build` | flash whatever is already in the build dir |
| `--no-nvs` | skip the on-board profile stamp |
| `--sd-port N` | port for `--provision-sd`, default 8765 |

### detect.py on its own

```bash
python3 tools/detect.py                    # readable report
python3 tools/detect.py --json             # machine-readable
python3 tools/detect.py --list-profiles
python3 tools/detect.py --cache-list
python3 tools/detect.py --forget 24:6f:28:aa:bb:cc
```

`--json` is the contract. `--shell` is the same object flattened into `EOS_*`
assignments, which is what `flash.sh` actually evaluates, because bash 3.2 has
neither a JSON parser nor associative arrays. `flash.sh` captures the JSON once
and pipes it back through `--shell --from-json -`, so the board is reset once per
run rather than twice — every esptool connection yanks DTR/RTS and reboots the
board.

## Two things that will surprise you

**Detection always connects at 115200.** Never at the profile's upload baud. The
wavvy CP2102 cable fails with `Invalid head of packet (0xFF)` at 460800 and
921600 *for plain reads*, not just writes, so a detector that used the upload
baud would fail to identify the one board whose upload baud matters most.

**The MAC cache lives outside the repo**, at `~/.penguinos/board-cache.json`
(override with `$EOS_BOARD_CACHE` or `--cache`). It is per-machine state about
physical boards, not source. To pin a board for everyone who clones the repo,
add its MAC to `identification.mac_allowlist` in its profile — the registry
outranks the cache, deliberately, because a checked-in file is a stronger
statement than something a tool wrote.

`detect.py` never writes the cache as a side effect of detecting. `flash.sh`
calls `--remember` once a human has answered, so a confirmation is always
something a person did.

## The prober

`probe/probe.ino` walks every panel configuration the chip could be wired to and
draws a labelled test card under each. Everything is constructed at runtime, so
one binary compares configurations without reflashing between them.

| Chip | Passes |
|---|---|
| `esp32` | ILI9341 @ HSPI 40MHz · ILI9488 @ VSPI 80MHz · ILI9488 @ VSPI 40MHz |
| `esp32c5` | ST7789 @ FSPI 40MHz · SSD1306 @ I2C 400kHz |

A card is right when it shows a **readable label**, **four clean colour bars**, a
**smooth grey ramp** and an **unbroken stripe field**. Anything else is a wrong
pass.

The stripe field is the part that matters for the wavvy pair. One-pixel vertical
stripes are the densest pattern the bus can carry, so a dropped byte shifts every
following pixel in that block and the eye sees a rectangle of inverted phase.
Solid fills hide that; gradients hide it; stripes do not. The two ILI9488 passes
are the same driver at two clocks, and the one whose stripe field holds is the
board you have.

Press a digit on the serial monitor to jump to that pass and record the choice;
it prints `[probe] PROBE-SELECT <profile-id>`. Any other key skips ahead. The
prober writes nothing to the board and loops forever.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `Invalid head of packet (0xFF)` | upload baud too high for the cable. On the wavvy CP2102 both 921600 and 460800 fail, esptool reads included. It is the cable, not the board. | Use 230400. The profile already says so; `--baud` overrides it and will warn you. |
| Board is not in `--list` at all | charge-only USB cable, or no bridge driver | Try another cable first — that is the usual answer. CH340 boards need the CH34x driver on older macOS; CP2102 and native USB do not. |
| `Failed to connect ... No serial data received` | board not in download mode | Hold BOOT, tap EN, release BOOT. Boards with `auto_reset: true` should not need this; if one does, its auto-reset circuit or the cable's DTR/RTS lines are the problem. |
| `A fatal error occurred: Could not open port ... Device or resource busy` | a serial monitor is already attached | Close it. Opening a monitor also resets the board, so an unexplained reboot mid-flash is usually a second terminal. |
| Two boards, one name | both wavvy CP2102 bridges report USB serial `0001`, so `/dev/cu.usbserial-0001` collides | Flash one at a time, or tell them apart by USB `locationID`, which is unique per physical port and is printed by `--list`. |
| Panel stays black, everything else fine | backlight not driven | The CYD's backlight is GPIO27 and the Waveshare C5's is GPIO10; both must be driven high. The two wavvy boards have no software backlight control at all (`backlight.pin: -1`), so a black panel there is not a backlight problem. |
| Panel renders blocks, lines and smearing | 80MHz on the 3.5in board | It is `wavvy-ili9488-35`, not `wavvy-ili9488-40`. Structured corruption means whole bytes lost mid-block, not bits flipped at random. Do not re-diagnose it as a wrong driver — the panel is an ILI9488 on both boards, verified with a test card. |
| Colours wrong but the label is readable | colour order or inversion | The pass still identifies the panel. `display.color_order` and `display.invert` in the profile are what the real image uses; the prober does not always match them. |
| `idf.py: command not found` | ESP-IDF not sourced | `. $IDF_PATH/export.sh`. `--list`, `--identify` and `--probe` work without it. |
| `no ESP-IDF project found` | nothing to build yet | Point at it with `--project DIR`, or use `--no-build` to flash a prebuilt image already in the build dir. |
| `the build directory is incomplete` | `flasher_args.json` names a file that is not there | Rebuild. The flasher checks every file exists before it offers to write anything. |
| Prober will not compile | missing library | `arduino-cli lib install "GFX Library for Arduino"`. The ESP32-C5 target needs esp32 core 3.3 or newer. |

## Safety

Nothing is written to a board without either `--yes` or a typed confirmation
that names the port, the profile and every image file with its offset.

`--yes` authorises writing. It does not authorise resolving an ambiguity: when
two profiles both match, `--yes` alone still stops, because flashing the 4.0in
profile onto the 3.5in board is silent and looks like a hardware fault.

The confirmation prompt reads from the terminal, not from stdin. Piping `y` into
the script does not answer it — that is deliberate. Use `--yes` when you mean it.

`--erase` is never implied by anything.

## Provisioning the microSD

```bash
tools/flash.sh --provision-sd            # serves <repo>/sdcard
tools/flash.sh --provision-sd path/to/sd --sd-port 8765
```

Serves the directory over HTTP and prints the URL with this machine's LAN
address filled in. On the board, point its provisioning mode at that URL — on
the cyd24 firmware that is `/sdload http://<ip>:8765/`, which reboots into a
WiFi-only mode, pulls the tree, and returns. Add files and run it again any time.
This touches no board and needs no serial port.
