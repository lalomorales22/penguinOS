# kernel/qr

A QR encoder scoped to one job: putting the SoftAP join string on the panel
during SETUP, so a phone camera joins by pointing instead of by typing.

`docs/provisioning.md` makes the screen the out-of-band channel. The board
raises a WPA2 SoftAP with a password nobody has ever seen, prints the password
on the LCD, and prints this QR next to it:

```
WIFI:S:esp-os-f048;T:WPA;P:k9mQ2xR7vT4b;;
```

41 bytes, which is a version 3 symbol at 29x29 modules.

## Scope

Deliberately narrow, and the header says so out loud.

| Axis | Supported | Not supported |
|---|---|---|
| Mode | byte (0100) | numeric, alphanumeric, kanji, ECI, structured append |
| ECC level | L | M, Q, H |
| Version | 1, 2, 3, 4 | 5 and up, and every Micro QR |
| Size | 21x21 .. 33x33 | — |
| Payload | 1 .. 78 bytes | 0 bytes, 79 and up: refused |

Over-length input returns `EOS_QR_ERR_TOO_LONG`. It is never truncated. A
truncated QR still scans and hands the phone the wrong password, which is a
worse failure than an error return.

Everything the standard requires inside that box is implemented: byte-mode
encoding, the capacity table, Reed-Solomon over GF(256), block interleaving,
finders, separators, timing, alignment, the dark module, the BCH(15,5) format
information, all eight mask patterns, and the four penalty rules that choose
between them.

## Files

| File | Lines | What |
|---|---|---|
| `include/eos_qr.h` | 160 | API, scope statement, buffer macros |
| `eos_qr.c` | 658 | the encoder |
| `test/test_qr.c` | 826 | 23,080 checks, then prints a scannable QR |

## Cost

| | |
|---|---|
| Flash, riscv32 `-Os` | 4,060 B |
| Flash, xtensa `-Os` | 3,479 B |
| `.data` / `.bss` | 0 B / 0 B |
| `sizeof(eos_qr_t)` | 472 B, caller-owned |
| Heap | none, ever |
| Encode time | ~160 us on the host; a few hundred us on the C6, once, at SETUP |

No allocation and no mutable global state. GF(256) multiplication is
shift-and-xor rather than exp/log tables: the tables would be 768 bytes of
`.bss` initialised on a first-call branch, and a version-4 symbol only needs
1,600 field multiplies, once, while the SoftAP is still coming up.

## Buffer layout

The module buffer lives inside `eos_qr_t` and uses a **fixed five-byte row
stride at every version**, not the `(size+7)/8` the version would justify. A
version-1 symbol wastes two bytes on each of its 21 rows.

That is on purpose. Five is the version-4 stride, so one compile-time size
serves every version, and the packing is byte-for-byte `EOS_PIXFMT_MONO1` with
`stride = EOS_QR_STRIDE`. The panel blits it with no repacking and no second
buffer, on a board with no PSRAM to spare for one.

| Symbol | Value |
|---|---|
| `EOS_QR_STRIDE` | 5 |
| `EOS_QR_BUF_BYTES` | 165 |
| Bit order | MSB first; bit 7 of the first byte of a row is x = 0 |
| Set bit | dark module |
| Out of range | `eos_qr_module()` reads light, so a renderer walks a padded box without clamping |

## Using it

```c
static eos_qr_t qr;                       /* 472 B, no allocation */
static uint8_t  px[EOS_QR_SCALED_BYTES(EOS_QR_MAX_SIZE, 5, EOS_QR_QUIET)];

if (eos_qr_encode(&qr, join_string) != EOS_QR_OK) { /* show text instead */ }

int w, h;
eos_qr_render(&qr, 5, EOS_QR_QUIET, px, sizeof px, &w, &h);

eos_bitmap_t bm = {
    px, (int16_t)w, (int16_t)h,
    EOS_QR_SCALED_STRIDE(qr.size, 5, EOS_QR_QUIET),
    EOS_PIXFMT_MONO1, EOS_COLOR_NONE, ink, paper
};
eos_display_blit(x, y, &bm);
```

`eos_qr.h` does not include `eos_display.h`. The encoder has no business
knowing about panels; the `eos_bitmap_t` above is the whole bridge.

Scale 5 with a 4-module quiet zone puts a version-3 symbol at 185x185 pixels
and 4,440 bytes, which leaves a 240x240 panel room for the AP name and password
underneath. Version 4 at scale 5 is 205x205 and 5,330 bytes. Scale 1 is for
tests and does not scan.

**The quiet zone is not optional.** `EOS_QR_QUIET` is 4 modules and the render
draws it, because a QR flush against a coloured background does not scan.

## Registering the component

`kernel/qr/` is not yet listed in `firmware/components/eos_kernel/CMakeLists.txt`;
four agents were editing that file at the same time this component was written,
so it was left alone. Two lines are needed:

```cmake
    SRCS
        ...
        "${EOS_KERNEL_DIR}/qr/eos_qr.c"
    INCLUDE_DIRS
        ...
        "${EOS_KERNEL_DIR}/qr/include"
```

There is nothing else. No new `REQUIRES`: the encoder is portable C99 with no
IDF dependency at all.

## Mask selection

The standard has four penalty rules and picks the mask with the lowest total.
Rule 3 — the 1:1:3:1:1 false-finder rule — is worded ambiguously enough that
three widely used encoders read it three different ways and disagree with each
other about the winning mask roughly half the time. This is what eos_qr does,
stated so it can be argued with:

| Rule | Weight | Reading used here |
|---|---|---|
| N1 | 3 | run of 5 or more same-colour modules in a row or column, `3 + (run - 5)` |
| N2 | 3 | per 2x2 block of one colour, counted at every offset |
| N3 | 40 | per occurrence of the seven-module `1011101`, **once**, when preceded **or** followed by four light modules. Off-symbol reads light, so a pattern flush against an edge qualifies. Overlapping occurrences each count. |
| N4 | 10 | `floor(abs(percent_dark - 50) / 5)` over the whole symbol |

Format information is placed before scoring, because it is part of the printed
symbol. Ties go to the lowest mask number. All eight scores are kept in
`qr.penalty[]` so a caller can see the comparison and not just the winner.

Mask choice is a print-quality heuristic. Every one of the eight masks produces
a symbol that decodes; a poor choice costs contrast margin on a bad print, never
correctness.

## Test

```
cc -std=c99 -Wall -Wextra -pedantic -O1 -Ikernel/qr/include \
   kernel/qr/eos_qr.c kernel/qr/test/test_qr.c -o /tmp/tq && /tmp/tq
```

23,080 checks, 0 failed. Also clean under `-fsanitize=address,undefined`, and
under `-Wall -Wextra -Werror -Os` for `riscv32-esp-elf-gcc` and
`xtensa-esp32-elf-gcc`.

### What the checks cover

| Group | What |
|---|---|
| capacity table | every version's byte capacity and size, and the boundary at 17/18, 32/33, 53/54, 78/79 |
| refusals | NULL, empty, 79 bytes, 200 bytes, version 0/5/-1, and a payload forced into a version too small for it. A refusal must leave the previous symbol untouched |
| reference matrices | all 2,996 modules of four symbols, one per version |
| codewords | the version-1 stream derived by hand in the comment, the pad pattern, and the codeword counts for all four versions |
| format information | both copies of the 15 bits, read back out of the symbol, against ISO/IEC 18004 Table C.1 |
| function patterns | finders, separators, both timing lines, the alignment centre per version, the dark module, and out-of-range reads |
| masks | the eight scores asserted literally, all eight non-zero, at least six distinct, argmin wins, no earlier mask ties it |
| determinism | the same bytes twice give byte-identical symbols, and a short payload after a long one leaves no residue |
| every length | all 78 payload lengths: correct version, finders present, argmin mask |
| render | buffer sizing, refusal on a short buffer, scale 1 reproduces the module map, scale 3 blocks are exact, the quiet zone is entirely light |

### Where the reference matrices came from

They are not this encoder's own output written down. Each was produced by an
independent implementation (the `python-qrcode` package) with the version and
mask forced to what eos_qr chose, and the two agreed module for module before
being pasted in.

Behind those four, a wider sweep was run during development and is worth
recording because it is the real evidence:

| Sweep | Result |
|---|---|
| 916 payloads vs `python-qrcode` — every version, every boundary length, arbitrary binary bytes, forced and automatic version | 0 structural differences |
| 600 payloads compared at **all eight** masks | 0 structural differences |
| 1,200 payloads vs `segno` | 0 structural differences |
| 142 symbols rendered to a bitmap and decoded by **OpenCV's** detector | 142 decoded, payload exact |

`segno` needed one correction before it could serve as a reference: its
`write_padding_bits()` appends 8 zero bits even when the stream is already on a
codeword boundary, which inserts a spurious `0x00` data codeword. In byte mode
with an 8-bit count indicator the stream is *always* aligned after the 4-bit
terminator, so this fires on every byte-mode symbol it produces. The symbols
still decode — the extra codeword lands in the padding region, which decoders
ignore — but they are not the canonical stream. `python-qrcode`, nayuki's
`qrcodegen` and this encoder all agree against it.

### The check that matters

The last thing the test does is print the real join string as a QR, large, with
a 4-module quiet zone, using ANSI background colours rather than glyphs — a
terminal with a dark theme would otherwise render the symbol inverted, and many
scanners refuse an inverted symbol. Point a phone at it. It should offer to join
`esp-os-f048`.

A QR that is subtly wrong scans on the phone of whoever wrote it and on nobody
else's, which is why that check is in the file and not in someone's head.
