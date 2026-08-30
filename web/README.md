# web — the ESP-OS companion app

Served by the board over WiFi, launched at startup, reached at
`http://<boardname>.local`. Four tabs: **Files**, **Settings**, **Buddy**,
**Console**. Vanilla HTML, CSS and JS — no framework, no CDN, no build step, no
webfont. Nothing outside this directory is ever fetched.

| File | Lines | Raw | Gzip | What |
|---|---|---|---|---|
| `index.html` | 264 | 10,781 | 3,041 | Structure and the inline SVG icon symbols |
| `style.css` | 488 | 17,103 | 4,207 | Palette as custom properties, mobile-first layout |
| `app.js` | 1,549 | 48,357 | 14,492 | API client, all four tabs |
| `voxel-editor.js` | 958 | 33,010 | 10,426 | The buddy editor and the `.vox` codec |
| **total served** | **3,259** | **109,251** | **32,166** | 29% of raw |

`README.md` is documentation and is not served.

`preview.html` used to live here and no longer does — it moved to
`design/preview.html`, because it pulls webfonts from `fonts.googleapis.com`
that an offline board cannot reach, and a directory that gets flashed is the
wrong place to keep something that large. It is now regenerated from the live
kernel by `design/build_preview.py`, so it cannot go stale either. See
`design/README.md`.

Ship only the four files in the table above; anything that globs `web/*` into a
flash image is wrong.

## Serving it

The four files live on the card, gzipped, and are served with
`Content-Encoding: gzip`. Nothing on the board ever compresses at runtime.

```
/sd/web/index.html.gz
/sd/web/style.css.gz
/sd/web/app.js.gz
/sd/web/voxel-editor.js.gz
```

```bash
# deploy: gzip onto the card, keeping the original names plus .gz
for f in index.html style.css app.js voxel-editor.js; do
    gzip -9 -c "$f" > "/Volumes/ESPOS/web/$f.gz"
done
```

Serving rules:

| Request | Serve | Content-Type |
|---|---|---|
| `/` | `/sd/web/index.html.gz` | `text/html; charset=utf-8` |
| `/style.css` | `/sd/web/style.css.gz` | `text/css; charset=utf-8` |
| `/app.js`, `/voxel-editor.js` | matching `.gz` | `application/javascript; charset=utf-8` |
| anything else not under `/api/` | 404 | — |

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
  bytes is `too_big` (400), never truncated: a truncated path names a different
  file.
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
  "net":    {"ip":"192.168.0.51","hostname":"esp-os","mdns":"esp-os.local",
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
| GET | `/api/apps` | — | `{"apps":[{"id":"term","name":"Terminal","tier_min":0}]}` |

Feeds the autostart picker. `tier_min` is the lowest `render.tier` the app runs
on; the picker shows everything and lets the board refuse.

## Buddy

| Method | Path | Params | Response |
|---|---|---|---|
| GET | `/api/buddy` | — | `{"buddy":{...buddy.json...},"state":"idle"}` |
| POST | `/api/buddy/reload` | — | `{"ok":true,"state":"idle"}` |

There is deliberately **no upload endpoint for the model**. The editor writes
`/sd/buddy/buddy.vox` and `/sd/buddy/buddy.json` through the ordinary chunked
`/api/fs/write`, then calls `/api/buddy/reload` to make the running OS pick them
up. One upload mechanism, one set of failure modes.

`state` is the `eos_buddy_state_t` name lowercased — `idle`, `thinking`,
`talking`, `listening`, `sleeping`, `happy`, `confused`.

`GET /api/buddy` returning `not_found` (404) is normal on a fresh card and the
editor handles it: it says "no buddy.vox on the card yet" and lets you build one.

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

**Syntax.** `node --check app.js voxel-editor.js` — both clean.

**No external resources.** `grep -E 'https?://|//cdn'` over `index.html` returns
nothing. Every `href`/`src` in the page is `data:,` (the favicon), `style.css`,
`app.js` or `voxel-editor.js`, and the CSS has no `url()` at all. The only
`http://` in the tree is the SVG XML namespace passed to
`document.createElementNS()` in `app.js`, which is an identifier and is never
fetched.

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

## Assumptions to confirm

Four things here are my invention rather than something the repo already fixed.
They are all cheap to change now and expensive later.

1. **`buddy.json` is a format I defined.** Nothing in `kernel/avatar/` reads it
   yet — `eos_buddy.h` has the fields but no JSON loader. The schema above maps
   onto `eos_buddy_cfg_t` field for field, but whoever writes the loader should
   confirm the names before both sides are built.
2. **The four `idle.behaviour` presets are named, not specified.** The editor
   writes the name; the board owns what each one does.
3. **Settings key names.** Chosen to fit 15 bytes for NVS. If the settings
   component already has a key scheme, that one wins.
4. **`/api/console/exec` is fire-and-forget with a shared log.** This assumes
   the OS has a single console log that both boot messages and command output
   flow into. If commands need their own output channel, `exec` should return a
   handle instead.
