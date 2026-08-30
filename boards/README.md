# boards — the ESP-OS board registry

Panel controllers are not reliably probeable. The ILI9488 answers register `0xD3`
with `00 7F DF`, which matches no known part; the Waveshare C5 does not wire MISO
to the panel at all, so nothing can be read back from it; and two of the five
boards here are electrically identical and differ only in a clock rate that one
of them cannot survive. `esptool` will tell you the chip, the flash size, whether
there is PSRAM and the MAC. It will not tell you what is on the other end of the
SPI bus.

So board identity comes from this directory plus a one-time human confirmation,
never from probing. Each profile carries the confirmation question the flasher
has to ask (`identification.confirm_prompt`) and a `mac_allowlist` that a human
fills in once, after which that physical board is pinned to that profile.

Everything that cost debugging time lives here as a field with a reason attached,
because a value with no reason gets "cleaned up" by the next person.

## The five boards

| Profile | Tier | Chip | Panel | Active | What will bite you |
|---|---|---|---|---|---|
| `cyd-2432s024n` | 0 | ESP32-D0WD-V3, 4MB, no PSRAM | ILI9341 SPI, HSPI @ 40MHz | 320x240 | **No touch controller at all.** It is the N variant and looks exactly like a touchscreen. Both boards were probed and nothing answered. |
| `waveshare-c5-lcd-147` | 1 | ESP32-C5, 4MB, no PSRAM | ST7789 SPI, SPI2 @ 40MHz | 320x172 | **The vendor BSP owns panel init**, including the ST7789 RAM offsets: it calls `esp_lcd_panel_set_gap(panel, 34, 0)` and `esp_lcd_panel_invert_color(panel, true)`, and the profile mirrors both. It pins LVGL to `>=8,<10`, so the LVGL version is not a free choice on this board. |
| `wavvy-ili9488-40` | 0 | ESP32-WROOM, 4MB, no PSRAM | ILI9488 SPI, VSPI @ 80MHz | 320x480 | **Upload baud must be 230400.** 921600 and 460800 both fail with `Invalid head of packet (0xFF)` on this CP2102 cable, esptool reads included. |
| `wavvy-ili9488-35` | 0 | ESP32-WROOM, 4MB, no PSRAM | ILI9488 SPI, VSPI @ **40MHz** | 320x480 | **Not stable at 80MHz.** It renders structured block corruption, not a failure. Identical to the 4.0in board in every other respect. |
| `wavvy-oled-c5` | 0 | ESP32-C5, 4MB, no PSRAM | SSD1306 I2C @ 400kHz, addr 0x3C | 128x64 | **A tier 1 chip running the tier 0 renderer, deliberately.** A 1024-byte 1bpp page does not want LVGL. Also: SH1106 modules look identical and answer at the same address. |

Both ILI9488 boards cost three bytes per pixel — the part has no 16-bit SPI pixel
mode — which is the hard ceiling on their frame rate and cannot be clocked away.

### What esptool can tell you

| Fact | esptool | This registry |
|---|---|---|
| Chip target and variant | yes | yes, cross-checked against esptool |
| Flash size | yes | yes, cross-checked |
| PSRAM present | yes | yes, cross-checked |
| MAC | yes | `identification.mac_allowlist`, after a human confirms once |
| Panel controller | **no** | `display.controller` |
| Touch chip present | **no** | `inputs.touch` |
| LED / speaker / sensor wiring | **no** | `peripherals` |
| Which of two identical boards this is | **no** | `identification.confirm_prompt` |

## Render tiers

The tier picks a renderer. It is mostly a function of RAM, but not entirely —
`wavvy-oled-c5` is a tier 1 chip on the tier 0 renderer because a 1KB mono page
is not worth LVGL. Every profile has to justify its tier in `render.reason`.

| Tier | Condition | Renderer | Buffers |
|---|---|---|---|
| 0 | No PSRAM, tight heap | 8-bit indexed or 1bpp software compositor | One buffer, or bands when the screen does not fit |
| 1 | Comfortable RAM, no PSRAM | LVGL 9 | Partial draw buffer, single |
| 2 | PSRAM present | LVGL 9 | Double buffered, animations on |

The generator derives the buffer arithmetic so a profile cannot disagree with
itself:

| Profile | `EOS_FB_BYTES` | `EOS_BLIT_BYTES` | Total |
|---|---|---|---|
| `cyd-2432s024n` | 76800 (full 320x240 indexed) | 640 (one line, RGB565) | 77440 |
| `waveshare-c5-lcd-147` | 0 (LVGL owns it) | 25600 (320x40 RGB565) | 25600 |
| `wavvy-ili9488-40` | 5120 (320x16 indexed band) | 15360 (320x16 at 3 bytes) | 20480 |
| `wavvy-ili9488-35` | 5120 | 15360 | 20480 |
| `wavvy-oled-c5` | 1024 (whole panel, 1bpp) | 0 (already wire format) | 1024 |

The two ILI9488 boards render in 16-row bands because 320x480 indexed is 153.6KB
in one contiguous block, which an ESP32 will not hand back once WiFi is up. Band
rendering is not an optimisation there; it is the only thing that fits, and it
means the draw list is walked once per band.

## Generating the header

```bash
# one board, filling the runtime struct in kernel/hal/include/eos_board.h
python3 tools/gen_board_header.py --hal boards/cyd-2432s024n.json boards/generated/cyd-2432s024n.h

# all of them, into boards/generated/
python3 tools/gen_board_header.py --all --hal

# validate without writing anything
python3 tools/gen_board_header.py --check --hal boards/cyd-2432s024n.json
```

python3, standard library only. Exit 0 on success, 1 when a file cannot be read
or is not a profile, 2 on bad usage or a profile that does not validate. Nothing
is written when validation fails.

`boards/generated/` is build output. Regenerate it; do not edit it.

### Two emission modes

`--hal` fills the runtime `eos_board_t` declared in
`kernel/hal/include/eos_board.h`, which is what the firmware build wants: the
board component includes the generated header and returns `&eos_board` from
`eos_board_get()`. The generated header includes `eos_board.h` and defines no
types of its own.

Without `--hal` the generator emits its own equivalent types alongside the data,
so a header is self-contained and can be compiled and inspected with nothing but
a C compiler. That mode is for testing the registry, not for the firmware build.
`--no-types` sits in between: the data and the macros, no types, for when some
other header owns them.

The coupling to the HAL is confined to the enum tables at the top of the `--hal`
section in the generator (`HAL_SOC`, `HAL_PANEL`, `HAL_BUS`, `HAL_TOUCH`,
`HAL_LED`, `HAL_AUDIO`, `HAL_SPI_HOST`). `--hal` runs an extra validation pass
against them, so a controller with no `eos_panel_t`, a speaker kind with no
`eos_audio_t`, or more buttons than `EOS_MAX_BUTTONS` is a loud error rather
than a compile failure in generated code. If `eos_board.h` moves, those tables
are the only thing to fix.

One field the HAL wants is not derivable from hardware: `eos_button_t.key`, the
`EOS_KEY_*` usage a button reports. The registry does not carry a keymap, so
`--hal` emits `.key = 0` and the board component sets it. Everything else in the
struct comes from the profile.

The header carries three things:

- **Macros**, for the code that has to branch at compile time: `EOS_TIER`,
  `EOS_TIER_0`, `EOS_USE_LVGL`, `EOS_COMPOSITOR_INDEXED8`, `EOS_LCD_W`,
  `EOS_LCD_H`, `EOS_LCD_PIN_*`, `EOS_FB_BYTES`, `EOS_HAS_TOUCH`,
  `EOS_BT_STACK_NIMBLE`, `EOS_UPLOAD_BAUD`, and so on. Macros cost nothing unless
  something references them.
- **One `static const eos_board_t eos_board`**, for the code that wants to pass
  the board around. Fixed-size arrays, string literals, no allocation anywhere.
  Define `EOS_BOARD_NO_INSTANCE` to get only the `EOS_BOARD_INIT` initialiser.
  Without `--hal` the types come with it, guarded by `EOS_BOARD_TYPES_DEFINED`.
- **Comments** carrying the `gotchas` list and the `unverified` list, so the
  facts are in front of whoever opens the header, at zero flash cost.

### What the generator checks

The JSON Schema in `schema.json` documents every field and checks shapes. The
generator's own validator is separate and stricter, because the interesting
mistakes are not shape errors:

- GPIO numbers against the chip target, including the ESP32 input-only range
  34-39 bound to something that has to be driven.
- Every pin claimed twice. The only tolerated overlap is a card slot sharing the
  panel's SPI wires, and only when `sdcard.shares_display_bus` says so — and if
  the card and the panel are on the same host and the profile does *not* say so,
  that is an error too.
- `speaker.kind: dac` on a target with no DAC, or on a GPIO that is not 25 or 26.
- `bluedroid` on a board with no PSRAM. 83KB against NimBLE's 19KB; it OOMs
  during BLE init rather than degrading.
- `bytes_per_pixel` against `color_depth`, and `supports_16bit_pixels` against
  both.
- Tier against PSRAM, compositor, LVGL, double buffering and animations.
- ADC channel against the GPIO it claims to be on, and ADC2 requested on an
  esp32 (the radio owns it).
- A known-bad baud rate that is also the upload baud.
- Every dotted path in `unverified` and in `gotchas[].field` actually resolving.
- `identification.auto_detectable` being anything but `false`.
- Non-ASCII anywhere in the profile, because the strings end up in a C99 header.
- `render.heap_budget_bytes` being smaller than the buffers the same profile
  asks for.
- The card and the internal filesystem mounted at the same point.
- `id` matching the filename stem.

It reports every problem it finds at once, with the field path, and writes
nothing.

## Fields that must carry a reason

The schema makes these required and non-empty, because each of them records a
value that looks wrong until you know why:

| Field | Records |
|---|---|
| `display.clock_reason` | Why the clock is what it is. A clock below the part's maximum is always the residue of a bug. |
| `display.pixel_format_reason` | Required when `supports_16bit_pixels` is false. What a pixel actually costs. |
| `inputs.touch.absent_reason` | Required when there is no touch controller. What was probed, so nobody probes it again. |
| `inputs.bluetooth_keyboard.reason` | Which stack and why. |
| `render.reason` | Why this board lands on this tier, in numbers. |
| `flashing.bad_baud_rates[].reason` | The exact failure text, so nobody retries hoping for a different result. |
| `identification.confirm_prompt` | The question a human answers before anything is written to the board. |

`render.heap_budget_bytes` is the one field here that is a policy rather than a
measurement: it is the line the OS agrees not to cross so the radios still have
room. It is in `unverified` on all five boards and should stay there until it has
been checked against a real heap dump.

`unverified` is the other half of that: a list of dotted paths whose values are
conservative defaults rather than bench measurements. Everything *not* in that
list was proven on hardware. When a board misbehaves, check `unverified` first.

## Adding a sixth board

1. Copy the closest existing profile to `boards/<new-id>.json`. The id must be a
   lowercase slug and must match the filename stem.
2. Fill in `chip`, `display`, `inputs`, `peripherals`, `flashing` from what you
   actually measured. Unused pins are `-1`, not omitted, and absent blocks still
   need every key with a null or a `-1` in it.
3. Pick the tier and justify it in `render.reason` with the buffer arithmetic.
   Tier 2 requires PSRAM; tier 0 must not set `lvgl`.
4. Put every conservative guess in `unverified` as a dotted path. Do not guess
   silently.
5. Write `identification`: what esptool will report, what the panel's ID register
   does (if anything), what a human looks at to tell this board from its
   nearest twin, and the exact confirmation question. Leave `mac_allowlist`
   empty; it gets filled in at first flash.
6. Add at least one entry to `gotchas`. If nothing has bitten you yet, the board
   has not been used enough to have a profile.
7. Run `python3 tools/gen_board_header.py --check boards/<new-id>.json` and fix
   everything it prints. Then `--all` to regenerate the headers.
8. Run `--check --hal` too. If the panel controller, the touch chip or the audio
   kind is new, it needs an entry in the matching `eos_*_t` enum in
   `kernel/hal/include/eos_board.h` and in the generator's mapping table.
9. If the board needs a field the schema does not have, add it to `schema.json`
   *and* to the generator's validator and both emitters. Do not smuggle it into
   an existing string field.

If the new board's silicon target is not `esp32`, `esp32c5` or `esp32s3`, add it
to `TARGETS` in `tools/gen_board_header.py` with its maximum GPIO, its input-only
pins and whether it has a DAC. That table is the only place the generator knows
anything about silicon.
