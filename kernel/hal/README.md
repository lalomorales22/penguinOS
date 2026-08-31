# kernel/hal — the line every board has to cross

Four headers and one `.c` file. The headers define the only things the rest of
penguinOS is allowed to know about hardware:

| Header | What it fixes |
|---|---|
| `include/eos_board.h` | the runtime mirror of `boards/*.json`, plus `eos_err_t` and the tier enum |
| `include/eos_display.h` | damage-driven blit interface; the one API three renderers satisfy |
| `include/eos_input.h` | one event queue over BLE keyboard, buttons, touch, web and serial |
| `include/eos_storage.h` | one namespace over the microSD and the internal LittleFS |

Above this line nothing includes a driver header, mentions a GPIO number, or
tests for a board. Below it, a backend implements 58 functions and gets an OS.

`eos_input.c` is the exception to "no `.c` files", and it is here rather than in
a backend directory because the part of input that is hard is not hardware. The
ring, the press/release diff over a HID report, the held-key table, key repeat
and the hold expiry are the same on every board in the fleet; what differs is
only which GPIOs are buttons. It is compiled by the host suite in
`kernel/svc/test/test_ble.c` with no ESP-IDF present, which is how the report
decoder gets attacked with malformed input.

| File | What |
|---|---|
| `eos_input.c` | the ring, the HID diff, held state, repeat, button debounce |

## The tier model

The tier is a property of the board, decided once in the registry, and it picks
which display backend gets linked. It is not negotiated at runtime and it is
not a quality setting the user can raise.

| Tier | Constant | Renderer | Framebuffer | Boards |
|---|---|---|---|---|
| 0 | `EOS_TIER_SOFT` | 8-bit indexed software compositor, no LVGL | full-screen I8 if it fits, otherwise banded | CYD, wavvy 4.0in / 3.5in, wavvy OLED |
| 1 | `EOS_TIER_LEAN` | LVGL 9, partial draw buffers | LVGL's, a few dozen rows | ESP32-C5-LCD-1.47 |
| 2 | `EOS_TIER_RICH` | LVGL 9, double buffered, animations | two full buffers in PSRAM | any PSRAM board |

Tier 0 exists because of one measurement: the CYD has roughly **20KB of free
heap** at steady state with WiFi and NimBLE up. A 320x240 indexed framebuffer
is 76,800 bytes and fits; LVGL on top of it does not. So tier 0 gets a
compositor that owns exactly one buffer and never allocates again.

Tier 0 is not automatically retained. The registry decides in
`render.full_framebuffer` and `render.band_height`; `eos_board_fb_bytes()` and
`eos_board_band_bytes()` are the arithmetic behind that decision, kept in the
header so a diagnostic can print the number that justified it.

| Panel | Screen | Full framebuffer | Registry says | Band cost |
|---|---|---|---|---|
| ILI9341 (CYD) | 320x240 I8 | 76,800 | retained | — |
| ST7789 (C5 1.47) | 320x172 | LVGL's | 40-row partial | tier 1 |
| ILI9488 (wavvy 4.0/3.5) | 320x480 I8 | 153,600 | **banded, 16 rows** | 5,120 |
| SSD1306 (wavvy OLED) | 128x64 1bpp | 1,024 | retained | — |

That is why the frame loop looks the way it does. See below.

## Colour

`eos_color_t` is always a palette index. Each backend resolves it its own way,
once, at `eos_display_palette()` time — never per pixel:

| Backend | Resolution |
|---|---|
| tier 0 indexed | the index is written straight into the framebuffer; the LUT is applied when a band is pushed over SPI |
| tier 1 / 2 LVGL | 256-entry index to RGB565 LUT |
| SSD1306 mono | `eos_luma()` of the palette entry, thresholded to one bit |

`EOS_COLOR_NONE` (0xFF) is reserved and never resolves to a pixel. Filling with
it is a no-op, text in it draws nothing, and it is the "no key, no background"
value for blits. That costs the palette one entry and saves a transparency flag
on every call.

Palettes are authored in 24-bit RGB. `eos_display_palette()` takes
`uint32_t` entries with a `first`/`count` window, so the theme glue can convert
its `eos_rgb_t` table sixteen entries at a time through a 64-byte stack buffer
rather than staging a full 1KB array on a 20KB heap. Do not call
`eos_display_match()` on a draw path — cache what it returns.

### Open defect: index 255 is claimed twice

The theme lays out all 256 indices (roles, ANSI, two ramps, then a 6x8x4 RGB
cube at 64..255), while `EOS_COLOR_NONE` reserves index 255. These are the same
byte, and the consequence is not theoretical:

| Check | Result |
|---|---|
| cube range | `EOS_PAL_CUBE_BASE 64` + 6·8·4 = 192 cells → 64..255 |
| `eos_theme_cube_index({255,255,255})` | **255**, i.e. `EOS_COLOR_NONE` |
| effect | **pure white never draws** — fill, text and blit all treat it as transparent |
| blast radius | 260 of 262,144 sampled RGB values quantise onto 255 and vanish |

Verified by linking `kernel/theme/eos_theme.c` and calling it, not by reading it.

The fix belongs in the theme: stop the cube at 254. That loses the cube's
brightest cell, whose nearest survivor is one blue step away, well inside the
cube's own worst-case error. There is no free index to move the sentinel to, so
the HAL cannot fix this on its own, and a backend must not paper over it by
rendering 255. The alternative — giving transparency its own flag argument on
every blit and fill — costs more on every call than the one colour is worth.

## The frame loop

```c
eos_display_damage(tile_rect);         // declare what changed, before the frame
eos_display_frame_begin();
while (eos_display_frame_band(&band))
    draw_scene(band);                  // must be re-runnable
eos_display_frame_end();
```

Two rules make this work on every tier:

1. **Damage is declared before the frame opens, never during it.** A banded
   backend has to know the bands before the first draw call, so a draw cannot
   retroactively dirty anything. Declare, then draw.
2. **The scene callback runs once per band.** On a retained backend that is
   once. On the 320x480 ILI9488 it is once per strip. Code written for the loop
   is correct on both; code that assumes one pass is not.

Retention buys a cheaper *flush*, not a cheaper *draw* — only damaged rows go
over the bus. Drawing outside the current band is clipped away for free, so the
scene callback never has to test the band itself.

`eos_display_frame_band()` and `eos_display_frame_end()` are the only calls in
the whole HAL display interface that touch the bus.

## What a display backend must implement

Sixteen functions. Everything else in `eos_display.h` is a `static inline`
written in terms of these, so shapes and text metrics are identical on all
three tiers by construction.

| Function | Notes |
|---|---|
| `eos_display_init` | the one allocation in the HAL: the framebuffer, at boot, from `render.heap_budget`, never returned |
| `eos_display_info` | fill in `w`/`h` post-rotation, `fmt`, `caps`, `band_h`, `max_bands` |
| `eos_display_palette` | convert to native format here, once |
| `eos_display_match` | nearest loaded entry; slow is fine |
| `eos_display_damage` / `_damage_all` | coalesce past `EOS_DAMAGE_MAX`, never drop |
| `eos_display_frame_begin` | coalesce damage, reset the clip stack. No bus, no block |
| `eos_display_frame_band` | push the band just drawn, install the next as the clip floor |
| `eos_display_frame_end` | retire the last transfer, clear damage |
| `eos_display_clip_push` / `_pop` / `clip` | fixed depth `EOS_CLIP_DEPTH`; the band is the floor and cannot be popped |
| `eos_display_fill` | clipped rect fill; the workhorse |
| `eos_display_blit` | I8 and MONO1 and A8 are mandatory; RGB565 only where `EOS_CAP_RGB565` |
| `eos_display_text` | rasterise `eos_font_t` 1bpp bits; return the pen advance |
| `eos_display_backlight` | `EOS_ERR_NODEV` where the board declares no pin |

`eos_font_get()` is declared in `eos_display.h` but belongs to the font tables,
not to a display backend. LVGL backends are expected to rasterise `eos_font_t`
themselves rather than substitute an LVGL font — a screenshot from the CYD and
one from a PSRAM board then line up pixel for pixel, and the shell's layout
arithmetic stays correct everywhere.

### Blit formats

| `fmt` | Source | Backend obligation |
|---|---|---|
| `EOS_PIXFMT_I8` | one palette index per pixel | mandatory; skip pixels equal to `.key` |
| `EOS_PIXFMT_MONO1` | 1bpp MSB first | mandatory; set bits get `.tint`, clear bits get `.bg` |
| `EOS_PIXFMT_A8` | 8-bit coverage | mandatory; blend where `EOS_CAP_BLEND`, otherwise threshold at 128 |
| `EOS_PIXFMT_RGB565` | native colour | only where `EOS_CAP_RGB565`; an indexed backend returns without drawing |

`stride` is in **bytes** for every format. Zero means tightly packed;
`eos_bitmap_stride()` works it out.

## The input implementation

Seventeen functions, all of them in `eos_input.c`. The keycode space is USB HID
usage codes, unchanged, because the K809 already speaks them and nothing should
sit between the radio and the queue.

| Group | Functions |
|---|---|
| lifecycle | `eos_input_init`, `eos_input_tick` |
| queue | `eos_input_poll`, `eos_input_peek`, `eos_input_flush`, `eos_input_push`, `eos_input_dropped` |
| held state | `eos_input_held`, `eos_input_mods`, `eos_input_held_ms`, `eos_input_any_held`, `eos_input_clear_held` |
| injection | `eos_input_hid_report`, `eos_input_inject_key`, `eos_input_inject_text`, `eos_input_inject_touch`, `eos_input_inject_conn` |

`eos_input_push()` is the only HAL function callable from an ISR or a BLE
notify callback. It drops on a full ring and counts the drop rather than
overwriting unread history — losing a key-up is worse than losing a key-down.

`eos_input_hid_report()` takes the raw 8-byte report (`mods`, reserved, six
usages) and does the press/release diff centrally, so no driver repeats it. A
HID keyboard reports STATE, not events — it never says a key was released, it
just stops mentioning it — so the whole event stream is a diff, and it is done
once, here.

Reports arrive over the air from a peripheral nobody audited, so the decoder is
written for any length from 0 to 255 and any byte values:

| Report | What happens |
|---|---|
| `len == 0`, or NULL | ignored |
| `len 1..2` | modifiers only; every key slot reads as released |
| `len >= 3` | modifiers plus `len - 2` usage slots, at most six honoured |
| a slot holding `0x01`, `0x02` or `0x03` | ErrorRollOver / POSTFail / ErrorUndefined: **the whole key array is discarded** and the previously held set is left alone. Modifiers still apply. |
| the same usage in two slots | one key |
| a seventh usage | ignored |

The rollover rule is the one worth stating twice. A cheap keyboard that cannot
resolve its matrix fills all six slots with `0x01`; reading that as six keys
going down turns a mashed keyboard into six spurious binds, and reading it as
six releases drops the keys that really were down.

Fixed cost, whatever is happening:

| Structure | Size | Note |
|---|---|---|
| event ring | 512 B | `EOS_INPUT_QUEUE` (32) x 16 B |
| held table | 256 B | 16 slots: six HID usages, eight modifiers, headroom |
| button state | 96 B | `EOS_MAX_BUTTONS` |

Sixteen held slots and not more: a seventeenth simultaneous key is refused
rather than evicting one, because evicting loses that key's release and latches
it down forever.

Two spinlocks, never nested in the other order: one guards the ring, one guards
the held table and modifier state. `eos_input_push()` takes only the first,
which is what makes it safe from a NimBLE callback or a GPIO ISR.

Modifier state is not a variable that could disagree with the held table — it
IS the held table, recomputed after every change, so a stuck modifier cannot
outlive the key it came from. Modifier presses are queued BEFORE the key in the
same report and released AFTER it, so `super+return` arrives as one chord and
not as a return that has not heard about super yet.

Buttons are polled with a 20 ms debounce rather than interrupt driven. A tact
switch bounces for a few milliseconds and an ISR per edge delivers that bounce
as keystrokes; none of these boards has hardware debounce anywhere.

Touch has no backend yet. The injection path (`eos_input_inject_touch`) is
complete and events flow, but no XPT2046 or GT911 driver is bound, so a board
declaring `inputs.touch.present` gets injection only. The C6-LCD-1.3 declares
no touch, so nothing is missing on the board this runs on.

Two behaviours the backend must get right, both learned the hard way on the
arcade build:

- **Held state is what games poll.** A ship moves while left is down, not once
  per keypress. `EOS_EV_KEY_REPEAT` is for text and menus; `eos_input_held()`
  is for anything with a frame rate.
- **Injected holds expire.** The phone page and the serial console cannot be
  trusted to deliver a release. Every `EOS_SRC_WEB` / `EOS_SRC_SERIAL` press
  arms `cfg.web_hold_ms` (600ms) and decays on its own. Keyboard holds do not
  expire; a disconnect clears the whole held set instead.

- **Motion events coalesce, button events never do.** A trackpad report is
  three bytes on its own handle and a swipe is hundreds of them; the ring is
  32 events. `eos_input_inject_pointer()` therefore merges an
  `EOS_EV_POINTER_MOVE` into the motion already at the tail rather than
  queueing a second one, so a swipe cannot fill the ring and evict the click
  at the end of it. Nothing is lost by that: the cursor's real position lives
  in `eos_pointer_t` and the events carry only a copy of it. Presses,
  releases and `EOS_EV_CLICK` are each pushed exactly once.

The four pointer events (`EOS_EV_POINTER_MOVE` / `_DOWN` / `_UP` and
`EOS_EV_CLICK`) carry the cursor's **absolute** screen position in `x`/`y` and
an `EOS_BTN_*` bitmap in `key`. The HAL never sees a relative count: it does
not know how big the screen is, so acceleration and clamping happen in
`kernel/shell/eos_pointer.c` and only the answer comes back through this door.

Caps lock is deliberately unhandled: the lock state lives in the HID LED output
report, which this HAL does not send, so treating capslock as shift would be
wrong half the time.

## What a storage backend must implement

Twenty-two functions plus the two opaque handle structs (`struct eos_file`,
`struct eos_dirh`), which are fixed pools of `EOS_MAX_FILES` and
`EOS_MAX_DIRS`. The whole-file, path-join and extension helpers are inline on
top of them.

Mount points are the first path component. `/int` is the internal LittleFS
partition; `/sd` is the card. Listing `"/"` enumerates the mounts themselves as
directories, so a file browser needs no special case for the top level.

The card is removable and the flash is not. `/sd` can be absent at boot, appear
later, and vanish mid-write; `/int` is either there or the board is broken.
`eos_storage_init()` therefore does **not** fail on a missing card — it reports
a present-but-unmounted mount, and `eos_storage_mount("/sd")` can be retried
when the user inserts one.

Not in this HAL, on purpose: NVS. Boot-mode flags and pairing bonds are
key/value, not files, and they belong to the components that own them.

There is one backend: `backend/storage/eos_storage_idf.c`, LittleFS on the
`int` partition through ESP-IDF's VFS, with `/sd` declared and answering
`EOS_ERR_NODEV` because no board in the registry has verified card pins. All
22 functions are implemented; `EOS_FS_FAT` is not. `backend/storage/README.md`
carries the path rules, the pool sizes, the flash cost and the blocking table.

## Feature by tier

| Feature | tier 0 SOFT | tier 1 LEAN | tier 2 RICH | SSD1306 (tier 0) |
|---|---|---|---|---|
| `eos_display_fill` / `blit` / `text` | yes | yes | yes | yes |
| damage + band loop | yes | yes | yes | yes |
| retained framebuffer | if it fits | LVGL's | yes | yes |
| bands per full-screen frame | 1 or many | 1 | 1 | 1 |
| `EOS_PIXFMT_I8` blit | yes | yes | yes | yes |
| `EOS_PIXFMT_MONO1` / `A8` blit | yes | yes | yes | yes |
| `EOS_PIXFMT_RGB565` blit | no | yes | yes | no |
| A8 blending (`EOS_CAP_BLEND`) | no, thresholds at 128 | yes | yes | no |
| palette (`EOS_CAP_PALETTE`) | yes, 255 entries | yes, 255 entries | yes, 255 entries | yes, resolved to 1bpp |
| animation (`EOS_CAP_ANIM`) | no | no | yes | no |
| backlight | yes | yes | yes | no pin — emissive |
| backlight dimming (`EOS_CAP_DIM`) | board dependent | board dependent | board dependent | no |
| BLE keyboard | yes, NimBLE only | yes | yes | yes |
| GPIO buttons | yes | yes | yes | yes |
| touch | board dependent | board dependent | board dependent | no |
| web / serial injection | yes | yes | yes | yes |
| `/int` LittleFS | yes | yes | yes | yes |
| `/sd` microSD | board dependent | board dependent | board dependent | board dependent |

BLE must be NimBLE (19KB), never Bluedroid (83KB, which is instant OOM on tier
0). That is a build-time fact, not a runtime one, but it is the reason tier 0
exists at all.

## Allocation and blocking

The rule is: **nothing in this HAL allocates**, with exactly one documented
exception, and only four calls block.

| Call | Allocates | Blocks |
|---|---|---|
| `eos_display_init` | **yes, once** — the framebuffer, at boot, from `render.heap_budget`, never freed | yes |
| `eos_display_frame_band` | no | **yes** — this is where the bus transfer happens |
| `eos_display_frame_end` | no | **yes** — waits for the last transfer to retire |
| `eos_display_backlight` | no | briefly, on the LEDC peripheral |
| every other `eos_display_*` | no | no |
| `eos_input_*` | no | no |
| `eos_storage_*` | no — fixed handle pools; IDF's VFS allocates behind `open`/`opendir` | **yes, all of them** — and a write turns the instruction cache off while it runs |
| `eos_board_*` | no | `eos_board_probe` only |

ISR safety: `eos_input_push` only. Everything else is main-loop.

## Board identity

`eos_board_t` is one flat const struct generated from `boards/<id>.json` into a
header, living in flash. The board component returns `&EOS_BOARD` and nothing
ever writes to it. It is field for field with the registry minus the parts only
the flashing tools need — `render`, `display`, `inputs`, `peripherals.sdcard`,
the LED/audio/light peripherals, and the identity block all have a home in the
struct.

`eos_board.h` is the only declaration of that struct and of the tier, SoC,
panel, bus, touch, LED and audio enums. The generated header includes it and
emits data. When the registry grows something the runtime needs, the field is
added here; a second declaration elsewhere is what made the boot glue
impossible to write the first time.

`render` is copied across rather than recomputed. The registry made that
decision once with a heap measurement in hand; a backend that re-derives it at
boot will eventually disagree with the number that was actually tested.

**None of it is discoverable.** The runtime can report the SoC, the flash size,
whether PSRAM answered, and the MAC — that is the entire list.
`eos_board_check()` verifies those and nothing else, because nothing else is
verifiable:

- The ILI9488 returns `00 7F DF` for register `0xD3`, matching no known part.
- The CYD "N" variant looks exactly like a touchscreen and has no touch chip.
  A 60-second hold test with I2C and XPT2046 probes on every candidate pin
  confirmed that on both boards.

Board identity is the registry plus one human confirmation, and
`confirm_prompt` carries the exact question first boot should ask. A nonzero
`eos_board_check()` means the wrong board header was flashed; the correct
response is to stop and say so, not to guess a different board.

### Registry rows this HAL was designed against

| Registry id | SoC | Panel | Screen | Bus | Clock | Tier |
|---|---|---|---|---|---|---|
| `cyd-2432s024n` | ESP32-D0WD-V3, 4MB, no PSRAM | ILI9341 | 320x240 | HSPI: SCK14 MOSI13 MISO12 DC2 CS15, BL27 | 40MHz | SOFT |
| `waveshare-c5-lcd-147` | ESP32-C5, 4MB, no PSRAM | ST7789 IPS | 320x172 | SPI2: CS23 CLK7 MOSI6 DC24 RST26 BL10 | 40MHz | LEAN |
| `wavvy-ili9488-40` | ESP32-WROOM-32, 4MB, no PSRAM | ILI9488 | 320x480 portrait | VSPI: DC2 CS5 RST4 SCK18 MOSI23 | 80MHz | SOFT |
| `wavvy-ili9488-35` | ESP32-WROOM-32, 4MB, no PSRAM | ILI9488 | 320x480 portrait | same pins | **40MHz** | SOFT |
| `wavvy-oled-c5` | ESP32-C5, 4MB, no PSRAM | SSD1306 | 128x64 mono | I2C 0x3C: SDA23 SCL24 | 400kHz | SOFT |

Facts the registry has to carry, because getting them wrong costs a day each:

- The 3.5in ILI9488 is **not stable at 80MHz**. It renders structured block
  corruption, not speckle, because whole bytes are lost mid-block. 40MHz fixes
  it and costs about 3 fps. The panel is genuinely an ILI9488 — that was
  verified visually, not assumed. Do not re-diagnose it as a wrong driver.
- ILI9488 has **no 16-bit SPI pixel mode**. Every pixel costs 3 bytes. That is
  the hard ceiling on frame rate and the reason `wire_bytes` sits beside
  `color_depth` in the struct: 18 bits of colour, 3 bytes on the wire.
- Upload baud on the wavvy CP2102 cable must be **230400**. 921600 and 460800
  both fail with `Invalid head of packet`, reads included.
- The CYD's RGB LED on GPIO 4/16/17 is **active low**.
- The CYD's panel runs at 40MHz, not 80. MISO is bound on GPIO12 because the
  module wires it, and a bound MISO caps what the ESP32 SPI driver will clock.
- The CYD's microSD is on VSPI while the panel is on HSPI, so they need no bus
  lock between them. The C5 board's card shares SPI2 with the panel and does —
  hence `storage.sd_shares_bus`.
- `rotation` tracks how the panel is physically **mounted**, not its size.

## Building against these headers

Include path needs both this directory and the window manager's, because
`eos_rect_t` is the window manager's rectangle and the HAL borrows it rather
than defining a second identical one:

```sh
cc -std=c99 -Wall -Wextra -O1 \
   -Ikernel/hal/include -Ikernel/wm/include ...
```

In ESP-IDF the `hal` component `REQUIRES wm`.

All four headers are self-contained, include-order independent, safe to include
twice, and compile clean under `-std=c99 -Wall -Wextra -pedantic` and under
C++11. The inline helpers carry no file-scope statics, so including a header
and using none of it emits no warnings.
