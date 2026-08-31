// eos_httpd — implementation. See eos_httpd.h for why any of this exists.
//
// Read in four parts: a JSON writer that assumes its input is hostile, a JSON
// reader that assumes the same, the pure request router and handlers, and then
// the ESP-IDF bindings behind #ifdef ESP_PLATFORM. Only the last part knows
// what a socket is, which is what lets the other three run under the host test.

#include "eos_httpd.h"

#include <string.h>
#include <stdio.h>

#ifndef EOS_HTTPD_UNUSED
#define EOS_HTTPD_UNUSED(x) ((void)(x))
#endif

// ==========================================================================
// JSON writing
// ==========================================================================

static void jput(eos_json_t *j, char c)
{
    if (j->ovf) return;
    if (j->len >= j->cap) { j->ovf = true; return; }
    j->buf[j->len++] = c;
    j->buf[j->len] = 0;
}

static void jputs(eos_json_t *j, const char *s)
{
    while (*s && !j->ovf) jput(j, *s++);
}

void eos_json_init(eos_json_t *j, char *buf, int cap)
{
    memset(j, 0, sizeof *j);
    j->buf = buf;
    j->cap = cap > 0 ? cap - 1 : 0;   // one byte always reserved for the NUL
    if (buf && cap > 0) buf[0] = 0;
    if (!buf || cap <= 0) j->ovf = true;
}

bool eos_json_ok(const eos_json_t *j) { return j && !j->ovf; }

// A comma is owed by whoever writes the next thing at this level. In an object
// that is always the key; in an array it is the value. Keeping the two apart is
// the only subtlety in the writer.
static bool jlevel_is_obj(const eos_json_t *j)
{
    return j->depth > 0 && (j->isobj & (1u << (j->depth - 1))) != 0;
}

static void jsep(eos_json_t *j)
{
    if (j->depth == 0) return;
    if (!j->first[j->depth - 1]) jput(j, ',');
    j->first[j->depth - 1] = false;
}

// Called before every value. Inside an object the key already paid the comma.
static void jval_pre(eos_json_t *j)
{
    if (j->depth == 0) return;
    if (jlevel_is_obj(j)) return;
    jsep(j);
}

static void jpush(eos_json_t *j, bool obj)
{
    if (j->depth >= EOS_HTTPD_JSON_DEPTH) { j->ovf = true; return; }
    if (obj) j->isobj |=  (1u << j->depth);
    else     j->isobj &= ~(1u << j->depth);
    j->first[j->depth] = true;
    j->depth++;
}

static void jpop(eos_json_t *j)
{
    if (j->depth == 0) { j->ovf = true; return; }
    j->depth--;
}

void eos_json_obj_open(eos_json_t *j) { jval_pre(j); jput(j, '{'); jpush(j, true); }
void eos_json_arr_open(eos_json_t *j) { jval_pre(j); jput(j, '['); jpush(j, false); }
void eos_json_obj_close(eos_json_t *j) { jpop(j); jput(j, '}'); }
void eos_json_arr_close(eos_json_t *j) { jpop(j); jput(j, ']'); }

// ------------------------------------------------------------ UTF-8 repair
//
// A well-formed sequence is copied through byte for byte. Anything that is not
// one — a stray continuation byte, a truncated lead, an overlong encoding, a
// surrogate half, a code point past U+10FFFF — becomes one U+FFFD and the walk
// resumes at the very next byte. Resyncing at the next byte rather than at the
// next lead byte is deliberate: it makes the output length a function of the
// input alone, which is what eos_json_escaped_len has to be able to promise.

static int utf8_run(const unsigned char *s, int n)
{
    unsigned char c = s[0];
    int len;
    unsigned long cp;

    if (c < 0x80) return 1;
    if      ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07u; }
    else return 0;                                  // continuation or 0xF8..0xFF

    if (len > n) return 0;                          // truncated by the buffer end
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (unsigned long)(s[i] & 0x3Fu);
    }
    if (len == 2 && cp < 0x80UL)    return 0;       // overlong
    if (len == 3 && cp < 0x800UL)   return 0;
    if (len == 4 && cp < 0x10000UL) return 0;
    if (cp >= 0xD800UL && cp <= 0xDFFFUL) return 0; // surrogate half
    if (cp > 0x10FFFFUL) return 0;
    return len;
}

static const char *ESC_SHORT(unsigned char c)
{
    switch (c) {
    case '"':  return "\\\"";
    case '\\': return "\\\\";
    case '\b': return "\\b";
    case '\f': return "\\f";
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    default:   return NULL;
    }
}

// One walk, two users: emits when j is non-NULL, always returns the byte count.
static int json_esc(eos_json_t *j, const char *src, int n)
{
    static const char HEX[] = "0123456789abcdef";
    const unsigned char *s = (const unsigned char *)src;
    int out = 0, i = 0;

    if (!src || n < 0) return 0;

    while (i < n) {
        unsigned char c = s[i];
        const char *sh;

        if (c < 0x80) {
            if ((sh = ESC_SHORT(c)) != NULL) {
                if (j) jputs(j, sh);
                out += 2;
                i++;
                continue;
            }
            if (c < 0x20 || c == 0x7F) {
                if (j) {
                    jputs(j, "\\u00");
                    jput(j, HEX[(c >> 4) & 0xF]);
                    jput(j, HEX[c & 0xF]);
                }
                out += 6;
                i++;
                continue;
            }
            if (j) jput(j, (char)c);
            out += 1;
            i++;
            continue;
        }

        int run = utf8_run(s + i, n - i);
        if (run == 0) {
            if (j) { jput(j, (char)0xEF); jput(j, (char)0xBF); jput(j, (char)0xBD); }
            out += 3;
            i += 1;
            continue;
        }
        if (j) for (int k = 0; k < run; k++) jput(j, (char)s[i + k]);
        out += run;
        i += run;
    }
    return out;
}

int eos_json_escaped_len(const char *s, int n) { return json_esc(NULL, s, n); }

void eos_json_strn(eos_json_t *j, const char *s, int n)
{
    jval_pre(j);
    jput(j, '"');
    json_esc(j, s, n);
    jput(j, '"');
}

void eos_json_str(eos_json_t *j, const char *s)
{
    eos_json_strn(j, s ? s : "", s ? (int)strlen(s) : 0);
}

void eos_json_hexn(eos_json_t *j, const void *bytes, int n)
{
    static const char HEX[] = "0123456789abcdef";
    const unsigned char *b = (const unsigned char *)bytes;
    jval_pre(j);
    jput(j, '"');
    for (int i = 0; b && i < n; i++) {
        jput(j, HEX[(b[i] >> 4) & 0xF]);
        jput(j, HEX[b[i] & 0xF]);
    }
    jput(j, '"');
}

void eos_json_int(eos_json_t *j, long v)
{
    char tmp[24];
    int n = 0;
    unsigned long u;
    bool neg = v < 0;

    jval_pre(j);
    u = neg ? (unsigned long)(-(v + 1)) + 1UL : (unsigned long)v;
    do { tmp[n++] = (char)('0' + (int)(u % 10UL)); u /= 10UL; } while (u && n < (int)sizeof tmp);
    if (neg) jput(j, '-');
    while (n > 0) jput(j, tmp[--n]);
}

void eos_json_bool(eos_json_t *j, bool v) { jval_pre(j); jputs(j, v ? "true" : "false"); }
void eos_json_null(eos_json_t *j)         { jval_pre(j); jputs(j, "null"); }

void eos_json_key(eos_json_t *j, const char *key)
{
    jsep(j);
    jput(j, '"');
    json_esc(j, key, key ? (int)strlen(key) : 0);
    jput(j, '"');
    jput(j, ':');
}

void eos_json_kv_strn(eos_json_t *j, const char *k, const char *s, int n)
{ eos_json_key(j, k); eos_json_strn(j, s, n); }
void eos_json_kv_str(eos_json_t *j, const char *k, const char *s)
{ eos_json_key(j, k); eos_json_str(j, s); }
void eos_json_kv_int(eos_json_t *j, const char *k, long v)
{ eos_json_key(j, k); eos_json_int(j, v); }
void eos_json_kv_bool(eos_json_t *j, const char *k, bool v)
{ eos_json_key(j, k); eos_json_bool(j, v); }
void eos_json_kv_null(eos_json_t *j, const char *k)
{ eos_json_key(j, k); eos_json_null(j); }

// ==========================================================================
// JSON reading
// ==========================================================================

typedef struct { const char *p, *end; } jr_t;

static void jr_ws(jr_t *r)
{
    while (r->p < r->end) {
        char c = *r->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') r->p++;
        else break;
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Appends one code point as UTF-8. Returns false when it will not fit.
static bool utf8_put(char *out, int cap, int *len, unsigned long cp)
{
    int need = cp < 0x80UL ? 1 : cp < 0x800UL ? 2 : cp < 0x10000UL ? 3 : 4;
    if (*len + need > cap) return false;
    switch (need) {
    case 1: out[(*len)++] = (char)cp; break;
    case 2: out[(*len)++] = (char)(0xC0 | (cp >> 6));
            out[(*len)++] = (char)(0x80 | (cp & 0x3F)); break;
    case 3: out[(*len)++] = (char)(0xE0 | (cp >> 12));
            out[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[(*len)++] = (char)(0x80 | (cp & 0x3F)); break;
    default: out[(*len)++] = (char)(0xF0 | (cp >> 18));
             out[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
             out[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
             out[(*len)++] = (char)(0x80 | (cp & 0x3F)); break;
    }
    return true;
}

// Reads a string starting at the opening quote. out may be NULL to skip.
// Returns EOS_JSON_FOUND / _BAD / _TOOBIG.
static eos_json_find_t jr_string(jr_t *r, char *out, int out_cap, int *out_len)
{
    int len = 0;

    if (r->p >= r->end || *r->p != '"') return EOS_JSON_BAD;
    r->p++;

    for (;;) {
        if (r->p >= r->end) return EOS_JSON_BAD;         // unterminated
        unsigned char c = (unsigned char)*r->p;

        if (c == '"') { r->p++; break; }

        if (c < 0x20) return EOS_JSON_BAD;               // raw control byte

        if (c != '\\') {
            // Raw byte, invalid UTF-8 included: an SSID is bytes, and repairing
            // it here would change which network the caller asked for.
            if (out) { if (len + 1 > out_cap) return EOS_JSON_TOOBIG; out[len] = (char)c; }
            len++;
            r->p++;
            continue;
        }

        r->p++;
        if (r->p >= r->end) return EOS_JSON_BAD;
        char e = *r->p++;
        char lit = 0;
        switch (e) {
        case '"':  lit = '"';  break;
        case '\\': lit = '\\'; break;
        case '/':  lit = '/';  break;
        case 'b':  lit = '\b'; break;
        case 'f':  lit = '\f'; break;
        case 'n':  lit = '\n'; break;
        case 'r':  lit = '\r'; break;
        case 't':  lit = '\t'; break;
        case 'u':  lit = 0;    break;
        default:   return EOS_JSON_BAD;
        }
        if (e != 'u') {
            if (out) { if (len + 1 > out_cap) return EOS_JSON_TOOBIG; out[len] = lit; }
            len++;
            continue;
        }

        if (r->end - r->p < 4) return EOS_JSON_BAD;
        unsigned long cp = 0;
        for (int i = 0; i < 4; i++) {
            int h = hexval(r->p[i]);
            if (h < 0) return EOS_JSON_BAD;
            cp = (cp << 4) | (unsigned long)h;
        }
        r->p += 4;

        if (cp >= 0xD800UL && cp <= 0xDBFFUL) {          // high surrogate
            if (r->end - r->p >= 6 && r->p[0] == '\\' && r->p[1] == 'u') {
                unsigned long lo = 0;
                bool ok = true;
                for (int i = 0; i < 4; i++) {
                    int h = hexval(r->p[2 + i]);
                    if (h < 0) { ok = false; break; }
                    lo = (lo << 4) | (unsigned long)h;
                }
                if (ok && lo >= 0xDC00UL && lo <= 0xDFFFUL) {
                    cp = 0x10000UL + ((cp - 0xD800UL) << 10) + (lo - 0xDC00UL);
                    r->p += 6;
                } else {
                    cp = 0xFFFDUL;                        // lone high surrogate
                }
            } else {
                cp = 0xFFFDUL;
            }
        } else if (cp >= 0xDC00UL && cp <= 0xDFFFUL) {
            cp = 0xFFFDUL;                                // lone low surrogate
        }

        // An escaped \u0000 would give the value an embedded NUL, and every
        // consumer of an SSID or an address here is a C string. Refuse it
        // rather than hand back a value that is silently shorter than it looks.
        if (cp == 0) return EOS_JSON_BAD;

        if (out) {
            if (!utf8_put(out, out_cap, &len, cp)) return EOS_JSON_TOOBIG;
        } else {
            len += cp < 0x80UL ? 1 : cp < 0x800UL ? 2 : cp < 0x10000UL ? 3 : 4;
        }
    }

    if (out_len) *out_len = len;
    if (out && len < out_cap) out[len] = 0;
    else if (out) return EOS_JSON_TOOBIG;                 // no room for the NUL
    return EOS_JSON_FOUND;
}

#ifndef EOS_JSON_SKIP_DEPTH
#define EOS_JSON_SKIP_DEPTH 16     // nesting a body may contain before it is BAD
#endif

// Steps over one value of any type without descending into it semantically.
static int jr_skip_value(jr_t *r)
{
    int depth = 0;

    jr_ws(r);
    for (;;) {
        if (r->p >= r->end) return -1;
        char c = *r->p;

        if (c == '"') {
            if (jr_string(r, NULL, 0, NULL) != EOS_JSON_FOUND) return -1;
        } else if (c == '{' || c == '[') {
            if (++depth > EOS_JSON_SKIP_DEPTH) return -1;
            r->p++;
        } else if (c == '}' || c == ']') {
            if (depth == 0) return -1;
            depth--;
            r->p++;
        } else if (c == 't' || c == 'f' || c == 'n') {
            const char *lit = c == 't' ? "true" : c == 'f' ? "false" : "null";
            size_t n = strlen(lit);
            if ((size_t)(r->end - r->p) < n || memcmp(r->p, lit, n) != 0) return -1;
            r->p += n;
        } else if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
            const char *s = r->p;
            while (r->p < r->end) {
                char d = *r->p;
                if ((d >= '0' && d <= '9') || d == '-' || d == '+' ||
                    d == '.' || d == 'e' || d == 'E') r->p++;
                else break;
            }
            if (r->p == s) return -1;
        } else if (c == ',' || c == ':') {
            if (depth == 0) return -1;
            r->p++;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            r->p++;
        } else {
            return -1;
        }

        if (depth == 0) { jr_ws(r); return 0; }
    }
}

// The one scanner behind all three getters. On EOS_JSON_FOUND, r->p sits on the
// first byte of the matching value.
static eos_json_find_t jr_seek_key(jr_t *r, const char *key, const char *body, int len)
{
    char kbuf[40];
    size_t klen = key ? strlen(key) : 0;

    if (!body || len < 0 || !key || klen == 0 || klen >= sizeof kbuf) return EOS_JSON_BAD;

    r->p = body;
    r->end = body + len;

    jr_ws(r);
    if (r->p >= r->end || *r->p != '{') return EOS_JSON_BAD;
    r->p++;
    jr_ws(r);
    if (r->p < r->end && *r->p == '}') return EOS_JSON_ABSENT;

    for (;;) {
        int klen_got = 0;
        const char *kstart;
        jr_ws(r);
        kstart = r->p;
        eos_json_find_t ks = jr_string(r, kbuf, (int)sizeof kbuf, &klen_got);
        bool match;
        if (ks == EOS_JSON_TOOBIG) {
            // Longer than any key we look for, so it cannot match — but the
            // read stopped mid-string. Rewind and consume it with no output
            // buffer, or the scan resumes inside the key and desynchronises.
            r->p = kstart;
            if (jr_string(r, NULL, 0, NULL) != EOS_JSON_FOUND) return EOS_JSON_BAD;
            match = false;
        } else if (ks != EOS_JSON_FOUND) {
            return EOS_JSON_BAD;
        } else {
            match = (klen_got == (int)klen) && memcmp(kbuf, key, klen) == 0;
        }

        jr_ws(r);
        if (r->p >= r->end || *r->p != ':') return EOS_JSON_BAD;
        r->p++;
        jr_ws(r);
        if (r->p >= r->end) return EOS_JSON_BAD;

        if (match) return EOS_JSON_FOUND;

        if (jr_skip_value(r) != 0) return EOS_JSON_BAD;
        jr_ws(r);
        if (r->p >= r->end) return EOS_JSON_BAD;
        if (*r->p == ',') { r->p++; continue; }
        if (*r->p == '}') return EOS_JSON_ABSENT;
        return EOS_JSON_BAD;
    }
}

eos_json_find_t eos_json_get_str(const char *body, int len, const char *key,
                                 char *out, int out_cap, int *out_len)
{
    jr_t r;
    eos_json_find_t s;

    if (out_len) *out_len = 0;
    if (out && out_cap > 0) out[0] = 0;
    if (!out || out_cap <= 0) return EOS_JSON_TOOBIG;

    s = jr_seek_key(&r, key, body, len);
    if (s != EOS_JSON_FOUND) return s;
    if (r.p >= r.end || *r.p != '"') return EOS_JSON_TYPE;
    return jr_string(&r, out, out_cap, out_len);
}

eos_json_find_t eos_json_get_int(const char *body, int len, const char *key, long *out)
{
    jr_t r;
    eos_json_find_t s = jr_seek_key(&r, key, body, len);
    bool neg = false;
    unsigned long acc = 0;
    int digits = 0;

    if (s != EOS_JSON_FOUND) return s;
    if (r.p < r.end && (*r.p == '-' || *r.p == '+')) { neg = (*r.p == '-'); r.p++; }
    while (r.p < r.end && *r.p >= '0' && *r.p <= '9') {
        acc = acc * 10UL + (unsigned long)(*r.p - '0');
        if (acc > 2147483647UL) return EOS_JSON_TOOBIG;
        digits++;
        r.p++;
    }
    if (digits == 0) return EOS_JSON_TYPE;
    // A fraction or an exponent is a number, but it is not the integer the
    // caller asked for, and rounding it silently is how a port becomes 0.
    if (r.p < r.end && (*r.p == '.' || *r.p == 'e' || *r.p == 'E')) return EOS_JSON_TYPE;
    if (out) *out = neg ? -(long)acc : (long)acc;
    return EOS_JSON_FOUND;
}

eos_json_find_t eos_json_get_bool(const char *body, int len, const char *key, bool *out)
{
    jr_t r;
    eos_json_find_t s = jr_seek_key(&r, key, body, len);
    size_t left;

    if (s != EOS_JSON_FOUND) return s;
    left = (size_t)(r.end - r.p);
    if (left >= 4 && memcmp(r.p, "true", 4) == 0)  { if (out) *out = true;  return EOS_JSON_FOUND; }
    if (left >= 5 && memcmp(r.p, "false", 5) == 0) { if (out) *out = false; return EOS_JSON_FOUND; }
    return EOS_JSON_TYPE;
}

// ==========================================================================
// URI parsing
// ==========================================================================

// eos_err_t values, spelled locally so this file does not drag in eos_board.h
// on the host. They are checked against the header in the test.
#define HTTPD_ERR_ARG      (-1)
#define HTTPD_ERR_NOTFOUND (-5)
#define HTTPD_ERR_TOOBIG   (-9)

static int pct_decode(const char *s, int n, char *out, int out_cap, bool plus_is_space)
{
    int len = 0;

    for (int i = 0; i < n; i++) {
        char c = s[i];
        int v;
        if (c == '%') {
            int hi, lo;
            if (i + 2 >= n) return HTTPD_ERR_ARG;
            hi = hexval(s[i + 1]);
            lo = hexval(s[i + 2]);
            if (hi < 0 || lo < 0) return HTTPD_ERR_ARG;
            v = (hi << 4) | lo;
            i += 2;
            if (v == 0) return HTTPD_ERR_ARG;   // %00 would truncate the value
        } else if (c == '+' && plus_is_space) {
            v = ' ';
        } else {
            v = (unsigned char)c;
        }
        if (len + 1 >= out_cap) return HTTPD_ERR_TOOBIG;
        out[len++] = (char)v;
    }
    if (out_cap > 0) out[len] = 0;
    return len;
}

int eos_httpd_path_of(const char *uri, char *out, int out_cap)
{
    int n = 0;

    if (!uri || !out || out_cap <= 0) return HTTPD_ERR_ARG;
    out[0] = 0;
    while (uri[n] && uri[n] != '?' && uri[n] != '#') n++;
    return pct_decode(uri, n, out, out_cap, false);
}

int eos_httpd_query_get(const char *uri, const char *name, char *out, int out_cap)
{
    const char *q;
    size_t nlen;
    char kbuf[40];

    if (!uri || !name || !out || out_cap <= 0) return HTTPD_ERR_ARG;
    out[0] = 0;
    nlen = strlen(name);

    q = strchr(uri, '?');
    if (!q) return HTTPD_ERR_NOTFOUND;
    q++;

    while (*q && *q != '#') {
        const char *amp = q;
        const char *eq = NULL;
        while (*amp && *amp != '&' && *amp != '#') {
            if (*amp == '=' && !eq) eq = amp;
            amp++;
        }
        {
            int klen = (int)((eq ? eq : amp) - q);
            int kd = pct_decode(q, klen, kbuf, (int)sizeof kbuf, true);
            if (kd >= 0 && (size_t)kd == nlen && memcmp(kbuf, name, nlen) == 0) {
                if (!eq) return 0;                       // bare `?flag`
                return pct_decode(eq + 1, (int)(amp - eq - 1), out, out_cap, true);
            }
        }
        q = (*amp == '&') ? amp + 1 : amp;
        if (*q == 0 || *(q - 1) == '#') break;
    }
    return HTTPD_ERR_NOTFOUND;
}

bool eos_httpd_flag(const char *v)
{
    if (!v) return false;
    if (v[0] == 0) return true;                          // present with no value
    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
           strcmp(v, "yes") == 0 || strcmp(v, "on") == 0;
}

// ==========================================================================
// Routing
// ==========================================================================

static const struct {
    const char *path;
    const char *method;
    uint8_t     route;
} ROUTES[] = {
    { "/api/wifi/scan",    "GET",  EOS_ROUTE_WIFI_SCAN    },
    { "/api/wifi/connect", "POST", EOS_ROUTE_WIFI_CONNECT },
    { "/api/wifi/forget",  "POST", EOS_ROUTE_WIFI_FORGET  },
    { "/api/net/status",   "GET",  EOS_ROUTE_NET_STATUS   },
    { "/api/ble/scan",     "GET",  EOS_ROUTE_BLE_SCAN     },
    { "/api/ble/pair",     "POST", EOS_ROUTE_BLE_PAIR     },
    { "/api/ble/status",   "GET",  EOS_ROUTE_BLE_STATUS   },
    { "/api/ble/forget",   "POST", EOS_ROUTE_BLE_FORGET   },
    // megabrain
    { "/api/brain/status", "GET",  EOS_ROUTE_BRAIN_STATUS },
    { "/api/brain/ask",    "POST", EOS_ROUTE_BRAIN_ASK    },
    { "/api/brain/cancel", "POST", EOS_ROUTE_BRAIN_CANCEL },
    // settings, system and themes. /api/settings is the one path in the whole
    // contract that answers two methods, which is why the scan below keeps
    // looking after a path match instead of settling for the first row.
    { "/api/settings",      "GET",  EOS_ROUTE_SETTINGS_GET   },
    { "/api/settings",      "POST", EOS_ROUTE_SETTINGS_SET   },
    { "/api/system",        "GET",  EOS_ROUTE_SYSTEM         },
    { "/api/system/health", "GET",  EOS_ROUTE_SYSTEM_HEALTH  },
    { "/api/system/reboot", "POST", EOS_ROUTE_SYSTEM_REBOOT  },
    { "/api/themes",        "GET",  EOS_ROUTE_THEMES         },
    // files, console, buddy and apps. Answered by kernel/svc/eos_apps.c
    // through the pointer eos_httpd_set_api() holds; named here because there
    // is one route table.
    { "/api/fs/list",         "GET",  EOS_ROUTE_FS_LIST      },
    { "/api/fs/stat",         "GET",  EOS_ROUTE_FS_STAT      },
    { "/api/fs/read",         "GET",  EOS_ROUTE_FS_READ      },
    { "/api/fs/usage",        "GET",  EOS_ROUTE_FS_USAGE     },
    { "/api/fs/write",        "POST", EOS_ROUTE_FS_WRITE     },
    { "/api/fs/upload/abort", "POST", EOS_ROUTE_FS_ABORT     },
    { "/api/fs/mkdir",        "POST", EOS_ROUTE_FS_MKDIR     },
    { "/api/fs/remove",       "POST", EOS_ROUTE_FS_REMOVE    },
    { "/api/fs/rename",       "POST", EOS_ROUTE_FS_RENAME    },
    { "/api/console/log",     "GET",  EOS_ROUTE_CONSOLE_LOG  },
    { "/api/console/exec",    "POST", EOS_ROUTE_CONSOLE_EXEC },
    { "/api/buddy",           "GET",  EOS_ROUTE_BUDDY        },
    { "/api/buddy/reload",    "POST", EOS_ROUTE_BUDDY_RELOAD },
    { "/api/apps",            "GET",  EOS_ROUTE_APPS         },
};
#define N_ROUTES ((int)(sizeof ROUTES / sizeof ROUTES[0]))

// The connectivity checks the five stacks that matter actually fetch. Getting
// anything other than what each one expects is what makes the portal pop, so a
// 302 here is the whole mechanism.
static const char *const PROBES[] = {
    "/generate_204",                    // Android, Chrome OS
    "/gen_204",
    "/hotspot-detect.html",             // iOS, macOS
    "/hotspotdetect.html",
    "/library/test/success.html",
    "/ncsi.txt",                        // Windows NCSI
    "/connecttest.txt",
    "/redirect",
    "/success.txt",                     // Firefox
    "/canonical.html",                  // GNOME / NetworkManager
    "/check_network_status.txt",
    "/nm-check.txt",
    "/kindle-wifi/wifistub.html",       // Kindle
    "/mobile/status.php",               // some Android OEM builds
};
#define N_PROBES ((int)(sizeof PROBES / sizeof PROBES[0]))

static bool ascii_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

bool eos_httpd_is_captive_probe(const char *path)
{
    if (!path) return false;
    for (int i = 0; i < N_PROBES; i++) if (ascii_ieq(path, PROBES[i])) return true;
    return false;
}

// True for a path that must never reach the filesystem: an escape upwards, a
// backslash a Windows-ish layer might fold into a separator, or a raw NUL.
static bool path_is_hostile(const char *p)
{
    if (p[0] != '/') return true;
    if (strchr(p, '\\')) return true;
    for (const char *s = p; *s; s++) {
        if (s[0] == '.' && s[1] == '.') return true;     // "..", "...", "%2e%2e"
    }
    return false;
}

eos_route_t eos_httpd_route(const char *method, const char *uri)
{
    char path[EOS_HTTPD_URI_MAX];
    bool is_get, is_post, api;
    int n;

    if (!method || !uri) return EOS_ROUTE_NONE;

    n = eos_httpd_path_of(uri, path, (int)sizeof path);
    if (n < 0) return EOS_ROUTE_NONE;

    is_get  = strcmp(method, "GET")  == 0;
    is_post = strcmp(method, "POST") == 0;

    api = strncmp(path, "/api/", 5) == 0;

    // A path may appear more than once — /api/settings takes both a GET and a
    // POST — so a row whose method does not match is remembered and the scan
    // continues. Settling for the first row would 405 the GET on a path whose
    // POST happened to be listed first, which is a bug that only appears once
    // somebody reorders the table.
    {
        bool path_seen = false;
        for (int i = 0; i < N_ROUTES; i++) {
            if (strcmp(path, ROUTES[i].path) != 0) continue;
            if (strcmp(method, ROUTES[i].method) == 0) return (eos_route_t)ROUTES[i].route;
            path_seen = true;
        }
        if (path_seen) return EOS_ROUTE_METHOD;
    }

    // A typo under /api/ is a 404, never a file. Falling through to the
    // filesystem would turn a mistyped endpoint into a path traversal surface.
    if (api) return (is_get || is_post) ? EOS_ROUTE_NONE : EOS_ROUTE_METHOD;

    if (!is_get && !is_post) return EOS_ROUTE_METHOD;
    if (is_post) return EOS_ROUTE_METHOD;                // nothing else takes a POST
    if (path_is_hostile(path)) return EOS_ROUTE_NONE;
    if (eos_httpd_is_captive_probe(path)) return EOS_ROUTE_CAPTIVE;
    return EOS_ROUTE_STATIC;
}

const char *eos_httpd_mime(const char *path)
{
    static const struct { const char *ext, *type; } MIME[] = {
        { ".html", "text/html; charset=utf-8"              },
        { ".htm",  "text/html; charset=utf-8"              },
        { ".css",  "text/css; charset=utf-8"               },
        { ".js",   "application/javascript; charset=utf-8" },
        { ".mjs",  "application/javascript; charset=utf-8" },
        { ".json", "application/json; charset=utf-8"       },
        { ".txt",  "text/plain; charset=utf-8"             },
        { ".svg",  "image/svg+xml"                         },
        { ".png",  "image/png"                             },
        { ".jpg",  "image/jpeg"                            },
        { ".ico",  "image/x-icon"                          },
        { ".webp", "image/webp"                            },
        { ".woff2","font/woff2"                            },
        { ".map",  "application/json; charset=utf-8"       },
    };
    size_t n;

    if (!path) return "application/octet-stream";
    n = strlen(path);
    // The transfer encoding is a header, not a type: index.html.gz is HTML.
    if (n > 3 && memcmp(path + n - 3, ".gz", 3) == 0) n -= 3;

    for (size_t i = 0; i < sizeof MIME / sizeof MIME[0]; i++) {
        size_t e = strlen(MIME[i].ext);
        if (n >= e && memcmp(path + n - e, MIME[i].ext, e) == 0) return MIME[i].type;
    }
    return "application/octet-stream";
}

const char *eos_httpd_auth_name(int auth)
{
    switch (auth) {
    case EOS_HTTPD_AUTH_OPEN:       return "open";
    case EOS_HTTPD_AUTH_WEP:        return "wep";
    case EOS_HTTPD_AUTH_WPA:        return "wpa";
    case EOS_HTTPD_AUTH_WPA2:       return "wpa2";
    case EOS_HTTPD_AUTH_WPA_WPA2:   return "wpa_wpa2";
    case EOS_HTTPD_AUTH_WPA3:       return "wpa3";
    case EOS_HTTPD_AUTH_WPA2_WPA3:  return "wpa2_wpa3";
    case EOS_HTTPD_AUTH_ENTERPRISE: return "enterprise";
    default:                        return "other";
    }
}

static const char *scan_state_name(int s)
{
    switch (s) {
    case EOS_HTTPD_SCAN_RUNNING: return "running";
    case EOS_HTTPD_SCAN_DONE:    return "done";
    case EOS_HTTPD_SCAN_FAILED:  return "failed";
    default:                     return "idle";
    }
}

static const char *net_state_name(int s)
{
    switch (s) {
    case EOS_HTTPD_NET_SETUP:   return "setup";
    case EOS_HTTPD_NET_JOINING: return "joining";
    case EOS_HTTPD_NET_UP:      return "up";
    default:                    return "down";
    }
}

const char *eos_httpd_join_state(int s)
{
    switch (s) {
    case EOS_HTTPD_JOIN_OK:      return "ok";
    case EOS_HTTPD_JOIN_RUNNING: return "trying";
    case EOS_HTTPD_JOIN_NONE:    return "none";
    default:                     return "failed";
    }
}

// NULL when there is nothing to explain. The strings are the keys in
// web/README.md's "Why a join failed" table, which is where each one becomes a
// sentence telling the person what to do about it.
const char *eos_httpd_join_reason(int s)
{
    switch (s) {
    case EOS_HTTPD_JOIN_AUTH:     return "bad_auth";
    case EOS_HTTPD_JOIN_NOTFOUND: return "no_ap";
    case EOS_HTTPD_JOIN_TIMEOUT:  return "ip_fail";
    case EOS_HTTPD_JOIN_FAILED:   return "failed";
    default:                      return NULL;
    }
}

const char *eos_httpd_ble_state_name(int s)
{
    switch (s) {
    case EOS_HTTPD_BLE_IDLE:       return "idle";
    case EOS_HTTPD_BLE_SCANNING:   return "scanning";
    case EOS_HTTPD_BLE_CONNECTING: return "connecting";
    case EOS_HTTPD_BLE_PAIRING:    return "pairing";
    case EOS_HTTPD_BLE_READY:      return "ready";
    default:                       return "off";
    }
}

// The one place a join failure is turned into words a person can act on. The
// web app shows this verbatim, so it says what to do, not what happened.
static const char *join_detail(int s)
{
    switch (s) {
    case EOS_HTTPD_JOIN_OK:       return "connected";
    case EOS_HTTPD_JOIN_RUNNING:  return "trying";
    case EOS_HTTPD_JOIN_AUTH:     return "the password was refused; nothing was saved";
    case EOS_HTTPD_JOIN_NOTFOUND: return "that network was not on the air; nothing was saved";
    case EOS_HTTPD_JOIN_TIMEOUT:  return "joined but never got an address; nothing was saved";
    case EOS_HTTPD_JOIN_FAILED:   return "the join failed; nothing was saved";
    default:                      return "nothing has been tried yet";
    }
}

// ==========================================================================
// Errors
// ==========================================================================

// The code/status pairs are web/README.md's error model, which the web app
// already knows how to render. Codes map onto eos_err_t one for one.
static const char *err_code(int e)
{
    switch (e) {
    case -1:  return "bad_argument";
    case -2:  return "no_such_device";
    case -3:  return "io_error";
    case -4:  return "pool_exhausted";
    case -5:  return "not_found";
    case -6:  return "exists";
    case -7:  return "unsupported";
    case -8:  return "busy";
    case -9:  return "too_big";
    case -10: return "readonly";
    case -11: return "state";
    default:  return "io_error";
    }
}

static int err_status(int e)
{
    switch (e) {
    case -1:  return 400;
    case -2:  return 503;
    case -4:  return 503;
    case -5:  return 404;
    case -6:  return 409;
    case -7:  return 501;
    case -8:  return 409;
    case -9:  return 413;
    case -10: return 403;
    case -11: return 409;
    default:  return 500;
    }
}

static int reply_json(eos_httpd_t *h, eos_httpd_resp_t *r, int status, const eos_json_t *j)
{
    r->kind             = EOS_HTTPD_BODY_BUF;
    r->content_type     = "application/json; charset=utf-8";
    r->content_encoding = NULL;
    r->cache_control    = "no-store";
    r->body             = h->resp;
    r->body_len         = j->len;
    r->status           = status;

    if (!eos_json_ok(j)) {
        // The document did not fit. Say so as JSON that certainly does rather
        // than shipping a truncated one the phone will fail to parse.
        static const char OVF[] =
            "{\"error\":\"io_error\",\"detail\":\"response did not fit\"}";
        r->body     = OVF;
        r->body_len = (int)sizeof OVF - 1;
        r->status   = 500;
    }
    return r->status;
}

static int fail(eos_httpd_t *h, eos_httpd_resp_t *r, int status,
                const char *code, const char *detail)
{
    eos_json_t j;
    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_str(&j, "error", code);
    eos_json_kv_str(&j, "detail", detail);
    eos_json_obj_close(&j);
    h->req_rejected++;
    return reply_json(h, r, status, &j);
}

static int fail_err(eos_httpd_t *h, eos_httpd_resp_t *r, int e, const char *detail)
{
    return fail(h, r, err_status(e), err_code(e), detail);
}

// The same five, exported, so kernel/svc/eos_apps.c stages its errors through
// this table rather than through a second copy of it. See eos_httpd.h.
const char *eos_httpd_err_code(int e)   { return err_code(e); }
int         eos_httpd_err_status(int e) { return err_status(e); }

int eos_httpd_reply_json(eos_httpd_t *h, eos_httpd_resp_t *r, int status,
                         const eos_json_t *j)
{
    return reply_json(h, r, status, j);
}

int eos_httpd_fail(eos_httpd_t *h, eos_httpd_resp_t *r, int status,
                   const char *code, const char *detail)
{
    return fail(h, r, status, code, detail);
}

int eos_httpd_fail_err(eos_httpd_t *h, eos_httpd_resp_t *r, int e, const char *detail)
{
    return fail_err(h, r, e, detail);
}

// The second route file, registered rather than called. NULL in an image that
// links no eos_apps.c, which is what those routes answering 501 means.
static eos_httpd_api_fn s_api;

void eos_httpd_set_api(eos_httpd_api_fn fn) { s_api = fn; }

// ==========================================================================
// The radio interlock
// ==========================================================================
//
// WiFi and BLE share one antenna on every board in this fleet, and on the C6
// they share it with 802.15.4 as well. The real mutual exclusion has to live
// under eos_net and eos_ble, because only they know when a scan actually
// finishes. What this does is make sure the HTTP surface never *asks* for the
// overlap: a rescan or a join while the other radio is scanning is a 409 with a
// sentence the UI can show, not a request queued behind a five-second radio.

static bool wifi_is_busy(eos_httpd_t *h)
{
    eos_httpd_net_t n;
    if (h->ports.wifi_scan_state &&
        h->ports.wifi_scan_state(h->ctx) == EOS_HTTPD_SCAN_RUNNING) return true;
    if (h->ports.net_status && h->ports.net_status(h->ctx, &n) &&
        n.state == EOS_HTTPD_NET_JOINING) return true;
    return false;
}

static bool ble_is_busy(eos_httpd_t *h)
{
    eos_httpd_ble_status_t s;
    if (h->ports.ble_scan_state &&
        h->ports.ble_scan_state(h->ctx) == EOS_HTTPD_SCAN_RUNNING) return true;
    if (h->ports.ble_status && h->ports.ble_status(h->ctx, &s) && s.pairing) return true;
    return false;
}

#define RADIO_BUSY_BLE  "the bluetooth radio is scanning or pairing; " \
                        "wifi and bluetooth share one antenna, so try again in a moment"
#define RADIO_BUSY_WIFI "the wifi radio is scanning or joining; " \
                        "wifi and bluetooth share one antenna, so try again in a moment"

// ==========================================================================
// Handlers
// ==========================================================================

static void write_mac(char *out, const uint8_t *m)
{
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 3 + 0] = HEX[(m[i] >> 4) & 0xF];
        out[i * 3 + 1] = HEX[m[i] & 0xF];
        if (i < 5) out[i * 3 + 2] = ':';
    }
    out[17] = 0;
}

// --------------------------------------------------------- GET /api/wifi/scan

static int h_wifi_scan(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    char qv[16];
    bool rescan;
    int state, total, shown = 0;
    bool truncated = false;
    int8_t rssi[EOS_HTTPD_SCAN_POOL];
    uint8_t order[EOS_HTTPD_SCAN_POOL];
    int pool, n;

    if (!h->ports.wifi_scan_state || !h->ports.wifi_scan_get || !h->ports.wifi_scan_count)
        return fail_err(h, r, -7, "this board has no wifi");

    rescan = eos_httpd_query_get(req->uri, "rescan", qv, (int)sizeof qv) >= 0 &&
             eos_httpd_flag(qv);

    state = h->ports.wifi_scan_state(h->ctx);

    if (rescan && state != EOS_HTTPD_SCAN_RUNNING) {
        if (ble_is_busy(h)) return fail_err(h, r, -8, RADIO_BUSY_BLE);
        if (h->ports.wifi_scan_start) {
            int e = h->ports.wifi_scan_start(h->ctx);
            if (e < 0) return fail_err(h, r, e, "the wifi scan could not be started");
            state = EOS_HTTPD_SCAN_RUNNING;
        }
    }

    total = h->ports.wifi_scan_count(h->ctx);
    if (total < 0) total = 0;

    // Rank the whole cache, then report the strongest EOS_HTTPD_SCAN_MAX of it.
    // Ranking only the first SCAN_MAX would make the short list an arbitrary
    // slice rather than the useful one, and on a tier-0 board where SCAN_MAX is
    // four that is the difference between seeing your network and not.
    pool = total > EOS_HTTPD_SCAN_POOL ? EOS_HTTPD_SCAN_POOL : total;
    n    = pool  > EOS_HTTPD_SCAN_MAX  ? EOS_HTTPD_SCAN_MAX  : pool;

    for (int i = 0; i < pool; i++) {
        eos_httpd_ap_t ap;
        rssi[i] = -127;
        if (h->ports.wifi_scan_get(h->ctx, i, &ap)) rssi[i] = ap.rssi;
        order[i] = (uint8_t)i;
    }
    // Insertion sort over an index array. The pool is at most 48 and the
    // alternative is holding 48 forty-four-byte records on a shared task stack.
    for (int i = 1; i < pool; i++) {
        uint8_t v = order[i];
        int k = i - 1;
        while (k >= 0 && rssi[order[k]] < rssi[v]) { order[k + 1] = order[k]; k--; }
        order[k + 1] = v;
    }

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_str (&j, "state", scan_state_name(state));
    eos_json_kv_bool(&j, "scanning", state == EOS_HTTPD_SCAN_RUNNING);
    eos_json_kv_int (&j, "age_ms", h->ports.wifi_scan_age_ms
                                   ? (long)h->ports.wifi_scan_age_ms(h->ctx) : 0);
    eos_json_kv_int (&j, "total", total);
    eos_json_key(&j, "networks");
    eos_json_arr_open(&j);

    for (int i = 0; i < n; i++) {
        eos_httpd_ap_t ap;
        char bssid[18];
        int est;

        if (!h->ports.wifi_scan_get(h->ctx, order[i], &ap)) continue;
        if (ap.ssid_len > 32) ap.ssid_len = 32;

        // Stop on the response budget rather than on a truncated document. The
        // constant is the fixed part of an entry, about 130 bytes, plus room
        // for the array close and the two trailing fields.
        est = 160 + eos_json_escaped_len((const char *)ap.ssid, ap.ssid_len)
                  + 2 * (int)ap.ssid_len;
        if (j.len + est > j.cap) { truncated = true; break; }

        write_mac(bssid, ap.bssid);
        eos_json_obj_open(&j);
        eos_json_kv_strn(&j, "ssid", (const char *)ap.ssid, ap.ssid_len);
        eos_json_key(&j, "ssid_hex");
        eos_json_hexn(&j, ap.ssid, ap.ssid_len);
        eos_json_kv_bool(&j, "hidden", ap.ssid_len == 0);
        // null rather than 00:00:00:00:00:00 when the scan source did not carry
        // one. A plausible-looking address nobody can connect to is worse than
        // an absent one, and eos_net's reduced scan does not keep BSSIDs.
        eos_json_key(&j, "bssid");
        if (memcmp(ap.bssid, "\0\0\0\0\0\0", 6) == 0) eos_json_null(&j);
        else                                          eos_json_str(&j, bssid);
        eos_json_kv_int (&j, "rssi", ap.rssi);
        eos_json_kv_int (&j, "channel", ap.channel);
        eos_json_kv_str (&j, "auth", eos_httpd_auth_name(ap.auth));
        eos_json_kv_bool(&j, "secure", ap.auth != EOS_HTTPD_AUTH_OPEN);
        eos_json_kv_bool(&j, "saved", ap.saved);
        eos_json_obj_close(&j);
        shown++;
    }

    eos_json_arr_close(&j);
    eos_json_kv_int (&j, "shown", shown);
    eos_json_kv_bool(&j, "truncated", truncated || total > n);
    eos_json_obj_close(&j);

    return reply_json(h, r, state == EOS_HTTPD_SCAN_RUNNING ? 202 : 200, &j);
}

// ---------------------------------------------------- POST /api/wifi/connect

static int hex_bytes(const char *s, int n, uint8_t *out, int cap)
{
    int len = 0;
    if (n % 2) return HTTPD_ERR_ARG;
    for (int i = 0; i < n; i += 2) {
        int hi = hexval(s[i]), lo = hexval(s[i + 1]);
        if (hi < 0 || lo < 0) return HTTPD_ERR_ARG;
        if (len >= cap) return HTTPD_ERR_TOOBIG;
        out[len++] = (uint8_t)((hi << 4) | lo);
    }
    return len;
}

static int h_wifi_connect(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    uint8_t ssid[32];
    int ssid_len = 0;
    char psk[64];
    int psk_len = 0;
    eos_json_find_t s;
    int e;

    if (!h->ports.wifi_join) return fail_err(h, r, -7, "this board has no wifi");
    if (req->body_truncated)
        return fail_err(h, r, -9, "the request body is larger than this board accepts");
    if (!req->body || req->body_len <= 0)
        return fail_err(h, r, -1, "expected a JSON body with ssid and psk");

    // ssid_hex wins when both are present: it is the exact bytes off the air
    // and survives an SSID that is not valid UTF-8, which "ssid" cannot.
    {
    int hexlen = 0;
    s = eos_json_get_str(req->body, req->body_len, "ssid_hex", h->arg,
                         (int)sizeof h->arg, &hexlen);
    if (s == EOS_JSON_FOUND) {
        ssid_len = hex_bytes(h->arg, hexlen, ssid, (int)sizeof ssid);
        if (ssid_len < 0) return fail_err(h, r, -1, "ssid_hex is not an even run of hex digits");
    } else if (s == EOS_JSON_BAD) {
        return fail_err(h, r, -1, "the request body is not a JSON object");
    } else if (s == EOS_JSON_TOOBIG) {
        return fail_err(h, r, -1, "ssid_hex is longer than 32 bytes of SSID");
    } else {
        int n = 0;
        s = eos_json_get_str(req->body, req->body_len, "ssid", h->arg, (int)sizeof h->arg, &n);
        if (s == EOS_JSON_ABSENT) return fail_err(h, r, -1, "ssid is required");
        if (s == EOS_JSON_TYPE)   return fail_err(h, r, -1, "ssid must be a string");
        if (s == EOS_JSON_TOOBIG) return fail_err(h, r, -1, "an SSID is at most 32 bytes");
        if (s != EOS_JSON_FOUND)  return fail_err(h, r, -1, "the request body is not a JSON object");
        if (n > 32) return fail_err(h, r, -1, "an SSID is at most 32 bytes");
        ssid_len = n;
        memcpy(ssid, h->arg, (size_t)n);
    }
    }
    if (ssid_len < 1) return fail_err(h, r, -1, "ssid is empty");
    if (ssid_len > 32) return fail_err(h, r, -1, "an SSID is at most 32 bytes");

    s = eos_json_get_str(req->body, req->body_len, "psk", psk, (int)sizeof psk, &psk_len);
    if (s == EOS_JSON_TOOBIG) return fail_err(h, r, -1, "a WPA passphrase is at most 63 characters");
    if (s == EOS_JSON_TYPE)   return fail_err(h, r, -1, "psk must be a string");
    if (s == EOS_JSON_BAD)    return fail_err(h, r, -1, "the request body is not a JSON object");
    if (s == EOS_JSON_ABSENT) { psk[0] = 0; psk_len = 0; }

    // 1..7 is not a WPA passphrase and not an open network. Catching it here
    // costs nothing; letting it through costs a fifteen-second join that was
    // never going to work, on the one screen the owner is watching.
    if (psk_len > 0 && psk_len < 8)
        return fail_err(h, r, -1, "a WPA passphrase is 8 to 63 characters; "
                                  "leave it empty for an open network");
    if (psk_len > 63) return fail_err(h, r, -1, "a WPA passphrase is at most 63 characters");

    if (ble_is_busy(h)) return fail_err(h, r, -8, RADIO_BUSY_BLE);
    if (wifi_is_busy(h)) return fail_err(h, r, -8, "a wifi scan or join is already running");

    e = h->ports.wifi_join(h->ctx, ssid, ssid_len, psk, psk_len);
    memset(psk, 0, sizeof psk);          // do not leave it in BSS after the call
    if (e < 0) return fail_err(h, r, e, "the join could not be started");

    // 202, not 200. The join takes up to fifteen seconds and this handler is
    // one of four workers; the client polls /api/net/status for the outcome.
    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "accepted", true);
    // "trying", not "ok". The web app treats {"ok":true} as a finished join and
    // goes looking for an address; this one has not even reached the radio yet.
    eos_json_kv_str (&j, "state", "trying");
    eos_json_kv_strn(&j, "ssid", (const char *)ssid, ssid_len);
    eos_json_key(&j, "ssid_hex");
    eos_json_hexn(&j, ssid, ssid_len);
    eos_json_kv_str (&j, "poll", "/api/net/status");
    eos_json_kv_int (&j, "poll_ms", 1000);
    eos_json_kv_int (&j, "budget_ms", 15000);
    eos_json_kv_str (&j, "persist", "on-success");
    eos_json_kv_str (&j, "note", "the credentials are written to flash only after "
                                 "the join succeeds; a wrong password leaves the "
                                 "board exactly as it was");
    eos_json_obj_close(&j);
    return reply_json(h, r, 202, &j);
}

// ----------------------------------------------------- POST /api/wifi/forget

static int h_wifi_forget(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_json_t j;
    int e;

    if (!h->ports.wifi_forget) return fail_err(h, r, -7, "this board has no wifi");
    e = h->ports.wifi_forget(h->ctx);
    if (e < 0) return fail_err(h, r, e, "the stored credentials could not be cleared");

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "ok", true);
    eos_json_kv_bool(&j, "stored", false);
    eos_json_kv_str (&j, "next", "setup");
    eos_json_kv_str (&j, "note", "the board returns to setup mode; its SoftAP name "
                                 "and password are on the panel");
    eos_json_obj_close(&j);
    return reply_json(h, r, 200, &j);
}

// ------------------------------------------------------- GET /api/net/status

static int h_net_status(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_json_t j;
    eos_httpd_net_t n;

    memset(&n, 0, sizeof n);
    if (!h->ports.net_status || !h->ports.net_status(h->ctx, &n))
        return fail_err(h, r, -7, "this board has no network stack");

    if (n.ssid_len > 32) n.ssid_len = 32;
    if (n.ap_ssid_len > 32) n.ap_ssid_len = 32;
    n.ip[sizeof n.ip - 1] = 0;
    n.ap_ip[sizeof n.ap_ip - 1] = 0;
    n.host[sizeof n.host - 1] = 0;

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_str(&j, "mode", h->cfg.mode == EOS_HTTPD_MODE_RUN ? "run" : "setup");
    eos_json_kv_str(&j, "state", net_state_name(n.state));

    eos_json_key(&j, "join");
    eos_json_obj_open(&j);
    eos_json_kv_str (&j, "state", eos_httpd_join_state(n.join));
    eos_json_key(&j, "reason");
    if (eos_httpd_join_reason(n.join)) eos_json_str(&j, eos_httpd_join_reason(n.join));
    else                               eos_json_null(&j);
    eos_json_kv_str (&j, "detail", join_detail(n.join));
    eos_json_kv_bool(&j, "running", n.join == EOS_HTTPD_JOIN_RUNNING);
    eos_json_obj_close(&j);

    // The station's address, name and signal are repeated at the top level as
    // well as inside "sta". The web app reads them there, and one duplicated
    // line of JSON is cheaper than a contract argument across two agents.
    eos_json_kv_str (&j, "ip", n.ip);
    eos_json_kv_strn(&j, "ssid", (const char *)n.ssid, n.ssid_len);
    eos_json_key(&j, "rssi");
    if (n.state == EOS_HTTPD_NET_UP) eos_json_int(&j, n.rssi);
    else                             eos_json_null(&j);

    eos_json_key(&j, "sta");
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "up", n.state == EOS_HTTPD_NET_UP);
    eos_json_kv_strn(&j, "ssid", (const char *)n.ssid, n.ssid_len);
    eos_json_key(&j, "ssid_hex");
    eos_json_hexn(&j, n.ssid, n.ssid_len);
    eos_json_kv_str (&j, "ip", n.ip);
    if (n.state == EOS_HTTPD_NET_UP) eos_json_kv_int(&j, "rssi", n.rssi);
    else                             eos_json_kv_null(&j, "rssi");
    eos_json_kv_bool(&j, "stored", n.ssid_stored);
    eos_json_obj_close(&j);

    eos_json_key(&j, "ap");
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "up", n.ap_up);
    eos_json_kv_strn(&j, "ssid", (const char *)n.ap_ssid, n.ap_ssid_len);
    eos_json_kv_str (&j, "ip", n.ap_ip);
    eos_json_kv_int (&j, "clients", n.ap_clients);
    eos_json_obj_close(&j);

    eos_json_kv_str(&j, "host", n.host);
    eos_json_kv_str(&j, "hostname", n.host);     // both spellings; the app reads either
    eos_json_key(&j, "mdns");
    if (n.host[0]) {
        char mdns[32];
        snprintf(mdns, sizeof mdns, "%s.local", n.host);
        eos_json_str(&j, mdns);
    } else {
        eos_json_null(&j);
    }

    eos_json_key(&j, "requests");
    eos_json_obj_open(&j);
    eos_json_kv_int(&j, "total",    (long)h->req_total);
    eos_json_kv_int(&j, "api",      (long)h->req_api);
    eos_json_kv_int(&j, "static",   (long)h->req_static);
    eos_json_kv_int(&j, "portal",   (long)h->req_portal);
    eos_json_kv_int(&j, "rejected", (long)h->req_rejected);
    eos_json_obj_close(&j);

    eos_json_obj_close(&j);
    return reply_json(h, r, 200, &j);
}

// ---------------------------------------------------------- GET /api/ble/scan

static int h_ble_scan(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    char qv[16];
    bool rescan;
    int state, total, shown = 0, pool, n;
    bool truncated = false;
    int8_t rssi[EOS_HTTPD_SCAN_POOL];
    uint8_t order[EOS_HTTPD_SCAN_POOL];

    if (!h->ports.ble_scan_state || !h->ports.ble_scan_get || !h->ports.ble_scan_count)
        return fail_err(h, r, -7, "this board has no bluetooth");

    rescan = eos_httpd_query_get(req->uri, "rescan", qv, (int)sizeof qv) >= 0 &&
             eos_httpd_flag(qv);

    state = h->ports.ble_scan_state(h->ctx);

    if (rescan && state != EOS_HTTPD_SCAN_RUNNING) {
        if (wifi_is_busy(h)) return fail_err(h, r, -8, RADIO_BUSY_WIFI);
        if (h->ports.ble_scan_start) {
            int e = h->ports.ble_scan_start(h->ctx);
            if (e < 0) return fail_err(h, r, e, "the bluetooth scan could not be started");
            state = EOS_HTTPD_SCAN_RUNNING;
        }
    }

    total = h->ports.ble_scan_count(h->ctx);
    if (total < 0) total = 0;

    // Strongest first here too. A keyboard on the desk in front of you should
    // be at the top of the list, not wherever the advertisement happened to land.
    pool = total > EOS_HTTPD_SCAN_POOL ? EOS_HTTPD_SCAN_POOL : total;
    n    = pool  > EOS_HTTPD_SCAN_MAX  ? EOS_HTTPD_SCAN_MAX  : pool;

    for (int i = 0; i < pool; i++) {
        eos_httpd_ble_dev_t d;
        rssi[i] = -127;
        if (h->ports.ble_scan_get(h->ctx, i, &d)) rssi[i] = d.rssi;
        order[i] = (uint8_t)i;
    }
    for (int i = 1; i < pool; i++) {
        uint8_t v = order[i];
        int k = i - 1;
        while (k >= 0 && rssi[order[k]] < rssi[v]) { order[k + 1] = order[k]; k--; }
        order[k + 1] = v;
    }

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_str (&j, "state", scan_state_name(state));
    eos_json_kv_bool(&j, "scanning", state == EOS_HTTPD_SCAN_RUNNING);
    eos_json_kv_int (&j, "age_ms", h->ports.ble_scan_age_ms
                                   ? (long)h->ports.ble_scan_age_ms(h->ctx) : 0);
    eos_json_kv_int (&j, "total", total);
    eos_json_key(&j, "devices");
    eos_json_arr_open(&j);

    for (int i = 0; i < n; i++) {
        eos_httpd_ble_dev_t d;
        int est;
        if (!h->ports.ble_scan_get(h->ctx, order[i], &d)) continue;
        if (d.name_len > 32) d.name_len = 32;
        d.addr[sizeof d.addr - 1] = 0;

        est = 128 + eos_json_escaped_len((const char *)d.name, d.name_len)
                  + 2 * (int)d.name_len;
        if (j.len + est > j.cap) { truncated = true; break; }

        eos_json_obj_open(&j);
        eos_json_kv_strn(&j, "name", (const char *)d.name, d.name_len);
        eos_json_key(&j, "name_hex");
        eos_json_hexn(&j, d.name, d.name_len);
        eos_json_kv_str (&j, "addr", d.addr);
        eos_json_kv_int (&j, "rssi", d.rssi);
        eos_json_kv_bool(&j, "hid", d.is_hid);
        eos_json_kv_bool(&j, "bonded", d.bonded);
        eos_json_obj_close(&j);
        shown++;
    }

    eos_json_arr_close(&j);
    eos_json_kv_int (&j, "shown", shown);
    eos_json_kv_bool(&j, "truncated", truncated || total > n);
    eos_json_obj_close(&j);

    return reply_json(h, r, state == EOS_HTTPD_SCAN_RUNNING ? 202 : 200, &j);
}

// ---------------------------------------------------------- POST /api/ble/pair

// "aa:bb:cc:dd:ee:ff", lowercased in place. Anything else is refused: a BLE
// address that is nearly right is a five-second connect attempt to nobody.
static bool addr_normalise(char *a)
{
    if (strlen(a) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (a[i] != ':') return false; continue; }
        if (hexval(a[i]) < 0) return false;
        if (a[i] >= 'A' && a[i] <= 'F') a[i] = (char)(a[i] + 32);
    }
    return true;
}

static int h_ble_pair(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    char addr[24];
    int n = 0, e;
    eos_json_find_t s;

    if (!h->ports.ble_pair) return fail_err(h, r, -7, "this board has no bluetooth");
    if (req->body_truncated)
        return fail_err(h, r, -9, "the request body is larger than this board accepts");
    if (!req->body || req->body_len <= 0)
        return fail_err(h, r, -1, "expected a JSON body with addr");

    s = eos_json_get_str(req->body, req->body_len, "addr", addr, (int)sizeof addr, &n);
    if (s == EOS_JSON_ABSENT) return fail_err(h, r, -1, "addr is required");
    if (s == EOS_JSON_TYPE)   return fail_err(h, r, -1, "addr must be a string");
    if (s != EOS_JSON_FOUND)  return fail_err(h, r, -1, "addr is not a bluetooth address");
    if (n != 17 || !addr_normalise(addr))
        return fail_err(h, r, -1, "addr must look like aa:bb:cc:dd:ee:ff");

    if (wifi_is_busy(h)) return fail_err(h, r, -8, RADIO_BUSY_WIFI);
    if (ble_is_busy(h))  return fail_err(h, r, -8, "a bluetooth scan or pairing is already running");

    e = h->ports.ble_pair(h->ctx, addr);
    if (e < 0) return fail_err(h, r, e, "the pairing could not be started");

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "accepted", true);
    eos_json_kv_str (&j, "state", "pairing");
    eos_json_kv_str (&j, "addr", addr);
    eos_json_kv_str (&j, "poll", "/api/ble/status");
    eos_json_kv_int (&j, "poll_ms", 1000);
    // The passkey has no screen on the keyboard's end, so it arrives through
    // /api/ble/status and on the panel, and the human types it on the keyboard.
    eos_json_kv_str (&j, "expect", "passkey");
    eos_json_kv_str (&j, "instruction", "a six-digit passkey will appear here and on "
                                        "the panel; type it on the keyboard and press "
                                        "enter");
    // docs/provisioning.md is explicit that this warning belongs at the moment
    // of pairing rather than in a footnote, so it ships in the response itself.
    // eos_ble owns the wording, because the panel prints the same sentence.
    eos_json_kv_str (&j, "warning",
                     h->ports.ble_pair_warning ? h->ports.ble_pair_warning(h->ctx)
                     : "a keyboard bonds to one board at a time; pairing it here will "
                       "silently break its pairing with whatever board it was on before");
    eos_json_obj_close(&j);
    return reply_json(h, r, 202, &j);
}

// -------------------------------------------------------- GET /api/ble/status

static int h_ble_status(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_json_t j;
    eos_httpd_ble_status_t s;

    memset(&s, 0, sizeof s);
    if (!h->ports.ble_status || !h->ports.ble_status(h->ctx, &s))
        return fail_err(h, r, -7, "this board has no bluetooth");

    if (s.name_len > 32) s.name_len = 32;
    s.addr[sizeof s.addr - 1] = 0;

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_str (&j, "state", eos_httpd_ble_state_name(s.state));
    eos_json_kv_bool(&j, "connected", s.connected);
    eos_json_kv_bool(&j, "pairing", s.pairing);

    // "bonded" is the bond record itself, or null. An object is truthy, so the
    // one field answers both "is anything bonded" and "what is it" — which is
    // exactly the two things the web app asks of it.
    eos_json_key(&j, "bonded");
    if (s.bonded) {
        eos_json_obj_open(&j);
        eos_json_kv_strn(&j, "name", (const char *)s.name, s.name_len);
        eos_json_key(&j, "name_hex");
        eos_json_hexn(&j, s.name, s.name_len);
        eos_json_kv_str(&j, "addr", s.addr);
        eos_json_obj_close(&j);
    } else {
        eos_json_null(&j);
    }
    eos_json_kv_strn(&j, "name", (const char *)s.name, s.name_len);
    eos_json_kv_str (&j, "addr", s.addr);

    if (s.battery >= 0 && s.battery <= 100) eos_json_kv_int(&j, "battery", s.battery);
    else                                    eos_json_kv_null(&j, "battery");

    // A string, always six digits. As a number, a passkey of 001234 renders as
    // 1234 and the human types four digits into a device expecting six.
    eos_json_key(&j, "passkey");
    if (s.passkey_shown) {
        char pk[8];
        snprintf(pk, sizeof pk, "%06lu", (unsigned long)(s.passkey % 1000000UL));
        eos_json_str(&j, pk);
    } else {
        eos_json_null(&j);
    }
    eos_json_kv_bool(&j, "passkey_shown", s.passkey_shown);

    // Why the last attempt failed, in the web app's vocabulary. Null is the
    // honest answer today: eos_ble_status_t carries no failure code, so
    // inventing one here would be a guess the UI would print as a fact.
    eos_json_key(&j, "reason");
    if (s.reason) eos_json_str(&j, s.reason);
    else          eos_json_null(&j);
    eos_json_obj_close(&j);
    return reply_json(h, r, 200, &j);
}

// -------------------------------------------------------- POST /api/ble/forget

static int h_ble_forget(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_json_t j;
    int e;

    if (!h->ports.ble_forget) return fail_err(h, r, -7, "this board has no bluetooth");
    e = h->ports.ble_forget(h->ctx);
    if (e < 0) return fail_err(h, r, e, "the bond could not be dropped");

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "ok", true);
    eos_json_kv_bool(&j, "bonded", false);
    eos_json_kv_str (&j, "note", "the keyboard still holds its side of the bond; "
                                 "clear it there too before pairing it again");
    eos_json_obj_close(&j);
    return reply_json(h, r, 200, &j);
}

// ==========================================================================
// Megabrain
// ==========================================================================
//
// Three endpoints over the four brain ports. The shape of the answer is
// web/README.md's "Megabrain" section, and the one decision that is not in it
// is how a reply that takes seconds reaches a browser.
//
// It is a single chunked text/plain response, drained here and flushed as the
// bytes decode. The alternatives were weighed and both lose:
//
//   SSE          costs the same socket for the same duration and adds framing
//                the client does not want — app.js reads r.body.getReader()
//                directly, not an EventSource — so it would buy nothing and
//                cost a second parser at both ends.
//   polling      is what every other slow thing on this board does, and is
//                wrong here alone. A poll still needs the ring buffer, because
//                the tokens arrive between polls whatever the client does; on
//                top of that it spends a worker AND the dispatch lock two to
//                five times a second for the whole reply, and delivers the
//                text in lumps. Strictly more machinery for a worse result.
//
// The cost of the choice is one worker held for the length of one reply.
// eos_brain allows one request in flight and a second ask is refused with 409
// before it can take a second worker, so three of the four are always free —
// which is more than the two concurrent requests the web app holds itself to.
//
// The three ways that worker is released, all of them bounded: the reply ends;
// the client vanishes, which fails the next send_chunk and cancels the request
// rather than leaving the model talking into a ring nobody drains; or nothing
// arrives for EOS_HTTPD_STREAM_IDLE_MS. There is no path on which a worker
// waits on the model forever.

// -------------------------------------------------------- GET /api/brain/status

static int h_brain_status(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_httpd_brain_t b;
    eos_json_t j;

    if (!h->ports.brain_status)
        return fail_err(h, r, -7, "this board has no megabrain client");

    memset(&b, 0, sizeof b);
    if (!h->ports.brain_status(h->ctx, &b))
        return fail_err(h, r, -3, "the megabrain client did not answer");

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_str (&j, "host", b.host);
    eos_json_kv_int (&j, "port", b.port);
    eos_json_kv_str (&j, "model", b.model);

    eos_json_key(&j, "models");
    eos_json_arr_open(&j);
    for (int i = 0; i < b.model_count && b.models; i++)
        if (b.models[i]) eos_json_str(&j, b.models[i]);
    eos_json_arr_close(&j);

    eos_json_kv_bool(&j, "reachable", b.reachable);
    eos_json_kv_bool(&j, "busy", b.busy);
    if (b.last_error) eos_json_kv_str(&j, "last_error", b.last_error);
    else              eos_json_kv_null(&j, "last_error");
    eos_json_obj_close(&j);
    return reply_json(h, r, 200, &j);
}

// -------------------------------------------------------- POST /api/brain/ask

// The settings range for brain.max, applied here as a clamp rather than a
// refusal. The web app puts this on a number input the owner types into, and a
// 400 on a typo in a field that is not the question is a worse answer than the
// nearest legal value.
#define ASK_MAX_LO 16
#define ASK_MAX_HI 2048

static int h_brain_ask(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_httpd_ask_t ask;
    eos_json_find_t f;
    long v = 0;
    int n = 0, e;

    if (!h->ports.brain_ask || !h->ports.brain_read)
        return fail_err(h, r, -7, "this board has no megabrain client");
    if (req->body_truncated)
        return fail_err(h, r, -9, "the question is larger than this board accepts");
    if (!req->body || req->body_len <= 0)
        return fail_err(h, r, -1, "the request body must be a JSON object with a q");

    h->ask_q[0] = h->ask_model[0] = h->ask_system[0] = '\0';

    f = eos_json_get_str(req->body, req->body_len, "q",
                         h->ask_q, (int)sizeof h->ask_q, &n);
    if (f == EOS_JSON_TOOBIG)
        return fail_err(h, r, -9, "the question is longer than this board can hold");
    if (f != EOS_JSON_FOUND || n <= 0)
        return fail_err(h, r, -1, "the request body must carry a non-empty q");

    f = eos_json_get_str(req->body, req->body_len, "model",
                         h->ask_model, (int)sizeof h->ask_model, &n);
    if (f == EOS_JSON_TOOBIG)
        return fail_err(h, r, -9, "that model name does not fit");
    if (f == EOS_JSON_BAD)
        return fail_err(h, r, -1, "the request body is not a JSON object");

    f = eos_json_get_str(req->body, req->body_len, "system",
                         h->ask_system, (int)sizeof h->ask_system, &n);
    if (f == EOS_JSON_TOOBIG)
        return fail_err(h, r, -9, "that system prompt does not fit");

    ask.q          = h->ask_q;
    ask.model      = h->ask_model;
    ask.system     = h->ask_system;
    ask.max_tokens = 0;
    if (eos_json_get_int(req->body, req->body_len, "max", &v) == EOS_JSON_FOUND) {
        if (v < ASK_MAX_LO) v = ASK_MAX_LO;
        if (v > ASK_MAX_HI) v = ASK_MAX_HI;
        ask.max_tokens = (int)v;
    }

    e = h->ports.brain_ask(h->ctx, &ask);
    if (e < 0)
        return fail_err(h, r, e, e == -8
            ? "megabrain is already answering something; stop that first"
            : "the question could not be sent");

    // From here the response is 200 and everything else is text. An error the
    // model raises after this point arrives as a line beginning "! ", because
    // the status is already on the wire — see web/README.md.
    r->kind          = EOS_HTTPD_BODY_STREAM;
    r->status        = 200;
    r->content_type  = "text/plain; charset=utf-8";
    r->cache_control = "no-store";
    r->body          = NULL;
    r->body_len      = 0;
    return 200;
}

// ----------------------------------------------------- POST /api/brain/cancel

static int h_brain_cancel(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_json_t j;
    bool had;

    if (!h->ports.brain_cancel)
        return fail_err(h, r, -7, "this board has no megabrain client");

    // Never an error. Cancelling nothing is the normal outcome of a stop button
    // pressed a moment after the reply finished, and a 409 there would make the
    // web app show a failure for something that worked.
    had = h->ports.brain_cancel(h->ctx);

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "cancelled", had);
    eos_json_obj_close(&j);
    return reply_json(h, r, 200, &j);
}

// ==========================================================================
// Settings, system and themes
// ==========================================================================
//
// Four endpoints and one rule they share: none of them blocks. A settings save
// mutates RAM and marks a store dirty — the flash write happens on the OS loop,
// debounced, because a LittleFS sync is a sector erase with the instruction
// cache off and this chip has one core running the panel, the radio and this
// server. A reboot is scheduled rather than performed, because a restart that
// races its own HTTP response looks like a crash to whoever clicked the button.
//
// Everything specific to the twelve settings keys lives in eos_settings. What
// is here is the JSON shape web/README.md fixed and nothing else.

// Keys enumerated from the store before the loop gives up. Larger than
// eos_settings' twelve so a key added there needs no change here, and small
// enough that the reboot_required bitmask below is one word.
#define SETTINGS_KEY_LIMIT 32

// Mounts reported by GET /api/system. Two on every board there is; the loop
// stops at the port's own false, so this is only a bound on a misbehaving one.
#define EOS_MOUNTS_REPORTED 4

// kv.key is reused on every pass of the patch loop, so the key that failed has
// to be copied out rather than pointed at. Copying the wrong one is a detail
// string that names an innocent field, which is worse than no detail at all.
#define KEEP_KEY(dst, src) do { snprintf((dst), sizeof (dst), "%s", (src)); } while (0)

const char *const eos_httpd_role_names[EOS_HTTPD_THEME_ROLES] = {
    "bg", "surface", "overlay", "text", "muted", "accent", "accent_alt",
    "ok", "warn", "err", "border_focused", "border_unfocused",
    "bar_bg", "bar_fg", "tab_active", "tab_inactive"
};

// ----------------------------------------------------------- the settings object

// Written by both the GET and the POST: the POST answers with the whole object
// as well, because web/README.md makes the board the single source of truth and
// the page re-renders from what comes back rather than from what it sent.
static void settings_object(eos_httpd_t *h, eos_json_t *j)
{
    eos_httpd_kv_t kv;
    int i;

    eos_json_key(j, "settings");
    eos_json_obj_open(j);
    for (i = 0; i < SETTINGS_KEY_LIMIT; i++) {
        memset(&kv, 0, sizeof kv);
        if (!h->ports.settings_get(h->ctx, i, &kv)) break;
        if (!kv.key[0]) continue;      // a key the store declines to report
        switch (kv.type) {
        case EOS_HTTPD_VAL_INT:  eos_json_kv_int(j, kv.key, kv.n);        break;
        case EOS_HTTPD_VAL_BOOL: eos_json_kv_bool(j, kv.key, kv.n != 0);  break;
        default:                 eos_json_kv_str(j, kv.key, kv.s);        break;
        }
    }
    eos_json_obj_close(j);
}

// ------------------------------------------------------- GET /api/settings

static int h_settings_get(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_json_t j;

    if (!h->ports.settings_get)
        return fail_err(h, r, -7, "this board has no settings store");

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    settings_object(h, &j);
    eos_json_obj_close(&j);
    return reply_json(h, r, 200, &j);
}

// ------------------------------------------------------ POST /api/settings

// The body carries only the keys the page actually changed, so the scan is the
// other way round from what a JSON parser wants: for every key the store knows,
// ask whether the body mentions it. That is what lets the whole thing run
// through eos_json_get_*, which finds one key at a time and never builds a
// document tree — the same reader the settings file itself is parsed with.
//
// A key in the body that the store does not know is ignored rather than
// refused, which is eos_theme's rule and is here for the same reason: a page
// from a newer build must not be able to make an older board reject a save
// outright. The response restates the whole object, so nothing silently
// pretends to have applied.
static int h_settings_set(eos_httpd_t *h, const eos_httpd_req_t *req,
                          eos_httpd_resp_t *r)
{
    eos_json_t j;
    eos_httpd_kv_t kv;
    uint32_t reboot_bits = 0;
    char err_key[EOS_HTTPD_KEY_MAX];
    int i, n_keys = 0;
    int first_err = 0;

    err_key[0] = '\0';
    if (!h->ports.settings_get || !h->ports.settings_set)
        return fail_err(h, r, -7, "this board has no settings store");
    if (req->body_truncated)
        return fail_err(h, r, -9, "the settings patch is larger than this board accepts");
    if (!req->body || req->body_len <= 0)
        return fail(h, r, 400, "bad_argument", "no settings in the body");

    for (i = 0; i < SETTINGS_KEY_LIMIT; i++) {
        eos_json_find_t f;
        char val[EOS_HTTPD_VALUE_MAX];
        long num = 0;
        bool is_num = false, reboot = false;
        int rc;

        memset(&kv, 0, sizeof kv);
        if (!h->ports.settings_get(h->ctx, i, &kv)) break;
        n_keys++;
        if (!kv.key[0]) continue;

        val[0] = '\0';
        f = eos_json_get_str(req->body, req->body_len, kv.key,
                             val, (int)sizeof val, NULL);
        if (f == EOS_JSON_BAD)
            return fail(h, r, 400, "bad_argument", "the settings patch is not a JSON object");
        if (f == EOS_JSON_TOOBIG) {
            if (!first_err) { first_err = -9; KEEP_KEY(err_key, kv.key); }
            continue;
        }
        if (f == EOS_JSON_TYPE) {
            // Present and not a string. A number is what the page sends for
            // brain.port, brain.max and ui.bright; a bool is nobody's, but
            // reading it costs one comparison and turns a 500 into a 400.
            if (eos_json_get_int(req->body, req->body_len, kv.key, &num) == EOS_JSON_FOUND) {
                is_num = true;
            } else {
                bool b = false;
                if (eos_json_get_bool(req->body, req->body_len, kv.key, &b) == EOS_JSON_FOUND) {
                    is_num = true;
                    num = b ? 1 : 0;
                } else {
                    if (!first_err) { first_err = -1; KEEP_KEY(err_key, kv.key); }
                    continue;
                }
            }
        } else if (f != EOS_JSON_FOUND) {
            continue;                       // ABSENT: the page did not change it
        }

        rc = h->ports.settings_set(h->ctx, kv.key, val, is_num, num, &reboot);
        if (rc < 0) {
            if (!first_err) { first_err = rc; KEEP_KEY(err_key, kv.key); }
            continue;
        }
        if (reboot && i < 32) reboot_bits |= (uint32_t)1u << i;
    }

    // Always, and even after a failure: this is what hands a staged WiFi pair
    // to eos_net and what wipes a passphrase that is not going to be used.
    if (h->ports.settings_commit) h->ports.settings_commit(h->ctx);

    if (first_err) {
        char detail[64];
        snprintf(detail, sizeof detail, "%s was refused", err_key);
        return fail_err(h, r, first_err, detail);
    }

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    settings_object(h, &j);

    eos_json_key(&j, "reboot_required");
    eos_json_arr_open(&j);
    for (i = 0; i < n_keys && i < 32; i++) {
        if (!(reboot_bits & ((uint32_t)1u << i))) continue;
        memset(&kv, 0, sizeof kv);
        if (!h->ports.settings_get(h->ctx, i, &kv)) break;
        if (kv.key[0]) eos_json_str(&j, kv.key);
    }
    eos_json_arr_close(&j);
    eos_json_obj_close(&j);
    return reply_json(h, r, 200, &j);
}

// --------------------------------------------------------- GET /api/system

static void sys_fs_array(eos_httpd_t *h, eos_json_t *j)
{
    eos_httpd_fs_t m;
    int i;

    if (!h->ports.fs_info) return;      // a group the page skips rather than blanks

    eos_json_key(j, "fs");
    eos_json_arr_open(j);
    for (i = 0; i < EOS_MOUNTS_REPORTED; i++) {
        memset(&m, 0, sizeof m);
        if (!h->ports.fs_info(h->ctx, i, &m)) break;
        eos_json_obj_open(j);
        eos_json_kv_str (j, "point",     m.point);
        eos_json_kv_str (j, "fs",        m.fs);
        eos_json_kv_bool(j, "mounted",   m.mounted);
        eos_json_kv_bool(j, "writable",  m.writable);
        eos_json_kv_bool(j, "removable", m.removable);
        // Sizes are bytes and a card is gigabytes, so they are emitted through
        // the same 32-bit-safe path as everything else by clamping: a listing
        // that says 4 GB on an 8 GB card is wrong in a way nobody acts on, and
        // a 64-bit printf in this writer would be a second number formatter.
        eos_json_kv_int (j, "total", (long)(m.total > 0x7FFFFFFFULL ? 0x7FFFFFFFULL : m.total));
        eos_json_kv_int (j, "used",  (long)(m.used  > 0x7FFFFFFFULL ? 0x7FFFFFFFULL : m.used));
        eos_json_obj_close(j);
    }
    eos_json_arr_close(j);
}

static void sys_net_group(eos_httpd_t *h, eos_json_t *j)
{
    eos_httpd_net_t n;
    char mdns[40];

    if (!h->ports.net_status || !h->ports.net_status(h->ctx, &n)) return;

    eos_json_key(j, "net");
    eos_json_obj_open(j);
    eos_json_kv_str (j, "ip",       n.ip);
    eos_json_kv_str (j, "hostname", n.host);
    snprintf(mdns, sizeof mdns, "%s.local", n.host);
    eos_json_kv_str (j, "mdns",     n.host[0] ? mdns : "");
    eos_json_kv_strn(j, "ssid",     (const char *)n.ssid, n.ssid_len);
    eos_json_kv_int (j, "rssi",     n.rssi);
    eos_json_kv_bool(j, "up",       n.state == EOS_HTTPD_NET_UP);
    eos_json_obj_close(j);
}

static int h_system(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_httpd_sys_t s;
    eos_json_t j;
    char mac[18];

    if (!h->ports.sys_info)
        return fail_err(h, r, -7, "this board does not describe itself");

    memset(&s, 0, sizeof s);
    if (!h->ports.sys_info(h->ctx, &s))
        return fail_err(h, r, -3, "the board description could not be read");

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);

    eos_json_key(&j, "board");
    eos_json_obj_open(&j);
    eos_json_kv_str(&j, "id",      s.board_id);
    eos_json_kv_str(&j, "name",    s.board_name);
    eos_json_kv_str(&j, "summary", s.board_summary);
    eos_json_obj_close(&j);

    write_mac(mac, s.mac);
    eos_json_key(&j, "chip");
    eos_json_obj_open(&j);
    eos_json_kv_str(&j, "target",   s.chip_target);
    eos_json_kv_str(&j, "variant",  s.chip_variant);
    eos_json_kv_int(&j, "cores",    s.chip_cores);
    eos_json_kv_int(&j, "rev",      s.chip_rev);
    eos_json_kv_int(&j, "flash_mb", (long)s.flash_mb);
    eos_json_key(&j, "psram");
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "present", s.psram_present);
    eos_json_kv_str (&j, "type",    s.psram_present ? s.psram_type : "none");
    eos_json_kv_int (&j, "size_mb", (long)s.psram_mb);
    eos_json_obj_close(&j);
    eos_json_kv_str(&j, "mac", mac);
    eos_json_obj_close(&j);

    eos_json_key(&j, "render");
    eos_json_obj_open(&j);
    eos_json_kv_int (&j, "tier",       s.render_tier);
    eos_json_kv_str (&j, "compositor", s.compositor);
    eos_json_kv_bool(&j, "lvgl",       s.lvgl);
    eos_json_obj_close(&j);

    eos_json_key(&j, "display");
    eos_json_obj_open(&j);
    eos_json_kv_str (&j, "controller", s.disp_controller);
    eos_json_kv_int (&j, "w",          s.disp_w);
    eos_json_kv_int (&j, "h",          s.disp_h);
    eos_json_kv_int (&j, "rotation",   s.disp_rotation);
    eos_json_kv_str (&j, "bus",        s.disp_bus);
    eos_json_kv_int (&j, "clock_hz",   (long)s.disp_clock_hz);
    eos_json_kv_bool(&j, "backlight",  s.backlight);
    eos_json_obj_close(&j);

    eos_json_key(&j, "heap");
    eos_json_obj_open(&j);
    eos_json_kv_int(&j, "free",           (long)s.heap_free);
    eos_json_kv_int(&j, "min_free",       (long)s.heap_min_free);
    eos_json_kv_int(&j, "largest_block",  (long)s.heap_largest);
    eos_json_kv_int(&j, "total",          (long)s.heap_total);
    eos_json_obj_close(&j);

    sys_fs_array(h, &j);
    sys_net_group(h, &j);

    eos_json_kv_int(&j, "uptime_ms", (long)s.uptime_ms);

    eos_json_key(&j, "time");
    eos_json_obj_open(&j);
    eos_json_kv_int (&j, "epoch",  (long)s.epoch);
    eos_json_kv_str (&j, "tz",     s.tz);
    eos_json_kv_bool(&j, "synced", s.time_synced);
    eos_json_obj_close(&j);

    eos_json_key(&j, "fw");
    eos_json_obj_open(&j);
    eos_json_kv_str(&j, "version", s.fw_version);
    eos_json_kv_str(&j, "idf",     s.fw_idf);
    eos_json_kv_str(&j, "built",   s.fw_built);
    eos_json_obj_close(&j);

    // The one group the client changes behaviour on. Everything above it is
    // informational; chunk_max is what bounds every upload the Files tab sends.
    eos_json_key(&j, "limits");
    eos_json_obj_open(&j);
    eos_json_kv_int(&j, "chunk_max",  s.chunk_max);
    eos_json_kv_int(&j, "path_max",   s.path_max);
    eos_json_kv_int(&j, "name_max",   s.name_max);
    eos_json_kv_int(&j, "list_max",   s.list_max);
    eos_json_kv_int(&j, "open_files", s.open_files);
    eos_json_obj_close(&j);

    eos_json_obj_close(&j);
    return reply_json(h, r, 200, &j);
}

// -------------------------------------------------- GET /api/system/health

// The cheap liveness probe. The web app never calls it; a script watching a
// board over a weekend does, and it is three fields instead of two kilobytes.
static int h_system_health(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_httpd_sys_t s;
    eos_json_t j;

    if (!h->ports.sys_info)
        return fail_err(h, r, -7, "this board does not describe itself");

    memset(&s, 0, sizeof s);
    (void)h->ports.sys_info(h->ctx, &s);

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "ok",        true);
    eos_json_kv_int (&j, "uptime_ms", (long)s.uptime_ms);
    eos_json_kv_int (&j, "heap_free", (long)s.heap_free);
    eos_json_obj_close(&j);
    return reply_json(h, r, 200, &j);
}

// -------------------------------------------------- POST /api/system/reboot

// The delay is the whole endpoint. The port schedules; something on the OS
// loop restarts once the deadline passes, by which time this response has been
// written and the socket closed. Restarting inside the handler would drop the
// connection mid-response and the page would report a network failure for a
// reboot that worked perfectly.
#define REBOOT_DELAY_MS 500

static int h_system_reboot(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_json_t j;
    int rc;

    if (!h->ports.reboot)
        return fail_err(h, r, -7, "this board cannot restart itself");

    rc = h->ports.reboot(h->ctx, REBOOT_DELAY_MS);
    if (rc < 0) return fail_err(h, r, rc, "the restart was refused");

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "ok",    true);
    eos_json_kv_int (&j, "in_ms", REBOOT_DELAY_MS);
    eos_json_obj_close(&j);
    return reply_json(h, r, 200, &j);
}

// --------------------------------------------------------- GET /api/themes

// Themes the picker may offer beyond the active one. Bounded because the
// response is built in one pass into a fixed buffer and a card with fifty theme
// files must produce a short list rather than a truncated document.
#define THEMES_REPORTED 16

static void theme_entry(eos_json_t *j, const eos_httpd_theme_t *t)
{
    int k;

    eos_json_obj_open(j);
    eos_json_kv_str(j, "name", t->name);
    eos_json_kv_str(j, "path", t->path);

    // Colours only for the theme the board is actually wearing. The web app
    // reads them from that entry alone — it writes them straight into CSS
    // custom properties so the page wears the same theme as the panel — and
    // parsing every file on the card to fill in the rest would mean a 4 KB read
    // buffer and a 700-byte theme struct on a 5 KB worker stack.
    if (t->has_colors) {
        eos_json_key(j, "colors");
        eos_json_obj_open(j);
        for (k = 0; k < EOS_HTTPD_THEME_ROLES; k++)
            eos_json_kv_str(j, eos_httpd_role_names[k], t->color[k]);
        eos_json_obj_close(j);

        eos_json_key(j, "metrics");
        eos_json_obj_open(j);
        eos_json_kv_int(j, "gap",    t->gap);
        eos_json_kv_int(j, "border", t->border);
        eos_json_kv_int(j, "bar_h",  t->bar_h);
        eos_json_kv_int(j, "tab_h",  t->tab_h);
        eos_json_kv_int(j, "radius", t->radius);
        eos_json_obj_close(j);
    }
    eos_json_obj_close(j);
}

static int h_themes(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_httpd_theme_t t;
    eos_json_t j;
    char active[sizeof t.name];
    int i, listed = 0;

    if (!h->ports.theme_active)
        return fail_err(h, r, -7, "this board has no theme service");

    memset(&t, 0, sizeof t);
    if (!h->ports.theme_active(h->ctx, &t))
        return fail_err(h, r, -3, "the active theme could not be read");
    snprintf(active, sizeof active, "%s", t.name);

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_str(&j, "active", active);
    eos_json_key(&j, "themes");
    eos_json_arr_open(&j);

    // The active theme first and always, even when no file for it exists. That
    // is the "the picker is never empty" rule: a board whose filesystem is
    // blank still offers the theme it is wearing, which came out of the image.
    theme_entry(&j, &t);
    listed++;

    for (i = 0; h->ports.theme_list && listed < THEMES_REPORTED; i++) {
        int need;
        memset(&t, 0, sizeof t);
        if (!h->ports.theme_list(h->ctx, i, &t)) break;
        if (!t.name[0]) continue;
        if (strcmp(t.name, active) == 0) continue;      // already emitted, with colours

        // Room for this entry, checked before it is written rather than
        // discovered afterwards: the writer's overflow is sticky and has no
        // rollback, so one entry too many would cost the whole document.
        need = eos_json_escaped_len(t.name, (int)strlen(t.name))
             + eos_json_escaped_len(t.path, (int)strlen(t.path)) + 32;
        if (j.len + need > j.cap - 16) break;

        t.has_colors = false;
        theme_entry(&j, &t);
        listed++;
    }

    eos_json_arr_close(&j);
    eos_json_obj_close(&j);
    return reply_json(h, r, 200, &j);
}

// ==========================================================================
// Static files and the portal
// ==========================================================================

// The last-resort setup page. It is not the web app: it is the smallest thing
// that can list networks and join one, and it exists so that a board whose /int
// is empty — every board that has never been provisioned — is still a board you
// can get onto a network. No external resource, no font, no image; it has to
// render on a phone that has this SoftAP and nothing else.
static const char BUILTIN_SETUP[] =
"<!doctype html><html lang=en><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>penguinOS setup</title>"
"<style>"
"body{margin:0;padding:16px;background:#262624;color:#e8e4dc;"
"font:16px/1.5 system-ui,-apple-system,sans-serif}"
"h1{font-size:18px;margin:0 0 4px}p{margin:4px 0 16px;color:#9a948a;font-size:14px}"
"button,input{font:inherit;box-sizing:border-box;width:100%;padding:10px;"
"border-radius:6px;border:1px solid #46443f;background:#1c1b1a;color:#e8e4dc}"
"button{background:#d88e56;color:#1c1b1a;border:0;margin-top:10px;font-weight:600}"
"li{list-style:none;margin:0 0 6px}"
".n{display:flex;justify-content:space-between;gap:8px;cursor:pointer;"
"padding:10px;border:1px solid #46443f;border-radius:6px;background:#1c1b1a}"
".n.on{border-color:#d88e56}.s{color:#9a948a;font-size:13px;white-space:nowrap}"
"#m{margin-top:12px;font-size:14px;min-height:20px}ul{padding:0;margin:0}"
"</style>"
"<h1>penguinOS setup</h1>"
"<p>Pick a network. The password is stored only after the board actually joins.</p>"
"<ul id=l></ul>"
"<button id=r>Rescan (this will blink the connection)</button>"
"<input id=p type=password placeholder=\"network password\" autocomplete=off>"
"<button id=j>Join</button><div id=m></div>"
"<script>"
"var sel=null,L=document.getElementById('l'),M=document.getElementById('m');"
"function esc(s){return s.replace(/[&<>]/g,function(c){"
"return {'&':'&amp;','<':'&lt;','>':'&gt;'}[c]})}"
"function draw(d){L.innerHTML='';(d.networks||[]).forEach(function(n){"
"var li=document.createElement('li');var b=document.createElement('div');"
"b.className='n'+(sel&&sel.h===n.ssid_hex?' on':'');"
"b.innerHTML='<span>'+(n.hidden?'<i>hidden</i>':esc(n.ssid))+'</span>"
"<span class=s>'+n.rssi+' dBm '+(n.secure?'lock':'open')+'</span>';"
"b.onclick=function(){sel={h:n.ssid_hex,s:n.ssid};draw(d)};"
"li.appendChild(b);L.appendChild(li)})}"
"function load(q){fetch('/api/wifi/scan'+(q||'')).then(function(r){return r.json()})"
".then(function(d){draw(d);if(d.scanning)setTimeout(load,1200)})"
".catch(function(e){M.textContent=e})}"
"document.getElementById('r').onclick=function(){M.textContent='scanning';"
"load('?rescan=1')};"
"document.getElementById('j').onclick=function(){if(!sel){M.textContent="
"'pick a network first';return}M.textContent='joining';"
"fetch('/api/wifi/connect',{method:'POST',body:JSON.stringify("
"{ssid_hex:sel.h,psk:document.getElementById('p').value})})"
".then(function(r){return r.json()}).then(function(d){"
"if(d.error){M.textContent=d.detail;return}poll()})};"
"function poll(){fetch('/api/net/status').then(function(r){return r.json()})"
".then(function(d){M.textContent=d.join.detail;"
"if(d.join.last==='ok'){M.textContent='joined. this board is now at '+d.sta.ip+"
"' \\u2014 rejoin your own network';return}"
"if(d.join.running||d.state==='joining'){setTimeout(poll,1000)}})}"
"load();"
"</script>";

static bool try_file(eos_httpd_t *h, eos_httpd_resp_t *r, const char *root,
                     const char *path, bool gz)
{
    long size = -1;
    void *fh;
    size_t rl = strlen(root), pl = strlen(path);

    if (!h->ports.file_open) return false;
    if (rl + pl + (gz ? 3u : 0u) + 1u > sizeof h->path) return false;

    memcpy(h->path, root, rl);
    memcpy(h->path + rl, path, pl);
    if (gz) memcpy(h->path + rl + pl, ".gz", 3);
    h->path[rl + pl + (gz ? 3u : 0u)] = 0;

    fh = h->ports.file_open(h->ctx, h->path, &size);
    if (!fh) return false;

    r->status           = 200;
    r->kind             = EOS_HTTPD_BODY_FILE;
    r->path             = h->path;
    r->file             = fh;
    r->file_size        = size;
    r->content_type     = eos_httpd_mime(h->path);
    r->content_encoding = gz ? "gzip" : NULL;
    // In SETUP the page is the instrument panel and must never come from a
    // cache; in RUN it is a 110 KB app served over a link the board is sharing
    // with everything else, and caching it is the difference between a reload
    // costing nothing and costing four seconds.
    r->cache_control    = (h->cfg.mode == EOS_HTTPD_MODE_RUN)
                          ? "public, max-age=600" : "no-store";
    return true;
}

static int redirect_portal(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    snprintf(h->loc, sizeof h->loc, "http://%s/",
             h->cfg.portal_ip ? h->cfg.portal_ip : "192.168.4.1");
    r->status        = 302;
    r->kind          = EOS_HTTPD_BODY_REDIRECT;
    r->location      = h->loc;
    r->content_type  = "text/html; charset=utf-8";
    r->cache_control = "no-store";
    // A body, because a handful of captive-portal clients render the response
    // instead of following it, and a blank page reads as a broken board.
    r->body          = "<html><body>penguinOS setup: <a href=\"/\">open the setup page</a></body></html>";
    r->body_len      = (int)strlen(r->body);
    h->req_portal++;
    return r->status;
}

const char *eos_httpd_builtin_setup(int *len_out)
{
    if (len_out) *len_out = (int)sizeof BUILTIN_SETUP - 1;
    return BUILTIN_SETUP;
}

static int h_static(eos_httpd_t *h, const char *path, eos_httpd_resp_t *r)
{
    const char *root = (h->cfg.mode == EOS_HTTPD_MODE_RUN) ? h->cfg.root_run
                                                           : h->cfg.root_setup;
    const char *want = (path[0] == '/' && path[1] == 0) ? "/index.html" : path;
    bool is_index = strcmp(want, "/index.html") == 0;

    if (!root) root = "";

    if (try_file(h, r, root, want, true))  return r->status;
    if (try_file(h, r, root, want, false)) return r->status;

    if (h->cfg.mode == EOS_HTTPD_MODE_SETUP) {
        if (is_index) {
            int n;
            r->status           = 200;
            r->kind             = EOS_HTTPD_BODY_BUF;
            r->content_type     = "text/html; charset=utf-8";
            r->content_encoding = NULL;
            r->cache_control    = "no-store";
            r->body             = eos_httpd_builtin_setup(&n);
            r->body_len         = n;
            return 200;
        }
        // Anything else in SETUP is a client looking for the internet. Send it
        // to the page rather than to a 404 it will render as "no connection".
        return redirect_portal(h, r);
    }

    return fail_err(h, r, -5, "no such file on this board");
}

// ==========================================================================
// Dispatch
// ==========================================================================

void eos_httpd_cfg_default(eos_httpd_cfg_t *cfg)
{
    if (!cfg) return;
    cfg->mode       = EOS_HTTPD_MODE_SETUP;
    cfg->root_setup = "/int/setup";
    cfg->root_run   = "/int/web";
    cfg->portal_ip  = "192.168.4.1";
    cfg->port       = 80;
    cfg->workers    = 4;
}

void eos_httpd_init(eos_httpd_t *h, const eos_httpd_ports_t *ports, void *ctx,
                    const eos_httpd_cfg_t *cfg)
{
    if (!h) return;
    memset(h, 0, sizeof *h);
    if (ports) h->ports = *ports;
    h->ctx = ctx;
    eos_httpd_cfg_default(&h->cfg);
    if (cfg) {
        h->cfg = *cfg;
        if (!h->cfg.root_setup) h->cfg.root_setup = "/int/setup";
        if (!h->cfg.root_run)   h->cfg.root_run   = "/int/web";
        if (!h->cfg.portal_ip)  h->cfg.portal_ip  = "192.168.4.1";
        if (!h->cfg.port)       h->cfg.port       = 80;
        if (!h->cfg.workers)    h->cfg.workers    = 4;
    }
}

int eos_httpd_dispatch(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_route_t route;
    int n;

    if (!h || !req || !r) return 500;

    memset(r, 0, sizeof *r);
    r->file_size = -1;
    h->req_total++;

    if (!req->method || !req->uri)
        return fail(h, r, 400, "bad_argument", "malformed request line");

    // The URI is bounded before anything looks at it, so a 4 KB request target
    // costs one comparison rather than a decode into a stack buffer.
    if (strlen(req->uri) >= (size_t)EOS_HTTPD_URI_MAX)
        return fail(h, r, 414, "too_big", "the request target is longer than this board accepts");

    n = eos_httpd_path_of(req->uri, h->uripath, (int)sizeof h->uripath);
    if (n < 0)
        return fail(h, r, 400, "bad_argument", "the request target is not a valid path");

    route = eos_httpd_route(req->method, req->uri);

    switch (route) {
    case EOS_ROUTE_METHOD:
        return fail(h, r, 405, "unsupported", "this board answers GET and POST only");

    case EOS_ROUTE_WIFI_SCAN:    h->req_api++; return h_wifi_scan(h, req, r);
    case EOS_ROUTE_WIFI_CONNECT: h->req_api++; return h_wifi_connect(h, req, r);
    case EOS_ROUTE_WIFI_FORGET:  h->req_api++; return h_wifi_forget(h, r);
    case EOS_ROUTE_NET_STATUS:   h->req_api++; return h_net_status(h, r);
    case EOS_ROUTE_BLE_SCAN:     h->req_api++; return h_ble_scan(h, req, r);
    case EOS_ROUTE_BLE_PAIR:     h->req_api++; return h_ble_pair(h, req, r);
    case EOS_ROUTE_BLE_STATUS:   h->req_api++; return h_ble_status(h, r);
    case EOS_ROUTE_BLE_FORGET:   h->req_api++; return h_ble_forget(h, r);

    // megabrain
    case EOS_ROUTE_BRAIN_STATUS: h->req_api++; return h_brain_status(h, r);
    case EOS_ROUTE_BRAIN_ASK:    h->req_api++; return h_brain_ask(h, req, r);
    case EOS_ROUTE_BRAIN_CANCEL: h->req_api++; return h_brain_cancel(h, r);

    // ---- kernel/svc/eos_apps.c: files, console, buddy, apps --------------
    // All fourteen go to one call. Listing them rather than range-checking the
    // enum is deliberate: three people append to eos_route_t and a range is the
    // thing that silently swallows the next route somebody inserts.
    case EOS_ROUTE_FS_LIST:      case EOS_ROUTE_FS_STAT:
    case EOS_ROUTE_FS_READ:      case EOS_ROUTE_FS_USAGE:
    case EOS_ROUTE_FS_WRITE:     case EOS_ROUTE_FS_ABORT:
    case EOS_ROUTE_FS_MKDIR:     case EOS_ROUTE_FS_REMOVE:
    case EOS_ROUTE_FS_RENAME:    case EOS_ROUTE_CONSOLE_LOG:
    case EOS_ROUTE_CONSOLE_EXEC: case EOS_ROUTE_BUDDY:
    case EOS_ROUTE_BUDDY_RELOAD: case EOS_ROUTE_APPS:
        h->req_api++;
        if (!s_api)
            return fail_err(h, r, -7, "this image was built without the files, "
                                      "console and buddy API");
        return s_api(h, (int)route, req, r);

    case EOS_ROUTE_SETTINGS_GET:   h->req_api++; return h_settings_get(h, r);
    case EOS_ROUTE_SETTINGS_SET:   h->req_api++; return h_settings_set(h, req, r);
    case EOS_ROUTE_SYSTEM:         h->req_api++; return h_system(h, r);
    case EOS_ROUTE_SYSTEM_HEALTH:  h->req_api++; return h_system_health(h, r);
    case EOS_ROUTE_SYSTEM_REBOOT:  h->req_api++; return h_system_reboot(h, r);
    case EOS_ROUTE_THEMES:         h->req_api++; return h_themes(h, r);

    case EOS_ROUTE_CAPTIVE:
        // In RUN mode the board is a host on somebody's LAN, not a portal.
        // Redirecting a probe there would tell every device on the network that
        // this one is a walled garden.
        if (h->cfg.mode == EOS_HTTPD_MODE_SETUP) return redirect_portal(h, r);
        h->req_static++;
        return h_static(h, h->uripath, r);

    case EOS_ROUTE_STATIC:
        h->req_static++;
        return h_static(h, h->uripath, r);

    default:
        break;
    }

    if (h->cfg.mode == EOS_HTTPD_MODE_SETUP && strncmp(h->uripath, "/api/", 5) != 0)
        return redirect_portal(h, r);
    return fail_err(h, r, -5, "no such endpoint on this board");
}

// ==========================================================================
// ESP-IDF bindings
// ==========================================================================
//
// Everything above this line is portable C99 and is what the host test runs.
// Below it: esp_http_server, the captive DNS responder, and the adapter from
// the port table onto eos_net and eos_ble.

#ifdef ESP_PLATFORM

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "eos_httpd";

// ---------------------------------------------------------------- file ports
//
// There are none here, on purpose, and this is a change from the first cut of
// this file. It used to wire file_open/read/close straight onto eos_storage,
// which made every image that starts this server drag in a storage backend —
// and kernel/hal has no storage backend yet, so the reference was undefined the
// moment anything called eos_httpd_start(). More to the point it was the wrong
// layer: a port table exists so the binding happens at the edge.
//
// eos_httpd_idf_bind() therefore leaves the three file ports NULL, which the
// header already documents as "no filesystem": every EOS_ROUTE_STATIC answers
// 404 and the built-in setup page — which needs no filesystem — is what SETUP
// serves. When kernel/hal grows a storage backend, the four lines that turn
// static serving back on go in the boot glue after the bind:
//
//     h.ports.file_open  = my_open;    // eos_storage_open + eos_storage_size
//     h.ports.file_read  = my_read;
//     h.ports.file_close = my_close;
//
// ------------------------------------------------------------- the responder

static const char *status_line(int s)
{
    switch (s) {
    case 200: return "200 OK";
    case 202: return "202 Accepted";
    case 302: return "302 Found";
    case 400: return "400 Bad Request";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 414: return "414 URI Too Long";
    case 500: return "500 Internal Server Error";
    case 501: return "501 Not Implemented";
    case 503: return "503 Service Unavailable";
    default:  return "500 Internal Server Error";
    }
}

// ------------------------------------------------------- the brain stream
//
// Runs with the dispatch lock RELEASED, which is the whole reason it can take
// seconds. Nothing below touches h->resp, h->path or h->uripath; the only
// server state it reads is the port table, which does not change while the
// server is up, and the ring behind brain_read has its own lock.

// The FreeRTOS tick rather than esp_timer, so this file keeps the same
// component dependencies it had before megabrain arrived. It is only ever used
// for two deadlines measured in tens of seconds, and it wraps in 49 days at
// 1 kHz — which unsigned subtraction handles and nothing here spans anyway.
static uint32_t ms_now(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// An error the model raised after the 200 was already on the wire. web/README.md
// spells this: a line beginning "! ", then close the stream.
static void stream_bang(httpd_req_t *rq, const char *why, bool sent_any)
{
    char line[128];
    int n = snprintf(line, sizeof line, "%s! %s\n", sent_any ? "\n" : "",
                     why ? why : "megabrain stopped answering");
    if (n > 0) (void)httpd_resp_send_chunk(rq, line, (size_t)n);
}

static esp_err_t send_stream(httpd_req_t *rq, eos_httpd_t *h)
{
    char     buf[EOS_HTTPD_STREAM_CHUNK];
    uint32_t t0 = ms_now(), t_last = t0;
    long     sent = 0;
    bool     sent_any = false, clean = false;

    if (!h->ports.brain_read) return ESP_FAIL;

    for (;;) {
        int n = h->ports.brain_read(h->ctx, buf, (int)sizeof buf);

        if (n > 0) {
            t_last = ms_now();
            if (httpd_resp_send_chunk(rq, buf, (size_t)n) != ESP_OK) {
                // The client is gone. Stop the model rather than let it talk
                // into a ring nobody is draining, and let the worker go.
                ESP_LOGW(TAG, "brain  client left after %ld B; cancelling", sent);
                if (h->ports.brain_cancel) h->ports.brain_cancel(h->ctx);
                return ESP_FAIL;
            }
            sent_any = true;
            sent += n;
            continue;
        }
        if (n == EOS_HTTPD_STREAM_END) { clean = true; break; }
        if (n == EOS_HTTPD_STREAM_FAIL) {
            eos_httpd_brain_t st;
            const char *why = NULL;
            memset(&st, 0, sizeof st);
            if (h->ports.brain_status && h->ports.brain_status(h->ctx, &st))
                why = st.last_error;
            stream_bang(rq, why, sent_any);
            break;
        }

        // Nothing decoded yet. Both deadlines exist because a model that has
        // stopped talking must not hold a worker for the length of a session.
        if ((ms_now() - t_last) >= EOS_HTTPD_STREAM_IDLE_MS) {
            if (h->ports.brain_cancel) h->ports.brain_cancel(h->ctx);
            stream_bang(rq, "megabrain went quiet", sent_any);
            break;
        }
        if ((ms_now() - t0) >= EOS_HTTPD_STREAM_TOTAL_MS) {
            if (h->ports.brain_cancel) h->ports.brain_cancel(h->ctx);
            stream_bang(rq, "megabrain took too long", sent_any);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(EOS_HTTPD_STREAM_POLL_MS));
    }

    // Only a clean END leaves the client's side of the reply released. On every
    // other exit the last read was not the one that finished it, so say so
    // once: it is the difference between the next ask working and the next ask
    // being 409 until the binding notices for itself.
    if (!clean && h->ports.brain_cancel) h->ports.brain_cancel(h->ctx);

    return httpd_resp_send_chunk(rq, NULL, 0);
}

static esp_err_t send_resp(httpd_req_t *rq, eos_httpd_t *h, eos_httpd_resp_t *r)
{
    esp_err_t err = ESP_OK;

    httpd_resp_set_status(rq, status_line(r->status));
    if (r->content_type)     httpd_resp_set_type(rq, r->content_type);
    if (r->content_encoding) httpd_resp_set_hdr(rq, "Content-Encoding", r->content_encoding);
    if (r->cache_control)    httpd_resp_set_hdr(rq, "Cache-Control", r->cache_control);
    if (r->location)         httpd_resp_set_hdr(rq, "Location", r->location);
    // The setup page is same-origin and the SoftAP is WPA2, but the API is
    // still reachable from any page the phone has open, so say no.
    httpd_resp_set_hdr(rq, "X-Content-Type-Options", "nosniff");

    if (r->kind == EOS_HTTPD_BODY_STREAM)
        return send_stream(rq, h);

    if (r->kind != EOS_HTTPD_BODY_FILE)
        return httpd_resp_send(rq, r->body ? r->body : "", r->body_len);

    // Streamed from flash, EOS_HTTPD_CHUNK at a time. The web app is ~110 KB
    // and the largest free block on a board with WiFi up will not hold it.
    {
        static char chunk[EOS_HTTPD_CHUNK];   // one server, one sender: see the lock
        for (;;) {
            int got = h->ports.file_read(h->ctx, r->file, chunk, (int)sizeof chunk);
            if (got < 0) { err = ESP_FAIL; goto done; }
            if (got == 0) break;
            err = httpd_resp_send_chunk(rq, chunk, got);
            if (err != ESP_OK) goto done;
        }
        err = httpd_resp_send_chunk(rq, NULL, 0);
    }
done:
    h->ports.file_close(h->ctx, r->file);
    r->file = NULL;
    return err;
}

// One handler for everything. esp_http_server's own URI table would be a second
// route table to keep in step with eos_httpd_route(), and two route tables is
// how a captive portal ends up 404ing the probe it was written to answer.
static esp_err_t on_request(httpd_req_t *rq)
{
    eos_httpd_t *h = (eos_httpd_t *)rq->user_ctx;
    eos_httpd_req_t req;
    eos_httpd_resp_t resp;
    esp_err_t err;
    bool oversize = false, streaming = false;
    int blen = 0;

    // The body lives here, on this worker's stack, and is read before the lock
    // is taken. A client that declares a content length and then stops sending
    // would otherwise hold every other request behind it.
    char body[EOS_HTTPD_BODY_MAX + 1];

    memset(&req, 0, sizeof req);
    req.method = (rq->method == HTTP_POST) ? "POST" : (rq->method == HTTP_GET) ? "GET" : "?";
    req.uri    = rq->uri;

    if (rq->content_len > 0) {
        if (rq->content_len > (size_t)EOS_HTTPD_BODY_MAX) {
            oversize = true;
        } else {
            int want = (int)rq->content_len, got = 0, stalls = 0;
            while (got < want) {
                int n = httpd_req_recv(rq, body + got, (size_t)(want - got));
                if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                    // Bounded, not endless. Each timeout is recv_wait_timeout
                    // seconds; a client that has gone quiet twice has gone.
                    if (++stalls > 2) { got = -1; break; }
                    continue;
                }
                if (n <= 0) { got = -1; break; }
                got += n;
            }
            if (got < 0) return ESP_FAIL;    // no lock held, nothing to release
            blen = got;
            body[blen] = 0;
            req.body = body;
            req.body_len = blen;
        }
    }
    req.body_truncated = oversize;

    if (h->lock) xSemaphoreTake((SemaphoreHandle_t)h->lock, portMAX_DELAY);

    eos_httpd_dispatch(h, &req, &resp);

    // A megabrain reply is seconds long. Holding the dispatch lock across it
    // would park the other three workers behind one chat request — the web app
    // polls /api/net/status and /api/console/log the whole time — so the lock
    // goes back the moment the response is staged. It is safe because a stream
    // reads none of the shared buffers the lock exists to protect: it drains a
    // ring that has its own. Every other response still holds it, because they
    // all point at h->resp.
    streaming = (resp.kind == EOS_HTTPD_BODY_STREAM);
    if (streaming && h->lock) xSemaphoreGive((SemaphoreHandle_t)h->lock);

    err = send_resp(rq, h, &resp);

    if (!streaming && h->lock) xSemaphoreGive((SemaphoreHandle_t)h->lock);

    // An oversized body was never drained, so the socket still holds it and the
    // next request on this keep-alive connection would start mid-JSON. Answer
    // the 413 first, then close.
    if (oversize) return ESP_FAIL;
    return err;
}


// ------------------------------------------------------------------ lifecycle
//
// No DNS responder here. The captive-portal DNS belongs to eos_net, which
// raises it with the SoftAP and tears it down with it — it is the thing that
// knows when 192.168.4.1 is real. A second listener on port 53 would only fail
// to bind, and the one that lost the race would be the one nobody noticed.

int eos_httpd_start(eos_httpd_t *h)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t srv = NULL;
    esp_err_t err;

    if (!h) return -1;
    if (h->running) return 0;

    if (!h->lock) {
        h->lock = xSemaphoreCreateMutex();
        if (!h->lock) return -4;
    }

    cfg.server_port       = h->cfg.port;
    cfg.max_open_sockets  = h->cfg.workers;
    cfg.max_uri_handlers  = 2;
    cfg.max_resp_headers  = EOS_HTTPD_HDR_MAX;
    cfg.stack_size        = 5376;   // the request body is on this stack
    cfg.recv_wait_timeout = 6;
    cfg.send_wait_timeout = 6;
    cfg.lru_purge_enable  = true;    // a phone that walks away must not hold a worker
    cfg.uri_match_fn      = httpd_uri_match_wildcard;

    err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err)); return -3; }

    // One handler for everything, because esp_http_server's own URI table would
    // be a second route table to keep in step with eos_httpd_route(), and two
    // route tables is how a portal ends up 404ing the probe it exists to answer.
    {
        httpd_uri_t any_get  = { .uri = "/*", .method = HTTP_GET,
                                 .handler = on_request, .user_ctx = h };
        httpd_uri_t any_post = { .uri = "/*", .method = HTTP_POST,
                                 .handler = on_request, .user_ctx = h };
        httpd_register_uri_handler(srv, &any_get);
        httpd_register_uri_handler(srv, &any_post);
    }

    h->impl = srv;
    h->running = true;

    ESP_LOGI(TAG, "http on :%u, mode %s, root %s", (unsigned)h->cfg.port,
             h->cfg.mode == EOS_HTTPD_MODE_RUN ? "run" : "setup",
             h->cfg.mode == EOS_HTTPD_MODE_RUN ? h->cfg.root_run : h->cfg.root_setup);
    return 0;
}

int eos_httpd_stop(eos_httpd_t *h)
{
    if (!h || !h->running) return 0;
    if (h->impl) httpd_stop((httpd_handle_t)h->impl);
    h->impl = NULL;
    h->running = false;
    return 0;
}

// ================================================================== adapter
//
// The port table, bound to the services that actually own the radios. This is
// the only part of the file that knows eos_net and eos_ble exist, which is what
// keeps the nine handlers above host-testable.
//
// The shape it has to absorb: eos_net_try() blocks for up to fifteen seconds
// and eos_net_scan(force) blocks for about three, and eos_net.h says in as many
// words not to call the first one from an HTTP handler — the phone asking for
// the join is a client of the SoftAP the radio is about to re-mode. So the
// ports queue, and eos_httpd_pump() drains from the OS loop.

#include "eos_net.h"
#include "eos_ble.h"

// One HTTP server per image, so this is a file static like eos_ble's own state.
// The HTTP workers only ever set the two queue flags and read; eos_httpd_pump()
// on the OS loop is the only writer of everything else. That is why no lock is
// needed here beyond the one already serialising the workers.
typedef struct {
    eos_net_t     *net;

    volatile bool  join_q;         // a join is queued
    volatile bool  join_busy;      // the pump is inside eos_net_try
    char           q_ssid[EOS_NET_SSID_MAX];
    char           q_psk[EOS_NET_PSK_MAX];
    uint8_t        join_last;      // eos_httpd_join_t

    volatile bool  forget_q;       // a forget is queued

    volatile bool  scan_q;
    volatile bool  scan_busy;
    bool           scan_ever;
    uint32_t       scan_done_ms;
    uint32_t       now_ms;

    eos_ble_dev_t  bdev[EOS_BLE_SCAN_MAX];
    int            bn;
} bind_t;

static bind_t s_b;

// eos_net has its own auth and error enums, and this file deliberately does not
// use them: the handlers must compile with no service layer present at all.
// Mapping them here is the price, and it is one switch rather than a coupling.
static uint8_t auth_map(int a)
{
    switch (a) {
    case EOS_NET_AUTH_OPEN:       return EOS_HTTPD_AUTH_OPEN;
    case EOS_NET_AUTH_WEP:        return EOS_HTTPD_AUTH_WEP;
    case EOS_NET_AUTH_WPA:        return EOS_HTTPD_AUTH_WPA;
    case EOS_NET_AUTH_WPA2:       return EOS_HTTPD_AUTH_WPA2;
    case EOS_NET_AUTH_WPA_WPA2:   return EOS_HTTPD_AUTH_WPA_WPA2;
    case EOS_NET_AUTH_WPA3:       return EOS_HTTPD_AUTH_WPA3;
    case EOS_NET_AUTH_WPA2_WPA3:  return EOS_HTTPD_AUTH_WPA2_WPA3;
    case EOS_NET_AUTH_ENTERPRISE: return EOS_HTTPD_AUTH_ENTERPRISE;
    default:                      return EOS_HTTPD_AUTH_OTHER;
    }
}

// eos_net reports one ERR_JOIN for every way an association can fail, so a
// refused password and an absent network arrive here indistinguishable. Rather
// than guess, both become FAILED, whose sentence is the true one: the join did
// not work and nothing was written.
static uint8_t join_map(eos_net_err_t e)
{
    switch (e) {
    case EOS_NET_OK:       return EOS_HTTPD_JOIN_OK;
    case EOS_NET_ERR_BUSY: return EOS_HTTPD_JOIN_RUNNING;
    default:               return EOS_HTTPD_JOIN_FAILED;
    }
}

// ------------------------------------------------------------- wifi ports

static int b_wifi_scan_state(void *c)
{
    (void)c;
    if (s_b.scan_q || s_b.scan_busy) return EOS_HTTPD_SCAN_RUNNING;
    if (!s_b.net) return EOS_HTTPD_SCAN_IDLE;
    return eos_net_scan_cached(s_b.net) ? EOS_HTTPD_SCAN_DONE : EOS_HTTPD_SCAN_IDLE;
}

static int b_wifi_scan_count(void *c)
{
    const eos_net_ap_t *aps;
    (void)c;
    if (!s_b.net) return 0;
    return eos_net_scan_results(s_b.net, &aps);
}

static bool b_wifi_scan_get(void *c, int i, eos_httpd_ap_t *o)
{
    const eos_net_ap_t *aps;
    const char *cur;
    size_t l;
    int n;

    (void)c;
    if (!s_b.net || !o) return false;
    n = eos_net_scan_results(s_b.net, &aps);
    if (i < 0 || i >= n) return false;

    memset(o, 0, sizeof *o);
    // eos_net keeps the SSID as a NUL-terminated string and has already dropped
    // hidden networks and deduplicated, so a length is all there is to recover.
    // The BSSID it does not keep at all; the handler emits null for that.
    l = strlen(aps[i].ssid);
    if (l > sizeof o->ssid) l = sizeof o->ssid;
    memcpy(o->ssid, aps[i].ssid, l);
    o->ssid_len = (uint8_t)l;
    o->rssi     = aps[i].rssi;
    o->channel  = aps[i].channel;
    o->auth     = auth_map((int)aps[i].auth);

    cur = eos_net_ssid(s_b.net);
    o->saved = eos_net_has_credentials(s_b.net) && cur && strcmp(aps[i].ssid, cur) == 0;
    return true;
}

static uint32_t b_wifi_scan_age(void *c)
{
    (void)c;
    if (!s_b.scan_ever) return 0;
    return s_b.now_ms - s_b.scan_done_ms;
}

static int b_wifi_scan_start(void *c)
{
    (void)c;
    if (!s_b.net) return -7;
    if (s_b.scan_q || s_b.scan_busy || s_b.join_q || s_b.join_busy) return -8;
    s_b.scan_q = true;
    return 0;
}

static int b_wifi_join(void *c, const uint8_t *ssid, int sl, const char *psk, int pl)
{
    (void)c;
    if (!s_b.net) return -7;
    if (s_b.join_q || s_b.join_busy) return -8;
    if (sl < 1 || sl > 32 || pl < 0 || pl > 63) return -1;
    // eos_net_try() takes a C string, so an SSID with a NUL inside it cannot be
    // handed on. Refusing is the honest answer; truncating would join a
    // different network from the one the user picked.
    if (memchr(ssid, 0, (size_t)sl) != NULL) return -1;

    memcpy(s_b.q_ssid, ssid, (size_t)sl);
    s_b.q_ssid[sl] = 0;
    memcpy(s_b.q_psk, psk, (size_t)pl);
    s_b.q_psk[pl] = 0;
    s_b.join_last = EOS_HTTPD_JOIN_RUNNING;
    s_b.join_q = true;             // set last: the pump may run the moment it is
    return 0;
}

// Queued, not run here, for the same three reasons the join is. eos_net_forget()
// erases NVS, drops the station link — which is the socket this request arrived
// on in RUN mode, so the 200 could never reach the phone — brings the SoftAP up
// with esp_wifi_set_mode/set_config, starts the portal DNS task, and then calls
// eos_net_scan(), which on a board that booted straight into RUN has no cached
// result and therefore blocks for seconds. Doing all of that on an HTTP worker
// held the server's mutex across it and made eos_net_t and esp_wifi's mode
// state have two writers, the other being eos_net_pump() on the frame loop.
static int b_wifi_forget(void *c)
{
    (void)c;
    if (!s_b.net) return -7;
    s_b.join_last = EOS_HTTPD_JOIN_NONE;
    // A join queued a moment ago is moot: the network it would save is the one
    // being dropped, and letting it run first would try to persist it.
    s_b.join_q    = false;
    s_b.q_ssid[0] = 0;
    s_b.forget_q  = true;
    return 0;
}

static bool b_net_status(void *c, eos_httpd_net_t *o)
{
    eos_net_mode_t m;
    const char *p;
    size_t l;

    (void)c;
    if (!s_b.net || !o) return false;
    memset(o, 0, sizeof *o);

    m = eos_net_mode(s_b.net);
    o->state = (m == EOS_NET_STA)     ? EOS_HTTPD_NET_UP
             : (m == EOS_NET_JOINING) ? EOS_HTTPD_NET_JOINING
             : (m == EOS_NET_SETUP)   ? EOS_HTTPD_NET_SETUP
             :                          EOS_HTTPD_NET_DOWN;

    // A queued join has not reached eos_net yet, and the second POST that would
    // race it arrives before the pump runs. Reporting JOINING from the queue is
    // what makes the handler's own busy check true in that window.
    if (s_b.join_q || s_b.join_busy || eos_net_trying(s_b.net))
        o->state = EOS_HTTPD_NET_JOINING;

    o->join = s_b.join_last;

    p = eos_net_ssid(s_b.net);
    if (p) { l = strlen(p); if (l > sizeof o->ssid) l = sizeof o->ssid;
             memcpy(o->ssid, p, l); o->ssid_len = (uint8_t)l; }

    o->ssid_stored = eos_net_has_credentials(s_b.net);
    o->rssi        = eos_net_rssi(s_b.net);
    eos_net_ip_str(eos_net_ip(s_b.net), o->ip, sizeof o->ip);
    if (strcmp(o->ip, "0.0.0.0") == 0) o->ip[0] = 0;

    o->ap_up = (m == EOS_NET_SETUP);
    p = eos_net_ap_ssid(s_b.net);
    if (p) { l = strlen(p); if (l > sizeof o->ap_ssid) l = sizeof o->ap_ssid;
             memcpy(o->ap_ssid, p, l); o->ap_ssid_len = (uint8_t)l; }
    if (o->ap_up) snprintf(o->ap_ip, sizeof o->ap_ip, "%s", EOS_NET_AP_IP_STR);

    p = eos_net_hostname(s_b.net);
    if (p) snprintf(o->host, sizeof o->host, "%s", p);
    return true;
}

// -------------------------------------------------------------- ble ports

static int b_ble_scan_state(void *c)
{
    (void)c;
    if (eos_ble_scanning()) return EOS_HTTPD_SCAN_RUNNING;
    return s_b.bn > 0 ? EOS_HTTPD_SCAN_DONE : EOS_HTTPD_SCAN_IDLE;
}

static int b_ble_scan_count(void *c)
{
    (void)c;
    // Snapshot here rather than in the getter: eos_ble's table is live and a
    // list that changed under the loop would emit two halves of two scans.
    s_b.bn = eos_ble_scan_results(s_b.bdev, EOS_BLE_SCAN_MAX);
    return s_b.bn;
}

static bool b_ble_scan_get(void *c, int i, eos_httpd_ble_dev_t *o)
{
    size_t l;
    (void)c;
    if (!o || i < 0 || i >= s_b.bn) return false;
    memset(o, 0, sizeof *o);
    l = strlen(s_b.bdev[i].name);
    if (l > sizeof o->name) l = sizeof o->name;
    memcpy(o->name, s_b.bdev[i].name, l);
    o->name_len = (uint8_t)l;
    o->rssi     = s_b.bdev[i].rssi;
    o->is_hid   = (s_b.bdev[i].flags & EOS_BLE_F_HID) != 0;
    o->bonded   = (s_b.bdev[i].flags & EOS_BLE_F_BONDED) != 0;
    eos_ble_addr_str(o->addr, (int)sizeof o->addr, s_b.bdev[i].addr);
    for (char *p = o->addr; *p; p++) if (*p >= 'A' && *p <= 'F') *p = (char)(*p + 32);
    return true;
}

static uint32_t b_ble_scan_age(void *c)
{
    uint32_t a;
    (void)c;
    a = eos_ble_scan_age_ms();
    return a == EOS_BLE_SCAN_NEVER ? 0 : a;   // 0 alongside state "idle"
}

static int b_ble_scan_start(void *c) { (void)c; return (int)eos_ble_scan_start(0); }
static int b_ble_forget(void *c)     { (void)c; return (int)eos_ble_forget(); }

// eos_ble_pair_addr() is the string form on purpose: the address TYPE is not on
// the wire and cannot be, so eos_ble recovers it from its own scan table and
// assumes random otherwise. Doing that lookup here would be a second copy of a
// table this file does not own.
static int b_ble_pair(void *c, const char *addr)
{
    (void)c;
    return (int)eos_ble_pair_addr(addr);
}

static bool b_ble_status(void *c, eos_httpd_ble_status_t *o)
{
    eos_ble_status_t st;
    size_t l;

    (void)c;
    if (!o) return false;
    eos_ble_status(&st);
    memset(o, 0, sizeof *o);
    o->bonded    = st.bonded;
    o->connected = st.connected;
    o->pairing   = (st.state == EOS_BLE_CONNECTING || st.state == EOS_BLE_PAIRING);
    o->battery   = (st.battery == EOS_BLE_BATTERY_UNKNOWN) ? -1 : (int16_t)st.battery;
    o->passkey_shown = st.passkey_shown;
    o->passkey       = st.passkey;
    o->state         = st.state;   // the two enums are asserted equal below
    o->reason        = NULL;       // eos_ble_status_t carries no failure code
    if (st.bonded) {
        l = strlen(st.bond.name);
        if (l > sizeof o->name) l = sizeof o->name;
        memcpy(o->name, st.bond.name, l);
        o->name_len = (uint8_t)l;
        eos_ble_addr_str(o->addr, (int)sizeof o->addr, st.bond.addr);
        for (char *p = o->addr; *p; p++) if (*p >= 'A' && *p <= 'F') *p = (char)(*p + 32);
    }
    return true;
}

static const char *b_ble_warning(void *c) { (void)c; return eos_ble_pair_warning(); }

// eos_httpd restates the BLE states rather than including eos_ble.h into the
// portable half. That is only safe while the two agree, so say so out loud —
// this is a build error, not a runtime surprise.
_Static_assert((int)EOS_HTTPD_BLE_OFF        == (int)EOS_BLE_OFF        &&
               (int)EOS_HTTPD_BLE_IDLE       == (int)EOS_BLE_IDLE       &&
               (int)EOS_HTTPD_BLE_SCANNING   == (int)EOS_BLE_SCANNING   &&
               (int)EOS_HTTPD_BLE_CONNECTING == (int)EOS_BLE_CONNECTING &&
               (int)EOS_HTTPD_BLE_PAIRING    == (int)EOS_BLE_PAIRING    &&
               (int)EOS_HTTPD_BLE_READY      == (int)EOS_BLE_READY,
               "eos_httpd_ble_state_t has drifted from eos_ble_state_t");

// ------------------------------------------------------------------ bind

void eos_httpd_idf_bind(eos_httpd_t *h, void *net)
{
    eos_httpd_ports_t p;
    eos_ble_status_t st;

    if (!h) return;
    memset(&s_b, 0, sizeof s_b);
    s_b.net = (eos_net_t *)net;

    // file_open/read/close stay NULL: see the note above the responder. The
    // caller assigns them after this returns if the board has a filesystem.
    memset(&p, 0, sizeof p);

    if (s_b.net) {
        p.wifi_scan_state  = b_wifi_scan_state;
        p.wifi_scan_count  = b_wifi_scan_count;
        p.wifi_scan_get    = b_wifi_scan_get;
        p.wifi_scan_age_ms = b_wifi_scan_age;
        p.wifi_scan_start  = b_wifi_scan_start;
        p.wifi_join        = b_wifi_join;
        p.wifi_forget      = b_wifi_forget;
        p.net_status       = b_net_status;
    }

    // A board that does not declare a BLE keyboard leaves eos_ble in OFF, and
    // leaving its ports NULL is what makes the four /api/ble endpoints answer
    // 501 instead of an empty list that looks like a broken radio. Bind after
    // eos_ble_init(), which on every board runs before WiFi anyway.
    eos_ble_status(&st);
    if (st.state != EOS_BLE_OFF) {
        p.ble_scan_state   = b_ble_scan_state;
        p.ble_scan_count   = b_ble_scan_count;
        p.ble_scan_get     = b_ble_scan_get;
        p.ble_scan_age_ms  = b_ble_scan_age;
        p.ble_scan_start   = b_ble_scan_start;
        p.ble_pair         = b_ble_pair;
        p.ble_forget       = b_ble_forget;
        p.ble_status       = b_ble_status;
        p.ble_pair_warning = b_ble_warning;
    }

    h->ports = p;
    h->ctx   = &s_b;
}

void eos_httpd_pump(eos_httpd_t *h, uint32_t now_ms)
{
    (void)h;
    s_b.now_ms = now_ms;
    if (!s_b.net) return;

    // A forget outranks both of the others: it is the thing that invalidates
    // what they were going to do, and it is what puts the SoftAP back up.
    if (s_b.forget_q) {
        eos_net_err_t fe;
        s_b.forget_q = false;
        s_b.scan_q   = false;
        fe = eos_net_forget(s_b.net);
        memset(s_b.q_psk, 0, sizeof s_b.q_psk);
        // The one outcome worth shouting about: the credentials are gone and
        // the SoftAP did not come up, so the board is now on no network and
        // serving no page. eos_net refuses to open an AP it cannot give a
        // password, which is the right refusal and the wrong silence.
        if (fe != EOS_NET_OK)
            ESP_LOGE(TAG, "forget left no way back in: %s", eos_net_err_name(fe));
    }

    // A queued join outranks a queued scan: the scan is a convenience and the
    // join is what the person at the panel is waiting on.
    if (s_b.scan_q && !s_b.join_q) {
        s_b.scan_q = false;
        s_b.scan_busy = true;
        (void)eos_net_scan(s_b.net, true);
        s_b.scan_busy = false;
        s_b.scan_ever = true;
        s_b.scan_done_ms = now_ms;   // the scan blocked, so this is the age from
                                     // when it started, not when it finished
    }

    if (s_b.join_q) {
        eos_net_err_t e;
        s_b.join_q = false;
        s_b.join_busy = true;
        e = eos_net_try(s_b.net, s_b.q_ssid, s_b.q_psk);
        if (e == EOS_NET_OK) {
            // The one place in this file where credentials reach flash, and it
            // is downstream of a join that actually worked. Save-then-try is how
            // one typo becomes a board that needs a serial cable.
            eos_net_err_t ce = eos_net_commit(s_b.net);
            s_b.join_last = (ce == EOS_NET_OK) ? EOS_HTTPD_JOIN_OK
                                               : EOS_HTTPD_JOIN_FAILED;
            if (ce != EOS_NET_OK)
                ESP_LOGE(TAG, "join succeeded but commit failed: %s", eos_net_err_name(ce));
        } else {
            s_b.join_last = join_map(e);
            ESP_LOGW(TAG, "join failed: %s (nothing written)", eos_net_err_name(e));
        }
        memset(s_b.q_psk, 0, sizeof s_b.q_psk);
        s_b.join_busy = false;
    }
}

#endif // ESP_PLATFORM
