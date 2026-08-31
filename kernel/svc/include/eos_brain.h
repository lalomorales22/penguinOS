// eos_brain — the OS side of MEGABRAIN, the local model server on the Mac mini.
//
// Asking is the easy half. Reading the answer is not: the server replies with
// HTTP/1.1 chunked transfer encoding and no content length, and the shell wants
// every byte on the panel the moment it lands. So the reply is consumed by an
// incremental parser that eats whatever the socket hands over, in whatever
// sizes it hands it over in, and calls back with decoded text.
//
// The non-obvious constraint: a chunk boundary falls wherever the server's
// writer happened to flush, which is regularly in the middle of a UTF-8
// sequence. A renderer handed half a character draws garbage. The parser
// therefore holds back a trailing incomplete sequence — never more than three
// bytes — until the rest of it arrives. No valid character is ever split across
// two callbacks and no byte is ever emitted twice.
//
// Bytes that are already not UTF-8 are the one exception, and deliberately so:
// a lead byte that the following byte proves can never be completed is released
// as a raw byte rather than held. Holding it would stall the stream on garbage,
// and there is no character there to keep whole.
//
// No allocation anywhere, one request in flight, and sockets live behind a
// four-function transport so the parser runs on the host with zero networking.

#ifndef EOS_BRAIN_H
#define EOS_BRAIN_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---------------------------------------------------------------- tunables
//
// These size fixed buffers inside eos_brain_t. The whole struct is ~2.1 KB and
// is meant to live in BSS, not on the heap — the CYD has about 20 KB free at
// steady state with WiFi and BLE up, and cannot spare a malloc this size.

#ifndef EOS_BRAIN_PROMPT_MAX
#define EOS_BRAIN_PROMPT_MAX 384   // bytes of user prompt kept, NUL included
#endif
#ifndef EOS_BRAIN_SYSTEM_MAX
#define EOS_BRAIN_SYSTEM_MAX 224
#endif
#ifndef EOS_BRAIN_MODEL_MAX
#define EOS_BRAIN_MODEL_MAX 32
#endif
#ifndef EOS_BRAIN_HOST_MAX
#define EOS_BRAIN_HOST_MAX 48
#endif
#ifndef EOS_BRAIN_REQ_MAX
#define EOS_BRAIN_REQ_MAX 1024     // whole request head, percent-encoded URL included
#endif
#ifndef EOS_BRAIN_LINE_MAX
#define EOS_BRAIN_LINE_MAX 96      // status / header / chunk-size line staging
#endif
#ifndef EOS_BRAIN_TEXT_MAX
#define EOS_BRAIN_TEXT_MAX 64      // decoded text staged before a callback
#endif
#ifndef EOS_BRAIN_RX_MAX
#define EOS_BRAIN_RX_MAX 192       // socket read granularity
#endif
#ifndef EOS_BRAIN_CHUNK_LIMIT
#define EOS_BRAIN_CHUNK_LIMIT 0x1000000u  // a chunk claiming more than 16 MB is garbage
#endif

// The counters that index these buffers are deliberately narrow, so an override
// that outgrows its counter is caught here rather than by silently wrapping mid
// stream. buf_len is a uint8_t, line_len and out_len are uint16_t.
#if EOS_BRAIN_TEXT_MAX > 255
#error "EOS_BRAIN_TEXT_MAX must fit eos_brain_parser_t.buf_len, a uint8_t"
#endif
#if EOS_BRAIN_TEXT_MAX < 8
#error "EOS_BRAIN_TEXT_MAX must leave room for a 4-byte UTF-8 sequence plus text"
#endif
#if EOS_BRAIN_LINE_MAX > 65535 || EOS_BRAIN_LINE_MAX < 16
#error "EOS_BRAIN_LINE_MAX must fit eos_brain_parser_t.line_len and hold a status line"
#endif
#if EOS_BRAIN_REQ_MAX > 65535 || EOS_BRAIN_REQ_MAX < 128
#error "EOS_BRAIN_REQ_MAX must fit eos_brain_t.out_len and hold a request head"
#endif

#define EOS_BRAIN_CANDIDATES 3     // cached host, mDNS answer, compiled-in fallback

// The board tries three sources in order: the host it last talked to
// successfully (cached in NVS), an mDNS lookup, then this compiled-in fallback.
//
// The fallback is EMPTY on purpose. It used to be a literal LAN address, which
// worked on exactly one network and on every other one pointed the board at
// whatever unrelated device happened to hold that address - a stranger's
// printer, say. Failing cleanly with "no host configured" is better than
// succeeding at reaching the wrong machine, and the Settings tab is one field.
#define EOS_BRAIN_FALLBACK_HOST ""

// The name looked up over mDNS when no host is set. There is no convention for
// this, so it is a starting guess rather than something that will resolve on an
// unconfigured network - set the host in Settings and this path never runs.
// Anyone who wants zero-configuration discovery can name their machine this.
#define EOS_BRAIN_MDNS_NAME     "penguinos-brain"
#define EOS_BRAIN_PATH_ASK      "/ask"
#define EOS_BRAIN_PATH_HEALTH   "/health"
#define EOS_BRAIN_DEFAULT_MODEL "qwen3.5:2b"

// A system prompt that fits a 320x240 panel. Plain text, no markdown, short.
#define EOS_BRAIN_SYSTEM_TINY \
    "You are a terminal AI on a tiny screen. Plain text only, no markdown, " \
    "no emoji. Keep replies under 80 words unless asked for more."

// ------------------------------------------------------------------ errors

typedef enum {
    EOS_BRAIN_OK = 0,
    EOS_BRAIN_ERR_ARG,        // caller passed something impossible
    EOS_BRAIN_ERR_BUSY,       // a request is already in flight
    EOS_BRAIN_ERR_TOO_LONG,   // prompt does not fit the fixed buffers
    EOS_BRAIN_ERR_NO_HOST,    // every discovery candidate refused
    EOS_BRAIN_ERR_CONNECT,    // transport open failed
    EOS_BRAIN_ERR_SEND,       // transport send failed
    EOS_BRAIN_ERR_TIMEOUT,    // no progress inside the deadline
    EOS_BRAIN_ERR_HTTP,       // reached the server, got a non-200
    EOS_BRAIN_ERR_PROTOCOL,   // framing is malformed; not HTTP at all
    EOS_BRAIN_ERR_TRUNCATED,  // stream died part way through the body
    EOS_BRAIN_ERR_CANCELLED   // eos_brain_cancel()
} eos_brain_err_t;

const char *eos_brain_err_name(eos_brain_err_t e);

// ------------------------------------------------------------------ parser
//
// Standalone and pure. Feed it the bytes of an HTTP response starting at the
// status line; it calls on_text with decoded body text. It holds one
// EOS_BRAIN_LINE_MAX line buffer and one EOS_BRAIN_TEXT_MAX text buffer and
// nothing else, whatever the reply size.

typedef void (*eos_brain_text_fn)(void *user, const char *text, size_t len);

typedef enum {
    EOS_BRAIN_PARSE_MORE = 0,  // consumed everything, wants more bytes
    EOS_BRAIN_PARSE_DONE,      // response complete
    EOS_BRAIN_PARSE_ERROR      // see eos_brain_parser_error()
} eos_brain_parse_t;

typedef struct {
    uint8_t  st;          // internal state
    uint8_t  body;        // 0 until close, 1 chunked, 2 content-length
    uint8_t  err;         // eos_brain_err_t
    bool     line_ovf;    // current header line overran; rest is discarded
    uint16_t status;      // HTTP status, 0 until the status line parses
    uint32_t chunk_left;  // bytes still owed by the current chunk
    uint32_t body_left;   // bytes still owed by content-length
    uint32_t chunks;      // chunks seen, terminator excluded
    uint32_t text_bytes;  // body bytes handed to on_text
    uint32_t fed;         // bytes fed in total

    uint16_t line_len;
    char     line[EOS_BRAIN_LINE_MAX];

    uint8_t  buf_len;     // staged text; the tail may be a partial UTF-8 seq
    char     buf[EOS_BRAIN_TEXT_MAX];

    eos_brain_text_fn on_text;
    void             *user;
} eos_brain_parser_t;

void              eos_brain_parser_init(eos_brain_parser_t *p, eos_brain_text_fn on_text, void *user);
eos_brain_parse_t eos_brain_parser_feed(eos_brain_parser_t *p, const uint8_t *data, size_t len);

// Call when the peer closes in an orderly way — a FIN, not a reset. Two shapes
// of response are complete at that point and settle as success: a body that was
// being read until close, where the close IS the terminator, and a chunked body
// whose zero-length chunk has already been read, where the only byte still owed
// is the blank line that ends the trailer section. Anything else becomes
// EOS_BRAIN_ERR_TRUNCATED. Either way the staged text is flushed first, minus a
// dangling partial UTF-8 sequence, which is dropped because there is no way to
// complete it.
eos_brain_parse_t eos_brain_parser_finish(eos_brain_parser_t *p);

// Call when the socket broke rather than closed — a reset, or an error from the
// transport. The difference matters for exactly one case: a read-until-close
// body has no framing of its own, so the orderly close is the ONLY evidence
// that the reply was all there, and a broken socket destroys that evidence.
// Such a stream is truncated here where finish() would have called it complete.
// A chunked body that has already read its zero-length chunk is complete on its
// own framing, and stays complete however the socket ended.
eos_brain_parse_t eos_brain_parser_abort(eos_brain_parser_t *p);

// True once the framing itself proves the body is all here, whatever the socket
// does next: content-length satisfied, zero-length chunk read, or already done.
// A read-until-close body is never body-complete, because nothing but the close
// can make it so.
bool eos_brain_parser_body_complete(const eos_brain_parser_t *p);

int             eos_brain_parser_status(const eos_brain_parser_t *p);
eos_brain_err_t eos_brain_parser_error(const eos_brain_parser_t *p);
bool            eos_brain_parser_done(const eos_brain_parser_t *p);

// Number of leading bytes of `buf` that end on a UTF-8 character boundary.
// Exposed because the renderer wants the same rule when it truncates a line.
size_t eos_brain_utf8_safe_len(const char *buf, size_t len);

// ---------------------------------------------------------------- requests

typedef enum {
    EOS_BRAIN_METHOD_AUTO = 0,  // GET while the URL fits, POST once it does not
    EOS_BRAIN_METHOD_GET,       // ?q=<encoded>  — the documented form
    EOS_BRAIN_METHOD_POST       // prompt in the body — no URL length ceiling
} eos_brain_method_t;

typedef void (*eos_brain_token_fn)(void *user, const char *text, size_t len);
typedef void (*eos_brain_done_fn)(void *user, eos_brain_err_t err);

typedef struct {
    const char *prompt;      // required
    const char *system;      // NULL for none
    const char *model;       // NULL for the server default
    int         max_tokens;  // <= 0 for the server default
    eos_brain_method_t method;
    uint32_t    timeout_ms;  // 0 for cfg.total_ms

    eos_brain_token_fn on_token;  // decoded text, never a split character, may be NULL
    eos_brain_done_fn  on_done;   // exactly once per accepted submit, may be NULL
    void              *user;
} eos_brain_req_t;

// Percent-encodes the RFC 3986 unreserved set (A-Z a-z 0-9 - _ . ~) and escapes
// everything else as %XX with uppercase hex, space included — never '+'.
// Always NUL-terminates when cap > 0. Returns the length the full encoding
// needs, excluding the NUL, so cap <= returned means it was truncated. A
// truncated result never ends inside a %XX triplet.
size_t eos_brain_urlencode(const char *src, char *dst, size_t cap);

// Builds "/ask?stream=1&max=..&system=..&model=..&q=.." for GET, dropping the
// q= for POST. Returns the length, or -1 if it does not fit.
int eos_brain_build_query(const eos_brain_req_t *req, eos_brain_method_t m,
                          char *out, size_t cap);

// Builds the whole request head, blank line included. Writes the method it
// settled on to *used when used is non-NULL. Returns the length, or -1.
int eos_brain_build_request(const eos_brain_req_t *req, const char *host,
                            uint16_t port, char *out, size_t cap,
                            eos_brain_method_t *used);

// --------------------------------------------------------------- transport
//
// Four functions and a clock. recv() must not block: it returns the bytes it
// has, 0 when it has none yet, EOS_BRAIN_EOF when the peer closed, and any
// other negative value for a broken socket.

#define EOS_BRAIN_EOF (-1)

typedef struct {
    void *ctx;
    int  (*open)(void *ctx, const char *host, uint16_t port, uint32_t timeout_ms);
    int  (*send)(void *ctx, const uint8_t *data, size_t len);
    int  (*recv)(void *ctx, uint8_t *buf, size_t cap);
    void (*close)(void *ctx);
    uint32_t (*now_ms)(void *ctx);
} eos_brain_transport_t;

// Discovery hooks. All optional; a NULL entry just means that source is skipped.
// Return 0 on success. mdns_lookup writes a dotted address into ip.
typedef struct {
    void *ctx;
    int  (*mdns_lookup)(void *ctx, const char *name, char *ip, size_t cap);
    int  (*cache_load)(void *ctx, char *host, size_t cap);
    void (*cache_save)(void *ctx, const char *host);
} eos_brain_hooks_t;

typedef struct {
    const char *fallback_host;  // NULL -> EOS_BRAIN_FALLBACK_HOST
    const char *mdns_name;      // NULL -> EOS_BRAIN_MDNS_NAME
    uint16_t    port;           // 0 -> 80
    uint32_t    connect_ms;     // 0 -> 3000
    uint32_t    idle_ms;        // 0 -> 20000, no bytes for this long is a timeout
    uint32_t    total_ms;       // 0 -> 60000, whole request
    uint32_t    link_ttl_ms;    // 0 -> 15000, how long a health probe stays trusted
} eos_brain_cfg_t;

// ------------------------------------------------------------------ events

// What the status bar reads.
typedef enum {
    EOS_BRAIN_LINK_UNKNOWN = 0,  // never probed
    EOS_BRAIN_LINK_TRYING,       // walking the candidate list
    EOS_BRAIN_LINK_UP,           // /health answered 200 within link_ttl_ms
    EOS_BRAIN_LINK_DOWN          // every candidate refused
} eos_brain_link_t;

// What the buddy animates from.
typedef enum {
    EOS_BRAIN_ST_IDLE = 0,
    EOS_BRAIN_ST_RESOLVING,
    EOS_BRAIN_ST_CONNECTING,
    EOS_BRAIN_ST_SENDING,
    EOS_BRAIN_ST_STREAMING
} eos_brain_state_t;

typedef enum {
    EOS_BRAIN_EV_LINK = 0,    // link state changed
    EOS_BRAIN_EV_STATE,       // request state changed
    EOS_BRAIN_EV_SUBMITTED,
    EOS_BRAIN_EV_FIRST_TOKEN, // the moment to stop the thinking animation
    EOS_BRAIN_EV_TOKEN,       // text/len valid
    EOS_BRAIN_EV_DONE,
    EOS_BRAIN_EV_FAILED,      // err valid
    EOS_BRAIN_EV_CANCELLED
} eos_brain_evt_kind_t;

typedef struct {
    eos_brain_evt_kind_t kind;
    eos_brain_link_t     link;
    eos_brain_state_t    state;
    uint32_t             req_id;
    const char          *host;   // never NULL; "" before discovery picks one
    const char          *text;   // EOS_BRAIN_EV_TOKEN only
    size_t               len;
    eos_brain_err_t      err;    // EOS_BRAIN_EV_FAILED only
} eos_brain_evt_t;

typedef void (*eos_brain_event_fn)(void *user, const eos_brain_evt_t *ev);

// ----------------------------------------------------------------- service

typedef struct {
    eos_brain_cfg_t              cfg;
    const eos_brain_transport_t *tp;
    const eos_brain_hooks_t     *hooks;

    eos_brain_parser_t parser;

    uint8_t  st;          // internal
    uint8_t  link;
    bool     pending;     // a submitted request is waiting or running
    bool     probing;     // the open socket is a /health probe
    bool     want_probe;  // a health probe was asked for and has not run yet
    bool     cancel;
    bool     first_token;
    bool     open_sock;

    char     host[EOS_BRAIN_HOST_MAX];
    char     cand[EOS_BRAIN_CANDIDATES][EOS_BRAIN_HOST_MAX];
    uint8_t  cand_n, cand_i;

    char     prompt[EOS_BRAIN_PROMPT_MAX];
    char     system[EOS_BRAIN_SYSTEM_MAX];
    char     model[EOS_BRAIN_MODEL_MAX];
    int      max_tokens;
    uint8_t  method;
    uint32_t timeout_ms;
    eos_brain_token_fn on_token;
    eos_brain_done_fn  on_done;
    void              *req_user;
    uint32_t req_id;

    char     out[EOS_BRAIN_REQ_MAX];
    uint16_t out_len, out_sent;
    uint32_t body_len, body_sent;

    uint32_t t_start, t_last, t_link;
    uint32_t rx_bytes;

    eos_brain_event_fn on_event;
    void              *evt_user;
    eos_brain_err_t    last_err;
} eos_brain_t;

// tp is required and must outlive the service. cfg and hooks may be NULL.
void eos_brain_init(eos_brain_t *b, const eos_brain_transport_t *tp,
                    const eos_brain_cfg_t *cfg, const eos_brain_hooks_t *hooks);

void eos_brain_set_event_cb(eos_brain_t *b, eos_brain_event_fn fn, void *user);

// Copies the request into the service and returns immediately with a request
// id (> 0). Nothing touches the network until eos_brain_pump(). Returns a
// negative eos_brain_err_t if a request is already in flight or the prompt does
// not fit.
int32_t eos_brain_submit(eos_brain_t *b, const eos_brain_req_t *req);

// Health probe only, no prompt. Same rules as submit; the completion arrives as
// a link event rather than on_done.
int32_t eos_brain_probe(eos_brain_t *b);

// Drives the state machine for at most budget_ms of wall clock, doing no more
// work than the transport will hand over without blocking. Call it from the OS
// loop. Returns true while there is still work to do.
bool eos_brain_pump(eos_brain_t *b, uint32_t budget_ms);

// Takes effect on the next pump: the socket is dropped and on_done fires with
// EOS_BRAIN_ERR_CANCELLED.
void eos_brain_cancel(eos_brain_t *b);

bool               eos_brain_busy(const eos_brain_t *b);
eos_brain_link_t   eos_brain_link(const eos_brain_t *b);
eos_brain_state_t  eos_brain_state(const eos_brain_t *b);
const char        *eos_brain_host(const eos_brain_t *b);
eos_brain_err_t    eos_brain_last_error(const eos_brain_t *b);

const char *eos_brain_link_name(eos_brain_link_t l);
const char *eos_brain_state_name(eos_brain_state_t s);

// ------------------------------------------------------- platform bindings
//
// Present only in an ESP-IDF build. The host test links neither.

#ifdef ESP_PLATFORM
const eos_brain_transport_t *eos_brain_lwip_transport(void);  // BSD sockets
const eos_brain_hooks_t     *eos_brain_idf_hooks(void);       // mDNS + NVS "brain"/"host"
#endif

#endif
