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
                                              SoftAP  penguinos-<last4 of MAC>
                                              captive portal on 192.168.4.1
                                              the same web app, setup mode
```

In SETUP the panel is the instruction sheet. It shows the AP name, the AP
password, the URL, and a **QR code encoding `WIFI:S:penguinos-f048;T:WPA;P:...;;`**
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
sleep. That is already working in `../penguinOS-CY24`.

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

"BLE scan" means every state in which the link layer is *listening*, not only
`ble_gap_disc()`. An initiator — `ble_gap_connect()`, which is both the pair
path and the reconnect-to-the-bonded-keyboard path — listens on the same three
advertising channels for the same reason, and the lock has to cover it. It
cannot be *held* across an established connection, though: a keyboard that is
attached would then own the antenna forever and WiFi would never scan again. So
the rule is: the lock is held while listening, and given back the moment the
link is up or the attempt ends.

## Memory, which decides how much of this each board gets

| Tier | Board | Free heap | Provisioning |
|---|---|---|---|
| 1 | C6-LCD-1.3 | 464 KB *(stale — see below)* | everything: AP + portal + both scans + QR |
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
repo — `cyd:mode` is an NVS flag and every wrong turn falls back to chat. It is
**not implemented**; the tier-1 boards do not need it and the CYD has not been
flashed with this image.

The 464 KB in that table was measured with both radios down and is dead. With
WiFi, NimBLE and `esp_http_server` in the image the C6-LCD-1.3 has **248,462
bytes** of DRAM left after static allocation, before the display takes its
38,400 byte DMA strips and before either radio takes a dynamic buffer. That is
still comfortable and it is still a guess about the runtime figure: `app_main`
now prints the free heap and the largest free block after every init step, and
those lines are what settle it. See the margins table in `firmware/README.md`
for the archive-by-archive split.

## Why this is worth doing properly

It is what turns these from bench toys into things you can hand to someone.
Power on, look at the screen, point a phone at the QR, pick a network, pair a
keyboard, done — no serial cable, no rebuild, no laptop. Every board in the
fleet gets it from one implementation because the capability differences are
already described in `boards/*.json`.

## As built

Everything above is implemented and in the image, on this commit, for
`waveshare-c6-lcd-13`. The pieces:

| Piece | Where |
|---|---|
| credential state machine, SoftAP, captive DNS, mDNS | `kernel/svc/eos_net.{h,c}` |
| BLE HID host: scan, pair, bond, reconnect, forget | `kernel/svc/eos_ble.{h,c}` |
| the radio lock | `kernel/svc/eos_radio.{h,c}` |
| the HTTP server and the eight endpoints | `kernel/svc/eos_httpd.{h,c}` |
| the QR encoder | `kernel/qr/eos_qr.{h,c}` |
| the panel: setup screen, passkey screen | `firmware/main/eos_setup_screen.{h,c}` |
| the input ring, HID diff, repeat, buttons | `kernel/hal/eos_input.c` |
| keystroke to window-manager move | `firmware/main/eos_shell_input.c` |
| the boot mode choice and the frame loop | `firmware/main/main.c` |
| the setup-mode web app | `web/setup.js`, `web/index.html`, `web/style.css` |

The two rules this page exists to protect both hold, and both are checkable
rather than asserted:

- **Persist only after a join succeeds.** `eos_net_commit()` is the only path to
  NVS and it refuses unless the last `eos_net_try()` returned OK, refuses a
  second time for the same success, and there is exactly one call to it in the
  tree — in `eos_httpd_pump()`, on the line after the try. `test_net.c` runs
  try-good then try-bad then commit and asserts the store's write counter is
  still zero.
- **The SoftAP is closed.** Twelve characters from a 32-symbol alphabet, 60 bits,
  generated from the platform RNG at first boot and kept in NVS. No entropy
  source is a hard failure — `eos_net` returns `ERR_DRIVER` and never calls
  `ap_start` — so there is no path that produces an open AP.

### Where it deviates from this page, and why

| Deviation | Why |
|---|---|
| `POST /api/wifi/connect` answers **202 with the attempt still running**, not "try it, report the outcome" | the join takes the radio away from the SoftAP the request arrived on, so the socket carrying a synchronous answer is the one most likely to die. The client polls `/api/net/status`. `web/setup.js` treats a dead socket, a 202 and a 200 as the same thing and polls in all three cases |
| `join.reason` is always `failed`, never `bad_auth` or `no_ap` | `eos_net_last_error()` collapses every association failure into one code. The three-way split is implemented and tested in `eos_httpd.c` and in `web/setup.js` and lights up the moment `eos_net` distinguishes them. Both the panel and the page still say **nothing was saved**, which is the part that stops a support call |
| the panel does not animate during a join | `eos_net_try()` blocks the main task for up to fifteen seconds and the frame loop is on that task. The panel keeps showing the QR and the credentials, which is what the person in front of it wants while their phone shows the progress. Fixing it properly means an async `sta_join`, which is a driver-contract change |
| the AP password survives `POST /api/wifi/forget` | it is printed on the panel and may already be typed into a phone. Rerolling it makes the screen the user is reading wrong |
| `POST /api/wifi/forget` answers **before** it drops the network | dropping it kills the socket the request came in on, and the work — erase NVS, re-mode the radio, raise the SoftAP, start the portal DNS, cache a scan — is seconds long. Like the join, it queues and `eos_httpd_pump()` runs it on the frame loop, which is also what leaves `eos_net_t` with a single writer |
| a BLE reconnect listens 1200 ms in every 3000, not continuously | `ble_gap_connect()` puts the link layer into *initiating*, which is a receiver on the advertising channels — the same conflict as a scan. It now takes the radio lock, so it has to give WiFi a gap to take it in |
| the web app in `web/` is not on the board | nothing implements `eos_storage.h` yet, so the `int` LittleFS partition is never mounted and `eos_httpd`'s three file ports are left NULL. SETUP serves the server's own built-in page, which lists networks and joins one and needs no filesystem. Static routes 404 until a storage backend lands |
| the radio lock is `eos_radio.h`, not a line inside either stack | it shipped at the tail of `eos_ble.h`, which meant `eos_net` could only reach it through a build flag and an image built without the BLE service was silently unserialised while still sharing an antenna. It now belongs to neither stack and both bind to it unconditionally |
| tier-0 sequencing (`cyd:mode`) is not implemented | the tier-1 boards do not need it and no CYD has been flashed with this image. The option stands as written |

### The setup screen, as it lands on a 240x240 panel

| Fact | Value |
|---|---|
| symbol | version 3, 29x29 modules, byte mode, ECC L |
| scale | 4 px per module, picked at runtime to fit what the text leaves |
| drawn | 148x148 px, quiet zone 16 px on all four sides |
| colours | black on white, **not** the theme's — a themed QR is a decoration and its failure is silent |
| under it | AP name in 8x13, AP password in 12x20, `http://192.168.4.1` and one status line in 6x8 |
| fallback | a panel that cannot give two pixels per module and still hold four text lines gets text only. That is the 128x64 OLED, as this page asks |

Verified on the host against `eos_qr_module()`: all 841 modules, all 16 pixels of
each. A longer AP name moves the symbol to version 4 and the scale to 3 and it
still fits.

### The pairing screen

Six digits of the 12x20 face at 3x — 36x60 pixels per digit — under the peer's
advertised name, with `eos_ble_pair_warning()` wrapped underneath. That warning
is a function and not a string constant precisely so the panel and the web page
cannot print two different versions of it. On a panel too small for the whole
warning the warning is what gets cut; the digits never are.
