# kernel/svc — services

Kernel services that are not the window manager: `eos_brain`, the MEGABRAIN
client; `eos_net`, the credential state machine and the SoftAP; `eos_ble`, the
BLE HID host a keyboard arrives through; `eos_radio`, the one lock the two
radios share; `eos_httpd`, the provisioning API; and `eos_settings`, the twelve
keys the Settings page edits and the one file on `/int` that carries them; and
`eos_apps`, the other half of the HTTP API — files, console, buddy and the app
list.

| File | What |
|---|---|
| `include/eos_brain.h` | MEGABRAIN client: streaming parser, request API, transport interface |
| `eos_brain.c` | Implementation, plus the ESP-IDF socket/mDNS/NVS bindings |
| `test/test_brain.c` | Host test, 182 checks, no networking |
| `include/eos_net.h` | WiFi: the try/commit credential machine, SoftAP, captive DNS, the QR payload |
| `eos_net.c` | Implementation, plus the ESP-IDF WiFi/NVS/mDNS bindings |
| `test/test_net.c` | Host test, 1276 checks, no radio |
| `include/eos_radio.h` | The single lock WiFi and BLE take around a scan or a join |
| `eos_radio.c` | Implementation: a flag and a spinlock, no creation step |
| `include/eos_ble.h` | BLE HID host: scan, pair, bond, reconnect, forget |
| `eos_ble.c` | Implementation, plus the ESP-IDF NimBLE bindings |
| `test/test_ble.c` | Host test, 570 checks, no radio |
| `include/eos_httpd.h` | The HTTP server: the provisioning API, the web app, the captive portal |
| `eos_httpd.c` | Implementation, plus the esp_http_server / eos_net / eos_ble bindings |
| `test/test_httpd.c` | Host test, 2111 checks, no sockets |
| `include/eos_settings.h` | The settings store: twelve dotted keys, the file, the debounce |
| `eos_settings.c` | Implementation. No IDF at all — it is eos_storage and the JSON reader |
| `test/test_settings.c` | Host test, 334 checks, on a real filesystem in /tmp |
| `include/eos_apps.h` | Files, console, buddy and apps: fourteen routes, two ports, the caps |
| `eos_apps.c` | Implementation. IDF only for the log hook and one mutex |
| `test/test_apps.c` | Host test, 368 checks, on a real filesystem in /tmp |

```bash
cc -std=c99 -Wall -Wextra -O1 -Ikernel/svc/include \
   kernel/svc/eos_brain.c kernel/svc/test/test_brain.c -o /tmp/test_brain && /tmp/test_brain

cc -std=c99 -Wall -Wextra -O1 -Ikernel/svc/include \
   kernel/svc/eos_net.c kernel/svc/eos_radio.c \
   kernel/svc/test/test_net.c -o /tmp/test_net && /tmp/test_net

cc -std=c99 -Wall -Wextra -O1 \
   -Ikernel/hal/include -Ikernel/svc/include -Ikernel/wm/include -Ikernel/shell/include \
   kernel/svc/eos_ble.c kernel/svc/eos_radio.c kernel/hal/eos_input.c \
   kernel/wm/eos_wm.c kernel/shell/eos_keys.c \
   kernel/svc/test/test_ble.c -o /tmp/test_ble && /tmp/test_ble

cc -std=c99 -Wall -Wextra -O1 -Ikernel/svc/include \
   kernel/svc/eos_httpd.c kernel/svc/test/test_httpd.c -o /tmp/test_httpd && /tmp/test_httpd

cc -std=c99 -Wall -Wextra -O1 \
   -Ikernel/hal/include -Ikernel/svc/include -Ikernel/wm/include \
   -Ikernel/avatar/include -Iboards/generated \
   kernel/svc/eos_apps.c kernel/svc/eos_httpd.c \
   kernel/hal/backend/storage/eos_storage_idf.c \
   kernel/avatar/eos_vox.c kernel/avatar/eos_buddy.c \
   kernel/svc/test/test_apps.c -o /tmp/test_apps && /tmp/test_apps

cc -std=c99 -Wall -Wextra -O1 \
   -Ikernel/hal/include -Ikernel/svc/include -Ikernel/wm/include -Iboards/generated \
   kernel/svc/eos_settings.c kernel/svc/eos_httpd.c \
   kernel/hal/backend/storage/eos_storage_idf.c \
   kernel/svc/test/test_settings.c -o /tmp/test_settings && /tmp/test_settings
```

The BLE suite links `eos_wm` and `eos_keys` because its last section is not a
unit test: it pushes a synthesised `super+return` report through the input HAL
into the real keybind table against a real `eos_wm_t` and checks that a window
opened. That is the whole chain from the wire to the window manager, running on
a laptop with no radio in the room.

The settings suite links the real storage backend and the real HTTP handlers for
the same kind of reason. "The settings file is truncated" means a truncated file
in a sandbox under `/tmp`, not a string handed to a parser, and the four
endpoints are driven through `eos_httpd_dispatch()` against the real store
rather than a mock of it. Also clean under `-fsanitize=address,undefined`.

## eos_settings

Twelve dotted keys, flat, none longer than fifteen bytes, because
`web/README.md` fixed that shape so a settings key can also be an NVS key.

| Group | Keys | Live? |
|---|---|---|
| Network | `wifi.ssid`, `wifi.psk`, `wifi.psk_set`, `net.host` | reboot |
| Megabrain | `brain.host`, `brain.port`, `brain.model`, `brain.max`, `brain.system` | live |
| Appearance | `ui.theme`, `ui.bright` | live |
| System | `sys.tz`, `sys.autostart` | tz live, autostart reboot |

Four rules the implementation rests on:

- **A bad file must never stop a boot.** Truncated, empty, garbled, absent, full
  of random bytes, every key at the wrong type — each one leaves the store
  holding defaults and returns a reason code for the log. This is
  `kernel/theme`'s rule and it is here for the same reason.
- **Numbers clamp, the document does not fail.** A brightness of 4000 is a typo
  in one field; rejecting the file over it would cost the owner every other
  setting in it.
- **WiFi credentials are not in this file.** `wifi.ssid` and `wifi.psk` route to
  `eos_net`, which is the only thing in the image that enforces
  try-then-commit. A second copy here would be a second chance to get
  save-then-try the wrong way round.
- **No HTTP worker writes flash.** A LittleFS sync is one or more 4 KB sector
  erases with the instruction cache off, on a chip with one core that is also
  driving the panel and the radio. `eos_settings_set()` mutates RAM;
  `eos_settings_pump()` writes it from the OS loop once the edits have been
  quiet for two seconds. Sixty slider positions cost one erase, which is the
  debounce `kernel/hal/backend/storage/README.md` asks the layer above it for.

## MEGABRAIN, the server

Local models on the Mac mini. Verified live on 2026-08-30.

| Fact | Value |
|---|---|
| Address | `192.168.0.139`, port **80** (Caddy in front of the model runner) |
| Ask | `GET /ask?stream=1&max=<int>&system=<enc>&model=<enc>&q=<enc>` |
| Ask, long prompts | `POST /ask?...` with the prompt as a `text/plain` body — both work |
| Health | `GET /health` -> `{"ok":true,"uptime":...,"ollama":"..."}` with a Content-Length |
| Reply framing | `Transfer-Encoding: chunked`, **no Content-Length**, `X-Accel-Buffering: no` |
| Reply body | `text/plain; charset=utf-8` — plain text, not SSE, not JSON |
| Models | `qwen3.5:2b` fast default, `gemma4:12b-it-qat` clean, `ornith:9b` |

Space encodes as `%20`, never `+`. The server accepts either but `+` in a `q=`
value is ambiguous with a literal plus, and prompts contain literal pluses.

## The parser

`eos_brain_parser_t` is a pure incremental state machine. Bytes go in via
`eos_brain_parser_feed()` in whatever sizes the socket produced; decoded text
comes out through a callback. It never allocates, never blocks, and never reads
past the buffer it was handed.

Fixed cost, whatever the reply size:

| Buffer | Default | Purpose |
|---|---|---|
| `line[]` | 96 B | status line, one header, one chunk-size line |
| `buf[]` | 64 B | decoded text staged for the callback |
| whole struct | **208 B** | |

It handles the status line, headers, and all three body framings the server can
produce (chunked, content-length, read-until-close), because a fallback path
that silently mis-frames is worse than one that does not exist.

### The UTF-8 rule

A chunk boundary falls wherever the writer flushed, which is regularly in the
middle of a multibyte character. The parser holds back a trailing incomplete
sequence — at most three bytes — until the rest arrives, so **no valid
character is ever split across two callbacks**. That invariant is asserted on
every callback in the test, at every possible split of every test stream.

Bytes that are already not UTF-8 are the exception, and it is deliberate. A lead
byte that the next byte proves can never be completed is released as a raw byte
instead of being held, because holding it would stall the stream on garbage and
there is no character there to keep whole. `test_parser_invalid_utf8` pins that:
the bytes come out byte-exact, none lost, none repeated.

If the connection dies mid-character the fragment is dropped, not emitted. There
is no way to complete it and a renderer handed half a character draws garbage.

`eos_brain_utf8_safe_len()` is public because the line-wrapper wants the same
rule when it truncates.

### Failure behaviour

| Input | Result |
|---|---|
| Stream dies mid-body | text so far is delivered, then `ERR_TRUNCATED` |
| Chunk claims more bytes than arrive | same — the arrived bytes are real |
| Non-hex or over-long size line | `ERR_PROTOCOL`, nothing emitted |
| Header line longer than 96 B | discarded, parsing continues |
| Reply is not HTTP at all | `ERR_PROTOCOL` on the status line, nothing emitted |
| Bytes after the terminating chunk | ignored |
| Feed or finish after a terminal state | inert |

Read-until-close is the one case where the socket closing is *success*, not
truncation. Everything else that ends early is truncation.

## The service

One request in flight, copied into fixed buffers at submit. `eos_brain_submit()`
returns immediately and touches nothing; `eos_brain_pump(b, budget_ms)` from the
OS loop does the work in bounded slices. There is no task, no stack, no queue —
tier 0 has about 20 KB of heap free with WiFi and BLE up and cannot afford any
of them. `sizeof(eos_brain_t)` is **2224 bytes**, meant for BSS.

| Knob | Default |
|---|---|
| prompt / system / model | 384 / 224 / 32 B |
| request head incl. encoded URL | 1024 B |
| connect / idle / total timeout | 3 s / 20 s / 60 s |
| link TTL | 15 s |

`EOS_BRAIN_METHOD_AUTO` (the default) builds a GET and falls back to POST if the
percent-encoded URL will not fit. Nothing is ever silently truncated: a request
that cannot be built is refused with `ERR_TOO_LONG`.

### Discovery

Three sources, walked lazily so a working cached address never pays the mDNS
timeout, each verified with `GET /health` before a prompt is sent to it:

1. the address cached in NVS (`brain` / `host`)
2. mDNS `megabrain`
3. the compiled-in `192.168.0.139`

The address that answered 200 is written back to NVS, and only when it changed —
one flash erase per prompt is not worth it. Duplicates across the three sources
are dialled once. A successful probe is trusted for `link_ttl_ms`, so back-to-back
prompts cost one round trip, not two.

This puts the cache ahead of mDNS rather than behind it, which is the point of
having a cache. On a board with an empty NVS the order is mDNS then the literal
IP, as intended.

### Events

`eos_brain_link_t` is what the status bar shows. The request lifecycle is what
the buddy animates from.

| Event | Buddy |
|---|---|
| `SUBMITTED` | start thinking |
| `FIRST_TOKEN` | stop thinking, start talking |
| `TOKEN` | text is on screen |
| `DONE` / `FAILED` / `CANCELLED` | settle / sulk |
| `LINK` | status bar reachable indicator |
| `STATE` | resolving / connecting / sending / streaming |

A health probe's JSON and any non-200 error body are parsed but never delivered
as tokens. The terminal must not print server noise as if the model said it.

### Transport

Five function pointers. `recv` must not block: bytes, `0` for nothing yet,
`EOS_BRAIN_EOF` for a closed peer, anything else negative for a broken socket.
`open` is the one call allowed to block, bounded by `connect_ms`.

This is why the whole thing is host-testable with zero networking. On ESP-IDF,
`eos_brain_lwip_transport()` is plain BSD sockets — `inet_pton` first so the
normal path never reaches the allocating resolver — and `eos_brain_idf_hooks()`
wires mDNS and NVS. Neither is compiled on the host.

## Test

182 checks. The interesting ones are exhaustive rather than illustrative:

- every chunked test stream fed at **every block size from 1 byte to the whole
  stream**, output compared byte for byte each time
- named splits inside the hex size line, the status line, and the CRLF pairs
- multibyte characters deliberately split across chunk boundaries, at every
  block size, with 2, 3 and 4-byte sequences
- **every possible truncation point** of a full response: must report
  `ERR_TRUNCATED` and the text delivered must be a prefix of the real answer
- 4 KB of random bytes, 4 KB of `0xFF`, 4 KB of NUL, at 64 block sizes
- an uncompletable lead byte passed through byte-exact at every block size,
  while a lead byte that can still complete is held
- framing lies that must not be believed: a chunk size past
  `EOS_BRAIN_CHUNK_LIMIT`, junk where a chunk's trailing CRLF belongs, bytes
  behind a content-length body, and a status line that is not `HTTP/`
- percent-encoder vectors plus all 255 byte values
- service: discovery walking all three candidates, cache write-back, link TTL
  shortcut, cancel mid-stream, timeout on a silent server, 500 on the ask,
  connection refused everywhere, busy and over-long submits

Every parser runs inside a 32-byte canary frame and every sink has guard bytes,
so an out-of-bounds write anywhere fails a check rather than passing quietly.
The suite is also clean under `-fsanitize=address,undefined` and under
`-Wpedantic -Wshadow -Wconversion -Werror` at `-O0` through `-O3` and `-Os`,
and passes with `EOS_BRAIN_TEXT_MAX` overridden to 32, 128 and 255, so the
parser is not tuned to one buffer size.

The buffer sizes are `#ifndef`-overridable, and the counters that index them are
narrow on purpose. `eos_brain.h` carries `#error` guards so an override that
outgrows its counter — `EOS_BRAIN_TEXT_MAX` past 255, where `buf_len` is a
`uint8_t` — fails the build instead of wrapping mid stream.

Verified end to end against the real mini at 3 bytes per `recv` — the worst
framing splits a real socket can produce — with accented and em-dash output
arriving intact over both GET and POST.


# eos_ble — the BLE HID host

The board is the central and the keyboard is the peripheral. It exists because
these panels have no keys: a board carried into a strange room has no network
and no way to type, and the only two channels out of that hole are the screen
and a keyboard that pairs over the air.

NimBLE, never Bluedroid. 19 KB against 83 KB, and on the tier-0 board the
difference is booting versus an instant out-of-memory. `eos_ble_init()` must run
before WiFi — the controller wants a large contiguous block and the WiFi stack
fragments the heap.

## The pairing problem, and why the screen solves it

Most BLE keyboards bond by **passkey entry**: the HOST picks a six-digit number
and the human types it ON THE KEYBOARD. The keyboard has no screen, so the
number has to come from somewhere else. It comes from the LCD. That is the
whole trick, and it is the same out-of-band channel the SoftAP password uses.

The board declares `BLE_HS_IO_DISPLAY_ONLY`, which is the truth about it and is
what selects passkey entry. `sm_bonding`, `sm_mitm` and `sm_sc` are all on: an
unauthenticated pairing to a device that will then send every keystroke is not
worth the two seconds it saves.

The number reaches two places at once. `eos_ble_on_passkey()` fires immediately
for the panel; `eos_ble_status()` carries it for the web page to poll. One
number, two readers, no second source of truth.

### A keyboard bonds to ONE board

The K809 bonds to whichever board it paired with last. Pairing it here silently
destroys its bond with the board it was on before, and the only symptom over
there is a keyboard that stopped working. `eos_ble_pair_warning()` is the exact
sentence to print, and it is a function rather than a macro so the LCD and the
web page cannot drift into two different warnings.

The other side of the same coin is the **stale bond**: a keyboard re-paired
elsewhere has wiped its half of the key and will reject ours. That is caught in
two places — `BLE_GAP_EVENT_REPEAT_PAIRING` before pairing starts, and an
encryption failure after it — and both delete the peer and pair fresh, once.
Once, not in a loop: a repair loop sits there showing passkeys forever.

## Scan, connect, subscribe

| Step | What |
|---|---|
| scan | active, 30 ms window in 60 ms, duplicate filtering OFF |
| merge | advertisement and scan response are merged by address |
| connect | `ble_gap_connect`, 10 s |
| encrypt | `ble_gap_security_initiate` FIRST, before any GATT read |
| discover | service `0x1812`, then its characteristics |
| subscribe | boot input `0x2A22` if present, else every notifying `0x2A4D` |

Encryption comes before discovery because a HID service read on an unencrypted
link is refused with insufficient authentication on every characteristic, and
the errors look like a broken keyboard rather than a missing pairing.

Duplicate filtering is off on purpose. An active scan reports the advertisement
and the scan response separately, milliseconds apart, and the name is usually
only in the second one — so a device seen twice is one device with more known
about it. The merge is in `eos_ble_devlist_add()` and it is host-tested.

The scan table is eight entries and a room holds more than eight beacons, so
entries are ranked: bonded beats keyboard beats HID beats named, and RSSI
breaks the tie. A loud beacon cannot displace a weak keyboard.

Boot protocol mode and report mode are exclusive. Boot mode fixes the report
layout at the eight bytes the spec defines, which is the only layout this host
understands; asking for it and also subscribing to the report-mode
characteristics would double every keystroke on a keyboard that ignored the
request. A notification shorter than eight bytes is a mouse or a consumer-control
key and is dropped — the HAL would survive it, but it would read the first byte
as modifiers and invent a chord out of a volume key.

## Reconnect costs no scan

A bonded keyboard needs no discovery: we know its address. `eos_ble_tick()`
issues a direct `ble_gap_connect` on a backoff, which simply waits for the
keyboard to advertise — which it does the moment a key is pressed — and costs
the radio nothing in between. Scanning is only ever for pairing something new.
Getting this wrong means the board scans every few seconds forever and WiFi
never gets the antenna.

## The bond record

48 bytes in NVS (`eos_ble` / `bond`). This is penguinOS's memory of WHICH device it
bonded to, not the bond itself — the link keys live in NimBLE's own store and
are never copied out of it. What is kept here is the address to reconnect to and
the name to show.

| Offset | Field |
|---|---|
| 0..2 | `'E'`, `'B'`, format version |
| 3 | address type |
| 4..9 | address, display order |
| 10 | name length, 0..31 |
| 11..42 | name, NUL padded |
| 44..45 | appearance, little endian |
| 47 | checksum: `0xA5` xor bytes 0..46 |

A record is either exactly right or it is rejected. There is no partial parse
and no "version 2 with extra fields at the end", because the thing on the other
side of this codec is a flash page that may have survived a brownout and the
only safe reading of a damaged one is to pair again. Every single-bit corruption
of a valid record is caught, and 20,000 random 48-byte blobs are all rejected —
both are asserted in the suite.

It is written only when a keyboard has actually delivered a usable subscription,
never at connect time. A record written earlier would name a device that turned
out not to be a keyboard at all, and the board would chase it on every boot.

## The radio lock

One radio, two stacks, and on the C6 802.15.4 as well. A BLE scan and a WiFi
scan running together do not merely go slowly — they corrupt each other's
results and can wedge the controller.

```c
if (!eos_radio_lock("wifi-scan", 2000)) return EOS_ERR_BUSY;
...
eos_radio_unlock("wifi-scan");
```

A flag and a spinlock rather than a FreeRTOS mutex, so the lock has no creation
step: it has to work whether the WiFi side or the BLE side reaches it first, and
a lazily created mutex is a race at exactly that moment. Releasing someone
else's lock does nothing, because a stray release presents as two stacks
scanning at once — the failure the lock exists to prevent — while a refused
release presents as a stall, which is findable.

It is **advisory**. It works only because both callers use it. It lives in
`eos_radio.{h,c}` and belongs to neither stack, so both bind to it
unconditionally: `eos_net.c` reaches it through `__has_include("eos_radio.h")`
and `eos_ble.c` includes it directly. There is no build flag and no CMake gate.
There used to be — `EOS_NET_BIND_RADIO_LOCK`, turned on only when `eos_ble.c`
was in `SRCS` — and it meant an image built without the BLE service was
silently unserialised while still sharing an antenna. Do not reintroduce it.

## The two halves of the file

Everything above the `ESP_PLATFORM` guard is plain C99 with no radio in it:
address formatting, the name sanitiser, the scan-table merge, the bond codec and
the radio lock. That half is what the host test runs, and it is the half where
the bugs that matter live — records read back from NVS and names read off the
air are the two places a bad byte can walk out of bounds.

Advertised names are sanitised once, at the point they enter the system:
anything outside printable ASCII becomes `'?'`, a NUL ends the name, and the
result always fits and always terminates. They come off the air from an
untrusted peripheral and end up drawn on an LCD and embedded in JSON.

Everything below the guard is NimBLE, driven entirely from `ble_gap_event()`,
because every GATT operation is asynchronous and the alternative — blocking a
task on each step — is what makes BLE bring-ups hang forever waiting for a
peripheral that walked out of range.

## Test

570 checks. The interesting ones are attacks rather than illustrations:

- HID reports at every length from 0 to 32 in exactly sized allocations, plus
  4000 random reports of random lengths inside canary frames
- every rollover shape: six `0x01`s, six `0x02`s, a lone `0x03`, and a rollover
  report that still carries a modifier
- six keys down, two released, four undisturbed
- every single-bit corruption of a valid bond record (384 of them), every wrong
  length, an over-long name length with a repaired checksum, and 20,000 random
  blobs
- every truncation of a valid address string
- the name sanitiser at every source length from 0 to 64 against random bytes,
  in a canary frame
- the radio lock: exclusion, a refused foreign release, an unconditional one
- a keyboard vanishing mid-chord, which must leave no modifier latched and must
  not make the next keyboard's first report look like six releases
- `super+return` and `super+2` on the wire, through the HAL, through
  `eos_keys_feed()`, into a real `eos_wm_t`

Clean under `-fsanitize=address,undefined` and under `-Wpedantic -Wshadow
-Wconversion -Werror` at `-O0` through `-O3` and `-Os`.

## What the HTTP layer calls

`docs/provisioning.md` names four BLE endpoints. This is the whole mapping, so
the adapter at the bottom of `eos_httpd.c` has one place to read:

| Endpoint | Calls |
|---|---|
| `GET /api/ble/scan` | `eos_ble_scanning()`, `eos_ble_scan_results()`, `eos_ble_scan_age_ms()`, `eos_ble_scan_start(0)` |
| `POST /api/ble/pair` | `eos_ble_pair_addr(addr)` — the JSON carries only a string |
| `GET /api/ble/status` | `eos_ble_status()` |
| `POST /api/ble/forget` | `eos_ble_forget()` |

Two things about the address. It is **display order** everywhere above this
file — `addr[0]` is the byte printed first — and NimBLE's little-endian
`ble_addr_t` is converted once, at the NimBLE boundary, so an address read off
the screen and typed into a form matches. And its TYPE is not on the wire,
because public and random addresses are indistinguishable as text;
`eos_ble_pair_addr()` recovers it from the scan table and assumes random for an
address that was never scanned, which is what almost every BLE keyboard uses.

The passkey reaches the page through `eos_ble_status()`'s `passkey` and
`passkey_shown`, and the warning through `eos_ble_pair_warning()` rather than a
string of its own. Two spellings of that warning is how one of them goes stale.

## What is not here

| Missing | Why |
|---|---|
| BLE HID **device** mode | the board is a host; nothing wants it to be a keyboard |
| report-descriptor parsing | boot protocol mode fixes the layout, and every keyboard supports it |
| mouse and consumer-control reports | nothing above the HAL binds them yet; they are dropped by length |
| more than one bonded keyboard | one keyboard, one panel; a second is a UI this OS does not have |

---

# eos_httpd — the HTTP server

The eight endpoints in `docs/provisioning.md`, the web app, and the captive
portal. In SETUP it lives behind the SoftAP and is the only way onto a board in
a strange room; in RUN it serves the same app over the joined network.

`sizeof(eos_httpd_t)` is **4752 bytes**, meant for BSS. On target the whole file
costs 16.6 KB of text, 7.7 KB of rodata and 1.5 KB of static buffers.

| Buffer | Default | Purpose |
|---|---|---|
| `resp[]` | 4096 B | the whole JSON document, built in one pass |
| body | 512 B | the request body, on the worker's own stack, outside the lock |
| `uripath[]` | 160 B | the decoded request path |
| `path[]` | 96 B | the staged static path, `.gz` included |
| `chunk[]` | 1024 B | file streaming, one static buffer behind the lock |

Every one is `#ifndef`-overridable and the header carries `#error` guards, so a
tier-0 board that cuts `EOS_HTTPD_RESP_MAX` and `EOS_HTTPD_SCAN_MAX` fails the
build rather than the request if it cuts one past what another needs. The suite
passes at 512/2 and at 16000/48.

## Nothing here waits for a radio

A WiFi scan takes about three seconds, a join up to fifteen, a BLE scan five.
`esp_http_server` has four workers and a phone gives up in about ten. So every
slow operation is a **job**: the request starts it and returns 202, and the
client polls a status endpoint.

| Request | Answers | Client then |
|---|---|---|
| `GET /api/wifi/scan` | 200 with the cache | renders it |
| `GET /api/wifi/scan?rescan=1` | 202, scan started | polls the same URL |
| `POST /api/wifi/connect` | 202, `state: trying` | polls `/api/net/status` |
| `GET /api/ble/scan?rescan=1` | 202, scan started | polls the same URL |
| `POST /api/ble/pair` | 202, `state: pairing` | polls `/api/ble/status` |

Holding the connection open across the operation loses either way. The join
takes the radio away from the SoftAP the request arrived on, so the socket
carrying the answer is the socket most likely to die; and four reloads of the
setup page would put four workers behind one antenna.

`eos_httpd_pump()` runs the queued work from the OS loop. It is also the one
place credentials reach flash: `eos_net_try()`, and `eos_net_commit()` **only
if** that returned OK. Save-then-try is how one typo becomes a board that needs
a serial cable, and there is exactly one line in the tree that could do it.

## One radio

WiFi and BLE share one antenna, and on the C6 so does 802.15.4. The real mutual
exclusion is `eos_radio_lock()` at the tail of `eos_ble.h`, which is where it
belongs — only the services know when a scan is actually over. What this layer
adds is that the HTTP surface never *asks* for the overlap: a rescan or a join
while the other radio is busy is a 409 carrying a sentence the UI can show,
rather than a request queued behind five seconds of antenna.

## The JSON writer

An SSID is 32 arbitrary bytes off the air. It will contain quotes, backslashes,
control bytes and invalid UTF-8, and a writer that assumes otherwise emits a
document the phone cannot parse — on exactly the screen the owner needs to get
the board onto a network.

`eos_json_*` writes into a caller buffer, escapes to RFC 8259, and replaces
every byte that is not part of a well-formed UTF-8 sequence with one U+FFFD.
Overflow is sticky and silent at the call site: a handler that checked after
every field is a handler that would forget one, and a truncated JSON document is
worse than a 500, so `eos_json_ok()` is the single place it is discovered.

Resync after a bad byte is at the **next byte**, not the next lead byte. That is
what makes the output length a function of the input alone, which is what
`eos_json_escaped_len()` has to promise — the scan handler budgets on it to
decide whether one more network still fits, so the list gets shorter rather than
the document getting truncated.

The repaired text is lossy by construction, so the raw bytes ride alongside:
every SSID and device name is also emitted as `ssid_hex` / `name_hex`, and
`POST /api/wifi/connect` accepts `ssid_hex` in place of `ssid`. That is the only
way a network whose name is not valid UTF-8 can be picked out of a list and then
joined again.

## The JSON reader

Flat, bounded, deliberately shallow: it finds a key at the top level of an
object and skips everything else without descending into it, so
`{"a":{"ssid":"decoy"},"ssid":"real"}` yields `real`. Escapes decode, surrogate
pairs included; a lone surrogate becomes U+FFFD; an escaped `\u0000` is refused
outright rather than silently shortening a value, because a truncated SSID names
a different network.

Raw bytes inside a string come through untouched. An SSID is bytes, and
repairing it on the way *in* would change which network the caller asked for.

## Routing

One handler for every request. `esp_http_server`'s own URI table would be a
second route table to keep in step with `eos_httpd_route()`, and two route
tables is how a captive portal ends up 404ing the probe it exists to answer.

A path under `/api/` that is not in the table is a 404 and never falls through
to the filesystem; `..`, a backslash and an encoded `%2e%2e` are refused before
any path is built. The board answers `GET` and `POST` and nothing else.

## The captive portal

Fourteen connectivity-probe URLs — iOS, Android, Windows, Firefox, GNOME,
Kindle — get a 302 to the portal in SETUP mode, which is what makes the page pop
by itself. In RUN mode they do not: the board is a host on somebody's LAN then,
and redirecting a probe there tells every device on the network that this one is
a walled garden.

**The DNS responder is not here.** It belongs to `eos_net`, which raises it with
the SoftAP and tears it down with it. A second listener on port 53 would only
fail to bind, and the one that lost the race would be the one nobody noticed.

## Serving the app

Pre-gzipped, streamed, never buffered. `<root>/<path>.gz` is tried first and
served with `Content-Encoding: gzip`; the type still comes from the name under
the `.gz`, because the encoding is a header and `index.html.gz` is HTML. The app
is ~110 KB raw and 32 KB gzipped and it moves 1 KB at a time out of flash — the
largest free block on a board with WiFi up would not hold it whole.

| Mode | Root | A file that is not there |
|---|---|---|
| SETUP | `/int/setup` | the built-in page at `/`, a portal redirect elsewhere |
| RUN | `/int/web` | 404 |

Both roots are config, so a board that keeps the app on the card points them at
`/sd/web` and nothing else changes.

### The built-in setup page

~2 KB of HTML compiled into the image, served in SETUP when the document root
has nothing in it — which is every board that has never been provisioned. It is
not the web app. It lists networks, joins one, and reports the outcome, and it
exists so that an empty filesystem is still a board you can get onto a network.
It fetches nothing off-board.

## Megabrain, over HTTP

Three endpoints, `web/README.md`'s "Megabrain" section, over four ports that
have the same shape as the radios' and exist for a sharper version of the same
reason.

| Method | Path | Answers |
|---|---|---|
| GET | `/api/brain/status` | host, port, model, models, reachable, busy, last_error |
| POST | `/api/brain/ask` | **streamed** `text/plain; charset=utf-8`, chunked |
| POST | `/api/brain/cancel` | `{"cancelled":bool}`, always 200 |

`eos_brain` is a single-request state machine with no lock in it, and its pump
blocks — three seconds on a connect to a mini that is switched off, two more on
an mDNS query nothing answers. So exactly one task may call it, and that task is
not an HTTP worker. `brain_ask` starts a request and returns; the worker drains
decoded text through `brain_read` from a ring the owning task fills. On the
board that task is `firmware/main/eos_brain_bridge.c`.

### Why the reply is a chunked response and not SSE or a poll

A reply takes seconds and arrives token by token, and this server has four
workers, one core and a frame loop it must not starve.

| | Verdict |
|---|---|
| chunked `text/plain` | **chosen.** One socket, no framing the client does not want; `app.js` already reads `r.body.getReader()` |
| SSE | same socket for the same duration, plus an event framing at both ends that buys nothing |
| polling a buffer | still needs the ring, because tokens arrive between polls whatever the client does — and then spends a worker AND the dispatch lock several times a second, and delivers the text in lumps. Strictly more machinery for a worse result |

The cost is one worker held for the length of one reply. `eos_brain` allows one
request in flight and a second ask is refused 409 before it can take a second
worker, so three of the four are always free — more than the two concurrent
requests the web app holds itself to.

The dispatch lock is **released before the drain begins**, and that is the one
change a stream makes to the responder. A stream reads none of the shared
buffers the lock protects — the test asserts this by sentinel, not by reading
the code — so holding it across a chat request would park the other three
workers for nothing.

### A worker is never lost

Four exits, all bounded:

| | What happens |
|---|---|
| the reply ends | `brain_read` says END; terminating chunk; worker returns |
| the client vanishes | the next `send_chunk` fails; the request is **cancelled** rather than left talking into a ring nobody drains |
| nothing arrives for 30 s | cancel, then a `! ` line, then close |
| 120 s in total | the same |
| the reader itself disappears | the binding takes the channel back 5 s after the reply settled, or the next ask would be 409 for the life of the boot |

An error raised after the stream has started cannot use a status code — the 200
is already on the wire. `web/README.md` spells the alternative and this
implements it: a line beginning `! `, then close.

## Test

2111 checks, no sockets, no radio, no IDF. The radios and megabrain reach the
handlers through a port table and the suite fills it with a scripted fake, so
the twelve handlers under test are the production ones.

Two properties are asserted on every string the writer produces, by validators
written in the test rather than borrowed from the code under test: the output is
**always well-formed JSON** and **always valid UTF-8**. Every buffer sits in a
canary frame, the whole `eos_httpd_t` included.

- 30 adversarial SSIDs — quotes, backslashes, every control byte, truncated
  leads, bare continuations, overlong encodings, UTF-8-encoded surrogates, a
  code point past U+10FFFF, `0xFE`/`0xFF` — each checked for valid JSON, valid
  UTF-8, an exact `escaped_len` prediction, and byte-exact hex
- all 256 single byte values, 32-byte fills at 16 values, and 400 random SSIDs
- a 32-byte SSID with no NUL in it, which is what the 802.11 field actually is
- the same document into every buffer size from 1 byte to two past what it
  needs: overflow reported iff it did not fit, always NUL-terminated, never a
  byte outside the frame
- 20 malformed bodies, every truncation point of a valid one, a 200-level
  nesting bomb, a key longer than the reader's own key buffer, and an output
  buffer at every size from 1 to 12 around a value of 8
- 8 endpoints against the wrong method, `/api/` typos, four shapes of path
  traversal, 11 probe URLs, and an over-long request target
- every rejection asserted to cost the radio nothing: the fake's join counter
  stays at 0 through all fifteen bad connect bodies
- the walkthrough — probe, portal, scan, wrong password, right password, pair —
  in the order a person does it, asserting at step 5 that a failed join leaves
  `stored:false` and says so
- the fields `web/README.md` says the page reads, one at a time

Clean under `-fsanitize=address,undefined`, under
`-Wpedantic -Wshadow -Wconversion -Werror` at `-O0` through `-O3` and `-Os`, and
with the buffer tunables overridden in six combinations.

## What is not here

| Missing | Why |
|---|---|
| `Range` on static files | the app never sends one, and it wants the whole file |
| `Content-Length` on files | responses are chunked, which is what streaming from flash costs |
| TLS | no certificate a phone would accept, and no clock to check one with |
| `Range` on `/api/fs/read` | same reason: the response is chunked, and the web app sends none |
| an SD card | `sdcard.present` is false on every board profile; `/sd` routes and answers `no_such_device` |
| model discovery | megabrain publishes no list; the three names are compiled into the binding |
| a second concurrent ask | `eos_brain` holds one request, so the second is 409 before it can take a worker |
| a per-request auth token | the SoftAP is WPA2 and its password is on the panel; that is the boundary |
| concurrent API requests | one server, one set of buffers, one mutex — four reloads cost no heap |


---

# eos_apps — files, console, buddy, apps

The other twenty endpoints `web/README.md` specifies, minus settings, system,
themes and megabrain. Fourteen routes, one translation unit, no allocation.

| Route | Method | Answers |
|---|---|---|
| `/api/fs/list` | GET | `path`, `offset`, `count` — a paged listing with `total` and `more` |
| `/api/fs/stat` | GET | `path` — size, mtime, is_dir |
| `/api/fs/read` | GET | `path` — the raw bytes, streamed, `application/octet-stream` |
| `/api/fs/usage` | GET | `point` — total, used, free for one mount |
| `/api/fs/write` | POST | `path`, `offset`, `final` — one upload chunk, raw body |
| `/api/fs/upload/abort` | POST | `path`, or nothing — drops the open handle |
| `/api/fs/mkdir` | POST | `path` — one level, parents are not created |
| `/api/fs/remove` | POST | `path` — files and empty directories |
| `/api/fs/rename` | POST | `from`, `to` — same mount only |
| `/api/console/log` | GET | `since`, `max` — a ring slice with `next` and `dropped` |
| `/api/console/exec` | POST | `{"cmd":"..."}` — seven words, 202, output through the log |
| `/api/buddy` | GET | the avatar's config, state, model and this board's caps |
| `/api/buddy/reload` | POST | re-reads `buddy.json` and `buddy.vox` with no reboot |
| `/api/apps` | GET | the windows the shell can open, for the autostart picker |

Errors are `web/README.md`'s table, produced by `eos_httpd_fail_err()` from an
`eos_err_t`, so there is one copy of the code/status mapping in the image.

## How it reaches the server

`eos_httpd.c` gained three things and no handlers: fourteen rows in `ROUTES[]`,
fourteen `case` labels that all call one function, and five one-line exports of
its own error helpers. The handlers are here.

The call is through a pointer `eos_apps_init()` registers, not a direct call:

| | Why |
|---|---|
| `test_httpd.c` stays a two-file link | it links no `eos_apps.c`, no `eos_storage`, no `eos_vox` |
| an image can leave this file out | the pointer is NULL and those routes answer 501 |
| three agents were adding routes at once | the diff in the shared file is 20 lines, all appends |

The fourteen `case` labels are spelled out rather than range-checked on the
enum. A range is what silently swallows the next route somebody inserts.

## The upload

`web/README.md`'s state machine, verbatim, plus the two rules that make a stuck
handle recoverable.

| `offset` | `final` | Board |
|---|---|---|
| `0` | `0` | create/truncate, keep the handle |
| `0` | `1` | create/truncate, write, sync, close |
| `== position` | `0` | append, keep |
| `== position` | `1` | append, sync, close |
| anything else | — | `state` (409), handle untouched |

A write to a **different** path while a handle is open is `busy` (409) at any
offset, zero included — that is the case that would otherwise silently switch
files. The escape is `/api/fs/upload/abort`, which is what the client already
calls after three failed retries, plus a **30 s idle timeout** drained by
`eos_apps_tick()` from the OS loop for the phone that never comes back. Both
leave the partial file where it is; the client knows what it was uploading.

`sync` happens once, on the final chunk. A sync is one or more 4 KB sector
erases with the instruction cache off on the one core also driving the panel,
the radio and this server — per chunk it would be a 100-chunk upload holding the
machine a hundred times.

### chunk_max is 512, and it is not a policy

`EOS_APPS_CHUNK_MAX` is `EOS_HTTPD_BODY_MAX`, by definition and not by
coincidence. The body arrives in `on_request()`'s **stack** buffer before the
dispatch lock is taken; a chunk larger than that buffer is not a slow upload, it
is a request the transport refused before any handler existed to see it. That is
why a 5 MB photo cannot cost the heap anything at any chunk size: the bound is a
stack buffer.

It also means the number moves for free. Raising `EOS_HTTPD_BODY_MAX` — and
`cfg.stack_size` in `eos_httpd_start()` with it, which its header already says —
raises the upload chunk with no change here.

512 is not fast: the largest thing this board can hold is the 960 KB partition,
and the app itself is 50 KB gzipped, or 100 chunks. What it is, is free. The
web app's own concurrency cap is two requests, so 100 chunks is under a second
on the house WiFi.

| Payload | Chunks |
|---|---|
| `buddy.json` | 1–2 |
| a 24-voxel `buddy.vox` | 3 |
| the biggest model this board stages, 6,144 B | 12 |
| the web app, gzipped, all five files | ~100 |

## Path checking, twice

Every path arrives as a query parameter, percent-decoded exactly once by
`eos_httpd_query_get()`, which already refuses a decoded NUL. `path_check()`
then refuses, before anything is opened:

| Rule | Refused as |
|---|---|
| does not start with `/` | `bad_argument` |
| a `..` **component**, anywhere | `bad_argument` |
| a backslash | `bad_argument` |
| a byte below 0x20, or 0x7F | `bad_argument` |
| 96 bytes or longer | `too_big`, never truncated |
| a component 40 bytes or longer | `too_big` |

`eos_storage`'s `path_split()` refuses all six again. That is the point, and it
is also the problem with testing it: **deleting every rule here changes no
endpoint's answer**, because the layer below still says no. So the suite drives
`path_check()` directly through a host-only hook, and the three mutants that
matter (`..`, backslash, control bytes) fail 10, 2 and 2 checks. Through the
endpoints alone all three survive.

The `..` check is per component and never `strstr`. `...bb...` contains `..` and
is a legal filename; a filter that gets that backwards blocks real names *and*
still lets the real escape through.

## The console

`/api/console/log` is a ring of two pools — `EOS_APPS_LOG_LINES` line records
and `EOS_APPS_LOG_BYTES` of text — because a boot log is forty short lines and a
stack trace is four long ones, and they run out at different rates. A line never
wraps the byte pool: a write that would not fit before the end restarts at zero
and evicts whatever it lands on, so every line is one contiguous run the JSON
writer takes in one call. Reassembling a wrapped line would need a copy buffer
this file does not have room for.

`eos_apps_log_install()` hooks `esp_log_set_vprintf`, so the Console tab shows
the whole boot and not only what was typed into it. The hook strips the ANSI
colour, the level letter and the timestamp, keeps `tag: message`, and forwards
the original line to the UART unchanged. The formatted line lives on the calling
task's stack, not in a shared buffer: it is called from every task in the image.

### The command table is closed, and that is the feature

`/api/console/exec` runs **seven words and takes no arguments**:
`help status heap reboot theme wifi brain`. Six are read-only; `reboot` does no
more than pulling the USB cable does, and it is armed for the OS loop rather
than performed in the handler, so the 202 gets out first.

The endpoint is reachable, unauthenticated, from any page open on any phone on
the same WiFi as a board holding that WiFi's password in NVS. There is no login
and there cannot usefully be one on a device provisioned by pointing a camera at
a QR code. So:

| Absent | Because |
|---|---|
| any form of eval | it is a shell, and a shell here is a credential dump one guest away |
| any read or write of an address | the same, with fewer steps |
| file operations | `/api/fs/*` is that, and it is where the path rules are written |
| anything that changes a setting | `POST /api/settings` validates once, in the component that owns the keys |

Matching is `strcmp` with no trimming and no argument splitting — `help ` is
refused. A table that forgives whitespace has started parsing, and the next
forgiving step is the one that lets something through. An unknown command is a
400 naming the whole table **and** a console line, so the person watching the
pane sees why.

## The buddy

`buddy.json` and `buddy.vox` on `/int/buddy`, written by the editor through the
ordinary chunked `/api/fs/write`. There is no upload endpoint for the model and
there should not be: one upload mechanism, one set of failure modes.

`/api/buddy/reload` hands the file to `eos_vox_parse()` and reports what it
said. Nothing is pre-validated here — that parser was fuzzed over eight thousand
mutated files and checks every offset against what remains in the buffer, so a
second opinion in the endpoint could only be wrong.

**A failed parse leaves the board with no model.** The parse fills the one voxel
pool this board has, so the previous model does not survive it; the endpoint
reports that honestly rather than describing a model that is now half of two. A
second pool to make a bad upload survivable is 5,120 bytes for a case the editor
already prevents.

`buddy.json` is re-emitted from what the board parsed, not echoed. Splicing
unvalidated file bytes into a response is how the phone gets a document it
cannot parse, and `model.*` is advisory in the spec anyway — what `/api/buddy`
returns is the model the board is actually holding.

Reading it needed one thing `eos_httpd`'s JSON reader does not do: `idle` and
`eyes` are nested, and that reader steps over nested objects by design. So
`obj_span()` finds a named object's extent and points the same hardened reader
at it. It is not a second JSON parser, which would be the one with the bug.

| `buddy.json` | maps to |
|---|---|
| `name` | reported, and available to whatever writes the status bar |
| `personality` | truncated to `EOS_APPS_BUDDY_PERSONA_MAX` on a UTF-8 boundary |
| `accent` | `#rrggbb` → `0x00rrggbb`; anything else is absent, not an error |
| `idle.home_yaw` | `cfg.home_yaw`, modulo `EOS_BUDDY_YAW_STEPS` |
| `idle.sleep_ms` | `cfg.idle_sleep_ms` |
| `idle.behaviour` | stored and reported; `sleepy` also halves `idle_sleep_ms` |
| `eyes.open_index` | `cfg.eye_ci` |
| `eyes.shut_index` | `cfg.eye_shut_ci` |

An unrecognised `idle.behaviour` falls back to `wander`, and a `buddy.json` that
is not JSON at all still leaves the `.vox` loaded with compiled-in defaults —
`web/README.md`'s rule for a schema the firmware does not know.

### The caps are this board's, not the format's

`EOS_VOX_MAX_VOXELS` is 4096. This board stages **1024**, because a 4096-voxel
pool is 20,480 bytes of `.bss` and the buffer for the file that fills it is
another 21 KB, on a board with 173 KB of heap. `/api/buddy` reports the cap in
`limits`, so the editor can warn before somebody spends an evening on a model
the board will refuse.

### Reaching the running system

A reload arrives on an HTTP worker; the state machine is ticked on the OS loop.
`eos_apps_buddy_generation()` increments on every success, and `main.c` compares
it and calls `eos_buddy_init()` from **its own** task. That is the whole
synchronisation and it is enough: one writer, and the pointer never moves.

## Static files: prefer the file, never serve nothing

`eos_apps_bind_files()` wraps whatever was bound before it. A real file on
storage wins; the copy linked into the image answers when there is not one. That
is `web/README.md`'s rule and it is what turns deploying the app onto `/int`
into a copy rather than a rebuild — `/int/web/app.js.gz` is served with
`Content-Encoding: gzip` the moment it exists, and the board is never left with
nothing in between.

`/api/fs/read` uses the same three ports with the **fallback switched off**. A
GET for a file that is not there must be a 404 and not the contents of a
same-named asset in the image.

The bind is idempotent, and it has to be: called twice, the naive version
captures its own ports as the fallback and the first miss recurses until the
5,376-byte worker stack is gone.

## Memory

Measured by building the tree twice with the same sdkconfig and toolchain, once
with this file and its wiring and once without, and diffing `penguinos.bin` and
`idf.py size`.

| | Flash | Static RAM |
|---|---|---|
| `eos_apps.c` | 8,714 B | 17,824 B `.bss` |
| the boot glue's ports, catalog and loop block | — | 72 B |
| `.rodata` and the newlib/`esp_log` paths the linker now keeps | 7,558 B | — |
| **total image delta** | **+16,272 B** | **+17,896 B** |

1,633,360 bytes of a 3 MB partition, up from 1,617,088 — 48% of the slot still
free. The static RAM is the number that matters: free DIRAM at link falls from
236,748 to 218,852.

| `.bss` | Bytes | What |
|---|---|---|
| `s_voxbuf` | 6,144 | the `.vox` staged whole; `eos_vox_parse()` does not seek |
| `s_bud` | 6,096 | the 1024-voxel pool, the 768-byte palette, the config and the strings |
| `s_lbuf` | 3,072 | the console ring's text |
| `s_lline` | 1,152 | 96 line records |
| `s_budjson` | 768 | `buddy.json` staged whole |
| `s_scr` | 288 | the request path, rename's second one, and a listing entry joined onto its directory — see below |
| `s_below`, `s_up`, `s_fslot`, the log mutex, the rest | 304 | |

**The scratch is static and not stack, deliberately.** The HTTP worker has
5,376 bytes of which 513 are already the request body, and three 96-byte paths
per frame is how four workers overflow one. It is safe because `eos_httpd` serialises dispatch behind one
mutex: there is exactly one request in flight, image-wide, at any instant. That
same fact is why one upload handle, one log ring and one buddy is the right
number of each.

**12,240 of the 17,896 is the buddy**, for a model nothing draws yet. If the
owner wants it back before there is a buddy window, it is one line:
`-DEOS_APPS_VOX_VOXELS=256 -DEOS_APPS_VOX_BYTES=2176` reclaims 7,808 bytes and
still holds a 256-voxel model. Nothing else here is over 3 KB. The `#error` in
the header refuses a pairing that could not hold a full `XYZI` plus `RGBA`, so
the two numbers cannot drift apart into a buffer overrun.

Runtime heap: **nothing in this file allocates.** What a request costs is
`eos_storage`'s, already accounted for in its own README — 648 B per open file
and 324 B per open directory scan, bounded by the pools at 4 and 2. The dispatch
mutex means one scan at a time, so a listing costs 324 B for its duration and an
upload holds 648 B between chunks.

## Host build

```sh
cc -std=c99 -Wall -Wextra -Werror -O1 \
   -Ikernel/hal/include -Ikernel/svc/include -Ikernel/wm/include \
   -Ikernel/avatar/include -Iboards/generated \
   kernel/svc/eos_apps.c kernel/svc/eos_httpd.c \
   kernel/hal/backend/storage/eos_storage_idf.c \
   kernel/avatar/eos_vox.c kernel/avatar/eos_buddy.c \
   kernel/svc/test/test_apps.c -o /tmp/tapps && /tmp/tapps
```

368 checks, 0 failed. Also clean under
`-fsanitize=address,undefined -Wshadow -Wconversion`, which is how it is meant
to be run, in 0.35 s, and at `-O0` through `-O3` and `-Os`. The suite builds a
sandbox under
`/tmp/eos-apps-test-<pid>`, points `EOS_STORAGE_HOST_ROOT` at it and removes it
on the way out.

It is not a unit test. It drives `eos_httpd_dispatch()` with real request lines
against the real storage backend and reads the JSON that comes back, because the
question worth answering is never "does `path_check()` reject dot dot" but "does
every one of the nine routes call it first", and only the second can be seen
from outside.

| Section | Covers |
|---|---|
| `t_routing` | the fourteen rows, the method table, and the 501 an image without this file answers |
| `t_traversal` | 22 hostile paths × 9 routes = 198 requests, none accepted, none staged as a file |
| `t_path_check_direct` | the same corpus at `path_check()` itself, plus the legal names it must not block |
| `t_bounds` | every network-supplied number: offset, count, since, max, final, and the 95/96-byte path boundary |
| `t_list` | 31 entries, seven pages at `count=5`, mtime, the root, and the three ways to get an error |
| `t_upload` | the whole state machine, both out-of-order directions, restart-at-zero, a zero-byte file |
| `t_abort` | abort by path, abort with no path, abort of nothing, and the partial file left behind |
| `t_upload_timeout` | an upload that is never finished, one millisecond either side of the 30 s |
| `t_mutations` | mkdir/remove/rename, and that none of them can pull a file out from under an open upload |
| `t_console_log` | the cursor, `dropped`, the byte budget, a flood that wraps both pools |
| `t_console_exec` | all seven, 19 refusals, four malformed bodies, and a `cmd` nested one level down |
| `t_buddy` | a real `.vox` built byte for byte, the nested config, four bad models, the editor's round trip |
| `t_static_fallback` | the file wins, the image answers when it does not, `fs/read` never falls back |
| `t_fuzz` | 60,000 assembled query strings across ten routes |

It also passes with the tunables overridden, which is what makes the "reclaim
the RAM" line above a supported change rather than a suggestion:
`EOS_APPS_VOX_VOXELS`/`_VOX_BYTES` at 256/2176, the whole log ring at 8 lines
and 512 bytes, `EOS_APPS_LIST_MAX` at 4, `EOS_APPS_CATALOG_MAX` at 2,
`EOS_APPS_UPLOAD_IDLE_MS` at 1 s, and `EOS_HTTPD_BODY_MAX`/`_RESP_MAX` at
2048/8192. It is **not** written against a `EOS_HTTPD_BODY_MAX` below 512: the
upload section writes 100-byte and 200-byte chunks by name, and below that the
transport refuses them, which is correct behaviour and a failing assertion.

### The fuzz

Same shape as `eos_storage`'s, and biased on purpose: three paths in four start
at a real mount, or nearly every one dies at "no such mount" and the half that
could escape is never reached. The bias is asserted, not assumed:

```
[fuzz] accepted=8203 mounts=45201 deep=35995 climbs=22172 files=96
```

Those five are checks. A change that made every request fail early would report
zero escapes and be worthless. The invariant is checked on the **input** — a
request carrying a `..` component never comes back 2xx — because checking only
the output would miss the wrong fix, folding `a/../b` into `b`, whose output
looks perfectly clean.

### Mutants

Every one run against the suite. The four that survive are listed too, because
which mutants live is more informative than which die.

| Mutant | Checks failed |
|---|---|
| fold `..` away instead of refusing it | 10 |
| refuse `..` only at the front of a path | 10 |
| `strstr("..")` instead of a per-component check | 5 |
| sync on every chunk instead of the last | 22 |
| drop the 30 s upload timeout | 8 |
| silently switch files instead of `busy` | 6 |
| let a wrong offset write anyway | 4 |
| prefix-match the command table | 3 |
| drop the backslash rule | 2 |
| drop the control-byte rule | 2 |
| drop the `body_truncated` check | 2 |
| `remove` allowed on a file an upload has open | 2 |
| log ring: evict on a full table but not on a byte overlap | 1 |
| truncate a personality mid-UTF-8-character | 1 |
| log ring: never reset the write position at the end of the pool | ASan: a hard overflow |
| `eos_apps_bind_files()` not idempotent | stack exhausted, no output |
| **`obj_span` accepts a non-object value** | **0 — equivalent** |
| **`/api/fs/read` falls back to the embedded copy** | **0 — equivalent** |

The last two are genuinely unreachable, not gaps. `obj_span`'s object check is
redundant because the leaf reader independently refuses anything that is not an
object; it is kept so the function means what its name says. And `fs/read`
`stat`s the path before it opens it, so a missing file is a 404 two independent
ways — the `false` is the second reason, and it is the one that would still hold
if the `stat` were ever dropped for the size.

## What is not here

| Missing | Why |
|---|---|
| `Range` and `Content-Length` on `/api/fs/read` | the response is chunked from flash, which is what streaming costs; the web app sends no `Range` |
| a recursive `remove` | `web/README.md` says a non-empty directory is `exists`, and the page never asks the board to walk a tree |
| cross-mount `rename` | `eos_storage_rename()` is same-mount; `/sd` answers `no_such_device` first anyway |
| a buddy window | `/api/apps` reports the four windows that exist and does not invent a fifth |
| `EOS_APPS_IDLE_*` drift behaviour | the preset is stored and reported; nothing on this board consumes yaw drift yet |
| sizes past 2^31 in `/api/fs/usage` | `long` is 32 bits here and the JSON writer takes a `long`; clamped, and `/int` is 960 KB |
| any authentication | same boundary as the rest of the API: the network is the boundary |
