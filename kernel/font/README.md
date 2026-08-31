# kernel/font

The four bitmap faces. `eos_display.h` declares `eos_font_t`, `eos_font_id_t`
and `eos_font_get()`; this component holds the glyphs and implements the getter.
Without it nothing in penguinOS can draw a character.

Static const throughout. No allocation, no writable state, `.data` and `.bss`
are both zero bytes — every glyph lives in flash.

## Files

| File | Lines | What |
|---|---|---|
| `include/eos_font.h` | 116 | API, packing spec, flash budget |
| `eos_font.c` | 122 | `eos_font_get`, glyph lookup, bitmap handoff, measure |
| `eos_font_data.inc` | 4,902 | generated tables, included by `eos_font.c` |
| `tools/gen_font.py` | 224 | art → tables, and the validator |
| `tools/art/{tiny,small,med,big}.txt` | 3,623 | the glyph art, source of truth |
| `test/test_font.c` | 457 | 418 checks, then renders every glyph to stdout |

`eos_font_data.inc` is an include, not a second translation unit, so a board
component adds exactly one `.c` file to get text.

## Faces

Advances and heights are the contract published by `eos_font_id_t`. They are
not adjustable: `eos_bar.c` fits the status bar by multiplying `char_w` by
`strlen`, and the themes name a face by these numbers.

| id | cell | advance | line pitch | ink box | flash |
|---|---|---|---|---|---|
| `EOS_FONT_TINY` | 4x6 | 4 | 6 | cols 0..2, rows 0..5 | 570 B |
| `EOS_FONT_SMALL` | 6x8 | 6 | 8 | cols 0..4, rows 0..7 | 760 B |
| `EOS_FONT_MED` | 8x13 | 8 | 13 | cols 0..6, rows 0..11 | 1,235 B |
| `EOS_FONT_BIG` | 12x20 | 12 | 20 | cols 0..9, rows 0..17 | 3,800 B |
| | | | | **total** | **6,365 B** |

`gap` and `leading` are 0 on all four. The inter-glyph and interline whitespace
is drawn into the cell instead — that is what the ink box column means. A glyph
that fills its cell edge to edge compiles and breaks bar fitting on every board,
so `gen_font.py` rejects it and `test_font.c` re-checks it.

### Vertical metrics

| face | cap/digit rows | x-height rows | baseline | descender rows | stroke |
|---|---|---|---|---|---|
| tiny | 0..4 | 2..4 | bottom of 4 | 5 | 1 px |
| small | 0..6 | 2..6 | bottom of 6 | 7 | 1 px |
| med | 0..8 | 3..8 | bottom of 8 | 9..11 | 1 px |
| big | 0..13 | 5..13 | bottom of 13 | 14..17 | 2 px |

`Q` is the one capital that descends, on every face.

`small` has no blank interline row: its cell is 8 tall and its descender row is
row 7, so two stacked lines of lowercase text touch exactly where a descender
meets the next line's ascender. That is the classic 5x7-in-a-6x8-cell terminal
look and the reason `small` is the terminal face.

## Packing

Stated unambiguously because the compositor blits these directly.

| Property | Value |
|---|---|
| Rows | top to bottom, row 0 is the top of the cell |
| Bit order | MSB first — bit 7 of the first byte is the leftmost pixel |
| Set bit | ink |
| Row stride | `(cell_w + 7) / 8` bytes: 1 for tiny/small/med, 2 for big |
| Padding bits | past `cell_w` in the last byte of a row, always clear |
| Glyph size | `stride * h` contiguous bytes |
| Glyph order | codepoint order from `first`, no header, no inter-glyph padding |
| Coverage | printable ASCII 32..126, fallback `'?'` |

That is byte for byte `EOS_PIXFMT_MONO1` with `stride = row_bytes`, so
`eos_font_glyph_bitmap()` hands the display an `eos_bitmap_t` pointing straight
into flash with no copy and no repacking.

## API

`eos_display.h` already supplies the inline half — `eos_font_glyph()`,
`eos_font_glyph_w()`, `eos_text_width()`, `eos_text_fit()`, `eos_font_line_h()`.
Everything below is additional.

| Call | Returns |
|---|---|
| `eos_font_get(id)` | the descriptor, NULL for an id outside `eos_font_id_t` |
| `eos_font_row_bytes(f)` | bytes per glyph row, 0 for NULL or proportional |
| `eos_font_glyph_bytes(f)` | bytes per glyph |
| `eos_font_glyph_bits(f, c)` | top row of the glyph; folds out-of-range `c` onto `'?'` |
| `eos_font_glyph_bitmap(f, c, tint, bg, out)` | fills an `eos_bitmap_t`, false if unavailable |
| `eos_font_measure(f, s, len, &h)` | widest line in pixels, total height out |
| `eos_font_measure_cb(s, ud)` | `eos_bar_metrics_t.measure` adapter, `ud` is the font |
| `eos_font_name(id)` / `eos_font_id_from_name(s)` | the theme's `"tiny"`/`"small"`/`"med"`/`"big"` strings |
| `eos_font_face_bytes(id)` / `eos_font_flash_bytes()` | flash cost |

`eos_font_glyph_bits()` returns NULL rather than guessing when handed a NULL
font, a font with no bits, an inverted `first`/`last`, or a proportional font
(`widths`/`offsets` non-NULL). It cannot index past the table: every path goes
through `eos_font_glyph()`, which folds anything outside `[first,last]` onto the
fallback and the fallback onto `first`.

`eos_font_measure()` treats `'\n'` as a line break; `eos_text_width()` does not.
For a single-line run the two agree exactly, and on these faces both equal
`cell_w * len`.

## Editing a glyph

The art is the source of truth. The `.inc` is generated and must not be edited.

```
$EDITOR kernel/font/tools/art/small.txt
python3 kernel/font/tools/gen_font.py            # regenerate
python3 kernel/font/tools/gen_font.py --check    # is the .inc stale?
```

Art format: `.` is paper, `#` is ink. Rows may be short (padded right with `.`)
and trailing blank rows may be omitted. `#` starts a comment only before the
first `@`. The generator refuses art that leaves the ink box, a missing
codepoint, a duplicate codepoint, or a cell that disagrees with the advance
contract.

## Test

```
cc -std=c99 -Wall -Wextra -pedantic -O1 -Ikernel/font/include -Ikernel/hal/include \
   -Ikernel/wm/include kernel/font/eos_font.c kernel/font/test/test_font.c -o /tmp/tf && /tmp/tf
```

418 checks, 0 failed. Also clean under `-fsanitize=address,undefined`, and under
`-Wall -Wextra -Werror -Os` for `riscv32-esp-elf-gcc` and `xtensa-esp32-elf-gcc`.

The test prints as well as asserts. A font's only real test is a human looking
at it, so it renders every glyph of every face as ASCII art, plus composed runs
(a clock, a heap figure, a pangram) and a `0O 1lI 5S 8B 2Z 6G` strip, because
the status bar renders a clock and a heap figure in tabular columns.
