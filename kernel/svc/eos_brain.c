#include "eos_brain.h"
#include <string.h>

// The parser is the whole point of this file, so it comes first and it does not
// know that sockets exist. Everything below it — the request builder, the
// discovery walk, the pump — is bookkeeping around handing it bytes.

// ------------------------------------------------------------------- names

const char *eos_brain_err_name(eos_brain_err_t e)
{
    switch (e) {
    case EOS_BRAIN_OK:            return "ok";
    case EOS_BRAIN_ERR_ARG:       return "bad-arg";
    case EOS_BRAIN_ERR_BUSY:      return "busy";
    case EOS_BRAIN_ERR_TOO_LONG:  return "too-long";
    case EOS_BRAIN_ERR_NO_HOST:   return "no-host";
    case EOS_BRAIN_ERR_CONNECT:   return "connect";
    case EOS_BRAIN_ERR_SEND:      return "send";
    case EOS_BRAIN_ERR_TIMEOUT:   return "timeout";
    case EOS_BRAIN_ERR_HTTP:      return "http";
    case EOS_BRAIN_ERR_PROTOCOL:  return "protocol";
    case EOS_BRAIN_ERR_TRUNCATED: return "truncated";
    case EOS_BRAIN_ERR_CANCELLED: return "cancelled";
    }
    return "?";
}

const char *eos_brain_link_name(eos_brain_link_t l)
{
    switch (l) {
    case EOS_BRAIN_LINK_UNKNOWN: return "unknown";
    case EOS_BRAIN_LINK_TRYING:  return "trying";
    case EOS_BRAIN_LINK_UP:      return "up";
    case EOS_BRAIN_LINK_DOWN:    return "down";
    }
    return "?";
}

const char *eos_brain_state_name(eos_brain_state_t s)
{
    switch (s) {
    case EOS_BRAIN_ST_IDLE:       return "idle";
    case EOS_BRAIN_ST_RESOLVING:  return "resolving";
    case EOS_BRAIN_ST_CONNECTING: return "connecting";
    case EOS_BRAIN_ST_SENDING:    return "sending";
    case EOS_BRAIN_ST_STREAMING:  return "streaming";
    }
    return "?";
}

// -------------------------------------------------------------------- utf8

// Scanning back at most four bytes is enough: that is the longest sequence
// UTF-8 can produce, so a lead byte further back than that is already complete.
// An invalid lead (0xF8..0xFF) counts as one byte and passes straight through —
// garbage in the stream must not stall the parser forever.
size_t eos_brain_utf8_safe_len(const char *buf, size_t len)
{
    size_t i = len, back = 0;

    if (!buf) return 0;
    while (i > 0 && back < 4) {
        unsigned char c = (unsigned char)buf[i - 1];
        size_t need;
        i--;
        back++;
        if ((c & 0xC0) == 0x80) continue;          // continuation, keep walking
        if (c < 0x80)             need = 1;
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else                      need = 1;        // invalid lead: one raw byte
        return (back >= need) ? len : i;
    }
    return len;   // nothing but continuation bytes, or nothing at all
}

// ------------------------------------------------------------------ parser

enum {
    P_STATUS = 0, P_HEADER, P_CHUNK_SIZE, P_CHUNK_DATA, P_CHUNK_TAIL,
    P_TRAILER, P_LEN_BODY, P_EOF_BODY, P_DONE, P_ERR
};

enum { BODY_EOF = 0, BODY_CHUNKED = 1, BODY_LENGTH = 2 };

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        char x = lower(*a++), y = lower(*b++);
        if (x != y) return x - y;
    }
    return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

static bool ci_has(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return true;
    for (; *hay; hay++) {
        size_t i;
        for (i = 0; i < n; i++) {
            if (!hay[i] || lower(hay[i]) != lower(needle[i])) break;
        }
        if (i == n) return true;
    }
    return false;
}

static void fail(eos_brain_parser_t *p, eos_brain_err_t e)
{
    p->err = (uint8_t)e;
    p->st  = P_ERR;
}

// Emits everything that ends on a character boundary and keeps the rest.
static void flush_text(eos_brain_parser_t *p)
{
    size_t n;

    if (!p->buf_len) return;
    n = eos_brain_utf8_safe_len(p->buf, p->buf_len);
    // A full buffer that would hold everything back cannot be allowed to stall;
    // it can only happen if EOS_BRAIN_TEXT_MAX were made absurdly small.
    if (n == 0 && p->buf_len >= EOS_BRAIN_TEXT_MAX) n = p->buf_len;
    if (!n) return;

    p->text_bytes += (uint32_t)n;
    if (p->on_text) p->on_text(p->user, p->buf, n);
    if (n < p->buf_len) memmove(p->buf, p->buf + n, (size_t)p->buf_len - n);
    p->buf_len = (uint8_t)(p->buf_len - n);
}

static void stage(eos_brain_parser_t *p, const char *src, size_t n)
{
    while (n) {
        size_t room, take;
        if (p->buf_len >= EOS_BRAIN_TEXT_MAX) flush_text(p);
        room = EOS_BRAIN_TEXT_MAX - p->buf_len;
        take = (n < room) ? n : room;
        memcpy(p->buf + p->buf_len, src, take);
        p->buf_len = (uint8_t)(p->buf_len + take);
        src += take;
        n   -= take;
        if (p->buf_len >= EOS_BRAIN_TEXT_MAX) flush_text(p);
    }
}

// Returns 1 once a full line has landed in p->line, NUL terminated and with the
// CR stripped. Over-long lines are truncated and flagged rather than overrun.
static int line_push(eos_brain_parser_t *p, char c)
{
    if (c == '\n') {
        if (p->line_len && p->line[p->line_len - 1] == '\r') p->line_len--;
        p->line[p->line_len] = 0;
        return 1;
    }
    if ((size_t)p->line_len + 1 < EOS_BRAIN_LINE_MAX) p->line[p->line_len++] = c;
    else p->line_ovf = true;
    return 0;
}

static void line_reset(eos_brain_parser_t *p)
{
    p->line_len = 0;
    p->line_ovf = false;
    p->line[0]  = 0;
}

static int parse_status_line(const char *s)
{
    int code = 0, i;

    if (strncmp(s, "HTTP/", 5) != 0) return -1;
    while (*s && *s != ' ') s++;
    while (*s == ' ') s++;
    for (i = 0; i < 3; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        code = code * 10 + (s[i] - '0');
    }
    return code;
}

// Hex size line, chunk extensions after ';' ignored. Anything else is garbage.
static int parse_chunk_size(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int digits = 0;

    for (; *s; s++) {
        int d;
        char c = *s;
        if (c == ';' || c == ' ' || c == '\t') break;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        if (++digits > 8) return -1;
        v = (v << 4) | (uint32_t)d;
    }
    if (!digits) return -1;
    *out = v;
    return 0;
}

static void scan_header(eos_brain_parser_t *p)
{
    char *colon = strchr(p->line, ':');
    char *val;
    char save;

    if (!colon) return;
    save   = *colon;
    *colon = 0;
    val    = colon + 1;
    while (*val == ' ' || *val == '\t') val++;

    if (ci_cmp(p->line, "transfer-encoding") == 0) {
        if (ci_has(val, "chunked")) p->body = BODY_CHUNKED;
    } else if (ci_cmp(p->line, "content-length") == 0) {
        uint32_t v = 0;
        const char *q = val;
        int digits = 0;
        for (; *q >= '0' && *q <= '9'; q++) {
            if (v > 0xFFFFFFFFu / 10u) { digits = 0; break; }
            v = v * 10u + (uint32_t)(*q - '0');
            digits++;
        }
        if (digits && p->body != BODY_CHUNKED) {
            p->body      = BODY_LENGTH;
            p->body_left = v;
        }
    }
    *colon = save;
}

void eos_brain_parser_init(eos_brain_parser_t *p, eos_brain_text_fn on_text, void *user)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->on_text = on_text;
    p->user    = user;
}

eos_brain_parse_t eos_brain_parser_feed(eos_brain_parser_t *p, const uint8_t *data, size_t len)
{
    size_t i = 0;

    if (!p) return EOS_BRAIN_PARSE_ERROR;
    if (p->st == P_ERR)  return EOS_BRAIN_PARSE_ERROR;
    if (p->st == P_DONE) return EOS_BRAIN_PARSE_DONE;
    if (!data && len)    { fail(p, EOS_BRAIN_ERR_ARG); return EOS_BRAIN_PARSE_ERROR; }
    p->fed += (uint32_t)len;

    while (i < len && p->st != P_DONE && p->st != P_ERR) {
        switch (p->st) {

        case P_STATUS:
            if (line_push(p, (char)data[i++])) {
                int code = p->line_ovf ? -1 : parse_status_line(p->line);
                if (code < 0) { fail(p, EOS_BRAIN_ERR_PROTOCOL); break; }
                p->status = (uint16_t)code;
                line_reset(p);
                p->st = P_HEADER;
            }
            break;

        case P_HEADER:
            if (line_push(p, (char)data[i++])) {
                bool blank = (!p->line_ovf && p->line_len == 0);
                if (!blank && !p->line_ovf) scan_header(p);
                line_reset(p);
                if (blank) {
                    if (p->body == BODY_CHUNKED)     p->st = P_CHUNK_SIZE;
                    else if (p->body == BODY_LENGTH) p->st = p->body_left ? P_LEN_BODY : P_DONE;
                    else                             p->st = P_EOF_BODY;
                }
            }
            break;

        case P_CHUNK_SIZE:
            if (line_push(p, (char)data[i++])) {
                uint32_t sz = 0;
                if (p->line_ovf || p->line_len == 0 ||
                    parse_chunk_size(p->line, &sz) != 0 || sz > EOS_BRAIN_CHUNK_LIMIT) {
                    fail(p, EOS_BRAIN_ERR_PROTOCOL);
                    break;
                }
                line_reset(p);
                if (sz == 0) {
                    p->st = P_TRAILER;
                } else {
                    p->chunk_left = sz;
                    p->chunks++;
                    p->st = P_CHUNK_DATA;
                }
            }
            break;

        case P_CHUNK_DATA: {
            size_t avail = len - i;
            size_t take  = (avail < (size_t)p->chunk_left) ? avail : (size_t)p->chunk_left;
            stage(p, (const char *)data + i, take);
            i += take;
            p->chunk_left -= (uint32_t)take;
            if (p->chunk_left == 0) p->st = P_CHUNK_TAIL;
            break;
        }

        case P_CHUNK_TAIL:
            if (line_push(p, (char)data[i++])) {
                if (p->line_ovf || p->line_len != 0) { fail(p, EOS_BRAIN_ERR_PROTOCOL); break; }
                line_reset(p);
                p->st = P_CHUNK_SIZE;
            }
            break;

        case P_TRAILER:
            if (line_push(p, (char)data[i++])) {
                bool blank = (!p->line_ovf && p->line_len == 0);
                line_reset(p);
                if (blank) p->st = P_DONE;
            }
            break;

        case P_LEN_BODY: {
            size_t avail = len - i;
            size_t take  = (avail < (size_t)p->body_left) ? avail : (size_t)p->body_left;
            stage(p, (const char *)data + i, take);
            i += take;
            p->body_left -= (uint32_t)take;
            if (p->body_left == 0) p->st = P_DONE;
            break;
        }

        case P_EOF_BODY:
            stage(p, (const char *)data + i, len - i);
            i = len;
            break;

        default:
            fail(p, EOS_BRAIN_ERR_PROTOCOL);
            break;
        }
    }

    flush_text(p);
    if (p->st == P_ERR)  return EOS_BRAIN_PARSE_ERROR;
    if (p->st == P_DONE) { p->buf_len = 0; return EOS_BRAIN_PARSE_DONE; }
    return EOS_BRAIN_PARSE_MORE;
}

// Shared tail of finish() and abort(). `complete` says whether this particular
// way of losing the socket leaves the response whole.
static eos_brain_parse_t end_of_stream(eos_brain_parser_t *p, bool complete)
{
    if (!p) return EOS_BRAIN_PARSE_ERROR;
    if (p->st == P_ERR)  return EOS_BRAIN_PARSE_ERROR;
    if (p->st == P_DONE) return EOS_BRAIN_PARSE_DONE;

    flush_text(p);
    p->buf_len = 0;             // a dangling partial character can never complete

    if (complete) {
        p->st = P_DONE;
        return EOS_BRAIN_PARSE_DONE;
    }
    fail(p, EOS_BRAIN_ERR_TRUNCATED);
    return EOS_BRAIN_PARSE_ERROR;
}

// Two questions decide whether a stream that stopped arriving is a finished
// reply or a lost one, and they are separate questions. The first is the
// framing's: P_TRAILER is only ever reached by reading the zero-length chunk,
// so every byte of the body is already in hand and the blank line still owed
// carries nothing. The second is the socket's: P_EOF_BODY has no framing at
// all, so the orderly close is the only thing that can say the reply ended
// there rather than was cut off. Everything else — mid-chunk, mid-size line,
// mid-header, short of a content-length — is a body with known bytes missing,
// and is truncated no matter how the connection ended.
eos_brain_parse_t eos_brain_parser_finish(eos_brain_parser_t *p)
{
    if (!p) return EOS_BRAIN_PARSE_ERROR;
    return end_of_stream(p, p->st == P_EOF_BODY || p->st == P_TRAILER);
}

eos_brain_parse_t eos_brain_parser_abort(eos_brain_parser_t *p)
{
    if (!p) return EOS_BRAIN_PARSE_ERROR;
    return end_of_stream(p, p->st == P_TRAILER);
}

bool eos_brain_parser_body_complete(const eos_brain_parser_t *p)
{
    return p && (p->st == P_DONE || p->st == P_TRAILER);
}

int             eos_brain_parser_status(const eos_brain_parser_t *p) { return p ? p->status : 0; }
eos_brain_err_t eos_brain_parser_error(const eos_brain_parser_t *p)  { return p ? (eos_brain_err_t)p->err : EOS_BRAIN_ERR_ARG; }
bool            eos_brain_parser_done(const eos_brain_parser_t *p)   { return p && p->st == P_DONE; }

// ------------------------------------------------------------ string build

typedef struct { char *p; size_t cap, len; bool ovf; } sb_t;

static void sb_init(sb_t *s, char *p, size_t cap)
{
    s->p = p; s->cap = cap; s->len = 0; s->ovf = (cap == 0);
    if (cap) p[0] = 0;
}

static void sb_putc(sb_t *s, char c)
{
    if (s->ovf) return;
    if (s->len + 1 >= s->cap) { s->ovf = true; return; }
    s->p[s->len++] = c;
    s->p[s->len]   = 0;
}

static void sb_puts(sb_t *s, const char *t)
{
    if (!t) return;
    while (*t) sb_putc(s, *t++);
}

static void sb_putu(sb_t *s, uint32_t v)
{
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v);
    while (n) sb_putc(s, t[--n]);
}

static const char HEXU[] = "0123456789ABCDEF";

static bool unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

static void sb_putenc(sb_t *s, const char *t)
{
    if (!t) return;
    for (; *t; t++) {
        unsigned char c = (unsigned char)*t;
        if (unreserved(c)) {
            sb_putc(s, (char)c);
        } else {
            sb_putc(s, '%');
            sb_putc(s, HEXU[c >> 4]);
            sb_putc(s, HEXU[c & 0x0F]);
        }
    }
}

size_t eos_brain_urlencode(const char *src, char *dst, size_t cap)
{
    size_t need = 0, out = 0;
    bool full = false;

    if (dst && cap) dst[0] = 0;
    if (!src) return 0;

    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        size_t w = unreserved(c) ? 1u : 3u;
        need += w;
        if (!dst || full) continue;
        // Stop writing entirely the first time something does not fit, so the
        // output is always a prefix of the true encoding and never ends inside
        // a %XX triplet.
        if (out + w + 1 > cap) { full = true; continue; }
        if (w == 1) {
            dst[out++] = (char)c;
        } else {
            dst[out++] = '%';
            dst[out++] = HEXU[c >> 4];
            dst[out++] = HEXU[c & 0x0F];
        }
    }
    if (dst && cap) dst[out] = 0;
    return need;
}

static void append_query(sb_t *s, const eos_brain_req_t *req, eos_brain_method_t m)
{
    sb_puts(s, EOS_BRAIN_PATH_ASK);
    sb_puts(s, "?stream=1");
    if (req->max_tokens > 0) {
        sb_puts(s, "&max=");
        sb_putu(s, (uint32_t)req->max_tokens);
    }
    if (req->system && req->system[0]) {
        sb_puts(s, "&system=");
        sb_putenc(s, req->system);
    }
    if (req->model && req->model[0]) {
        sb_puts(s, "&model=");
        sb_putenc(s, req->model);
    }
    if (m != EOS_BRAIN_METHOD_POST) {
        sb_puts(s, "&q=");
        sb_putenc(s, req->prompt);
    }
}

int eos_brain_build_query(const eos_brain_req_t *req, eos_brain_method_t m,
                          char *out, size_t cap)
{
    sb_t s;

    if (!req || !req->prompt || !out) return -1;
    sb_init(&s, out, cap);
    append_query(&s, req, m);
    return s.ovf ? -1 : (int)s.len;
}

static int build_head(const eos_brain_req_t *req, eos_brain_method_t m,
                      const char *host, uint16_t port, char *out, size_t cap)
{
    sb_t s;

    sb_init(&s, out, cap);
    sb_puts(&s, (m == EOS_BRAIN_METHOD_POST) ? "POST " : "GET ");
    append_query(&s, req, m);
    sb_puts(&s, " HTTP/1.1\r\nHost: ");
    sb_puts(&s, host);
    if (port && port != 80) { sb_putc(&s, ':'); sb_putu(&s, port); }
    sb_puts(&s, "\r\nUser-Agent: penguinos/1\r\nAccept: text/plain\r\n");
    // Connection: close is deliberate. The board has no use for keep-alive and
    // the close doubles as a backstop terminator if the framing goes wrong.
    sb_puts(&s, "Connection: close\r\n");
    if (m == EOS_BRAIN_METHOD_POST) {
        sb_puts(&s, "Content-Type: text/plain\r\nContent-Length: ");
        sb_putu(&s, (uint32_t)strlen(req->prompt));
        sb_puts(&s, "\r\n");
    }
    sb_puts(&s, "\r\n");
    return s.ovf ? -1 : (int)s.len;
}

int eos_brain_build_request(const eos_brain_req_t *req, const char *host,
                            uint16_t port, char *out, size_t cap,
                            eos_brain_method_t *used)
{
    eos_brain_method_t m;
    int n;

    if (!req || !req->prompt || !host || !out || !cap) return -1;

    m = req->method;
    if (m == EOS_BRAIN_METHOD_AUTO) {
        n = build_head(req, EOS_BRAIN_METHOD_GET, host, port, out, cap);
        if (n > 0) {
            if (used) *used = EOS_BRAIN_METHOD_GET;
            return n;
        }
        m = EOS_BRAIN_METHOD_POST;   // the prompt outgrew the URL buffer
    }
    n = build_head(req, m, host, port, out, cap);
    if (n > 0 && used) *used = m;
    return n;
}

// ----------------------------------------------------------------- service

enum { S_IDLE = 0, S_RESOLVE, S_OPEN, S_SEND, S_RECV };

static uint32_t now_ms(eos_brain_t *b)
{
    return (b->tp && b->tp->now_ms) ? b->tp->now_ms(b->tp->ctx) : 0;
}

static eos_brain_state_t pub_state(uint8_t st)
{
    switch (st) {
    case S_RESOLVE: return EOS_BRAIN_ST_RESOLVING;
    case S_OPEN:    return EOS_BRAIN_ST_CONNECTING;
    case S_SEND:    return EOS_BRAIN_ST_SENDING;
    case S_RECV:    return EOS_BRAIN_ST_STREAMING;
    default:        return EOS_BRAIN_ST_IDLE;
    }
}

static void emit(eos_brain_t *b, eos_brain_evt_kind_t k,
                 const char *text, size_t len, eos_brain_err_t err)
{
    eos_brain_evt_t ev;

    if (!b->on_event) return;
    ev.kind   = k;
    ev.link   = (eos_brain_link_t)b->link;
    ev.state  = pub_state(b->st);
    ev.req_id = b->req_id;
    ev.host   = b->host;
    ev.text   = text;
    ev.len    = len;
    ev.err    = err;
    b->on_event(b->evt_user, &ev);
}

static void set_link(eos_brain_t *b, eos_brain_link_t l)
{
    if (b->link == (uint8_t)l) return;
    b->link = (uint8_t)l;
    emit(b, EOS_BRAIN_EV_LINK, NULL, 0, EOS_BRAIN_OK);
}

static void set_st(eos_brain_t *b, uint8_t st)
{
    if (b->st == st) return;
    b->st = st;
    emit(b, EOS_BRAIN_EV_STATE, NULL, 0, EOS_BRAIN_OK);
}

static void sock_close(eos_brain_t *b)
{
    if (b->open_sock && b->tp && b->tp->close) b->tp->close(b->tp->ctx);
    b->open_sock = false;
}

static void finish_request(eos_brain_t *b, eos_brain_err_t err)
{
    eos_brain_done_fn done = b->on_done;
    void *user = b->req_user;

    sock_close(b);
    b->pending    = false;
    b->want_probe = false;
    b->probing    = false;
    b->cancel     = false;
    b->last_err   = err;
    set_st(b, S_IDLE);

    if (err == EOS_BRAIN_OK)                  emit(b, EOS_BRAIN_EV_DONE, NULL, 0, err);
    else if (err == EOS_BRAIN_ERR_CANCELLED)  emit(b, EOS_BRAIN_EV_CANCELLED, NULL, 0, err);
    else                                      emit(b, EOS_BRAIN_EV_FAILED, NULL, 0, err);

    // Last, so a resubmit from inside the callback finds the service idle.
    if (done) done(user, err);
}

// Text arrives here from the parser. A health probe's JSON and any non-200
// error body are swallowed: they are not tokens and the terminal must not
// print them as if the model had said them.
static void on_body_text(void *user, const char *text, size_t len)
{
    eos_brain_t *b = (eos_brain_t *)user;

    if (b->probing || b->parser.status != 200) return;
    if (!b->first_token) {
        b->first_token = true;
        emit(b, EOS_BRAIN_EV_FIRST_TOKEN, NULL, 0, EOS_BRAIN_OK);
    }
    if (b->on_token) b->on_token(b->req_user, text, len);
    emit(b, EOS_BRAIN_EV_TOKEN, text, len, EOS_BRAIN_OK);
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (!cap) return;
    if (src) while (src[n] && n + 1 < cap) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

static bool fits(const char *src, size_t cap)
{
    return !src || strlen(src) + 1 <= cap;
}

// Discovery, walked lazily so a working cached host never pays the mDNS
// timeout. Sources in order: NVS cache, mDNS, the compiled-in fallback IP.
static bool next_host(eos_brain_t *b)
{
    char tmp[EOS_BRAIN_HOST_MAX];

    while (b->cand_i < EOS_BRAIN_CANDIDATES) {
        uint8_t src = b->cand_i++;
        uint8_t i;

        tmp[0] = 0;
        if (src == 0) {
            if (b->hooks && b->hooks->cache_load)
                (void)b->hooks->cache_load(b->hooks->ctx, tmp, sizeof tmp);
        } else if (src == 1) {
            if (b->hooks && b->hooks->mdns_lookup)
                (void)b->hooks->mdns_lookup(b->hooks->ctx, b->cfg.mdns_name, tmp, sizeof tmp);
        } else {
            copy_str(tmp, sizeof tmp, b->cfg.fallback_host);
        }
        tmp[sizeof tmp - 1] = 0;
        if (!tmp[0]) continue;

        for (i = 0; i < b->cand_n; i++)
            if (strcmp(b->cand[i], tmp) == 0) break;
        if (i < b->cand_n) continue;                 // already tried this address

        if (b->cand_n < EOS_BRAIN_CANDIDATES)
            copy_str(b->cand[b->cand_n++], EOS_BRAIN_HOST_MAX, tmp);
        copy_str(b->host, sizeof b->host, tmp);
        return true;
    }
    return false;
}

static bool build_current_request(eos_brain_t *b)
{
    eos_brain_method_t used = EOS_BRAIN_METHOD_GET;
    eos_brain_req_t r;
    int n;

    if (b->probing) {
        sb_t s;
        sb_init(&s, b->out, sizeof b->out);
        sb_puts(&s, "GET " EOS_BRAIN_PATH_HEALTH " HTTP/1.1\r\nHost: ");
        sb_puts(&s, b->host);
        if (b->cfg.port && b->cfg.port != 80) { sb_putc(&s, ':'); sb_putu(&s, b->cfg.port); }
        sb_puts(&s, "\r\nUser-Agent: penguinos/1\r\nConnection: close\r\n\r\n");
        if (s.ovf) return false;
        b->out_len  = (uint16_t)s.len;
        b->body_len = 0;
        return true;
    }

    memset(&r, 0, sizeof r);
    r.prompt     = b->prompt;
    r.system     = b->system[0] ? b->system : NULL;
    r.model      = b->model[0]  ? b->model  : NULL;
    r.max_tokens = b->max_tokens;
    r.method     = (eos_brain_method_t)b->method;

    n = eos_brain_build_request(&r, b->host, b->cfg.port, b->out, sizeof b->out, &used);
    if (n <= 0) return false;
    b->out_len  = (uint16_t)n;
    b->body_len = (used == EOS_BRAIN_METHOD_POST) ? (uint32_t)strlen(b->prompt) : 0u;
    return true;
}

// A response finished (cleanly or not) on the currently open socket.
static bool complete(eos_brain_t *b, eos_brain_parse_t r)
{
    int status = b->parser.status;
    eos_brain_err_t perr = (eos_brain_err_t)b->parser.err;

    sock_close(b);

    if (b->probing) {
        b->probing = false;
        if (r == EOS_BRAIN_PARSE_DONE && status == 200) {
            set_link(b, EOS_BRAIN_LINK_UP);
            b->t_link     = now_ms(b);
            b->want_probe = false;
            if (b->hooks && b->hooks->cache_save)
                b->hooks->cache_save(b->hooks->ctx, b->host);
            if (b->pending) { set_st(b, S_OPEN); return true; }
            set_st(b, S_IDLE);
            return false;
        }
        // Something answered but it is not megabrain. Try the next candidate.
        set_link(b, EOS_BRAIN_LINK_TRYING);
        set_st(b, S_RESOLVE);
        return true;
    }

    if (r == EOS_BRAIN_PARSE_DONE && status == 200) { finish_request(b, EOS_BRAIN_OK); return false; }
    if (r == EOS_BRAIN_PARSE_DONE)                  { finish_request(b, EOS_BRAIN_ERR_HTTP); return false; }
    finish_request(b, perr ? perr : EOS_BRAIN_ERR_PROTOCOL);
    return false;
}

// One slice of work. Returns false to give the CPU back to the OS loop.
static bool step(eos_brain_t *b)
{
    uint32_t now = now_ms(b);

    switch (b->st) {

    case S_IDLE:
        return false;

    case S_RESOLVE:
        // A health probe holds for link_ttl_ms; inside that window an ask goes
        // straight out on the known-good host and costs one round trip, not two.
        if (b->pending && !b->want_probe && b->link == EOS_BRAIN_LINK_UP &&
            b->host[0] && (now - b->t_link) < b->cfg.link_ttl_ms) {
            b->probing = false;
            set_st(b, S_OPEN);
            return true;
        }
        if (!next_host(b)) {
            set_link(b, EOS_BRAIN_LINK_DOWN);
            if (b->pending) { finish_request(b, EOS_BRAIN_ERR_NO_HOST); return false; }
            b->want_probe = false;
            set_st(b, S_IDLE);
            return false;
        }
        set_link(b, EOS_BRAIN_LINK_TRYING);
        b->probing = true;
        set_st(b, S_OPEN);
        return true;

    case S_OPEN:
        if (!b->tp || !b->tp->open || !b->tp->send || !b->tp->recv) {
            finish_request(b, EOS_BRAIN_ERR_ARG);
            return false;
        }
        if (b->tp->open(b->tp->ctx, b->host, b->cfg.port, b->cfg.connect_ms) != 0) {
            b->open_sock = false;
            set_link(b, EOS_BRAIN_LINK_TRYING);   // also kills the ttl shortcut
            set_st(b, S_RESOLVE);
            return true;
        }
        b->open_sock = true;
        eos_brain_parser_init(&b->parser, on_body_text, b);
        if (!build_current_request(b)) {
            finish_request(b, EOS_BRAIN_ERR_TOO_LONG);
            return false;
        }
        b->out_sent  = 0;
        b->body_sent = 0;
        b->t_last    = now;
        set_st(b, S_SEND);
        return true;

    case S_SEND: {
        int n;
        if (b->out_sent < b->out_len) {
            n = b->tp->send(b->tp->ctx, (const uint8_t *)b->out + b->out_sent,
                            (size_t)(b->out_len - b->out_sent));
        } else if (b->body_sent < b->body_len) {
            n = b->tp->send(b->tp->ctx, (const uint8_t *)b->prompt + b->body_sent,
                            (size_t)(b->body_len - b->body_sent));
        } else {
            b->t_last = now;
            set_st(b, S_RECV);
            return true;
        }
        if (n < 0) {
            sock_close(b);
            if (b->probing) {
                b->probing = false;
                set_link(b, EOS_BRAIN_LINK_TRYING);
                set_st(b, S_RESOLVE);
                return true;
            }
            finish_request(b, EOS_BRAIN_ERR_SEND);
            return false;
        }
        if (n == 0) {
            if ((now - b->t_last) >= b->cfg.idle_ms) {
                sock_close(b);
                finish_request(b, EOS_BRAIN_ERR_TIMEOUT);
            }
            return false;                       // socket is not ready; yield
        }
        b->t_last = now;
        if (b->out_sent < b->out_len) b->out_sent  = (uint16_t)(b->out_sent + n);
        else                          b->body_sent += (uint32_t)n;
        return true;
    }

    case S_RECV: {
        uint8_t rx[EOS_BRAIN_RX_MAX];
        eos_brain_parse_t r;
        int n = b->tp->recv(b->tp->ctx, rx, sizeof rx);

        if (n > 0) {
            b->t_last   = now;
            b->rx_bytes += (uint32_t)n;
            r = eos_brain_parser_feed(&b->parser, rx, (size_t)n);
        } else if (n == 0) {
            if ((now - b->t_last) >= b->cfg.idle_ms) {
                // A server that sends the whole reply and then neither closes
                // nor writes the trailer's blank line has still answered. The
                // silence is its keep-alive, not a lost reply, so end on the
                // framing rather than call a finished answer a timeout.
                if (eos_brain_parser_body_complete(&b->parser))
                    return complete(b, eos_brain_parser_finish(&b->parser));
                sock_close(b);
                if (b->probing) {
                    b->probing = false;
                    set_link(b, EOS_BRAIN_LINK_TRYING);
                    set_st(b, S_RESOLVE);
                    return true;
                }
                finish_request(b, EOS_BRAIN_ERR_TIMEOUT);
                return false;
            }
            return false;                       // nothing yet; yield
        } else if (n == EOS_BRAIN_EOF) {
            r = eos_brain_parser_finish(&b->parser);
        } else {
            r = eos_brain_parser_abort(&b->parser);
        }
        if (r == EOS_BRAIN_PARSE_MORE) return true;
        return complete(b, r);
    }

    default:
        set_st(b, S_IDLE);
        return false;
    }
}

void eos_brain_init(eos_brain_t *b, const eos_brain_transport_t *tp,
                    const eos_brain_cfg_t *cfg, const eos_brain_hooks_t *hooks)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->tp    = tp;
    b->hooks = hooks;
    if (cfg) b->cfg = *cfg;
    if (!b->cfg.fallback_host) b->cfg.fallback_host = EOS_BRAIN_FALLBACK_HOST;
    if (!b->cfg.mdns_name)     b->cfg.mdns_name     = EOS_BRAIN_MDNS_NAME;
    if (!b->cfg.port)          b->cfg.port          = 80;
    if (!b->cfg.connect_ms)    b->cfg.connect_ms    = 3000;
    if (!b->cfg.idle_ms)       b->cfg.idle_ms       = 20000;
    if (!b->cfg.total_ms)      b->cfg.total_ms      = 60000;
    if (!b->cfg.link_ttl_ms)   b->cfg.link_ttl_ms   = 15000;
    b->link = EOS_BRAIN_LINK_UNKNOWN;
    b->st   = S_IDLE;
    eos_brain_parser_init(&b->parser, on_body_text, b);
}

void eos_brain_set_event_cb(eos_brain_t *b, eos_brain_event_fn fn, void *user)
{
    if (!b) return;
    b->on_event = fn;
    b->evt_user = user;
}

int32_t eos_brain_submit(eos_brain_t *b, const eos_brain_req_t *req)
{
    if (!b || !req || !req->prompt || !req->prompt[0]) return -(int32_t)EOS_BRAIN_ERR_ARG;
    if (b->pending || b->want_probe)               return -(int32_t)EOS_BRAIN_ERR_BUSY;
    if (!fits(req->prompt, EOS_BRAIN_PROMPT_MAX) ||
        !fits(req->system, EOS_BRAIN_SYSTEM_MAX) ||
        !fits(req->model,  EOS_BRAIN_MODEL_MAX))   return -(int32_t)EOS_BRAIN_ERR_TOO_LONG;

    copy_str(b->prompt, sizeof b->prompt, req->prompt);
    copy_str(b->system, sizeof b->system, req->system);
    copy_str(b->model,  sizeof b->model,  req->model);
    b->max_tokens = req->max_tokens;
    b->method     = (uint8_t)req->method;
    b->timeout_ms = req->timeout_ms ? req->timeout_ms : b->cfg.total_ms;
    b->on_token   = req->on_token;
    b->on_done    = req->on_done;
    b->req_user   = req->user;

    b->pending     = true;
    b->cancel      = false;
    b->first_token = false;
    b->cand_i      = 0;
    b->cand_n      = 0;
    b->rx_bytes    = 0;
    b->last_err    = EOS_BRAIN_OK;
    b->t_start     = now_ms(b);
    b->t_last      = b->t_start;
    b->req_id++;
    if (!b->req_id) b->req_id = 1;

    set_st(b, S_RESOLVE);
    emit(b, EOS_BRAIN_EV_SUBMITTED, NULL, 0, EOS_BRAIN_OK);
    return (int32_t)b->req_id;
}

int32_t eos_brain_probe(eos_brain_t *b)
{
    if (!b) return -(int32_t)EOS_BRAIN_ERR_ARG;
    if (b->pending || b->want_probe) return -(int32_t)EOS_BRAIN_ERR_BUSY;

    b->want_probe = true;
    b->cancel     = false;
    b->cand_i     = 0;
    b->cand_n     = 0;
    b->timeout_ms = b->cfg.total_ms;
    b->t_start    = now_ms(b);
    b->t_last     = b->t_start;
    b->req_id++;
    if (!b->req_id) b->req_id = 1;

    set_st(b, S_RESOLVE);
    return (int32_t)b->req_id;
}

void eos_brain_cancel(eos_brain_t *b)
{
    if (b && (b->pending || b->want_probe)) b->cancel = true;
}

bool eos_brain_pump(eos_brain_t *b, uint32_t budget_ms)
{
    uint32_t t0;
    int guard = 512;   // bytes can arrive faster than a fake clock advances

    if (!b || !b->tp) return false;
    if (!b->pending && !b->want_probe) return false;

    t0 = now_ms(b);
    for (;;) {
        uint32_t now = now_ms(b);

        if (b->cancel) {
            sock_close(b);
            if (b->pending) { finish_request(b, EOS_BRAIN_ERR_CANCELLED); }
            else {
                b->want_probe = false;
                b->probing    = false;
                b->cancel     = false;
                set_st(b, S_IDLE);
            }
            break;
        }
        if ((b->pending || b->want_probe) && b->timeout_ms &&
            (now - b->t_start) >= b->timeout_ms) {
            sock_close(b);
            if (b->pending) finish_request(b, EOS_BRAIN_ERR_TIMEOUT);
            else {
                b->want_probe = false;
                b->probing    = false;
                set_link(b, EOS_BRAIN_LINK_DOWN);
                set_st(b, S_IDLE);
            }
            break;
        }
        if (!b->pending && !b->want_probe) break;
        if (!step(b)) break;
        if (--guard <= 0) break;
        if ((now_ms(b) - t0) >= budget_ms) break;
    }
    return b->pending || b->want_probe;
}

bool              eos_brain_busy(const eos_brain_t *b)       { return b && (b->pending || b->want_probe); }
eos_brain_link_t  eos_brain_link(const eos_brain_t *b)       { return b ? (eos_brain_link_t)b->link : EOS_BRAIN_LINK_UNKNOWN; }
eos_brain_state_t eos_brain_state(const eos_brain_t *b)      { return b ? pub_state(b->st) : EOS_BRAIN_ST_IDLE; }
const char       *eos_brain_host(const eos_brain_t *b)       { return b ? b->host : ""; }
eos_brain_err_t   eos_brain_last_error(const eos_brain_t *b) { return b ? b->last_err : EOS_BRAIN_ERR_ARG; }

// ------------------------------------------------------- platform bindings
//
// Only compiled into an ESP-IDF build. The host test sees none of this, which
// is the point of the transport indirection.

#ifdef ESP_PLATFORM

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include "esp_timer.h"
#include "esp_netif.h"
#include "mdns.h"
#include "nvs.h"

typedef struct { int fd; } brain_sock_t;
static brain_sock_t s_sock = { -1 };

static int brain_sock_open(void *ctx, const char *host, uint16_t port, uint32_t timeout_ms)
{
    brain_sock_t *c = (brain_sock_t *)ctx;
    struct sockaddr_in sa;
    struct timeval tv;
    int fd, flags;

    if (c->fd >= 0) { close(c->fd); c->fd = -1; }

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);

    // Discovery hands us dotted quads, so the normal path never allocates.
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        struct addrinfo hints, *res = NULL;
        char pstr[8];
        memset(&hints, 0, sizeof hints);
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(pstr, sizeof pstr, "%u", (unsigned)port);
        if (getaddrinfo(host, pstr, &hints, &res) != 0 || !res) return -1;
        sa = *(struct sockaddr_in *)res->ai_addr;
        freeaddrinfo(res);
    }

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;

    tv.tv_sec  = (time_t)(timeout_ms / 1000u);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    // The connect blocks, bounded by SO_SNDTIMEO. It is the one place the pump
    // can stall, and connect_ms is what bounds it.
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }

    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    c->fd = fd;
    return 0;
}

static int brain_sock_send(void *ctx, const uint8_t *data, size_t len)
{
    brain_sock_t *c = (brain_sock_t *)ctx;
    int n;

    if (c->fd < 0) return -1;
    n = (int)send(c->fd, data, len, 0);
    if (n >= 0) return n;
    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) return 0;
    return -1;
}

static int brain_sock_recv(void *ctx, uint8_t *buf, size_t cap)
{
    brain_sock_t *c = (brain_sock_t *)ctx;
    int n;

    if (c->fd < 0) return EOS_BRAIN_EOF;
    n = (int)recv(c->fd, buf, cap, 0);
    if (n > 0) return n;
    if (n == 0) return EOS_BRAIN_EOF;
    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) return 0;
    return -2;
}

static void brain_sock_close(void *ctx)
{
    brain_sock_t *c = (brain_sock_t *)ctx;
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
}

static uint32_t brain_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const eos_brain_transport_t s_lwip_tp = {
    &s_sock, brain_sock_open, brain_sock_send, brain_sock_recv,
    brain_sock_close, brain_now_ms
};

const eos_brain_transport_t *eos_brain_lwip_transport(void) { return &s_lwip_tp; }

#define EOS_BRAIN_NVS_NS  "brain"
#define EOS_BRAIN_NVS_KEY "host"

static int idf_mdns_lookup(void *ctx, const char *name, char *ip, size_t cap)
{
    esp_ip4_addr_t addr;
    (void)ctx;

    addr.addr = 0;
    if (mdns_query_a(name, 2000, &addr) != ESP_OK || addr.addr == 0) return -1;
    snprintf(ip, cap, IPSTR, IP2STR(&addr));
    return 0;
}

static int idf_cache_load(void *ctx, char *host, size_t cap)
{
    nvs_handle_t h;
    size_t len = cap;
    (void)ctx;

    host[0] = 0;
    if (nvs_open(EOS_BRAIN_NVS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    if (nvs_get_str(h, EOS_BRAIN_NVS_KEY, host, &len) != ESP_OK) { nvs_close(h); host[0] = 0; return -1; }
    nvs_close(h);
    return host[0] ? 0 : -1;
}

static void idf_cache_save(void *ctx, const char *host)
{
    nvs_handle_t h;
    char cur[EOS_BRAIN_HOST_MAX];
    (void)ctx;

    // Only write when it changed. Flash wear on a value that rarely moves is
    // not worth one erase per prompt.
    if (idf_cache_load(NULL, cur, sizeof cur) == 0 && strcmp(cur, host) == 0) return;
    if (nvs_open(EOS_BRAIN_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, EOS_BRAIN_NVS_KEY, host) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static const eos_brain_hooks_t s_idf_hooks = {
    NULL, idf_mdns_lookup, idf_cache_load, idf_cache_save
};

const eos_brain_hooks_t *eos_brain_idf_hooks(void) { return &s_idf_hooks; }

#endif  // ESP_PLATFORM
