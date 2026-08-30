# waveshare-c6-lcd-13 — bring-up record

Everything specific to one physical board lives here: the profile is
`../waveshare-c6-lcd-13.json`, the factory image is in `backup/`, and `probe/`
is the ESP-IDF app that proved the pinout.

| | |
|---|---|
| Chip | ESP32-C6FH4 (QFN32) rev v0.2, single RISC-V core @160MHz + LP core |
| Flash | 4 MB **embedded** (in package), mfr `0x46` device `0x4016` |
| PSRAM | none — the C6 has no PSRAM support at all |
| SRAM | 512 KB HP; **425,648 B free** measured with radios down |
| Panel | ST7789, 240×240 IPS, SPI, 40 MHz verified |
| Radios | Wi-Fi 6, BT 5 LE, IEEE 802.15.4 |
| USB | native USB-Serial/JTAG, VID `0x303A` PID `0x1001` |
| This unit's MAC | `58:e6:c5:15:f0:48` |

## Pinout

Verified on hardware, not transcribed. See below for how.

| Function | GPIO | How it was established |
|---|---|---|
| LCD SCLK | 7 | `FSPICLK_OUT` (sig 63) read from the GPIO matrix |
| LCD MOSI | 6 | `FSPID_OUT` (sig 65) |
| LCD CS | 14 | plain GPIO; the only pin **toggling** during panel writes |
| LCD DC | 15 | plain GPIO; sat high — parked in data mode after the last pixel |
| LCD RST | 21 | plain GPIO; sat high — released |
| Backlight | 22 | `LEDC_LS_SIG_OUT0` (sig 0), so it is PWM-capable |
| RGB LED | 8 | `RMT_SIG_OUT0` (sig 71); matches the FastLED in the factory app |
| Free header | 1, 2, 3, 12, 13, 16, 17, 20, 23 | broken out to the 2.54 mm headers |

`col_offset` and `row_offset` are both **0**, unlike the C5-LCD-1.47 which needs
34. `invert` is **true**.

## How the pinout was recovered

No datasheet has it. `waveshare.com/wiki` returns 403, and every third-party
page either omits the pins or quotes them from the 1.47-inch sibling or a
generic Adafruit example. So it came off the board:

```bash
esptool --port <port> read-flash 0 0x400000 backup/factory-4MB.bin   # FIRST
. ~/esp/esp-idf/export.sh
openocd -f board/esp32c6-builtin.cfg \
  -c "init; halt" \
  -c "mdw 0x60091020 1"    `# GPIO_ENABLE - which pins are outputs` \
  -c "mdw 0x60091554 31"   `# GPIO_FUNCn_OUT_SEL_CFG - one word per GPIO` \
  -c "shutdown"
```

Decode the signal indices against
`components/soc/esp32c6/include/soc/gpio_sig_map.h`. A value of `0x80` (128)
means plain GPIO, so CS/DC/RST carry no index — those were found by sampling
`GPIO_OUT` at `0x60091004` across repeated halt/resume cycles and seeing which
bits changed.

**Back the flash up before you overwrite it.** Once the factory app is gone the
evidence is gone with it, and the pins become unrecoverable by this method.

Unsolved by it: the TF slot. A scan of `GPIO_ENABLE` only sees outputs, so the
card's MISO is invisible by construction. Reading `GPIO_FUNCn_IN_SEL_CFG` at
`0x60091154` from a restored factory image is the way to finish that.

## macOS: the board that looks dead but is not

A native-USB board can enumerate completely — name, vendor and MAC all readable
via `ioreg` — and still produce **no `/dev/cu.usbmodem` node at all**. The tell:

```
+-o USB JTAG/serial debug unit  ... !registered, !matched
"UsbEnumerationState" = 2          and a sessionID that never changes
```

That is macOS declining to configure the device, not a fault. Fix it at
**System Settings → Privacy & Security → Allow accessories to connect →
Always**. Granting it fixed the *existing* session here without a replug.
Boards behind a CH340 or CP2102 bridge never hit this; native-USB ones do.

## probe/

The app that proved the pinout: `esp_lcd` ST7789 on the pins above, eight
colour bars, a 2 px border, a sweeping line, and a pure-red block at the origin
whose colour settles `invert` unambiguously (red and cyan are exact
complements). It also prints the heap measurements that made
`heap_budget_bytes` a number rather than a guess.

```bash
. ~/esp/esp-idf/export.sh
cd boards/waveshare-c6-lcd-13/probe && idf.py set-target esp32c6 && idf.py -p <port> flash monitor
```

## backup/

`factory-4MB.bin` is the as-shipped image, SHA-256 in the `.sha256` beside it.
Restore with:

```bash
esptool --port <port> write-flash 0x0 boards/waveshare-c6-lcd-13/backup/factory-4MB.bin
```
