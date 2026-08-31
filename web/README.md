# web — the penguinOS companion app

Served by the board, launched at startup, reached at `http://<boardname>.local`.
Two pages out of one bundle: the four-tab app — **Files**, **Settings**,
**Buddy**, **Console** — and the setup screen a phone lands on after joining a
new board's own access point. Vanilla HTML, CSS and JS — no framework, no CDN,
no build step, no webfont. Nothing outside this directory is ever fetched.

| File | Lines | Raw | Gzip | What |
|---|---|---|---|---|
| `index.html` | 448 | 19,252 | 5,102 | Structure and the inline SVG icon symbols |
| `style.css` | 735 | 25,454 | 6,050 | Palette as custom properties, mobile-first layout |
| `app.js` | 1,802 | 59,835 | 18,530 | API client, all four tabs |
| `setup.js` | 1,252 | 44,354 | 13,808 | The setup screen: WiFi, BLE pairing, radio etiquette |
| `voxel-editor.js` | 1,151 | 42,438 | 13,822 | The buddy editor and the `.vox` codec |
| **total served** | **5,388** | **191,333** | **57,312** | 30% of raw |

Setup added 62,332 raw and 18,183 gzipped across four files. Most of that is
`setup.js`, and most of `setup.js` is English: nine ways a join can fail, six
ways a bond can, and what to do about each. That prose is the feature.

`README.md` is documentation and is not served.

`preview.html` used to live here and no longer does — it moved to
`design/preview.html`, because it pulls webfonts from `fonts.googleapis.com`
that an offline board cannot reach, and a directory that gets flashed is the
wrong place to keep something that large. It is now regenerated from the live
kernel by `design/build_preview.py`, so it cannot go stale either. See
`design/README.md`.

Ship only the five files in the table above; anything that globs `web/*` into a
flash image is wrong.

**This document is the contract and the firmware now implements all of it.** It
was written first and against nothing; for one release the board served the five
files and answered eight of the thirty-one endpoints, so the Files tab was
empty, Settings saved nothing and Megabrain did nothing. That gap is closed —
see *What the board answers* below for the handful of places where the board's
real behaviour is narrower than what this document allows, and
`firmware/README.md`'s *The API* for which file owns which route.

## Serving it

The five files live on the filesystem, gzipped, and are served with
`Content-Encoding: gzip`. Nothing on the board ever compresses at runtime.

```
/sd/web/index.html.gz          # or /int/web/... on a board with no card
/sd/web/style.css.gz
/sd/web/app.js.gz
/sd/web/setup.js.gz
/sd/web/voxel-editor.js.gz
```

**What the C6 actually does today.** It has no card, `/int` is a mounted but
empty 960 KB LittleFS, and nothing deploys the five files onto it yet — so all
five are linked into the image with `EMBED_FILES` and served from flash. The
rule above is still the rule: `eos_apps_bind_files()` wraps the embedded ports
and prefers a real file the moment one exists, and the fallback means the board
is never left with nothing to serve. See *Serving the web app* in
`firmware/README.md`.

```bash
# deploy: gzip onto the card, keeping the original names plus .gz
for f in index.html style.css app.js setup.js voxel-editor.js; do
    gzip -9 -c "$f" > "/Volumes/ESPOS/web/$f.gz"
done
```

Serving rules:

| Request | Serve | Content-Type |
|---|---|---|
| `/` | `/sd/web/index.html.gz` | `text/html; charset=utf-8` |
| `/style.css` | `/sd/web/style.css.gz` | `text/css; charset=utf-8` |
| `/app.js`, `/setup.js`, `/voxel-editor.js` | matching `.gz` | `application/javascript; charset=utf-8` |
| anything else not under `/api/` | 404 | — |

**The captive portal must serve all five.** A board in SETUP is the case where
this matters most and the case where the file set is easiest to get wrong: the
setup page is `index.html`, the same one, and it will not work without
`setup.js`. `setup.js` loads before `app.js`, because `app.js` reads
`window.EOS_SETUP` at boot to decide which page this is.

`voxel-editor.js` is 10,426 gzipped bytes that a board in SETUP never uses, and
loading it lazily on the first visit to the Buddy tab would take that off the
captive-portal path. It is not done here: it makes `buddyInit()` asynchronous,
and 10 KB over an access point three feet away is a few milliseconds. The
number is recorded so the trade is a decision rather than an oversight.

Always send `Content-Encoding: gzip` for these. The page asks for no other
static asset: the favicon is a `data:` URI precisely so the board is never
asked for `/favicon.ico`, and the icons are inline SVG `<symbol>`s.

## Design constraints that shaped the contract

These are the reasons the API looks the way it does. Changing them changes the
firmware's memory profile, so they are recorded here rather than rediscovered.

- **Uploads are chunked and the board sets the chunk size.** `/api/system`
  reports `limits.chunk_max`; the client never sends a body larger than that.
  A request bigger than the receive buffer is not a slow upload, it is a failed
  allocation. There is no multipart parsing anywhere — bodies are raw bytes and
  every parameter is in the query string, so the board can route a request
  without buffering its body.
- **The client holds itself to two concurrent requests.** A browser will open
  six connections; an ESP32 HTTP server has roughly four workers. The queue is
  in `app.js` because that is where it is free.
- **Settings keys are flat, dotted, and at most 15 bytes.** They land in NVS and
  an NVS key is 15 usable bytes. A nested settings document would have to be
  parsed and re-serialised whole on a 20KB heap; a flat key/value patch does not.
- **Polling, not SSE or websockets.** Both hold a socket open for the whole
  session, and the board has few. The console log and the system stats are
  polled with exponential backoff instead. The one streaming response is
  `/api/brain/ask`, which is a proxy of a stream the board is already consuming.
- **Every response is small enough to build in one pass.** Directory listings
  page; nothing returns an unbounded array.

## Conventions

- Base path `/api/`. Everything returns `application/json; charset=utf-8` except
  `/api/fs/read` (raw bytes) and `/api/brain/ask` (streamed text).
- Paths are `eos_storage` paths — `/sd/...` or `/int/...` — passed as a
  **query parameter**, percent-encoded. `EOS_PATH_MAX` is 96, so a path over 95
  bytes is `too_big` (413), never truncated: a truncated path names a different
  file. The status is 413 and not 400 — this sentence used to say 400 and the
  table below said 413, which is the sort of disagreement that is only ever
  found by implementing both. `eos_httpd_err_status()` is the one authority and
  it says 413.
- Booleans are JSON booleans. Sizes and offsets are numbers, in bytes.
  Durations are milliseconds. Times are unix seconds.
- `POST` is used for every mutation, including ones that would be idiomatic as
  `DELETE`, `PUT` or `PATCH`, because a small HTTP server's method table is one
  more thing to get wrong and the browser needs no preflight for POST. There
  are no exceptions: the board answers `GET` and `POST` and nothing else.
  `POST /api/settings` still carries only the changed keys — a partial update
  is a property of the body, not of the verb.

### Error model

Failures return the HTTP status below plus
`{"error": "<code>", "detail": "<free text>"}`. Codes map one-to-one onto
`eos_err_t` so the firmware can translate mechanically.

| `error` | `eos_err_t` | Status | Means |
|---|---|---|---|
| `bad_argument` | `EOS_ERR_ARG` | 400 | Malformed or missing parameter |
| `too_big` | `EOS_ERR_TOOBIG` | 413 | Body over `chunk_max`, or path over 95 bytes |
| `readonly` | `EOS_ERR_READONLY` | 403 | Mount is not writable |
| `not_found` | `EOS_ERR_NOTFOUND` | 404 | No such path, mount, theme or app |
| `exists` | `EOS_ERR_EXISTS` | 409 | Target exists, or directory not empty |
| `busy` | `EOS_ERR_BUSY` | 409 | Another upload or brain request holds the resource |
| `state` | `EOS_ERR_STATE` | 409 | Called out of order — see the upload state machine |
| `pool_exhausted` | `EOS_ERR_POOL` | 503 | `EOS_MAX_FILES` / `EOS_MAX_DIRS` exhausted |
| `no_such_device` | `EOS_ERR_NODEV` | 503 | The card is not mounted, or was pulled mid-call |
| `io_error` | `EOS_ERR_IO` | 500 | Media or bus failure |
| `unsupported` | `EOS_ERR_UNSUPPORTED` | 501 | Valid call, not available on this tier |

`no_such_device` is expected during normal operation, not a bug: `/sd` is
removable and can vanish between any two requests.

---

## Setup mode

A board that has never joined a network cannot serve this app over one. It
comes up in **SETUP** instead: its own WPA2 access point, a captive portal at
`192.168.4.1`, and the AP name and password printed on the panel — the only
out-of-band channel these boards have. `docs/provisioning.md` is the
specification. This section is only the part the web app consumes.

`setup.js` owns that screen and every endpoint below. `app.js` knows exactly
one thing about it, `window.EOS_SETUP`, and nothing about `/api/wifi/*`,
`/api/ble/*` or `/api/net/status`.

Setup replaces the page rather than adding a fifth tab. A board in SETUP has no
network, so Files, Console and Buddy have nothing to show. The same screen is
reachable in run mode from **Settings → Network and input**, because pairing a
keyboard needs the same radio etiquette and the same passkey panel, and because
that is the only place a board on the wrong network can be moved from.

### Which page is this

`app.js` calls `EOS_SETUP.probe()` before it boots anything. That is one
`GET /api/net/status` and it is the only extra request run mode pays.

| `mode` | The page becomes |
|---|---|
| `setup`, `ap`, `apsta`, `portal`, `provision`, `provisioning`, `connecting` | the setup screen |
| `run`, any other value, or no reply at all | the four-tab app |

`#setup` in the URL forces the setup screen without asking, which is how it is
driven against a running board.

While setup holds the page the app's own *board not answering* bar is muted.
Setup knocks the board off the air on purpose — a scan retunes the radio, a
join takes it away — so that bar would be both true and useless. Setup says
something specific in its place.

### Endpoints

Every one of these is in the table in `docs/provisioning.md`. The columns below
are what the page actually reads, and what it does when a field is missing.

| Method | Path | Sent | Read | Missing |
|---|---|---|---|---|
| GET | `/api/net/status` | — | `mode`, `ip`, `ssid`, `rssi`, `hostname`, `mdns`, `ap.ssid`, `join.*` | no reply is treated as run mode |
| GET | `/api/wifi/scan` | — | `networks[]`: `ssid`, `rssi`, `auth`, `channel`, `hidden` | inline failure, manual entry still works |
| POST | `/api/wifi/connect` | `{ssid, psk}` | `ok`, `state`, `reason`, `detail`, `ip` | falls back to polling `/api/net/status` |
| POST | `/api/wifi/forget` | — | ignored beyond the status code | toast on failure |
| GET | `/api/ble/scan` | — | `devices[]`: `addr`, `name`, `rssi`, `bonded`; `scanning` | inline failure |
| POST | `/api/ble/pair` | `{addr}` | ignored; the outcome comes from `/api/ble/status` | polls anyway |
| GET | `/api/ble/status` | — | `state`, `passkey`, `bonded.{addr,name}`, `connected`, `battery`, `reason` | polls until the pairing budget runs out |
| POST | `/api/ble/forget` | — | status code only | inline failure |
| GET | `/api/system` | — | `board`, `chip`, `display`, `fw`, `heap`, `net.hostname` | step 1 says so and setup continues |

`/api/system` is the only endpoint shared with the rest of the app. Setup calls
it once, for identity, and never polls it.

Alternate spellings are accepted for every list and field the board might name
differently — `networks`/`aps`/`results`, `devices`/`peripherals`,
`addr`/`address`/`mac`, `hostname`/`host`, `auth`/`authmode`/`security`. A bare
array is accepted where an object was expected. This is one `pick()` call per
field and it costs nothing; getting it wrong costs a firmware round trip.

`auth` may be the `wifi_auth_mode_t` **number** or a string. Both are rendered.
Enterprise (5, 10, or anything matching `ent`/`802.1x`/`eap`) is listed but not
selectable, because the board cannot join it and finding that out after a
fifteen-second join is worse than being told.

### The three rules this page is built around

**Credentials are persisted only after a join succeeds.** That is the board's
rule, not the page's, but the page is written so nothing depends on breaking
it: the success screen says so out loud, and every failure screen ends with
"nothing was saved, so the board is still on its own access point". If the
board ever saves first, that copy becomes a lie and the bug becomes visible.

**One radio.** WiFi and BLE share it on the C6, and the access point serving
this page is running on it. Every operation that touches the radio takes a
client-side mutex; while it is held, the other radio user's buttons are
disabled rather than left live and silently inert. A `busy` (409) from the
board is retried, not reported as a failure.

**Scan once, rescan on purpose.** The WiFi scan runs when setup opens, while
the person is still reading step 1, which is when the disruption costs least.
After that it is the Rescan button, and the text next to it says the page may
stall for a few seconds and will recover.

### Surviving the radio moving

A `GET` that dies mid-flight is the normal case here, not an error. Anything
that is not a definite 4xx is retried up to five times with backoff, and each
retry writes `the link dropped while the radio moved — retrying (n of 4)` on
the line the person is already watching. A page that goes quiet for eight
seconds reads as broken.

The join is worse: the request that starts it is the request most likely to
die, because joining takes the radio away from the access point serving it. So
the response is never the only record of the outcome.

| What the POST does | The page then |
|---|---|
| answers `{"ok":true,...}` or carries an `ip` | reads `/api/net/status` once for the real address, then shows it |
| answers `202`, or `{"state":"trying"}` | polls `/api/net/status` for 45 s |
| answers `{"ok":false,"reason":...}` | shows that reason |
| times out, or the socket dies | polls `/api/net/status` for 45 s |

Polling ends in one of three places. `join.state` of `ok`/`failed` is taken as
given. With no `join` block at all, an `ip` that is not `0.0.0.0`, a matching
`ssid` and a `mode` that is no longer SETUP is read as a join — a board that
reports only what `docs/provisioning.md` lists still works.

The third place is the one that matters: if the board simply stops answering
and never comes back, the page does **not** say "failed". It says the board
went quiet, that this most often means it joined and shut the access point
down, gives the address to look for, and says a power cycle brings it straight
back here with nothing saved. That is the likeliest real outcome of a
successful first-boot join, and calling it a failure would be a lie.

### Why a join failed

"Failed" on its own is useless: wrong password, wrong name and too far away
need three different things from the person. `reason` is accepted as a string
or as a `wifi_err_reason_t` number, and both fold into the same table.

| Key | `wifi_err_reason_t` | What the page says to do |
|---|---|---|
| `bad_auth` | 2, 3, 15, 202, 204 | password refused; it is case sensitive, watch for phone autocapitalisation |
| `no_ap` | 201 | nothing by that name answered; check spelling, or the board is out of range even if the phone is not |
| `assoc_fail` | 4, 6, 7, 8, 203, 205 | heard but refused; distance, a MAC filter, or a 5 GHz-only network |
| `ap_full` | 5 | the router will not take another device |
| `weak` | 200 | in range at scan, gone during the join; move it closer |
| `enterprise` | 23 | 802.1X needs a username; this board cannot |
| `ip_fail` | — | joined, but DHCP never issued an address |
| `timeout` | — | did not finish in the board's budget |
| anything else | — | the raw code and detail, plus "nothing was saved" |

Unrecognised strings are matched loosely (`/wrong.?pass|4way|handshake/` and so
on) before falling through, so a firmware that spells them differently still
gets the useful sentence.

### Pairing a keyboard

The board is the BLE HID host, `IO_DISPLAY_ONLY`, so the six-digit passkey is
generated on the board and typed **on the keyboard**, which has no screen. It
appears on the panel and here, large enough to read across a room, on one line
— a passkey that wraps reads as two numbers.

The warning that a keyboard bonds to one host at a time is a full-width block
in the flow, between choosing a device and pairing it, with two large buttons.
`docs/provisioning.md` is explicit that this must be at the moment of pairing
and not in a footnote, so it is not a `<dialog>`: the captive portal webview on
older iOS may not have one, and this is the critical path.

Failure reasons are the five ways `claude_term024`'s `kbTryConnect()` can
actually fail, plus timeout: `not_found`, `connect_fail`, `bond_fail`,
`no_hid`, `no_reports`, `timeout`. Unrecognised codes are matched loosely the
same way the WiFi ones are.

### One-handed on a phone

Every tap target is at least 46 px tall and most rows are 56. Nothing is behind
a hover. Password and SSID inputs are 16 px, because iOS zooms the viewport on
any smaller field and the zoom does not come back. `autocapitalize`,
`autocorrect` and `spellcheck` are off on both — a capitalised first letter is
the most common cause of a "wrong password" that was typed correctly.
Password visibility is a labelled Show/Hide button, not an icon.

The "still joining" state is an indeterminate bar **and** a live seconds count.
The bar alone can look like a frozen page; a number that keeps going up cannot.
Under `prefers-reduced-motion` the bar stops outright rather than inheriting
the sheet's blanket 1 ms animation, which would turn an infinite sweep into a
strobe.

## System

| Method | Path | Params | Response |
|---|---|---|---|
| GET | `/api/system` | — | Full board description, below |
| GET | `/api/system/health` | — | `{"ok":true,"uptime_ms":N,"heap_free":N}` |
| POST | `/api/system/reboot` | — | `{"ok":true,"in_ms":500}` then reboot |

`/api/system/health` exists as a cheap liveness probe; the web app does not
call it. `/api/system` is polled every 5s while the page is visible.

```json
{
  "board":  {"id":"cyd-2432s024n","name":"ESP32-2432S024N (CYD, N variant)",
             "summary":"2.4in ILI9341, no touch"},
  "chip":   {"target":"esp32","variant":"ESP32-D0WD-V3","cores":2,"rev":3,
             "flash_mb":4,
             "psram":{"present":false,"type":"none","size_mb":0},
             "mac":"a0:b7:65:1c:2d:8e"},
  "render": {"tier":0,"compositor":"indexed8","lvgl":false},
  "display":{"controller":"ILI9341","w":320,"h":240,"rotation":1,
             "bus":"hspi","clock_hz":40000000,"backlight":true},
  "heap":   {"free":21344,"min_free":18120,"largest_block":10240,"total":295000},
  "fs":     [{"point":"/sd","fs":"fat","mounted":true,"writable":true,
              "removable":true,"total":7948206080,"used":124035}],
  "net":    {"ip":"192.168.0.51","hostname":"penguinos","mdns":"penguinos.local",
             "ssid":"WavvyWorld","rssi":-58,"up":true},
  "uptime_ms": 128394,
  "time":   {"epoch":1756500000,"tz":"PST8PDT,M3.2.0,M11.1.0","synced":true},
  "fw":     {"version":"0.1.0","idf":"v5.3.1","built":"2026-08-29T20:00:00Z"},
  "limits": {"chunk_max":4096,"path_max":96,"name_max":40,
             "list_max":128,"open_files":4}
}
```

`board`, `chip`, `render` and `display` mirror `boards/*.json` through
`eos_board_t`. `fs` mirrors `eos_storage_mounts()`. Every field is displayed
read-only in Settings; a missing group is skipped rather than shown blank, so
adding fields is safe and removing them is not fatal.

`limits.chunk_max` is the one field the client changes behaviour on. Everything
else is informational.

## Files

| Method | Path | Params | Response |
|---|---|---|---|
| GET | `/api/fs/list` | `path`, `offset`=0, `count`=`list_max` | Paged directory listing |
| GET | `/api/fs/stat` | `path` | `{"size":N,"mtime":N,"is_dir":bool}` |
| GET | `/api/fs/read` | `path` | Raw bytes, `application/octet-stream` |
| GET | `/api/fs/usage` | `point` | `{"point":"/sd","total":N,"used":N,"free":N}` |
| POST | `/api/fs/write` | `path`, `offset`, `final`=0\|1 | One upload chunk, raw body |
| POST | `/api/fs/upload/abort` | `path` | `{"aborted":bool}` |
| POST | `/api/fs/mkdir` | `path` | `{"ok":true}` |
| POST | `/api/fs/remove` | `path` | `{"ok":true}` |
| POST | `/api/fs/rename` | `from`, `to` | `{"ok":true}` |

`/api/fs/stat` is part of the contract but the web app does not call it; the
listing already carries size and type.

**`/api/fs/list`** returns

```json
{"path":"/sd","entries":[{"name":"buddy","size":0,"is_dir":true,"mtime":1756500000}],
 "offset":0,"total":37,"more":true}
```

`name` is the component only, never a full path — `EOS_NAME_MAX` is 40.
`more` is true when `offset + entries.length < total`; the client then requests
the next page at that offset. Listing `/` enumerates the mounts themselves as
directories, exactly as `eos_storage_opendir("/")` does, so the browser needs no
special case for the top level.

**`/api/fs/read`** must honour `Range` if it can; the web app does not send one,
but a resumable download is free if the handler already seeks. Send
`Content-Length` so the browser can show progress.

**`/api/fs/remove`** deletes files and *empty* directories, matching
`eos_storage_remove()`. A non-empty directory is `exists` (409), not a
recursive delete — the web app never asks the board to walk a tree.

**`/api/fs/rename`** is same-mount only, matching `eos_storage_rename()`.
Cross-mount is `unsupported` (501).

### The upload state machine

One upload at a time, board-wide. The board keeps a single open write handle
keyed by `path`.

| `offset` | `final` | Board does |
|---|---|---|
| `0` | `0` | Create/truncate `path`, keep the handle open. Any previous handle is discarded first. |
| `0` | `1` | Create/truncate, write, sync, close. A whole small file in one request. |
| `== handle position` | `0` | Append, keep open |
| `== handle position` | `1` | Append, **sync**, close |
| anything else | — | `state` (409), handle untouched |

Response on success:

```json
{"path":"/sd/buddy/buddy.vox","offset":4096,"size":4096,"final":false}
```

`offset` in the response is the position for the *next* chunk. Rules:

- A body larger than `limits.chunk_max` is `too_big` (413).
- A write to a different `path` while a handle is open is `busy` (409). It does
  not silently switch files.
- An idle handle is dropped after **30 s** and the partial file is left on the
  card. The client retries transport failures up to 3 times with backoff, then
  calls `/api/fs/upload/abort` so the next upload is not refused as busy.
  A 4xx is never retried — it means the request was wrong, and repeating it
  only costs the board time.
- Parent directories are **not** created implicitly. `mkdir` first;
  `eos_storage_mkdir()` is one level, so create parents in order.
- A zero-byte file is `offset=0&final=1` with an empty body.

## Settings

| Method | Path | Params | Response |
|---|---|---|---|
| GET | `/api/settings` | — | `{"settings":{...}}` |
| POST | `/api/settings` | JSON body: changed keys only | `{"settings":{...},"reboot_required":[...]}` |

The client sends only keys whose value it actually changed, and re-renders from
the `settings` object in the response, so the board is the single source of
truth. `reboot_required` lists the keys in *this* patch that will not take
effect until reboot; an empty array means everything applied live.

| Key | Type | Range / note | Live? |
|---|---|---|---|
| `wifi.ssid` | string | ≤ 32 | reboot |
| `wifi.psk` | string | ≤ 63, **write only** | reboot |
| `wifi.psk_set` | bool | read only, never the value itself | — |
| `net.host` | string | ≤ 24, also the mDNS name | reboot |
| `brain.host` | string | ≤ 47, host or IP, no scheme | live |
| `brain.port` | number | 1–65535 | live |
| `brain.model` | string | ≤ 31 | live |
| `brain.max` | number | 16–2048 max tokens | live |
| `brain.system` | string | ≤ 223, fits `EOS_BRAIN_SYSTEM_MAX` | live |
| `ui.theme` | string | a `name` from `/api/themes` | live |
| `ui.bright` | number | 0–255 backlight | live, applied on change |
| `sys.tz` | string | POSIX TZ, e.g. `PST8PDT,M3.2.0,M11.1.0` | live |
| `sys.autostart` | string | an `id` from `/api/apps`, or `""` | reboot-ish |

Every key is ≤ 15 bytes. **Do not add a longer one** — it will not fit an NVS
key and the failure appears at write time, not at compile time.

`wifi.psk` is write-only in both directions: `GET` never returns it, and the
board reports only `wifi.psk_set`. An empty string in the body means "leave it
alone", not "clear it"; the UI labels the field accordingly.

`brain.host` is a **host, not a URL**. `eos_brain_build_request()` builds
`/ask?stream=1&max=..&system=..&model=..&q=..` itself, so a scheme or a path
here would have to be parsed off on the board for no benefit.

## Themes

| Method | Path | Params | Response |
|---|---|---|---|
| GET | `/api/themes` | — | `{"active":"cyd-amber","themes":[...]}` |

```json
{"active":"cyd-amber",
 "themes":[{"name":"cyd-amber","path":"/sd/themes/cyd-amber.json",
            "colors":{"bg":"#262624", "...all 16 roles...":""},
            "metrics":{"gap":2,"border":1,"bar_h":12,"tab_h":11,"radius":0}}]}
```

`colors` carries all sixteen `eos_role_t` names, lowercased without the
`EOS_ROLE_` prefix, exactly as the theme file spells them. The web app writes
them straight into CSS custom properties of the same name with `_` → `-`, so
**the page wears whatever theme the OS is wearing**. `ansi` is not sent — the
web app has no terminal emulator and it would double the response.

Switching theme is `POST /api/settings {"ui.theme":"gruvbox"}`; the client
re-fetches `/api/themes` afterwards and restyles without a reload.

## Apps

| Method | Path | Params | Response |
|---|---|---|---|
| GET | `/api/apps` | — | `{"apps":[{"id":"chat","name":"chat","summary":"ask megabrain, and watch the reply arrive","tier_min":0}]}` |

Feeds the autostart picker. `tier_min` is the lowest `render.tier` the app runs
on; the picker shows everything and lets the board refuse.

**`id` is the identifier and `name` is the label.** They are two columns of one
table and a board is free to spell them differently, so a client must send `id`
back in `sys.autostart` and must never send `name`. `web/app.js` does this
already (`{ v: a.id, l: a.name || a.id }`), and as of this pass the firmware
resolves the stored value through the same column rather than through the tab
label it was matching before.

There is one table behind this on the board —
`firmware/main/eos_app_registry.c` — and `/api/apps`, the panel's `super+space`
app launcher, the tab labels and `sys.autostart` all read it. On the
C6-LCD-1.3 that is ten rows: `clock`, `board`, `heap`, `keys`, `buddy`, `chat`,
`settings`, `files`, `media`, `party`. `media` is the RGB LED and its `summary`
says so in its first four words: there is no speaker on that board.

## Buddy

| Method | Path | Params | Response |
|---|---|---|---|
| GET | `/api/buddy` | — | `{"buddy":{...},"state":"idle","error":null,"limits":{...},"dir":"/int/buddy"}` |
| POST | `/api/buddy/reload` | — | `{"ok":true,"state":"idle","buddy":{...}}` |

There is deliberately **no upload endpoint for the model**. The editor writes
`buddy.vox` and `buddy.json` into the reported `dir` through the ordinary
chunked `/api/fs/write`, then calls `/api/buddy/reload` to make the running OS
pick them up. One upload mechanism, one set of failure modes.

`dir` is `EOS_APPS_BUDDY_DIR`, which is `/int/buddy` on every image built
today. The editor does not hardcode it: it writes where the board says, and
falls back to `/int/buddy` then `/sd/buddy` only when the board answered 404
and therefore told it nothing.

`limits` is what this board can hold, not what the format allows —
`{"voxels":1536,"bytes":7264,"dim":32}` on esp32c6 against a format ceiling of
4096 and 17480. The editor installs them before it parses anything, so a model
the board would refuse is refused in the browser with those numbers in the
message. A board that 404s reports no limits, and the editor says so rather
than guessing; it re-asks after the first successful save.

`model.voxels` in the response is the count **after** `eos_vox_finish()` has
dropped the fully buried voxels, and the file's `XYZI` count is the number
before. Pip is 1,280 in the file and 572 on the panel. The editor shows both,
because one number on its own reads like half the upload went missing.

`state` is the `eos_buddy_state_t` name lowercased — `idle`, `thinking`,
`talking`, `listening`, `sleeping`, `happy`, `confused`.

`GET /api/buddy` returning `not_found` (404) is normal on a board with no
buddy at all, and the editor handles it: it says so and lets you build one.

### buddy.json

Written by the editor, read by the avatar component. Fields map onto
`eos_buddy_cfg_t`.

```json
{
  "schema_version": 1,
  "name": "pip",
  "personality": "terse, dry, helpful. never uses exclamation marks.",
  "accent": "#d88e56",
  "idle":  {"behaviour":"wander","sleep_ms":120000,"home_yaw":0},
  "eyes":  {"open_index":0,"shut_index":0},
  "model": {"file":"buddy.vox","dim":[16,16,16],"voxels":13}
}
```

| Field | Maps to | Note |
|---|---|---|
| `name` | — | ≤ 31 bytes, shown in the status bar |
| `personality` | prepended to `EOS_BRAIN_SYSTEM_TINY` | ≤ 480 bytes here; the board must truncate to `EOS_BRAIN_SYSTEM_MAX` |
| `accent` | `EOS_ROLE_ACCENT` override for the avatar | `#rrggbb` |
| `idle.home_yaw` | `cfg.home_yaw` | 0–31, `EOS_BUDDY_YAW_STEPS` |
| `idle.sleep_ms` | `cfg.idle_sleep_ms` | 0 disables sleeping |
| `idle.behaviour` | yaw drift preset, below | |
| `eyes.open_index` | `cfg.eye_ci` | palette index, 0 = no eyes |
| `eyes.shut_index` | `cfg.eye_shut_ci` | 0 = solid blink |
| `model.*` | — | advisory; the `.vox` file is authoritative |

`idle.behaviour` presets — the editor only writes the name, the board owns the
numbers:

| Value | Intent |
|---|---|
| `still` | holds `home_yaw`, blinks only |
| `wander` | slow continuous yaw drift around home |
| `curious` | occasional quick turns to a new yaw, then settles |
| `sleepy` | drifts, sleeps at half `sleep_ms`, wakes slowly |

Unknown values fall back to `wander`. Reading a `schema_version` the firmware
does not know should fall back to the compiled-in buddy rather than refuse to
boot — same rule as `eos_theme`.

### buddy.vox

A real MagicaVoxel file, restricted to the subset `eos_vox_parse()` reads:
`"VOX "` + version 150, one `MAIN`, then `SIZE`, `XYZI` and `RGBA` as its
direct children. Verified by compiling `kernel/avatar/eos_vox.c` and parsing a
file the editor produced — see *Verification*.

| Limit | Value | Source |
|---|---|---|
| Max dimension | 32 on any axis | `EOS_VOX_MAX_DIM` |
| Max voxels | 4096 | `EOS_VOX_MAX_VOXELS` |
| Palette index 0 | empty, never stored | `eos_vox_parse()` skips `ci == 0` |
| `RGBA` mapping | file entry `j` ↔ palette index `j+1` | MagicaVoxel spec |

The editor enforces both caps in the UI — the voxel counter turns red at 4096
and refuses further placement — so the board never has to reject a file the
owner has already spent an evening on.

## Console

| Method | Path | Params | Response |
|---|---|---|---|
| GET | `/api/console/log` | `since`=0, `max`=4096 | Ring-buffer slice |
| POST | `/api/console/exec` | JSON body `{"cmd":"..."}` | `202` `{"seq":N,"accepted":true}` |

```json
{"lines":[{"text":"heap free 21344","level":"I"}],"next":41,"dropped":0}
```

`since` is an opaque monotonically increasing line counter, **not** a byte
offset. The client sends back `next` from the previous reply. `dropped` is how
many lines fell out of the ring between `since` and the first line returned, so
the UI can say so rather than silently losing output. A `since` beyond `next`
clamps rather than errors — the board reboots and the counter restarts, and the
page must not wedge when it does.

`level` is one character: `E`, `W`, `I`, `D`, matching ESP-IDF log levels. The
UI colours `E` and `W`.

`exec` is fire-and-forget: it returns `202` immediately and the command's output
arrives through `/api/console/log` like everything else. A synchronous exec
would have to buffer the whole output on a 20KB heap.

## Megabrain

| Method | Path | Params | Response |
|---|---|---|---|
| GET | `/api/brain/status` | — | Host, model list, reachability |
| POST | `/api/brain/ask` | JSON body, below | **Streamed** `text/plain; charset=utf-8` |
| POST | `/api/brain/cancel` | — | `{"cancelled":bool}` |

```json
{"host":"192.168.0.139","port":80,"model":"qwen3.5:2b",
 "models":["qwen3.5:2b","gemma4:12b-it-qat","ornith:9b"],
 "reachable":true,"busy":false,"last_error":null}
```

`ask` body: `{"q":"...", "model":"...", "max":256, "system":"..."}` — `model`,
`max` and `system` are optional and fall back to the `brain.*` settings.

The response is chunked `text/plain`, flushed as the board decodes it. The board
is already parsing megabrain's HTTP chunked framing in `eos_brain`, and that
parser **holds back a trailing incomplete UTF-8 sequence**, so every flush the
board emits is whole characters. The client still decodes with a streaming
`TextDecoder` in case a proxy re-splits them.

`eos_brain` allows one request in flight, so a second `ask` while one is running
is `busy` (409). Errors after the stream has started cannot use a status code —
append a line beginning `! ` and close the stream; the client renders it as an
error. `/api/brain/cancel` maps to `eos_brain_cancel()`.

---

## The voxel editor

The marquee tab. It renders the way `kernel/avatar/eos_buddy.c` does rather than
the way a browser could: axis-aligned cubes, at most three visible quads each,
three fixed brightness levels by face orientation, painter order with no depth
buffer. That is deliberate — **what you see in the canvas is what a 320x240
ILI9341 can actually draw**. A WebGL preview with real lighting would flatter
models the board cannot render.

There is no raycaster. The painter walk already visits every visible face in
strict far-to-near order, so picking is that same walk with a
point-in-parallelogram test that keeps the *last* hit. Last is nearest, exactly,
with no epsilon and no DDA, and it yields the face normal for free — which is
what "build" needs to know where the new voxel goes.

### Controls

| Input | Desktop | Touch |
|---|---|---|
| Current tool | left drag | one finger drag |
| Orbit | right drag, middle drag, or shift+drag | **Orbit** toggle, then one finger |
| Zoom | wheel | pinch |
| Pan | — | two-finger drag |
| Undo / redo | ctrl/cmd+Z, shift for redo | Undo / Redo buttons |

A drag paints continuously and counts as **one** undo record. Lifting one finger
from a two-finger gesture leaves you orbiting, never painting — a stray voxel on
the way out of a pinch is the most annoying thing a voxel editor can do.

| Tool | Does |
|---|---|
| **Build** | Adds a voxel on the face you clicked, or on the active layer |
| **Paint** | Recolours the voxel you clicked |
| **Erase** | Removes the voxel you clicked |
| **Eye** | Adopts the clicked voxel's palette index as `eyes.open_index` |

| Toggle | Does |
|---|---|
| **Slice** | Hides everything above the Z slider and puts *every* placement on that layer. This is what makes the interior of a model reachable. |
| **Mirror X** | Mirrors every edit across the middle of the X axis |

### Palette

256 entries. Index 0 is empty and never stored. Indices 1–208 are a fixed grid —
one 16-step grey ramp then twelve hues of 16, dark-saturated up to light-pastel,
laid out as 13 columns so the swatches read as a colour wheel. Indices **209–255
are free slots** the colour picker fills: *Add* matches an existing entry
exactly if there is one, otherwise takes the next free slot, otherwise recycles a
custom slot no voxel is using. With all 47 in use by live voxels it says so
rather than silently stealing a colour.

The written `RGBA` chunk always carries the full palette, so a model is
self-describing and does not depend on the stock MagicaVoxel table.

---

## Verification

Everything below was run against this directory.

**Syntax.** `node --check app.js setup.js voxel-editor.js` — all three clean.

**No external resources.** `grep -rnE 'https?://|//cdn'` over `*.html`, `*.js`
and `*.css` in this directory returns two lines, both in `app.js`, both the SVG
XML namespace passed to `document.createElementNS()`. It is an identifier and
is never fetched. `setup.js` adds none: it builds its `<use>` icons by writing
constant markup, which the HTML parser namespaces for it, and it writes
addresses as protocol-relative `//host/` hrefs so no scheme appears in the
source at all. `grep 'url('` over `style.css` returns nothing.

**Setup, driven end to end.** A mock firmware implementing every endpoint in
*Setup mode* was run against the real page in a browser at 375x812 and at
1200x900. What was exercised, and what it did:

| Case | Result |
|---|---|
| Board in SETUP | probe took the page, step 1 showed board, chip, panel, MAC, firmware and the AP name to compare against the panel |
| Scan of 8 APs | two entries for one SSID collapsed to one row at the louder reading, sorted by signal, enterprise and hidden rows listed but not selectable |
| Password under 8 characters | refused before the radio moved, with the length it got |
| Wrong password (reason 202) | "Wrong password", the case-sensitivity and autocapitalisation warning, Try again keeps the network |
| Correct password | joined, both addresses, and the line that credentials are saved only now |
| Hidden SSID by hand | leading and trailing spaces trimmed, 202-then-poll, reason 201 rendered as "nothing called HiddenHouse answered" |
| Scan socket killed 3 times | `retrying (1 of 4)`, `(2 of 4)`, then 7 networks; the app's offline bar stayed hidden throughout |
| Every scan killed | inline failure in the panel, button re-enabled, manual entry still offered |
| Board silent from the POST onward | 45 s of live seconds count and rising "no answer for N checks", then the *stopped answering* screen, not a failure |
| BLE scan and pair | 3 devices, the one-host warning, `Connecting…`, then 428193 on one line, then bonded with battery |
| BLE bond refused | the clear-the-other-bond instruction, passkey panel closed, list back |
| Radio mutex | both scan buttons and Join disabled for the whole join, re-enabled after |
| Run mode | the four tabs booted normally; Settings → Set up WiFi took the page and Back returned it, with `/api/system` polling resumed |

**Regressions.** The only console errors across the whole run were
`ERR_EMPTY_RESPONSE` from the sockets the mock destroyed on purpose. No
exception, and no unhandled rejection.

**The `.vox` files are files the board reads.** `kernel/avatar/eos_vox.c` was
compiled and pointed at a file this editor produced, including one drawn by hand
in a browser and uploaded through the chunked write endpoint:

```
c : parse -> ok
c : size 16x16x16, 13 voxels kept after cull, sorted=1 culled=1
c : pal[0]   00 00 00
```

The palette round-trips at the right offset — a colour written at editor index
209 comes back as `pal.rgb[209]`, confirming the file-entry-`j` ↔
index-`j+1` mapping in both directions.

**The painter order is correct.** The production `_walk()` was driven through 80
camera angles over a solid blob and rasterised into a depth buffer, which is the
same proof `eos_buddy.h`'s `audit_depth` field exists to make:

```
painter audit: 80 camera angles, 35760 faces rasterised
  faces drawn against a solid neighbour (should be 0): 0
  pixels overwritten by something FARTHER (should be 0): 0
```

**Input handling.** The real `_bindInput()` listeners were driven with
synthesised events: shift-drag and right-drag orbit without painting, a plain
drag paints and is one undo record, touch paints or orbits per the toggle, a
two-finger pinch zooms and pans and never paints, the wheel zooms, and pitch
clamps below 90°. 16 checks, 0 failed.

**End to end.** A mock firmware implementing every endpoint in this document was
run against the real page: files listed and paged, a model drawn by clicking,
saved through chunked upload, written to the card and parsed by the C. Settings
loaded and patched, the console ran a command and showed the reply, and the
megabrain chat streamed a chunked response token by token.

**Degradation.** Killing the board mid-session turns the link dot red, shows a
retry bar, and leaves every already-loaded panel on screen and usable; a forced
refresh reports `cannot read /sd - Failed to fetch` inline. Restarting the board
and pressing retry recovers with no reload. Verified at 375x812 as well as
desktop.

## What the board answers

Every endpoint above is implemented. `test_httpd.c`'s `test_every_endpoint()`
holds this document's complete list against the firmware's one route table and
then drives all thirty-one through the real dispatch; `firmware/README.md`'s
*The API* section says which file owns which. What follows is only the places
where the board's real behaviour is narrower than what this document allows.

| Where | What the board does |
|---|---|
| `limits.chunk_max` | **512**, not 4,096. It is `EOS_HTTPD_BODY_MAX`, the body lands in a worker's stack frame before the dispatch lock is taken, and four workers times any increase comes straight out of the heap. `/api/system` reports it by calling `eos_apps_chunk_max()` rather than restating it. The web app gzipped is about 100 chunks; the biggest `.vox` this board stages is 12 |
| `limits.list_max` | 32 |
| `fs` | `/int` is a 960 KB LittleFS and is mounted. `/sd` is declared and reports `mounted: false` — the slot exists on the C6 board but its pins are not known, so `EOS_FS_FAT` is not implemented and `/sd` answers `no_such_device` without touching a bus |
| `GET /api/buddy` | 404s on a board that has a buddy on its panel. It reports `/int/buddy/buddy.json`, and the avatar the panel is drawing is compiled into the image. This document already calls a 404 normal on a fresh card, and that is exactly what this is |
| `POST /api/system/reboot` | answers, then reboots on the OS loop's next tick. Restarting inside the handler would drop the socket mid-response and the page would report a network failure for a reboot that worked |
| `POST /api/settings` | the whole patch is capped at 512 bytes with the body. A maximum-length Megabrain group save is about 400 B and fits; a hand-written client sending a long `brain.system` in the same document gets a 413 from the transport before a handler sees it |
| `wifi.ssid` + `wifi.psk` | a network change needs both. A new SSID with a blank password is refused rather than retried with the old network's passphrase, which would fail and be reported as "wrong password" |
| settings durability | an edit is lost if the board dies within 2 s of it. That is the debounce window: a LittleFS sync is a 4 KB sector erase with the instruction cache off, and a 60-step brightness drag has to cost one erase and not sixty |
| `time.synced` | always false. There is no SNTP client, so `epoch` is whatever `time()` says and the first file written on a fresh board carries a 1970 mtime for ever |
| `sys.autostart` | stored, reported, and applied at boot as **which window has the focus**. Six of the ten windows are open from boot and the other four are opened from the panel's own `super+space` launcher; there is no process to launch. The value is matched against the `id` column of `/api/apps`, so a value the picker offered always resolves |
| `/api/apps` | the ten windows the panel itself can show. There is no separate app process model and none is planned; the board's `apps/` directory is empty and superseded by the table in `firmware/main/eos_app_registry.c` |
| `fw.built` | `__DATE__ " " __TIME__`, not ISO 8601, and `fw.version` is the string `0.1.0`. There is no build-system field for either yet |

## Assumptions to confirm

Nine things here were invention rather than something the repo already fixed.
Four of them have since been implemented against and are settled; the rest are
still open. Items 5 to 9 are the setup screen's.

1. ~~**`buddy.json` is a format I defined.**~~ **Settled.**
   `kernel/svc/eos_apps.c` reads it, field for field as documented, and
   `firmware/main/main.c` adopts the result. A `schema_version` the firmware
   does not know falls back to the compiled-in buddy rather than refusing to
   boot, as this document asked.
2. **The four `idle.behaviour` presets are named, not specified.** The editor
   writes the name; the board owns what each one does. Partly landed: the preset
   is stored and reported, and `sleepy` halves `idle_sleep_ms`, but nothing
   consumes yaw drift yet.
3. ~~**Settings key names.**~~ **Settled.** `kernel/svc/eos_settings.h` uses
   these twelve names exactly. All of them fit 15 bytes.
4. ~~**`/api/console/exec` is fire-and-forget with a shared log.**~~ **Settled.**
   `eos_apps_log_install()` puts the ESP-IDF log hook in before anything logs, so
   boot messages and command output share one ring. The command table is closed
   at seven words with no arguments — `help status heap reboot theme wifi brain`
   — because the endpoint is unauthenticated and reachable from any phone on the
   same WiFi as a board holding that WiFi's password in NVS.
5. **`/api/net/status` carries `mode`, `ap` and `join`.**
   `docs/provisioning.md` describes it as "Mode, IP, RSSI, mDNS name" and does
   not name the fields. The page reads `mode` (which page to be), `ap.ssid`
   (so step 1 can be checked against the panel, and so the last screen can name
   the access point that is about to disappear) and `join.{state,ssid,reason,detail}`
   (the outcome of a join whose HTTP response died). Of these, **`join` is the
   one that has to exist**: without it the page falls back to inferring a join
   from `ip` plus `ssid` plus a `mode` that is no longer SETUP, which works but
   cannot tell a wrong password from an out-of-range network, and the whole
   point of the failure table above is that it can.
6. **`POST /api/wifi/connect` reports the outcome as
   `{"ok":false,"reason":...}` with a 200, not as a 4xx.** A wrong password is
   not a malformed request. The page handles a 4xx anyway, but a board that
   returns 400 for a bad password will lose `reason` into the generic error
   model and the person will get worse text.
7. **`/api/ble/status` exposes `passkey` while pairing.**
   `docs/provisioning.md` lists only "bonded device, connected yes/no, battery
   if exposed", but it also says the passkey must appear in the web page, and
   this is the only endpoint it can come from. `claude_term024` already puts
   `passkey` in its status JSON, so the shape is proven; the field name here is
   the same. The page also reads `state`
   (`connecting`/`passkey`/`bonded`/`failed`) and `reason`, and degrades to
   inferring bonded from a `bonded` object if `state` is absent.
8. **The BLE failure reasons are named after the five ways
   `claude_term024`'s `kbTryConnect()` can fail.** `not_found`,
   `connect_fail`, `bond_fail`, `no_hid`, `no_reports`. If the port names them
   differently the loose matcher probably still catches them, but the exact
   strings are cheaper.
9. **`GET /api/ble/scan` blocks for the scan and returns the result.** If it
   returns immediately with `scanning: true` instead, the page polls it until
   that clears or 25 s pass, so either shape works. What it must not do is
   return an empty list with no `scanning` flag while a scan is still running,
   because that is indistinguishable from "no keyboards".
