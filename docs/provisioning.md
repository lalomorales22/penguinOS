# Provisioning — first boot on an unknown network

The problem in one line: **the web app is how you set the WiFi, and the web app
is served over WiFi.** A board carried somewhere new is unreachable — it cannot
join a network it has never heard of, and you cannot tell it about one.

The same applies to input. These boards have no keyboard and, on most of them,
no touch layer. So a brand-new board in a strange room has no network and no
way to type. Both have to be solvable with nothing but the board, its screen,
and a phone.

The thing that makes this tractable, and that most IoT provisioning does not
have, is that **every board in this fleet has a display**. A screen is an
out-of-band channel. Use it.

## Network: three-state boot

```
boot
 ├── NVS holds credentials?
 │     ├── join succeeds (15 s budget) ──> RUN. mDNS at <name>.local
 │     └── join fails ─────────────────┐
 └── no credentials ───────────────────┴──> SETUP
                                              SoftAP  esp-os-<last4 of MAC>
                                              captive portal on 192.168.4.1
                                              the same web app, setup mode
```

In SETUP the panel is the instruction sheet. It shows the AP name, the AP
password, the URL, and a **QR code encoding `WIFI:S:esp-os-f048;T:WPA;P:...;;`**
so a phone camera joins the AP by pointing at the screen. On a 240x240 panel a
QR is comfortable; on the 128x64 OLED fall back to text.

Captive portal: run a DNS responder that answers every query with
192.168.4.1, so joining the AP pops the setup page by itself on iOS and
Android. Without it the user has to know to type an IP, which they will not.

### The AP must not be open

An open AP serving a page that accepts WiFi passwords is a real exposure —
anyone in range can join, read the config, and set their own. Generate a random
WPA2 password at first boot, store it in NVS, and **print it on the LCD**. The
screen is the out-of-band channel; that is the whole point. It costs the user
one glance and removes the exposure entirely.

### Endpoints

| Method | Path | Notes |
|---|---|---|
| GET | `/api/wifi/scan` | SSID, RSSI, auth mode, channel. Sorted by signal. |
| POST | `/api/wifi/connect` | `{ssid, psk}`. Try it, report the outcome. |
| POST | `/api/wifi/forget` | Drop stored credentials, return to SETUP. |
| GET | `/api/net/status` | Mode, IP, RSSI, mDNS name. |

**Persist credentials only after a join succeeds.** If you save first and try
second, one typo leaves the board booting into a network it cannot reach with
credentials it will keep retrying, and the only way out is a serial cable. Try,
then save. This is the single most important rule on this page.

Scanning while the AP is up puts the radio in APSTA and briefly disrupts AP
clients — the phone may drop for a second mid-scan. Scan once when SETUP
starts, cache the result, and make rescan an explicit button that says it will
blink the connection.

## Bluetooth keyboard pairing

The board is a BLE HID **host**. Same shape as the network flow:

| Method | Path | Notes |
|---|---|---|
| GET | `/api/ble/scan` | Peripherals advertising HID (service `0x1812`, appearance `0x03C1`). Name, address, RSSI. |
| POST | `/api/ble/pair` | `{addr}`. Connect and bond. |
| GET | `/api/ble/status` | Bonded device, connected yes/no, battery if exposed. |
| POST | `/api/ble/forget` | Drop the bond. |

Most BLE keyboards bond by **passkey entry**: the host picks a six-digit
number, the human types it *on the keyboard*, and the keyboard has no screen to
show it. So the number has to come from somewhere — show it on the LCD **and**
in the web page. The existing `claude_term024` already puts the passkey on the
panel, so this is proven on hardware, just not exposed over HTTP yet.

Once bonded, the bond lives in NVS and the keyboard reconnects by itself after
sleep. That is already working in `../ESP-OS-CY24`.

### Two gotchas from the existing build

- **A keyboard bonds to one board at a time.** The K809 "bonds to whichever
  board it paired with last" — pairing it to a second board silently breaks the
  first. The UI must say so at the moment of pairing, not in a footnote.
- **NimBLE only.** Bluedroid is 83 KB against NimBLE's 19 KB and is an instant
  OOM on the CYD. Release classic-BT memory and init BLE *before* WiFi.

## Radio coexistence

WiFi and BLE share one radio. Never run a BLE scan and a WiFi scan at the same
time — serialise them behind a single lock and make the UI wait. On the C6 this
also shares with 802.15.4, though nothing here uses Thread or Zigbee yet.

## Memory, which decides how much of this each board gets

| Tier | Board | Free heap | Provisioning |
|---|---|---|---|
| 1 | C6-LCD-1.3 | 464 KB | everything: AP + portal + both scans + QR |
| 1 | C5-LCD-1.47 | ~130 KB | everything, with the scan cache mandatory |
| 0 | CYD 2432S024N | **~20 KB** | AP + WiFi form only. See below. |
| 0 | wavvy OLED | tight, 1bpp | text-only setup screen, no QR |

The CYD is the constraint, as always. SoftAP + captive DNS + HTTP server +
NimBLE scanning + a framebuffer does not fit in 20 KB. Options there, in order:

1. **Sequence it.** Provisioning is a *mode*, not a background service. Reboot
   into a dedicated setup mode with the WM and the avatar shut down, exactly
   like the existing `/sdload` mode already does on that board. Almost all the
   heap comes back.
2. Serve WiFi setup only; pair the keyboard over serial or from a stronger
   board.

Option 1 is the right one and there is already a precedent for it in this
repo — `cyd:mode` is an NVS flag and every wrong turn falls back to chat.

## Why this is worth doing properly

It is what turns these from bench toys into things you can hand to someone.
Power on, look at the screen, point a phone at the QR, pick a network, pair a
keyboard, done — no serial cable, no rebuild, no laptop. Every board in the
fleet gets it from one implementation because the capability differences are
already described in `boards/*.json`.
