# kernel/svc — services

Kernel services that are not the window manager: `eos_brain`, the MEGABRAIN
client; `eos_net`, the credential state machine and the SoftAP; `eos_ble`, the
BLE HID host a keyboard arrives through; `eos_radio`, the one lock the two
radios share; and `eos_httpd`, the provisioning API.

| File | What |
|---|---|
| `include/eos_brain.h` | MEGABRAIN client: streaming parser, request API, transport interface |
| `eos_brain.c` | Implementation, plus the ESP-IDF socket/mDNS/NVS bindings |
| `test/test_brain.c` | Host test, 168 checks, no networking |
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
| `test/test_httpd.c` | Host test, 2009 checks, no sockets |

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
```

The BLE suite links `eos_wm` and `eos_keys` because its last section is not a
unit test: it pushes a synthesised `super+return` report through the input HAL
into the real keybind table against a real `eos_wm_t` and checks that a window
opened. That is the whole chain from the wire to the window manager, running on
a laptop with no radio in the room.

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

48 bytes in NVS (`eos_ble` / `bond`). This is ESP-OS's memory of WHICH device it
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

## Test

2009 checks, no sockets, no radio, no IDF. The radios reach the handlers through
a port table and the suite fills it with a scripted fake, so the nine handlers
under test are the production ones.

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
| `/api/system`, `/api/fs/*`, `/api/settings` | the rest of `web/README.md`, and not this run's scope |
| a per-request auth token | the SoftAP is WPA2 and its password is on the panel; that is the boundary |
| concurrent API requests | one server, one set of buffers, one mutex — four reloads cost no heap |
