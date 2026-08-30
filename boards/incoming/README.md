# incoming — boards with pinouts captured, profiles not yet written

Staging area. Each entry below has a pin table taken from the **vendor's own
"Used Pins" diagram**, which is a much better source than the third-party pages
that sent us wrong for the C6-LCD-1.3 — but it is still documentation, not a
measurement. The house standard is hardware verification, so every table here
is marked `vendor-documented` until someone runs the JTAG recovery in
`../waveshare-c6-lcd-13/README.md` against the actual board.

Promote one of these to a real profile by writing `boards/<id>.json` and a
`boards/<id>/` directory beside it.

---

## 1. Waveshare ESP32-C5-LCD-1.47 — *already profiled, now corroborated*

`../waveshare-c5-lcd-147.json` exists. The vendor table **matches it exactly**,
every pin, which independently validates a profile that was written from the
BSP component rather than from hardware.

| Function | GPIO |
|---|---|
| LCD_CLK | 7 |
| LCD_DIN (MOSI) | 6 |
| LCD_CS | 23 |
| LCD_DC | 24 |
| LCD_RST | 26 |
| LCD_BL | 10 |
| SD_MISO | 5 |
| SD_MOSI | 6 |
| SD_CLK | 7 |
| SD_CS | 4 |
| WS2812 RGB | 8 |

**New information the profile does not have:** `BOOT` is **GPIO28**, and RESET is
a dedicated pin. `inputs.buttons` is currently empty and listed as unverified —
this fills it. RAM is 384 KB. Header pins: GPIO0–5 (ADC1_CH0–CH4 on 0–4),
GPIO9, GPIO11 (UART0_TX), GPIO12 (UART0_RX), GPIO13 (USB_N), GPIO14 (USB_P),
GPIO15, GPIO27.

---

## 2. LILYGO T-Display C5 — **the first tier 2 board in the fleet**

ESP32-C5-WROOM-1U. **16 MB flash and 8 MB PSRAM**, which makes it the first
board here that meets the tier 2 definition: LVGL double-buffered with an
animation budget. Everything else we own is tier 0 or tier 1.

| Function | GPIO |
|---|---|
| LCD_CS | 26 |
| LCD_SCK | 7 |
| LCD_MOSI | 9 |
| LCD_DC | 8 |
| LCD_RST | 23 |
| LCD_BLK | 25 |
| QWIIC SDA | 2 |
| QWIIC SCL | 3 |
| BMU_INT (AXP2602) | 10 |

Panel: ST7789 1.9 inch, **170x320** IPS, RGB565.

Notes and open questions:
- It has an **AXP2602 battery management unit** and a JST-GH battery connector.
  No other board here has power management, so `eos_board_t` has no field for
  it. Battery state belongs in the status bar eventually.
- A Qwiic/STEMMA JST-SH 4-pin I2C connector on GPIO2/GPIO3.
- The diagram labels GPIO7/8/9/10 as SDIO-D1/D0/CLK/CMD, but those collide with
  the LCD pins. Read as **alternate-function labels for the pads, not a wired
  SD slot** - there is no TF slot on this board. Do not write an sdcard block.
- BOOT is ambiguous in the diagram: it shows both `BOOT(io28)` and `IO00`.
  Resolve on hardware before binding anything to it.
- 170x320 needs a **column offset**, the same trap as the C5-LCD-1.47's 34.
  Work out the correct gap or the picture sits shifted.

---

## 3. Waveshare ESP32-S3-1.47inch-Touch-LCD — **the first touch board**

ESP32-S3R8, 16 MB flash, 8 MB PSRAM. Also **tier 2**. Dual core at 240 MHz.

| Function | GPIO |
|---|---|
| LCD_CLK | 38 |
| LCD_DIN (MOSI) | 39 |
| LCD_CS | 21 |
| LCD_DC | 45 |
| LCD_RST | 40 |
| LCD_BL | 46 |
| TP_SDA | 42 |
| TP_SCL | 41 |
| TP_RST | 47 |
| TP_INT | 48 |
| SD_CLK | 16 |
| SD_CMD | 15 |
| SD_D0 | 17 |
| SD_D1 | 18 |
| SD_D2 | 13 |
| SD_D3 | 14 |

Panel: 172x320 IPS, 262K colour, **capacitive touch**.

Notes:
- **First board with a touch layer.** `eos_input.h` models touch and every
  profile so far says `touch.present: false`. This is the one that exercises
  that path. The controller model is not named in the diagram - identify it over
  I2C on GPIO41/42 before writing the profile.
- **First SD card on SDMMC rather than SPI**, and 4-bit wide (D0–D3 + CMD +
  CLK). The schema allows `bus: "sdmmc"` but nothing has used it yet. This card
  does *not* share the panel bus, unlike the C5's.
- 172x320 again, so it needs the same **column offset** treatment as the C5.
- Caution: the header diagram appears to label UART0_TX as GPIO45, which is
  also LCD_DC in the Used Pins table. On the S3 the ROM default is TX 43 /
  RX 44. Treat the UART labels as unresolved; the LCD table is the reliable one.

---

## 4. LAFVIN ESP32-C6 1.47inch LCD — **pinout still unknown**

ESP32-C6, 4 MB flash, 160 MHz single core, 172x320 262K panel. A *different*
board from the Waveshare C6-LCD-1.3 we profiled: same chip family, different
panel geometry, different vendor.

The listing shows only the broken-out header pins - **GP0–GP5, GP9, GP12, GP13,
GP18, GP19, GP20, GP23, TX, RX** - and no Used Pins table, so the LCD wiring is
not known.

Two ways to settle it, in order of preference:
1. Run the JTAG pin recovery from `../waveshare-c6-lcd-13/README.md`. It is a
   C6, so the register addresses and the signal map are identical to the ones
   that worked there. Back the flash up first.
2. Compare against the Waveshare ESP32-C6-LCD-1.47, which shares the panel
   size - but only as a hypothesis to test, never as the profile. Assuming one
   vendor's pinout matches another's is exactly the mistake that wasted an hour
   on the C6-LCD-1.3.

---

## What these four add to the design

| Capability | First introduced by | Consequence |
|---|---|---|
| PSRAM / tier 2 | T-Display C5, S3-Touch | `EOS_TIER_RICH` has no board today; the display backend's double-buffer path is untested |
| Capacitive touch | S3-Touch-LCD | `eos_input.h`'s touch path has never run |
| SDMMC 4-bit | S3-Touch-LCD | `eos_storage` has only ever seen SPI cards |
| Battery / PMU | T-Display C5 | no `eos_board_t` field exists for it yet |
| 170x320 and 172x320 | three of the four | column offsets, like the C5's 34 |
