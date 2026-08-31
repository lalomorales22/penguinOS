# lafvin-c6-lcd-147 — bring-up record

| | |
|---|---|
| Chip | ESP32-C6FH4 (QFN32) rev v0.2, single RISC-V @160MHz + LP core |
| Flash | 4 MB **embedded**, mfr `0x46` device `0x4016` |
| PSRAM | none |
| Free heap | **431,112 B** measured with radios down |
| Panel | ST7789, **172x320** IPS, SPI, 40 MHz verified |
| This unit's MAC | `58:e6:c5:11:f4:78` |

## Pinout — identical to the Waveshare C6-LCD-1.3

| Function | GPIO | How |
|---|---|---|
| LCD SCLK | 7 | `FSPICLK_OUT` (sig 63) from the GPIO matrix |
| LCD MOSI | 6 | `FSPID_OUT` (sig 65) |
| LCD CS | 14 | plain GPIO, the only pin **toggling** |
| LCD DC | 15 | plain GPIO, sat high — parked in data mode |
| LCD RST | 21 | plain GPIO, sat high — released |
| Backlight | 22 | `LEDC_LS_SIG_OUT0` (sig 0) |
| RGB LED | 8 | `RMT_SIG_OUT0` (sig 71) |

`col_offset` is **34** and that is the one thing NOT shared with the square
board: 172 columns sit centred in the ST7789's 240-column window, so
`(240-172)/2 = 34`. Without it the picture looks entirely fine and sits 34 px
sideways — the same trap the C5-LCD-1.47 profile documents. `invert` is true.

`GPIO_ENABLE` differs — `0x0060c1d0` here against `0x00f3c1de` on the Waveshare
— because LAFVIN drives fewer header pins. Every LCD pin matches.

## The procedure took ninety seconds

The first board's pin recovery took about an hour, most of it working out what
to read and how to decode it. This one took a minute and a half, because
`../waveshare-c6-lcd-13/README.md` already had the commands and the signal map.
Run it on the next board too: **that two vendors' boards agreed is a fact about
this pair, not a rule.** The whole point of measuring is that you do not have to
know in advance whether the guess would have been right.

esptool cannot tell these two apart — same part, same revision, same 4 MB, same
absence of PSRAM. Only the MAC and the panel shape differ.

## Building for it

```bash
. ~/esp/esp-idf/export.sh
cd firmware && idf.py -B build-lafvin -DEOS_BOARD_ID=lafvin-c6-lcd-147 flash
```

A separate build directory keeps the other board's build intact; the board is a
build-time flag and never a source edit.

## backup/

`factory-4MB.bin` is the as-shipped image, SHA-256 beside it. Restore with:

```bash
esptool --port <port> write-flash 0x0 boards/lafvin-c6-lcd-147/backup/factory-4MB.bin
```
