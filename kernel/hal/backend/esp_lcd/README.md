# kernel/hal/backend/esp_lcd — ST7789 over esp_lcd

The first real `eos_display` backend. It satisfies every declaration in
`kernel/hal/include/eos_display.h` on an ST7789 SPI panel driven through
ESP-IDF's `esp_lcd`, and it is the file that turns the kernel's host checks
into pixels on the Waveshare ESP32-C6-LCD-1.3.

| File | Lines | What |
|---|---|---|
| `eos_display_st7789.c` | 735 | the backend: all 16 `eos_display_*` functions |
| `test/test_display.c` | 472 | 109 host checks over the compositor half |

## What it is

A **banded RGB565 compositor**. There is no framebuffer: a strip of
`render.band_h` rows lives in DMA-capable SRAM, the scene is composited into it
once per band, and `esp_lcd_panel_draw_bitmap()` clocks it out. The panel's own
GRAM is the frame store.

The strip is small on purpose. This board could hold a whole 240x240 frame
(115,200 of 415,000 bytes free) and the banded path is still the one that is
implemented, because it is also the path that works on the CYD's 20KB and on the
320x480 wavvy panels. There is one loop, and it is correct everywhere.

## Board coupling

Nothing in this file names a pin. Everything comes from `eos_board_get()`:

| Field read | Used for |
|---|---|
| `panel.sck/mosi/miso/dc/cs/rst`, `panel.spi_host`, `panel.hz` | SPI bus and panel io |
| `panel.invert`, `panel.bgr`, `panel.rotation`, `panel.col_offset`, `panel.row_offset` | panel init |
| `panel.bl`, `panel.bl_pwm`, `panel.bl_active_low` | LEDC backlight and the `BACKLIGHT`/`DIM` caps |
| `render.band_h`, `render.full_framebuffer`, `render.heap_budget` | strip size |
| `render.animations` | the `ANIM` cap |
| `tier` | `eos_display_info_t.tier` |

`eos_display_init()` refuses anything that is not `EOS_PANEL_ST7789` on
`EOS_BUS_SPI` at 16bpp with `wire_bytes == 2`, returning `EOS_ERR_UNSUPPORTED`
rather than driving a panel it was not written for.

## The one allocation

| What | When | Size on this board | Freed |
|---|---|---|---|
| `BUF_COUNT` strips, one contiguous `heap_caps_malloc(MALLOC_CAP_DMA \| MALLOC_CAP_8BIT)` | `eos_display_init()`, once | 2 x 19,200 = 38,400 B | never |

That is the whole of it. Every other call in this file draws into memory that
already exists. The 256-entry palette LUT (512 B), the clip stack, the damage
list and the seed `eos_theme_t` (~700 B) are all BSS.

ESP-IDF's own `spi_bus_initialize()`, `esp_lcd_new_panel_io_spi()` and
`ledc_*_config()` allocate internally at init as well. Those are IDF's, they
happen once inside `eos_display_init()`, and they are not returned either.

## Two strips, not one

`esp_lcd_panel_io_spi` drains its queued colour transactions before it sends the
next command, so the pipeline is one transfer deep no matter how many strips
exist. The second strip still pays for itself: the CPU composites band N+1 while
band N is on the wire, and the wait lands inside band N+1's `draw_bitmap()`
instead of before it. `BUF_COUNT 1` is a legal build; it just blocks more.

## Colour

`eos_color_t` is a palette index throughout. The backend holds one 256-entry LUT
of **already byte-swapped** RGB565, because the ST7789 clocks big-endian and the
draw path must not swap per pixel. Conversion happens exactly twice:

- `eos_display_palette()` — 24-bit in, `eos_rgb565()`, swap, store.
- `lut_seed_from_theme()` at init — `eos_theme_default()`'s `pal565`, swapped.

Seeding from the theme means the first frame is drawn in the real theme colours
before anything has loaded one off the card. It costs a link dependency: the
component that builds this file **must also link `kernel/theme/eos_theme.c`** and
carry `kernel/theme/include` on its include path.

Index 255 is `EOS_COLOR_NONE` and is never rendered. `eos_display_palette()`
ignores writes to it, `eos_display_match()` searches 0..254 only, and fill, blit
and text all treat it as transparent.

## Building it into a component

```cmake
idf_component_register(
  SRCS "${EOS}/kernel/hal/backend/esp_lcd/eos_display_st7789.c"
       "${EOS}/kernel/theme/eos_theme.c"
  INCLUDE_DIRS "${EOS}/kernel/hal/include" "${EOS}/kernel/wm/include"
               "${EOS}/kernel/theme/include" "${EOS}/boards/generated"
  REQUIRES esp_lcd esp_driver_spi esp_driver_ledc esp_driver_gpio)
```

Unresolved symbols this file expects someone else to provide:

| Symbol | Owner |
|---|---|
| `eos_board_get()` | the board component |
| `eos_theme_default()`, `eos_theme_palette565()` | `kernel/theme/eos_theme.c` |

It calls no font function. `eos_display_text()` rasterises `eos_font_t` bits
directly, so `kernel/font/` only has to supply tables and `eos_font_get()` for
callers, not for this backend.

## Host build

Everything above the SPI call is arithmetic, so the file compiles and runs with
`ESP_PLATFORM` unset: the strip moves to BSS, the push becomes a no-op, and
`eos_display_host_band()` hands a test the strip to read back.

```sh
cc -std=c99 -Wall -Wextra -Werror -O1 \
   -Ikernel/hal/include -Ikernel/wm/include -Ikernel/theme/include \
   -Iboards/generated -Ikernel/font \
   kernel/hal/backend/esp_lcd/eos_display_st7789.c kernel/theme/eos_theme.c \
   kernel/hal/backend/esp_lcd/test/test_display.c -lm -o /tmp/tdisp && /tmp/tdisp
```

109 checks, 0 failed. `-Ikernel/font` is required: twelve of those checks render
the real `eos_font_data.inc` tables through the rasteriser, and the suite fails
loudly rather than silently skipping if that path is missing.

A green run means the drawing is right, not that the wiring is; the wiring is
`boards/waveshare-c6-lcd-13/probe`'s job and is already verified on hardware.

## Glyph bit format

`eos_font_t.bits` is read as **MSB-first rows padded to whole bytes**, glyphs
concatenated in codepoint order — which is to say a glyph is already an
`EOS_PIXFMT_MONO1` bitmap with `stride = (w + 7) / 8`. That is exactly what
`kernel/font/tools/gen_font.py` emits.

| Quantity | Value |
|---|---|
| row pitch | `((w + 7) / 8) * 8` bits, **not** `w` |
| glyph base, fixed cell | `glyph * pitch * h` |
| glyph base, proportional | `offsets[glyph]` (a bit offset, always byte-aligned) |
| row `r` of a glyph | `base + r * pitch` |

The pitch rule is the whole of the risk here: striding by the glyph width
instead reads glyph N's rows out of glyph N+1 on every face wider than one byte,
which is `EOS_FONT_BIG` (12 px, two bytes per row). `test_display.c` pins it
with a synthetic 12-wide face and then renders all 95 codepoints of all four
real faces through the rasteriser.

## Untested paths

| Path | Why |
|---|---|
| `rotation` 1, 2, 3 | this board is mounted at 0; the swap_xy/mirror mapping is the usual ST77xx one but has not been seen on glass |
| `col_offset` / `row_offset` | both 0 on this board |
| `bgr`, `bl_active_low`, `spi_host == 2` | false / SPI2 on this board |
