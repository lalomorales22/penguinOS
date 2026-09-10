# LILYGO T-Display C5

ESP32-C5 rev v1.2, 16 MB flash, 8 MB PSRAM, 1.9in 170x320 ST7789, dual-band
Wi-Fi 6, BLE 5, an AXP2602 battery management unit and a Qwiic connector.

Second C5 in the fleet and the first board whose flash size does not match its
target's assumptions — see `firmware/sdkconfig.board.lilygo-t-display-c5`.

## The factory backup — READ THIS BEFORE REFLASHING

`backup/factory-16MB.bin` is a **complete 16 MB image of this unit as it
arrived**, taken before penguinOS was written to it. It was not a blank board:
it carried a program its owner wants back, so this dump is the only copy of it
that exists.

| | |
|---|---|
| Size | 16,777,216 bytes — the whole part, not a region |
| SHA-256 | `bf97f178738e1a9f8e0ce90b59d81c498cc1734bdc69b182bc8787fef3d92629` |
| Verified | bootloader magic `0xE9` at **0x2000**, partition magic `0xAA50` at 0x8000, app magic `0xE9` at 0x10000 |
| Carries | 1,579,283 bytes of non-blank data; the rest is erased flash |

The `.bin` is gitignored — 16 MB does not belong in git — but the `.sha256`
next to it **is** committed, so a restored copy can always be proved identical
to what came off the board. Check it with:

```bash
shasum -a 256 -c boards/lilygo-t-display-c5/backup/factory-16MB.bin.sha256
```

**The bootloader is at 0x2000, not 0x1000.** That is the C5 offset, and it is
why byte 0 of this dump reads `0xFF` and looks briefly like a failed capture.

### What was on it

The original partition table, recovered from the dump:

| Label | Type | Offset | Size |
|---|---|---|---|
| `nvs` | data/nvs | 0x009000 | 20 KB |
| `otadata` | data | 0x00E000 | 8 KB |
| `app0` | app/ota_0 | 0x010000 | 3072 KB |
| `app1` | app/ota_1 | 0x310000 | 3072 KB |
| `ffat` | data/fat | 0x610000 | 10112 KB |
| `coredump` | data | 0xFF0000 | 64 KB |

An `app0`/`app1` OTA pair with a 10 MB FAT volume — the Arduino-style layout,
not penguinOS's. **Anything the original program stored lives in `ffat`**, and
it is inside this dump like everything else.

### Restoring it

```bash
esptool.py --port /dev/cu.usbmodemXXXX -b 921600 \
    write_flash 0x0 boards/lilygo-t-display-c5/backup/factory-16MB.bin
```

Offset `0x0` and the whole file, deliberately: this is a full-part image, so it
restores its own bootloader, its own partition table and its own data together.
Do not try to restore pieces of it at penguinOS's offsets — the two layouts do
not agree about where anything lives.

Writing penguinOS over it again afterwards is safe and repeatable. The dump is
the thing that is not repeatable, which is why it was taken first.

## Pinout

From the vendor pinmap. **Nothing here has been confirmed against the glass
yet** beyond the panel lighting up — see `unverified` in the profile.

| | |
|---|---|
| LCD | ST7789, 1.9in, 170x320 |
| LCD_SCK | GPIO 7 |
| LCD_MOSI | GPIO 9 |
| LCD_CS | GPIO 26 |
| LCD_DC | GPIO 8 |
| LCD_RST | GPIO 23 |
| LCD_BL | GPIO 25 |
| Qwiic SDA / SCL | GPIO 2 / GPIO 3 |
| BMU (AXP2602) INT | GPIO 10 |
| BOOT / IO0 | GPIO 28 / GPIO 0 |

No microSD and no touch layer. There **is** a battery management unit, which
no other board in the fleet has and which penguinOS does not talk to at all
yet — the board runs from USB regardless.
