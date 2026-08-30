// Host test for eos_brain. The streaming parser is the thing that has to be
// right, so most of this file is an attack on it: every hand-built chunked
// stream is fed at every possible split, one byte at a time and in every block
// size, and the decoded output must be byte-identical each time. Truncation is
// tested at every possible cut point, and each parser runs inside a canary
// frame so an out-of-bounds write anywhere shows up as a corrupted guard.
//
// The service half runs on a scripted fake transport: no sockets, no clock, no
// WiFi. That is the whole reason the transport is an injectable struct.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "eos_brain.h"

static int checks = 0, fails = 0;

#define CK(cond, msg) do { \
    checks++; \
    if (!(cond)) { fails++; printf("    FAIL: %s\n", msg); } \
} while (0)

#define CKS(got, want, msg) do { \
    checks++; \
    if (strcmp((got), (want)) != 0) { \
        fails++; \
        printf("    FAIL: %s\n      got  [%s]\n      want [%s]\n", msg, got, want); \
    } \
} while (0)

// ------------------------------------------------------------ byte builder

typedef struct { char b[8192]; size_t n; } buf_t;

static void bputn(buf_t *b, const char *s, size_t n)
{
    if (b->n + n >= sizeof b->b) { printf("    builder overflow\n"); exit(2); }
    memcpy(b->b + b->n, s, n);
    b->n += n;
    b->b[b->n] = 0;
}
static void bput(buf_t *b, const char *s) { bputn(b, s, strlen(s)); }

static void bchunk(buf_t *b, const char *data, size_t len)
{
    char hdr[32];
    snprintf(hdr, sizeof hdr, "%X\r\n", (unsigned)len);
    bput(b, hdr);
    bputn(b, data, len);
    bput(b, "\r\n");
}
static void bchunks(buf_t *b, const char *data) { bchunk(b, data, strlen(data)); }
static void bend(buf_t *b) { bput(b, "0\r\n\r\n"); }

static const char *HDR_CHUNKED =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Cache-Control: no-cache\r\n"
    "X-Accel-Buffering: no\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n";

// ------------------------------------------------------------------- sink

#define SINK_GUARD 32

typedef struct {
    char   pre[SINK_GUARD];
    char   text[8192];
    char   post[SINK_GUARD];
    size_t len;
    int    calls;
    size_t biggest;
    int    unsplit_ok;     // every callback ended on a UTF-8 boundary
    int    overflow;
} sink_t;

static void sink_init(sink_t *s)
{
    memset(s, 0, sizeof *s);
    memset(s->pre, 0x5A, SINK_GUARD);
    memset(s->post, 0x5A, SINK_GUARD);
    s->unsplit_ok = 1;
}

static int sink_guards_ok(const sink_t *s)
{
    int i;
    for (i = 0; i < SINK_GUARD; i++)
        if ((unsigned char)s->pre[i] != 0x5A || (unsigned char)s->post[i] != 0x5A) return 0;
    return 1;
}

static void on_text(void *user, const char *text, size_t len)
{
    sink_t *s = (sink_t *)user;
    s->calls++;
    if (len > s->biggest) s->biggest = len;
    // The contract: a callback never carries a partial character.
    if (eos_brain_utf8_safe_len(text, len) != len) s->unsplit_ok = 0;
    if (s->len + len >= sizeof s->text) { s->overflow = 1; return; }
    memcpy(s->text + s->len, text, len);
    s->len += len;
    s->text[s->len] = 0;
}

// A parser inside a canary frame.
typedef struct {
    unsigned char pre[SINK_GUARD];
    eos_brain_parser_t p;
    unsigned char post[SINK_GUARD];
} framed_t;

static void framed_init(framed_t *f, sink_t *s)
{
    memset(f->pre, 0xA5, SINK_GUARD);
    memset(f->post, 0xA5, SINK_GUARD);
    eos_brain_parser_init(&f->p, on_text, s);
}

static int framed_ok(const framed_t *f)
{
    int i;
    for (i = 0; i < SINK_GUARD; i++)
        if (f->pre[i] != 0xA5 || f->post[i] != 0xA5) return 0;
    return 1;
}

// Feeds `stream` in fixed-size pieces (piece == 0 means one shot), then closes.
// Returns the result of the close.
static eos_brain_parse_t run(const char *stream, size_t len, size_t piece,
                             sink_t *sink, framed_t *f, int close_at_end)
{
    eos_brain_parse_t r = EOS_BRAIN_PARSE_MORE;
    size_t i = 0;

    sink_init(sink);
    framed_init(f, sink);
    if (piece == 0) piece = len ? len : 1;

    while (i < len) {
        size_t n = (len - i < piece) ? (len - i) : piece;
        r = eos_brain_parser_feed(&f->p, (const uint8_t *)stream + i, n);
        i += n;
        if (r != EOS_BRAIN_PARSE_MORE) break;
    }
    if (close_at_end && r == EOS_BRAIN_PARSE_MORE) r = eos_brain_parser_finish(&f->p);
    return r;
}

// Same, for a literal response where the length is just its strlen.
#define RUNS(str, piece, sink, f, close) \
    run((str), strlen(str), (piece), (sink), (f), (close))

// ------------------------------------------------------------------- utf8

static void test_utf8(void)
{
    static const char e_acute[]  = { (char)0xC3, (char)0xA9, 0 };            // e-acute
    static const char ellipsis[] = { (char)0xE2, (char)0x80, (char)0xA6, 0 };
    static const char emoji[]    = { (char)0xF0, (char)0x9F, (char)0x98, (char)0x80, 0 };

    printf("  utf8 boundary rule\n");
    CK(eos_brain_utf8_safe_len("", 0) == 0, "empty is safe at 0");
    CK(eos_brain_utf8_safe_len("abc", 3) == 3, "pure ascii is fully safe");
    CK(eos_brain_utf8_safe_len(e_acute, 2) == 2, "complete 2-byte sequence passes");
    CK(eos_brain_utf8_safe_len(e_acute, 1) == 0, "lone 2-byte lead is held back");
    CK(eos_brain_utf8_safe_len(ellipsis, 3) == 3, "complete 3-byte sequence passes");
    CK(eos_brain_utf8_safe_len(ellipsis, 2) == 0, "2 of 3 bytes held back");
    CK(eos_brain_utf8_safe_len(ellipsis, 1) == 0, "1 of 3 bytes held back");
    CK(eos_brain_utf8_safe_len(emoji, 4) == 4, "complete 4-byte sequence passes");
    CK(eos_brain_utf8_safe_len(emoji, 3) == 0, "3 of 4 bytes held back");
    CK(eos_brain_utf8_safe_len("ab\xC3", 3) == 2, "text plus dangling lead splits at 2");
    CK(eos_brain_utf8_safe_len("\x80\x80\x80\x80\x80", 5) == 5,
       "orphan continuation bytes are not held forever");
    CK(eos_brain_utf8_safe_len("\xFF", 1) == 1, "invalid lead passes as one raw byte");
    CK(eos_brain_utf8_safe_len(NULL, 4) == 0, "NULL is safe");
}

// -------------------------------------------------------------- urlencode

static void test_urlencode(void)
{
    char out[256];
    size_t need;

    printf("  percent encoder\n");

    eos_brain_urlencode("", out, sizeof out);              CKS(out, "", "empty string");
    eos_brain_urlencode("hello", out, sizeof out);         CKS(out, "hello", "plain ascii is untouched");
    eos_brain_urlencode("hello world", out, sizeof out);   CKS(out, "hello%20world", "space is %20, never +");
    eos_brain_urlencode("a+b=c&d", out, sizeof out);       CKS(out, "a%2Bb%3Dc%26d", "query metacharacters escape");
    eos_brain_urlencode("-_.~", out, sizeof out);          CKS(out, "-_.~", "RFC 3986 unreserved set survives");
    eos_brain_urlencode("/?#[]@", out, sizeof out);        CKS(out, "%2F%3F%23%5B%5D%40", "reserved set escapes");
    eos_brain_urlencode("100%", out, sizeof out);          CKS(out, "100%25", "percent escapes itself");
    eos_brain_urlencode("a\nb\tc", out, sizeof out);       CKS(out, "a%0Ab%09c", "control characters escape");
    eos_brain_urlencode("caf\xC3\xA9", out, sizeof out);   CKS(out, "caf%C3%A9", "utf8 escapes per byte, uppercase hex");
    eos_brain_urlencode("\xF0\x9F\x98\x80", out, sizeof out); CKS(out, "%F0%9F%98%80", "4-byte emoji escapes as 4 bytes");
    eos_brain_urlencode("Aa0-Zz9", out, sizeof out);       CKS(out, "Aa0-Zz9", "alnum boundaries");

    need = eos_brain_urlencode("hello world", NULL, 0);
    CK(need == 13, "returns the needed length with no destination");

    memset(out, 0x7F, sizeof out);
    need = eos_brain_urlencode("a\xC3\xA9", out, 3);
    CK(need == 7, "needed length reported even when truncated");
    CKS(out, "a", "truncation stops before a %XX triplet, never inside one");

    need = eos_brain_urlencode("hello", out, 1);
    CK(need == 5 && out[0] == 0, "cap of 1 yields an empty but terminated string");

    // Every byte value 1..255 round-trips through the encoder without overrun.
    {
        char src[256], enc[1024];
        int i, n = 0;
        for (i = 1; i < 256; i++) src[n++] = (char)i;
        src[n] = 0;
        need = eos_brain_urlencode(src, enc, sizeof enc);
        CK(need == strlen(enc), "all 255 byte values encode without truncation");
        CK(strchr(enc, '+') == NULL, "encoder never emits a bare +");
        {
            int bad = 0;
            size_t k;
            for (k = 0; k < strlen(enc); k++) {
                unsigned char c = (unsigned char)enc[k];
                if (c == '%') { k += 2; continue; }
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~'))
                    bad = 1;
            }
            CK(!bad, "output contains only unreserved characters and %XX triplets");
        }
    }
}

// ----------------------------------------------------------- url building

static void test_build(void)
{
    char out[2048];
    eos_brain_req_t r;
    eos_brain_method_t used = EOS_BRAIN_METHOD_AUTO;
    int n;

    printf("  request building\n");

    memset(&r, 0, sizeof r);
    r.prompt     = "what is a tiling wm";
    r.system     = "be brief";
    r.model      = "qwen3.5:2b";
    r.max_tokens = 350;

    n = eos_brain_build_query(&r, EOS_BRAIN_METHOD_GET, out, sizeof out);
    CK(n > 0, "query builds");
    CKS(out, "/ask?stream=1&max=350&system=be%20brief&model=qwen3.5%3A2b&q=what%20is%20a%20tiling%20wm",
        "GET query matches the documented endpoint shape");

    n = eos_brain_build_query(&r, EOS_BRAIN_METHOD_POST, out, sizeof out);
    CK(n > 0 && strstr(out, "&q=") == NULL, "POST query drops q= (the prompt is the body)");

    n = eos_brain_build_request(&r, "192.168.0.139", 80, out, sizeof out, &used);
    CK(n > 0 && used == EOS_BRAIN_METHOD_GET, "AUTO picks GET when the URL fits");
    CK(strncmp(out, "GET /ask?stream=1", 17) == 0, "request line is a GET on /ask");
    CK(strstr(out, "\r\nHost: 192.168.0.139\r\n") != NULL, "Host header carries the discovered address");
    CK(strstr(out, "\r\nConnection: close\r\n") != NULL, "Connection: close is requested");
    CK(strstr(out, "\r\n\r\n") != NULL && out[n - 1] == '\n', "head ends with a blank line");
    CK((size_t)n == strlen(out), "returned length matches the built string");

    r.method = EOS_BRAIN_METHOD_POST;
    n = eos_brain_build_request(&r, "megabrain.local", 8080, out, sizeof out, &used);
    CK(n > 0 && used == EOS_BRAIN_METHOD_POST, "explicit POST is honoured");
    CK(strstr(out, "Content-Length: 19\r\n") != NULL, "POST declares the prompt length");
    CK(strstr(out, "Host: megabrain.local:8080\r\n") != NULL, "non-80 port appears in Host");

    // A prompt too long for the URL buffer must fall back rather than truncate.
    {
        static char big[900];
        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = 0;
        r.prompt = big;
        r.method = EOS_BRAIN_METHOD_AUTO;
        n = eos_brain_build_request(&r, "192.168.0.139", 80, out, 512, &used);
        CK(n > 0 && used == EOS_BRAIN_METHOD_POST, "AUTO falls back to POST when the URL will not fit");
        CK(strstr(out, "Content-Length: 899\r\n") != NULL, "fallback POST declares the right length");
        n = eos_brain_build_request(&r, "192.168.0.139", 80, out, 64, &used);
        CK(n < 0, "an impossible buffer is refused, not truncated");
    }
}

// ------------------------------------------------------------ parser core

static void test_parser_nominal(void)
{
    buf_t s;
    sink_t sink;
    framed_t f;
    eos_brain_parse_t r;

    printf("  chunked stream, nominal\n");

    s.n = 0; s.b[0] = 0;
    bput(&s, HDR_CHUNKED);
    bchunks(&s, "Hello ");
    bchunks(&s, "from the ");
    bchunks(&s, "mini.");
    bend(&s);

    r = run(s.b, s.n, 0, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_DONE, "complete response parses to DONE");
    CKS(sink.text, "Hello from the mini.", "decoded text is the chunk payloads, framing stripped");
    CK(eos_brain_parser_status(&f.p) == 200, "status line parsed");
    CK(f.p.chunks == 3, "three data chunks counted");
    CK(f.p.text_bytes == 20, "text byte counter matches");
    CK(sink.unsplit_ok, "no callback carried a partial character");
    CK(framed_ok(&f) && sink_guards_ok(&sink), "no writes outside the parser or sink");
}

// Every block size from 1 up to the whole stream must decode identically.
static void test_parser_every_split(void)
{
    buf_t s;
    sink_t sink;
    framed_t f;
    const char *want = "The tiling tree collapses to tabs when a split cannot fit.";
    size_t piece;
    int bad = 0, guard_bad = 0, partial = 0;

    printf("  chunked stream, every block size\n");

    s.n = 0; s.b[0] = 0;
    bput(&s, HDR_CHUNKED);
    bchunks(&s, "The tiling tree ");
    bchunks(&s, "collapses to tabs ");
    bchunks(&s, "when a split ");
    bchunks(&s, "cannot fit.");
    bend(&s);

    for (piece = 1; piece <= s.n; piece++) {
        eos_brain_parse_t r = run(s.b, s.n, piece, &sink, &f, 1);
        if (r != EOS_BRAIN_PARSE_DONE || strcmp(sink.text, want) != 0) bad++;
        if (!framed_ok(&f) || !sink_guards_ok(&sink)) guard_bad++;
        if (!sink.unsplit_ok) partial++;
        if (sink.biggest > EOS_BRAIN_TEXT_MAX) partial++;
    }
    CK(bad == 0, "identical output at every block size from 1 byte to the whole stream");
    CK(guard_bad == 0, "no out-of-bounds write at any block size");
    CK(partial == 0, "no oversized or partial callback at any block size");

    // The pathological splits called out by name, so a failure says which.
    {
        struct { const char *needle; const char *why; } spots[] = {
            { "\r\n10\r\n",   "split inside the hex size line" },
            { "HTTP/1.1 200", "split inside the status line" },
            { "\r\n\r\n",     "split inside the header terminator" },
        };
        size_t k;
        for (k = 0; k < sizeof spots / sizeof spots[0]; k++) {
            const char *at = strstr(s.b, spots[k].needle);
            size_t off;
            int ok = 1;
            size_t d;
            if (!at) { CK(0, spots[k].why); continue; }
            off = (size_t)(at - s.b);
            for (d = 1; d < strlen(spots[k].needle); d++) {
                eos_brain_parse_t r;
                size_t cut = off + d;
                sink_init(&sink);
                framed_init(&f, &sink);
                r = eos_brain_parser_feed(&f.p, (const uint8_t *)s.b, cut);
                if (r == EOS_BRAIN_PARSE_MORE)
                    r = eos_brain_parser_feed(&f.p, (const uint8_t *)s.b + cut, s.n - cut);
                if (r == EOS_BRAIN_PARSE_MORE) r = eos_brain_parser_finish(&f.p);
                if (r != EOS_BRAIN_PARSE_DONE || strcmp(sink.text, want) != 0) ok = 0;
                if (!framed_ok(&f)) ok = 0;
            }
            CK(ok, spots[k].why);
        }
    }
}

// A chunk boundary landing inside a multibyte character is the whole reason the
// parser exists, so it gets its own stream with every character length in it.
static void test_parser_utf8_splits(void)
{
    buf_t s;
    sink_t sink;
    framed_t f;
    // "ok" + e-acute + " naive" + ellipsis + " " + emoji + " done"
    static const char want[] =
        "ok\xC3\xA9 naive\xE2\x80\xA6 \xF0\x9F\x98\x80 done";
    size_t piece;
    int bad = 0, partial = 0;

    printf("  multibyte characters split across chunks\n");

    s.n = 0; s.b[0] = 0;
    bput(&s, HDR_CHUNKED);
    bchunk(&s, "ok\xC3", 3);          // chunk ends mid 2-byte character
    bchunk(&s, "\xA9 naive\xE2\x80", 9);  // ends 2 bytes into a 3-byte character
    bchunk(&s, "\xA6 \xF0", 3);       // ends 1 byte into a 4-byte character
    bchunk(&s, "\x9F\x98\x80 done", 8);
    bend(&s);

    for (piece = 1; piece <= s.n; piece++) {
        eos_brain_parse_t r = run(s.b, s.n, piece, &sink, &f, 1);
        if (r != EOS_BRAIN_PARSE_DONE || strcmp(sink.text, want) != 0) bad++;
        if (!sink.unsplit_ok) partial++;
    }
    CK(bad == 0, "utf8 reassembles identically at every block size");
    CK(partial == 0, "never hands the renderer half a character");

    (void)run(s.b, s.n, 1, &sink, &f, 1);
    CK(strcmp(sink.text, want) == 0, "one byte at a time still decodes exactly");
    CK(sink.calls >= 4, "text is delivered progressively, not all at the end");
}

// A lead byte that is never completed. Holding it back would stall the buffer
// for the rest of the stream, so the parser releases it as a raw byte once the
// following byte proves it can never be a character. That is the one case where
// a callback does NOT end on a character boundary, and it is deliberate: the
// bytes are already not text. What must never happen is a *valid* character
// being split across two callbacks, or a byte being lost or repeated.
static void test_parser_invalid_utf8(void)
{
    buf_t s;
    sink_t sink;
    framed_t f;
    // "ab" + a dangling C3 lead, then D0 91 ("B" in Cyrillic) + "z". The C3 can
    // never complete because D0 is a lead, not a continuation.
    static const char want[] = "ab\xC3\xD0\x91z";
    size_t piece;
    int bad = 0, guard_bad = 0;

    printf("  invalid utf8 is released, not held forever\n");

    s.n = 0; s.b[0] = 0;
    bput(&s, HDR_CHUNKED);
    bchunk(&s, "ab\xC3", 3);
    bchunk(&s, "\xD0\x91z", 3);
    bend(&s);

    for (piece = 1; piece <= s.n; piece++) {
        eos_brain_parse_t r = run(s.b, s.n, piece, &sink, &f, 1);
        if (r != EOS_BRAIN_PARSE_DONE) bad++;
        if (sink.len != sizeof want - 1) bad++;
        if (memcmp(sink.text, want, sizeof want - 1) != 0) bad++;
        if (!framed_ok(&f) || !sink_guards_ok(&sink)) guard_bad++;
    }
    CK(bad == 0, "an uncompletable lead byte is passed through, byte-exact, at every block size");
    CK(guard_bad == 0, "and writes nothing outside the parser");

    // The stall this guards against: a held byte must not swallow what follows.
    (void)run(s.b, s.n, 1, &sink, &f, 1);
    CK(sink.len == sizeof want - 1, "no byte is lost behind the dangling lead");
    CK(sink.calls >= 2, "and the stream keeps flowing after it");

    // A lead byte at the very end of a live stream is still held, not emitted:
    // that one can still complete, so it waits.
    {
        buf_t t;
        eos_brain_parse_t r;
        t.n = 0; t.b[0] = 0;
        bput(&t, HDR_CHUNKED);
        bchunk(&t, "hi\xC3", 3);
        sink_init(&sink);
        framed_init(&f, &sink);
        r = eos_brain_parser_feed(&f.p, (const uint8_t *)t.b, t.n);
        CK(r == EOS_BRAIN_PARSE_MORE, "stream is still open");
        CKS(sink.text, "hi", "a completable lead is held back while the stream is alive");
    }
}

// Cut the stream at every possible point and close the socket there.
static void test_parser_truncation(void)
{
    buf_t s;
    sink_t sink;
    framed_t f;
    const char *want = "alpha beta gamma";
    size_t cut;
    int wrong_state = 0, not_prefix = 0, guard_bad = 0;

    printf("  stream dies at every possible offset\n");

    s.n = 0; s.b[0] = 0;
    bput(&s, HDR_CHUNKED);
    bchunks(&s, "alpha ");
    bchunks(&s, "beta ");
    bchunks(&s, "gamma");
    bend(&s);

    for (cut = 0; cut < s.n; cut++) {
        eos_brain_parse_t r = run(s.b, cut, 1, &sink, &f, 1);
        if (r != EOS_BRAIN_PARSE_ERROR) wrong_state++;
        if (eos_brain_parser_error(&f.p) != EOS_BRAIN_ERR_TRUNCATED) wrong_state++;
        if (strncmp(want, sink.text, sink.len) != 0) not_prefix++;
        if (!framed_ok(&f) || !sink_guards_ok(&sink)) guard_bad++;
    }
    CK(wrong_state == 0, "every truncation reports TRUNCATED, none reports success");
    CK(not_prefix == 0, "text delivered before the cut is always a prefix of the real answer");
    CK(guard_bad == 0, "no out-of-bounds write on any truncation");

    // Losing the connection mid-character must drop the fragment, not print it.
    {
        buf_t t;
        t.n = 0; t.b[0] = 0;
        bput(&t, HDR_CHUNKED);
        bchunk(&t, "hi\xE2\x80", 4);
        (void)run(t.b, t.n, 3, &sink, &f, 1);
        CKS(sink.text, "hi", "a partial character at the cut is dropped, not emitted");
    }
}

static void test_parser_edges(void)
{
    buf_t s;
    sink_t sink;
    framed_t f;
    eos_brain_parse_t r;

    printf("  framing edge cases\n");

    // Terminating chunk on its own: a legal, empty answer.
    s.n = 0; s.b[0] = 0;
    bput(&s, HDR_CHUNKED);
    bend(&s);
    r = run(s.b, s.n, 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_DONE, "a lone zero chunk is a complete empty response");
    CK(sink.len == 0 && sink.calls == 0, "empty response emits no text at all");

    // Zero-length chunk terminates even with data behind it.
    s.n = 0; s.b[0] = 0;
    bput(&s, HDR_CHUNKED);
    bchunks(&s, "kept");
    bend(&s);
    bput(&s, "5\r\nafter\r\n");
    r = run(s.b, s.n, 0, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_DONE, "zero chunk ends the body");
    CKS(sink.text, "kept", "bytes after the terminator are ignored, not decoded");

    // A chunk that claims more than it delivers.
    s.n = 0; s.b[0] = 0;
    bput(&s, HDR_CHUNKED);
    bput(&s, "64\r\nonly ten.");            // claims 100 bytes, sends 9
    r = run(s.b, s.n, 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_ERROR, "an oversized chunk that never completes is an error");
    CK(eos_brain_parser_error(&f.p) == EOS_BRAIN_ERR_TRUNCATED, "and it is reported as truncation");
    CKS(sink.text, "only ten.", "the bytes that did arrive are still delivered");

    // Chunk data must be followed by a bare CRLF. Anything else means the size
    // line lied about the length and the frame is no longer trustworthy.
    r = RUNS("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhiXX\r\n0\r\n\r\n",
             1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_ERROR && eos_brain_parser_error(&f.p) == EOS_BRAIN_ERR_PROTOCOL,
       "junk where a chunk's trailing CRLF belongs is a protocol error");

    // Chunk extensions.
    s.n = 0; s.b[0] = 0;
    bput(&s, HDR_CHUNKED);
    bput(&s, "5;name=value\r\nextn!\r\n");
    bend(&s);
    r = run(s.b, s.n, 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_DONE, "chunk extensions are skipped");
    CKS(sink.text, "extn!", "extension does not corrupt the payload");

    // A size line that is not hex.
    s.n = 0; s.b[0] = 0;
    bput(&s, HDR_CHUNKED);
    bput(&s, "zz\r\nnope\r\n");
    r = run(s.b, s.n, 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_ERROR && eos_brain_parser_error(&f.p) == EOS_BRAIN_ERR_PROTOCOL,
       "a non-hex size line is a protocol error");
    CK(sink.len == 0, "nothing is emitted from a broken frame");

    // A size line longer than the line buffer.
    s.n = 0; s.b[0] = 0;
    bput(&s, HDR_CHUNKED);
    {
        char big[512];
        memset(big, 'a', sizeof big - 1);
        big[sizeof big - 1] = 0;
        bput(&s, big);
        bput(&s, "\r\n");
    }
    r = run(s.b, s.n, 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_ERROR, "an over-long size line is rejected");
    CK(framed_ok(&f), "an over-long size line does not overrun the line buffer");

    // A header line longer than the line buffer must be survivable: the server
    // is free to send headers we do not care about, at any length.
    s.n = 0; s.b[0] = 0;
    bput(&s, "HTTP/1.1 200 OK\r\nX-Junk: ");
    {
        char big[1024];
        memset(big, 'j', sizeof big - 1);
        big[sizeof big - 1] = 0;
        bput(&s, big);
    }
    bput(&s, "\r\nTransfer-Encoding: chunked\r\n\r\n");
    bchunks(&s, "survived");
    bend(&s);
    r = run(s.b, s.n, 7, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_DONE, "an over-long header line is discarded, not fatal");
    CKS(sink.text, "survived", "parsing continues past a discarded header");
    CK(framed_ok(&f), "an over-long header does not overrun the line buffer");

    // Content-Length body: this is what /health actually returns.
    r = RUNS("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
             "Content-Length: 22\r\n\r\n{\"ok\":true,\"up\":12345}", 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_DONE, "content-length body completes without a close");
    CKS(sink.text, "{\"ok\":true,\"up\":12345}", "content-length body decodes exactly");

    // Content-Length: 0.
    r = RUNS("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n", 1, &sink, &f, 0);
    CK(r == EOS_BRAIN_PARSE_DONE && sink.len == 0, "a zero-length body completes immediately");

    // A content-length body with bytes behind it. Caddy will not do this, but a
    // proxy that keeps the connection alive will, and the count is the only
    // thing that says where the body stops.
    r = RUNS("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloTRAILING-JUNK", 0, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_DONE, "a content-length body ends at the count, not at the socket");
    CKS(sink.text, "hello", "bytes past content-length are not decoded as body");

    // A chunk size larger than EOS_BRAIN_CHUNK_LIMIT. Believing it would park
    // the parser on a 4 GB chunk that is never coming.
    r = RUNS("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFF\r\n", 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_ERROR && eos_brain_parser_error(&f.p) == EOS_BRAIN_ERR_PROTOCOL,
       "an absurd chunk size is refused up front, not waited on");
    r = RUNS("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1000001\r\n", 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_ERROR && eos_brain_parser_error(&f.p) == EOS_BRAIN_ERR_PROTOCOL,
       "the chunk size limit is enforced one byte over the line");

    // No framing headers at all: the body runs until the socket closes.
    r = RUNS("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nold school", 3, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_DONE, "a body read until close ends cleanly on close");
    CKS(sink.text, "old school", "read-until-close body decodes exactly");

    // Non-200 with a body. The parser still parses; judging is the caller's job.
    s.n = 0; s.b[0] = 0;
    bput(&s, "HTTP/1.1 503 Service Unavailable\r\nTransfer-Encoding: chunked\r\n\r\n");
    bchunks(&s, "ollama is down");
    bend(&s);
    r = run(s.b, s.n, 4, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_DONE && eos_brain_parser_status(&f.p) == 503,
       "a non-200 response is parsed and its status reported");

    // Bare LF line endings, which some hand-rolled servers emit.
    r = RUNS("HTTP/1.1 200 OK\nTransfer-Encoding: chunked\n\n3\nabc\n0\n\n", 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_DONE, "bare LF framing is accepted");
    CKS(sink.text, "abc", "bare LF framing decodes exactly");

    // Header casing must not matter.
    r = RUNS("HTTP/1.1 200 OK\r\nTRANSFER-ENCODING: Chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n", 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_DONE, "header names and values are matched case-insensitively");
    CKS(sink.text, "hi", "upper case Transfer-Encoding still frames correctly");
}

static void test_parser_garbage(void)
{
    sink_t sink;
    framed_t f;
    eos_brain_parse_t r;
    char junk[4096];
    unsigned seed = 12345u;
    size_t i;
    int emitted = 0, guard_bad = 0;

    printf("  garbage in\n");

    r = RUNS("MEOW MEOW MEOW\r\n\r\n", 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_ERROR && eos_brain_parser_error(&f.p) == EOS_BRAIN_ERR_PROTOCOL,
       "a reply that is not HTTP is a protocol error");
    CK(sink.len == 0, "nothing is emitted from a non-HTTP reply");

    // Something that is shaped exactly like a status line but is not HTTP. The
    // three digits alone must not be enough to believe it.
    r = RUNS("XTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_ERROR && eos_brain_parser_error(&f.p) == EOS_BRAIN_ERR_PROTOCOL,
       "a status line not starting with HTTP/ is refused");
    CK(sink.len == 0, "and its body is never delivered");
    r = RUNS("ICY 200 OK\r\nContent-Length: 2\r\n\r\nhi", 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_ERROR && eos_brain_parser_error(&f.p) == EOS_BRAIN_ERR_PROTOCOL,
       "an ICY-style banner is refused too");

    for (i = 0; i < sizeof junk; i++) {
        seed = seed * 1103515245u + 12345u;
        junk[i] = (char)((seed >> 16) & 0xFF);
    }
    for (i = 1; i <= 64; i++) {
        r = run(junk, sizeof junk, i, &sink, &f, 1);
        if (r != EOS_BRAIN_PARSE_ERROR) emitted++;
        if (sink.len != 0) emitted++;
        if (!framed_ok(&f) || !sink_guards_ok(&sink)) guard_bad++;
    }
    CK(emitted == 0, "4 KB of random bytes is rejected at every block size, emitting nothing");
    CK(guard_bad == 0, "random bytes never write outside the parser");

    memset(junk, 0xFF, sizeof junk);
    r = run(junk, sizeof junk, 13, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_ERROR, "a wall of 0xFF is rejected");

    memset(junk, 0, sizeof junk);
    r = run(junk, sizeof junk, 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_ERROR, "a wall of NULs is rejected");

    r = run("", 0, 1, &sink, &f, 1);
    CK(r == EOS_BRAIN_PARSE_ERROR && eos_brain_parser_error(&f.p) == EOS_BRAIN_ERR_TRUNCATED,
       "a socket that closes before a single byte is truncation");

    // Feeding after a terminal state must be inert rather than confusing.
    {
        buf_t s;
        s.n = 0; s.b[0] = 0;
        bput(&s, HDR_CHUNKED);
        bchunks(&s, "done");
        bend(&s);
        r = run(s.b, s.n, 0, &sink, &f, 1);
        CK(r == EOS_BRAIN_PARSE_DONE, "stream completed");
        r = eos_brain_parser_feed(&f.p, (const uint8_t *)"junk", 4);
        CK(r == EOS_BRAIN_PARSE_DONE, "feeding a finished parser stays DONE");
        CKS(sink.text, "done", "and adds no text");
        r = eos_brain_parser_finish(&f.p);
        CK(r == EOS_BRAIN_PARSE_DONE, "finishing twice is harmless");
    }
    r = eos_brain_parser_feed(NULL, (const uint8_t *)"x", 1);
    CK(r == EOS_BRAIN_PARSE_ERROR, "a NULL parser is refused, not dereferenced");
}

// ------------------------------------------------------- fake transport

typedef struct {
    const char *data;
    size_t      len;
    int         refuse;      // this connection attempt is refused outright
} attempt_t;

typedef struct {
    attempt_t attempt[8];
    int       n, cur;
    size_t    pos;
    size_t    drip;          // bytes handed over per recv, 0 = everything
    int       opens;
    int       stall_ms;      // clock advance each time recv has nothing
    int       stall_every;   // hand over bytes only every other recv, so the
    int       stalled;       // pump actually yields the way a real socket makes it
    int       never_eof;     // pretend the peer is alive but silent, forever
    int       is_open;
    char      host[8][EOS_BRAIN_HOST_MAX];
    int       nhost;
    char      sent[8192];
    size_t    sent_len;
    uint32_t  now;
} fake_t;

static int fake_open(void *ctx, const char *host, uint16_t port, uint32_t timeout_ms)
{
    fake_t *t = (fake_t *)ctx;
    (void)port; (void)timeout_ms;

    t->opens++;
    if (t->nhost < 8) snprintf(t->host[t->nhost++], EOS_BRAIN_HOST_MAX, "%s", host);
    if (t->cur >= t->n) return -1;
    if (t->attempt[t->cur].refuse) { t->cur++; return -1; }
    t->pos     = 0;
    t->is_open = 1;
    return 0;
}

static int fake_send(void *ctx, const uint8_t *data, size_t len)
{
    fake_t *t = (fake_t *)ctx;
    if (!t->is_open) return -1;
    if (t->sent_len + len < sizeof t->sent) {
        memcpy(t->sent + t->sent_len, data, len);
        t->sent_len += len;
        t->sent[t->sent_len] = 0;
    }
    return (int)len;
}

static int fake_recv(void *ctx, uint8_t *buf, size_t cap)
{
    fake_t *t = (fake_t *)ctx;
    attempt_t *a;
    size_t n;

    if (!t->is_open) return EOS_BRAIN_EOF;
    if (t->stall_every) { t->stalled ^= 1; if (t->stalled) return 0; }
    a = &t->attempt[t->cur];
    if (t->pos >= a->len) {
        if (t->never_eof) { t->now += (uint32_t)t->stall_ms; return 0; }
        return EOS_BRAIN_EOF;
    }
    n = a->len - t->pos;
    if (t->drip && n > t->drip) n = t->drip;
    if (n > cap) n = cap;
    memcpy(buf, a->data + t->pos, n);
    t->pos += n;
    return (int)n;
}

static void fake_close(void *ctx)
{
    fake_t *t = (fake_t *)ctx;
    if (t->is_open) { t->is_open = 0; t->cur++; }
}

static uint32_t fake_now(void *ctx) { return ((fake_t *)ctx)->now; }

static void fake_reset(fake_t *t)
{
    memset(t, 0, sizeof *t);
}

// ------------------------------------------------------------ hook doubles

typedef struct {
    char cached[EOS_BRAIN_HOST_MAX];
    char mdns[EOS_BRAIN_HOST_MAX];
    char saved[EOS_BRAIN_HOST_MAX];
    int  mdns_calls, load_calls, save_calls;
} hookctx_t;

static int h_mdns(void *ctx, const char *name, char *ip, size_t cap)
{
    hookctx_t *h = (hookctx_t *)ctx;
    (void)name;
    h->mdns_calls++;
    if (!h->mdns[0]) return -1;
    snprintf(ip, cap, "%s", h->mdns);
    return 0;
}

static int h_load(void *ctx, char *host, size_t cap)
{
    hookctx_t *h = (hookctx_t *)ctx;
    h->load_calls++;
    if (!h->cached[0]) return -1;
    snprintf(host, cap, "%s", h->cached);
    return 0;
}

static void h_save(void *ctx, const char *host)
{
    hookctx_t *h = (hookctx_t *)ctx;
    h->save_calls++;
    snprintf(h->saved, sizeof h->saved, "%s", host);
}

// ------------------------------------------------------------ service test

typedef struct {
    char  order[256];   // one letter per event, in order
    char  text[2048];
    size_t len;
    int   done_calls;
    eos_brain_err_t done_err;
    int   links[16];
    int   nlinks;
} watch_t;

static void w_put(watch_t *w, char c)
{
    size_t n = strlen(w->order);
    if (n + 1 < sizeof w->order) { w->order[n] = c; w->order[n + 1] = 0; }
}

static void w_event(void *user, const eos_brain_evt_t *ev)
{
    watch_t *w = (watch_t *)user;
    switch (ev->kind) {
    case EOS_BRAIN_EV_LINK:
        if (w->nlinks < 16) w->links[w->nlinks++] = (int)ev->link;
        break;
    case EOS_BRAIN_EV_STATE:                       break;
    case EOS_BRAIN_EV_SUBMITTED:   w_put(w, 'S');  break;
    case EOS_BRAIN_EV_FIRST_TOKEN: w_put(w, 'F');  break;
    case EOS_BRAIN_EV_TOKEN:       w_put(w, 't');  break;
    case EOS_BRAIN_EV_DONE:        w_put(w, 'D');  break;
    case EOS_BRAIN_EV_FAILED:      w_put(w, 'X');  break;
    case EOS_BRAIN_EV_CANCELLED:   w_put(w, 'C');  break;
    }
}

static void w_token(void *user, const char *text, size_t len)
{
    watch_t *w = (watch_t *)user;
    if (w->len + len < sizeof w->text) {
        memcpy(w->text + w->len, text, len);
        w->len += len;
        w->text[w->len] = 0;
    }
}

static void w_done(void *user, eos_brain_err_t err)
{
    watch_t *w = (watch_t *)user;
    w->done_calls++;
    w->done_err = err;
}

static const eos_brain_transport_t *fake_tp(fake_t *t, eos_brain_transport_t *tp)
{
    tp->ctx    = t;
    tp->open   = fake_open;
    tp->send   = fake_send;
    tp->recv   = fake_recv;
    tp->close  = fake_close;
    tp->now_ms = fake_now;
    return tp;
}

static int spin(eos_brain_t *b)
{
    int n = 0;
    while (eos_brain_pump(b, 5) && ++n < 5000) { }
    return n;
}

static const char HEALTH_OK[] =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
    "Content-Length: 22\r\n\r\n{\"ok\":true,\"up\":12345}";
static const char HEALTH_404[] =
    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
static const char ASK_OK[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
    "Transfer-Encoding: chunked\r\n\r\n"
    "6\r\nHello \r\n5\r\nthere\r\n0\r\n\r\n";
static const char ASK_500[] =
    "HTTP/1.1 500 Internal Server Error\r\nTransfer-Encoding: chunked\r\n\r\n"
    "9\r\nno model!\r\n0\r\n\r\n";

static void arm(fake_t *t, int i, const char *s) { t->attempt[i].data = s; t->attempt[i].len = strlen(s); }

static void test_service_happy(void)
{
    fake_t t;
    eos_brain_transport_t tp;
    hookctx_t h;
    eos_brain_hooks_t hooks = { NULL, h_mdns, h_load, h_save };
    eos_brain_t b;
    watch_t w;
    eos_brain_req_t r;
    int32_t id;

    printf("  service: discovery, request, streaming reply\n");

    fake_reset(&t);
    t.drip = 3;                       // hand over three bytes at a time
    arm(&t, 0, HEALTH_OK);
    arm(&t, 1, ASK_OK);
    t.n = 2;

    memset(&h, 0, sizeof h);
    memset(&w, 0, sizeof w);
    hooks.ctx = &h;

    eos_brain_init(&b, fake_tp(&t, &tp), NULL, &hooks);
    eos_brain_set_event_cb(&b, w_event, &w);

    memset(&r, 0, sizeof r);
    r.prompt     = "hello there";
    r.system     = EOS_BRAIN_SYSTEM_TINY;
    r.model      = EOS_BRAIN_DEFAULT_MODEL;
    r.max_tokens = 350;
    r.on_token   = w_token;
    r.on_done    = w_done;
    r.user       = &w;

    id = eos_brain_submit(&b, &r);
    CK(id > 0, "submit returns a request id");
    CK(eos_brain_busy(&b), "submit leaves the service busy");
    CK(t.opens == 0, "submit itself touches no network");

    spin(&b);
    CK(!eos_brain_busy(&b), "the pump runs the request to completion");
    CK(w.done_calls == 1 && w.done_err == EOS_BRAIN_OK, "completion fires exactly once, with OK");
    CKS(w.text, "Hello there", "tokens arrive decoded and in order");
    CK(strncmp(w.order, "SFt", 3) == 0, "events run submitted, first-token, token...");
    CK(w.order[strlen(w.order) - 1] == 'D', "...and end with done");
    CK(strchr(w.order, 'X') == NULL, "no failure event on the happy path");
    CK(eos_brain_link(&b) == EOS_BRAIN_LINK_UP, "the link reads up afterwards");
    CK(eos_brain_state(&b) == EOS_BRAIN_ST_IDLE, "the state falls back to idle");

    CK(t.opens == 2, "one connection for the health probe, one for the ask");
    CKS(t.host[0], EOS_BRAIN_FALLBACK_HOST, "with no cache and no mDNS it lands on the fallback IP");
    CKS(h.saved, EOS_BRAIN_FALLBACK_HOST, "the host that worked is written to the cache");
    CK(h.save_calls == 1, "the cache is written once, not per chunk");

    CK(strstr(t.sent, "GET /health HTTP/1.1") != NULL, "the probe is a GET on /health");
    CK(strstr(t.sent, "q=hello%20there") != NULL, "the prompt is percent-encoded into the query");
    CK(strstr(t.sent, "model=qwen3.5%3A2b") != NULL, "the model is passed through encoded");
    CK(strstr(t.sent, "stream=1") != NULL, "streaming is requested");
    CK(strstr(t.sent, "&max=350") != NULL, "the token cap is passed through");

    // Second request inside the link ttl must skip the probe entirely.
    fake_reset(&t);
    arm(&t, 0, ASK_OK);
    t.n = 1;
    memset(&w, 0, sizeof w);
    b.link  = EOS_BRAIN_LINK_UP;      // (the pump already set this; be explicit)
    id = eos_brain_submit(&b, &r);
    CK(id > 0, "a second request is accepted once the first finished");
    spin(&b);
    CK(t.opens == 1, "inside the link ttl the health probe is skipped");
    CKS(w.text, "Hello there", "the shortcut path still streams correctly");
}

static void test_service_discovery(void)
{
    fake_t t;
    eos_brain_transport_t tp;
    hookctx_t h;
    eos_brain_hooks_t hooks = { NULL, h_mdns, h_load, h_save };
    eos_brain_t b;
    watch_t w;
    eos_brain_req_t r;

    printf("  service: discovery walks cache, mDNS, then the known IP\n");

    fake_reset(&t);
    t.attempt[0].refuse = 1;          // the cached address is stale: refused
    arm(&t, 1, HEALTH_404);           // mDNS points at something that is not megabrain
    arm(&t, 2, HEALTH_OK);            // the compiled-in fallback answers properly
    arm(&t, 3, ASK_OK);
    t.n = 4;

    memset(&h, 0, sizeof h);
    snprintf(h.cached, sizeof h.cached, "10.0.0.7");
    snprintf(h.mdns,   sizeof h.mdns,   "192.168.0.55");
    hooks.ctx = &h;
    memset(&w, 0, sizeof w);

    eos_brain_init(&b, fake_tp(&t, &tp), NULL, &hooks);
    eos_brain_set_event_cb(&b, w_event, &w);

    memset(&r, 0, sizeof r);
    r.prompt   = "ping";
    r.on_token = w_token;
    r.on_done  = w_done;
    r.user     = &w;

    CK(eos_brain_submit(&b, &r) > 0, "submit accepted");
    spin(&b);

    CKS(t.host[0], "10.0.0.7", "the cached host is tried first, before paying for mDNS");
    CKS(t.host[1], "192.168.0.55", "then the mDNS answer");
    CKS(t.host[2], EOS_BRAIN_FALLBACK_HOST, "then the compiled-in fallback");
    CK(h.mdns_calls == 1, "mDNS is queried once, and only after the cache failed");
    CK(w.done_err == EOS_BRAIN_OK, "the request completes on the third candidate");
    CKS(w.text, "Hello there", "and streams normally");
    CKS(h.saved, EOS_BRAIN_FALLBACK_HOST, "the cache is rewritten to the host that worked");
    CK(eos_brain_link(&b) == EOS_BRAIN_LINK_UP, "link is up");

    // Nothing answers anywhere.
    fake_reset(&t);
    t.attempt[0].refuse = 1;
    t.attempt[1].refuse = 1;
    t.attempt[2].refuse = 1;
    t.n = 3;
    memset(&w, 0, sizeof w);
    eos_brain_init(&b, fake_tp(&t, &tp), NULL, &hooks);
    eos_brain_set_event_cb(&b, w_event, &w);
    CK(eos_brain_submit(&b, &r) > 0, "submit accepted with the mini switched off");
    spin(&b);
    CK(w.done_err == EOS_BRAIN_ERR_NO_HOST, "every candidate refusing gives NO_HOST");
    CK(eos_brain_link(&b) == EOS_BRAIN_LINK_DOWN, "and the status bar can read the link as down");
    CK(t.opens == 3, "each candidate was tried exactly once");
    CK(strchr(w.order, 'X') != NULL, "a failure event was raised");
    CK(w.len == 0, "no text was invented");

    // A duplicate cache entry must not be dialled twice.
    fake_reset(&t);
    t.attempt[0].refuse = 1;
    t.attempt[1].refuse = 1;
    t.n = 2;
    memset(&h, 0, sizeof h);
    snprintf(h.cached, sizeof h.cached, EOS_BRAIN_FALLBACK_HOST);
    snprintf(h.mdns,   sizeof h.mdns,   EOS_BRAIN_FALLBACK_HOST);
    eos_brain_init(&b, fake_tp(&t, &tp), NULL, &hooks);
    memset(&w, 0, sizeof w);
    eos_brain_set_event_cb(&b, w_event, &w);
    eos_brain_submit(&b, &r);
    spin(&b);
    CK(t.opens == 1, "the same address discovered three ways is dialled once");
}

static void test_service_failures(void)
{
    fake_t t;
    eos_brain_transport_t tp;
    eos_brain_t b;
    watch_t w;
    eos_brain_req_t r;
    eos_brain_cfg_t cfg;

    printf("  service: cancel, timeout, http errors, refusals\n");

    memset(&r, 0, sizeof r);
    r.prompt   = "hello there";
    r.on_token = w_token;
    r.on_done  = w_done;

    // Non-200 on the ask itself.
    fake_reset(&t);
    arm(&t, 0, HEALTH_OK);
    arm(&t, 1, ASK_500);
    t.n = 2;
    memset(&w, 0, sizeof w);
    r.user = &w;
    eos_brain_init(&b, fake_tp(&t, &tp), NULL, NULL);
    eos_brain_set_event_cb(&b, w_event, &w);
    eos_brain_submit(&b, &r);
    spin(&b);
    CK(w.done_err == EOS_BRAIN_ERR_HTTP, "a 500 on the ask is reported as an http error");
    CK(w.len == 0, "the error body is never delivered as model output");
    CK(strchr(w.order, 't') == NULL, "and raises no token events");

    // The connection dies part way through the answer.
    fake_reset(&t);
    arm(&t, 0, HEALTH_OK);
    t.attempt[1].data = ASK_OK;
    t.attempt[1].len  = 97;            // cut after "Hello " but before the rest
    t.n = 2;
    memset(&w, 0, sizeof w);
    eos_brain_init(&b, fake_tp(&t, &tp), NULL, NULL);
    eos_brain_set_event_cb(&b, w_event, &w);
    eos_brain_submit(&b, &r);
    spin(&b);
    CK(w.done_err == EOS_BRAIN_ERR_TRUNCATED, "a dropped connection is reported as truncation");
    CK(w.len > 0, "the text that did arrive was still shown");
    CK(strncmp("Hello there", w.text, w.len) == 0, "and it is the real prefix of the answer");

    // Cancel mid-stream.
    fake_reset(&t);
    arm(&t, 0, HEALTH_OK);
    arm(&t, 1, ASK_OK);
    t.drip = 1;
    t.stall_every = 1;
    t.n = 2;
    memset(&w, 0, sizeof w);
    eos_brain_init(&b, fake_tp(&t, &tp), NULL, NULL);
    eos_brain_set_event_cb(&b, w_event, &w);
    eos_brain_submit(&b, &r);
    {
        int n = 0;
        while (eos_brain_busy(&b) && w.len == 0 && n < 2000) { eos_brain_pump(&b, 1); n++; }
    }
    CK(w.len > 0 && eos_brain_busy(&b), "the stream is genuinely mid-flight before the cancel");
    eos_brain_cancel(&b);
    spin(&b);
    CK(!eos_brain_busy(&b), "cancel releases the service");
    CK(w.done_err == EOS_BRAIN_ERR_CANCELLED, "cancel reports CANCELLED to the caller");
    CK(strchr(w.order, 'C') != NULL, "and raises a cancelled event");
    CK(eos_brain_submit(&b, &r) > 0, "the service accepts work again after a cancel");
    eos_brain_cancel(&b);
    spin(&b);

    // A server that connects, answers nothing, and never hangs up.
    memset(&cfg, 0, sizeof cfg);
    cfg.idle_ms  = 500;
    cfg.total_ms = 2000;
    fake_reset(&t);
    arm(&t, 0, HEALTH_OK);
    t.attempt[1].data = "";
    t.attempt[1].len  = 0;
    t.n = 2;
    t.never_eof = 1;
    t.stall_ms  = 50;
    memset(&w, 0, sizeof w);
    eos_brain_init(&b, fake_tp(&t, &tp), &cfg, NULL);
    eos_brain_set_event_cb(&b, w_event, &w);
    eos_brain_submit(&b, &r);
    spin(&b);
    CK(!eos_brain_busy(&b), "a silent server does not hang the pump forever");
    CK(w.done_err == EOS_BRAIN_ERR_TIMEOUT, "it times out instead");

    // Argument checks.
    fake_reset(&t);
    arm(&t, 0, HEALTH_OK);
    arm(&t, 1, ASK_OK);
    t.n = 2;
    eos_brain_init(&b, fake_tp(&t, &tp), NULL, NULL);
    CK(eos_brain_submit(&b, NULL) < 0, "a NULL request is refused");
    {
        eos_brain_req_t bad;
        memset(&bad, 0, sizeof bad);
        bad.prompt = "";
        CK(eos_brain_submit(&b, &bad) < 0, "an empty prompt is refused");
        {
            static char huge[EOS_BRAIN_PROMPT_MAX + 64];
            memset(huge, 'x', sizeof huge - 1);
            huge[sizeof huge - 1] = 0;
            bad.prompt = huge;
            CK(eos_brain_submit(&b, &bad) == -(int32_t)EOS_BRAIN_ERR_TOO_LONG,
               "a prompt larger than the fixed buffer is refused, not truncated");
        }
    }
    memset(&w, 0, sizeof w);
    r.user = &w;
    CK(eos_brain_submit(&b, &r) > 0, "a good request is accepted");
    CK(eos_brain_submit(&b, &r) == -(int32_t)EOS_BRAIN_ERR_BUSY,
       "a second request while one is in flight is refused as busy");
    spin(&b);
    CK(w.done_calls == 1, "the completion callback still fired exactly once");

    // A standalone health probe, which is what the status bar polls with.
    fake_reset(&t);
    arm(&t, 0, HEALTH_OK);
    t.n = 1;
    memset(&w, 0, sizeof w);
    eos_brain_init(&b, fake_tp(&t, &tp), NULL, NULL);
    eos_brain_set_event_cb(&b, w_event, &w);
    CK(eos_brain_probe(&b) > 0, "a bare probe is accepted");
    spin(&b);
    CK(eos_brain_link(&b) == EOS_BRAIN_LINK_UP, "a good probe brings the link up");
    CK(w.len == 0 && strchr(w.order, 't') == NULL, "the health JSON is never shown as model output");
    CK(w.done_calls == 0, "a probe does not fire the request completion callback");
    CK(w.nlinks >= 1 && w.links[w.nlinks - 1] == EOS_BRAIN_LINK_UP,
       "the link transition is delivered as an event");
}

int main(void)
{
    printf("\neos_brain host test\n\n");
    printf("  sizeof(eos_brain_parser_t) = %u bytes\n", (unsigned)sizeof(eos_brain_parser_t));
    printf("  sizeof(eos_brain_t)        = %u bytes\n\n", (unsigned)sizeof(eos_brain_t));

    test_utf8();
    test_urlencode();
    test_build();
    test_parser_nominal();
    test_parser_every_split();
    test_parser_utf8_splits();
    test_parser_invalid_utf8();
    test_parser_truncation();
    test_parser_edges();
    test_parser_garbage();
    test_service_happy();
    test_service_discovery();
    test_service_failures();

    printf("\n=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
