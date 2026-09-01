# XIAO ESP32-S3 Sense — a camera node, NOT a board profile

This directory holds a factory-firmware backup and nothing else, deliberately.

**Do not add a `boards/xiao-esp32s3-sense.json`.** The board registry is built
around a panel: every profile carries `display.controller`, `display.pins`,
`display.byte_swap`, `display.mirror_x`, a render tier and a band height, and
`gen_board_header.py` requires all of them. This device has no screen. A profile
for it would have to lie in a dozen fields, and the whole point of that registry
is that its fields are measured facts.

It is a **camera node** instead. `firmware-cam/` is a separate ESP-IDF project
that runs the parts of penguinOS which do not need a display — `eos_net.c` for
Wi-Fi and provisioning, `eos_httpd.c` for the server — and serves frames on
`/api/cam/*`. It provisions through the same captive portal and answers on the
same mDNS pattern as every board here, so it is part of the fleet without
pretending to be a screen.

## What is measured about this unit

| | |
|---|---|
| Chip | ESP32-S3 (QFN56) rev v0.2, 8 MB octal PSRAM, 8 MB flash |
| MAC | `ac:27:6e:a7:a6:dc` |
| USB | native (no bridge chip) |
| Sensor | **OV3660**, PID `0x3660`, at SCCB address `0x3c` |

**The sensor is an OV3660, not the OV2640 the retail listing claims.** These
ship with either depending on batch. `esp32-camera` supports both and identifies
it at runtime, so nothing needs changing — but do not size frames from the
listing's spec sheet.

## If frames come back black

The camera is on a separate module behind a flex connector, and a ribbon that is
**seated but not latched** still makes SCCB contact. So `esp_camera_init()`
succeeds, the sensor reports its ID, and every frame is black — which is
indistinguishable from a software bug and has cost people hours.

Ask the sensor for its own test pattern:

```
http://<node>/api/cam/frame?colorbar=1&w=240&h=320
```

The bar is generated inside the sensor and travels out over the same eight data
lines a real frame does. **A clean bar exonerates the ribbon and D0–D7 together**
and sends you looking elsewhere. A black or scrambled bar convicts them, and no
amount of reading code will help.

## The pins

They are in `firmware-cam/main/eos_cam.c`, and the data lines are **not** in
GPIO order: 15, 17, 18, 16, 14, 12, 11, 48. That looks like a transcription
error and invites tidying. It is correct — five sources agree, including Seeed's
schematic. Swapping two does not fail init; it scrambles colour, which takes far
longer to notice.
