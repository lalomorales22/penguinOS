# kernel/svc — services

Kernel services that are not the window manager. Right now: `eos_brain`.

| File | What |
|---|---|
| `include/eos_brain.h` | MEGABRAIN client: streaming parser, request API, transport interface |
| `eos_brain.c` | Implementation, plus the ESP-IDF socket/mDNS/NVS bindings |
| `test/test_brain.c` | Host test, 168 checks, no networking |

```bash
cc -std=c99 -Wall -Wextra -O1 -Ikernel/svc/include \
   kernel/svc/eos_brain.c kernel/svc/test/test_brain.c -o /tmp/test_brain && /tmp/test_brain
```

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
