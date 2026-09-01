// Host test for eos_httpd. Everything above the ESP-IDF bindings runs here with
// no sockets, no WiFi and no Bluetooth, because the radios reach the handlers
// through a port table and the test fills it with a scripted fake.
//
// Most of this file is an attack on the two parsers. An SSID is 32 arbitrary
// bytes off the air and a request body is whatever a phone or a script decided
// to send, so the interesting cases are the ones nobody would type on purpose:
// quotes and backslashes in a network name, every control byte, every shape of
// invalid UTF-8, a 32-byte SSID with no NUL anywhere in it, lone surrogates,
// \u0000, nesting bombs, and buffers one byte too small at every size.
//
// Two properties are asserted rather than sampled, on every string the file
// produces: the writer's output is always well-formed JSON, and it is always
// valid UTF-8. Both are checked by validators written here, not by the code
// under test. Every buffer sits inside a canary frame, so an out-of-bounds
// write anywhere fails a check instead of passing quietly.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "eos_httpd.h"

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

#define CKI(got, want, msg) do { \
    checks++; \
    if ((long)(got) != (long)(want)) { \
        fails++; \
        printf("    FAIL: %s (got %ld, want %ld)\n", msg, (long)(got), (long)(want)); \
    } \
} while (0)

// ---------------------------------------------------------- canary frames

#define GUARD 32
#define FRAME 8192

typedef struct { unsigned char pre[GUARD]; char b[FRAME]; unsigned char post[GUARD]; } frame_t;

static void frame_init(frame_t *f)
{
    memset(f->pre,  0x5A, GUARD);
    memset(f->post, 0xA5, GUARD);
    memset(f->b, 0xCC, FRAME);
}

static bool frame_intact(const frame_t *f)
{
    for (int i = 0; i < GUARD; i++) if (f->pre[i]  != 0x5A) return false;
    for (int i = 0; i < GUARD; i++) if (f->post[i] != 0xA5) return false;
    return true;
}

// ------------------------------------------------------------- validators

// Byte-exact UTF-8 validation, written independently of the one in eos_httpd.c
// so a shared misconception cannot make both agree.
static bool utf8_valid(const char *s, int n)
{
    const unsigned char *p = (const unsigned char *)s;
    int i = 0;
    while (i < n) {
        unsigned char c = p[i];
        int need;
        unsigned long cp;
        if (c < 0x80) { i++; continue; }
        if      ((c & 0xE0) == 0xC0) { need = 2; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { need = 3; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { need = 4; cp = c & 0x07u; }
        else return false;
        if (i + need > n) return false;
        for (int k = 1; k < need; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (unsigned long)(p[i + k] & 0x3Fu);
        }
        if (need == 2 && cp < 0x80UL) return false;
        if (need == 3 && cp < 0x800UL) return false;
        if (need == 4 && cp < 0x10000UL) return false;
        if (cp >= 0xD800UL && cp <= 0xDFFFUL) return false;
        if (cp > 0x10FFFFUL) return false;
        i += need;
    }
    return true;
}

// A small recursive-descent JSON validator. Returns the end pointer or NULL.
static const char *jv_value(const char *p, const char *e, int depth);

static const char *jv_ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *jv_string(const char *p, const char *e)
{
    if (p >= e || *p != '"') return NULL;
    p++;
    while (p < e) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') return p + 1;
        if (c < 0x20) return NULL;               // raw control byte
        if (c == '\\') {
            p++;
            if (p >= e) return NULL;
            if (strchr("\"\\/bfnrt", *p)) { p++; continue; }
            if (*p == 'u') {
                if (e - p < 5) return NULL;
                for (int i = 1; i <= 4; i++)
                    if (!strchr("0123456789abcdefABCDEF", p[i])) return NULL;
                p += 5;
                continue;
            }
            return NULL;
        }
        p++;
    }
    return NULL;
}

static const char *jv_value(const char *p, const char *e, int depth)
{
    if (depth > 32) return NULL;
    p = jv_ws(p, e);
    if (p >= e) return NULL;
    if (*p == '"') return jv_string(p, e);
    if (*p == '{') {
        p++;
        p = jv_ws(p, e);
        if (p < e && *p == '}') return p + 1;
        for (;;) {
            p = jv_ws(p, e);
            p = jv_string(p, e);
            if (!p) return NULL;
            p = jv_ws(p, e);
            if (p >= e || *p != ':') return NULL;
            p = jv_value(p + 1, e, depth + 1);
            if (!p) return NULL;
            p = jv_ws(p, e);
            if (p < e && *p == ',') { p++; continue; }
            if (p < e && *p == '}') return p + 1;
            return NULL;
        }
    }
    if (*p == '[') {
        p++;
        p = jv_ws(p, e);
        if (p < e && *p == ']') return p + 1;
        for (;;) {
            p = jv_value(p, e, depth + 1);
            if (!p) return NULL;
            p = jv_ws(p, e);
            if (p < e && *p == ',') { p++; continue; }
            if (p < e && *p == ']') return p + 1;
            return NULL;
        }
    }
    if ((size_t)(e - p) >= 4 && memcmp(p, "true", 4) == 0) return p + 4;
    if ((size_t)(e - p) >= 5 && memcmp(p, "false", 5) == 0) return p + 5;
    if ((size_t)(e - p) >= 4 && memcmp(p, "null", 4) == 0) return p + 4;
    {
        const char *s = p;
        if (p < e && *p == '-') p++;
        while (p < e && *p >= '0' && *p <= '9') p++;
        if (p == s || (p == s + 1 && *s == '-')) return NULL;
        return p;
    }
}

static bool json_valid(const char *s, int n)
{
    const char *e = s + n;
    const char *p = jv_value(s, e, 0);
    if (!p) return false;
    p = jv_ws(p, e);
    return p == e;
}

// Finds "key": in a flat object and returns a pointer to the value. Used only
// to read the handlers' own output, so it does not need to be general.
static const char *field(const char *doc, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    return strstr(doc, pat) ? strstr(doc, pat) + strlen(pat) : NULL;
}

static bool has_kv(const char *doc, const char *key, const char *val)
{
    const char *v = field(doc, key);
    if (!v) return false;
    return strncmp(v, val, strlen(val)) == 0;
}

// The same, but starting the search at `anchor`. Needed because "state" appears
// both at the top level of /api/net/status and inside its "join" object, and
// the first match is not the one under test.
static bool has_kv_after(const char *doc, const char *anchor, const char *key, const char *val)
{
    const char *a = strstr(doc, anchor);
    return a && has_kv(a, key, val);
}

// ==========================================================================
// The JSON writer
// ==========================================================================

static void test_json_shapes(void)
{
    frame_t f;
    eos_json_t j;

    printf("  json writer: shapes\n");

    frame_init(&f);
    eos_json_init(&j, f.b, 256);
    eos_json_obj_open(&j);
    eos_json_kv_str(&j, "a", "1");
    eos_json_kv_int(&j, "b", 2);
    eos_json_kv_bool(&j, "c", true);
    eos_json_kv_null(&j, "d");
    eos_json_obj_close(&j);
    CKS(f.b, "{\"a\":\"1\",\"b\":2,\"c\":true,\"d\":null}", "flat object");
    CK(eos_json_ok(&j), "flat object did not overflow");
    CK(frame_intact(&f), "flat object stayed inside its buffer");

    frame_init(&f);
    eos_json_init(&j, f.b, 256);
    eos_json_arr_open(&j);
    eos_json_int(&j, 1);
    eos_json_int(&j, 2);
    eos_json_str(&j, "x");
    eos_json_bool(&j, false);
    eos_json_null(&j);
    eos_json_arr_close(&j);
    CKS(f.b, "[1,2,\"x\",false,null]", "array commas");

    frame_init(&f);
    eos_json_init(&j, f.b, 256);
    eos_json_obj_open(&j);
    eos_json_key(&j, "l");
    eos_json_arr_open(&j);
    eos_json_obj_open(&j);
    eos_json_kv_int(&j, "n", 1);
    eos_json_obj_close(&j);
    eos_json_obj_open(&j);
    eos_json_kv_int(&j, "n", 2);
    eos_json_obj_close(&j);
    eos_json_arr_close(&j);
    eos_json_kv_bool(&j, "more", false);
    eos_json_obj_close(&j);
    CKS(f.b, "{\"l\":[{\"n\":1},{\"n\":2}],\"more\":false}", "array of objects inside an object");

    frame_init(&f);
    eos_json_init(&j, f.b, 256);
    eos_json_obj_open(&j);
    eos_json_obj_close(&j);
    CKS(f.b, "{}", "empty object");

    frame_init(&f);
    eos_json_init(&j, f.b, 256);
    eos_json_arr_open(&j);
    eos_json_arr_close(&j);
    CKS(f.b, "[]", "empty array");

    // Numbers, including the two that a naive negate gets wrong.
    frame_init(&f);
    eos_json_init(&j, f.b, 256);
    eos_json_arr_open(&j);
    eos_json_int(&j, 0);
    eos_json_int(&j, -1);
    eos_json_int(&j, 2147483647L);
    eos_json_int(&j, -2147483647L - 1L);
    eos_json_int(&j, -128);
    eos_json_arr_close(&j);
    CKS(f.b, "[0,-1,2147483647,-2147483648,-128]", "integer edges");

    frame_init(&f);
    eos_json_init(&j, f.b, 256);
    {
        const unsigned char m[6] = {0x00, 0x01, 0x7F, 0x80, 0xAB, 0xFF};
        eos_json_hexn(&j, m, 6);
    }
    CKS(f.b, "\"00017f80abff\"", "hex is lowercase and zero-padded");

    frame_init(&f);
    eos_json_init(&j, f.b, 256);
    eos_json_hexn(&j, NULL, 0);
    CKS(f.b, "\"\"", "hex of nothing is an empty string");

    // Depth beyond the tracked stack must overflow, not corrupt.
    frame_init(&f);
    eos_json_init(&j, f.b, 512);
    for (int i = 0; i < EOS_HTTPD_JSON_DEPTH + 4; i++) eos_json_arr_open(&j);
    CK(!eos_json_ok(&j), "nesting past EOS_HTTPD_JSON_DEPTH is an overflow");
    CK(frame_intact(&f), "over-deep nesting stayed inside its buffer");

    // Closing more than was opened is an overflow, never an underflow.
    frame_init(&f);
    eos_json_init(&j, f.b, 64);
    eos_json_obj_open(&j);
    eos_json_obj_close(&j);
    eos_json_obj_close(&j);
    CK(!eos_json_ok(&j), "an unbalanced close is an overflow");
    CK(frame_intact(&f), "unbalanced close stayed inside its buffer");
}

// The adversarial SSID corpus. Every one of these is a network name the board
// can genuinely see, and every one breaks a writer that treats it as a C string.
typedef struct { const char *bytes; int len; const char *why; } ssid_case_t;

static const ssid_case_t SSIDS[] = {
    { "plain",                    5,  "ordinary" },
    { "",                         0,  "hidden network, zero length" },
    { "say \"hi\"",               9,  "double quotes" },
    { "C:\\Users\\net",          13,  "backslashes" },
    { "\\\"",                     2,  "backslash then quote" },
    { "tab\there",                8,  "a literal tab" },
    { "nl\nhere",                 7,  "a literal newline" },
    { "cr\rhere",                 7,  "a literal carriage return" },
    { "bell\a",                   5,  "a BEL byte" },
    { "\x01\x02\x03",             3,  "low control bytes" },
    { "\x1f",                     1,  "0x1F, the last control byte" },
    { "\x7f",                     1,  "DEL" },
    { "caf\xc3\xa9",              5,  "valid two-byte UTF-8" },
    { "\xe2\x98\x95",             3,  "valid three-byte UTF-8" },
    { "\xf0\x9f\x93\xb6",         4,  "valid four-byte UTF-8, an emoji" },
    { "\xc3",                     1,  "a lead byte with nothing after it" },
    { "\xc3\x28",                 2,  "a lead byte followed by ASCII" },
    { "\x80",                     1,  "a bare continuation byte" },
    { "\xbf\xbf\xbf",             3,  "three bare continuation bytes" },
    { "\xc0\x80",                 2,  "overlong NUL" },
    { "\xe0\x80\x80",             3,  "overlong three-byte" },
    { "\xf0\x80\x80\x80",         4,  "overlong four-byte" },
    { "\xed\xa0\x80",             3,  "a UTF-8-encoded high surrogate" },
    { "\xed\xbf\xbf",             3,  "a UTF-8-encoded low surrogate" },
    { "\xf4\x90\x80\x80",         4,  "a code point past U+10FFFF" },
    { "\xf8\x88\x80\x80\x80",     5,  "a five-byte sequence, which does not exist" },
    { "\xfe\xff",                 2,  "0xFE and 0xFF, which never appear in UTF-8" },
    { "a\xff" "b",               3,  "one bad byte between two good ones" },
    { "\xe2\x98",                 2,  "a three-byte sequence cut short" },
    { "mix\xc3\xa9\xff\"\\\n",    9,  "everything at once" },
};
#define N_SSIDS ((int)(sizeof SSIDS / sizeof SSIDS[0]))

static void test_json_escaping(void)
{
    frame_t f;
    eos_json_t j;

    printf("  json writer: adversarial SSIDs\n");

    for (int i = 0; i < N_SSIDS; i++) {
        char msg[160];
        int want_len, got_len;

        frame_init(&f);
        eos_json_init(&j, f.b, FRAME);
        eos_json_obj_open(&j);
        eos_json_kv_strn(&j, "ssid", SSIDS[i].bytes, SSIDS[i].len);
        eos_json_key(&j, "ssid_hex");
        eos_json_hexn(&j, SSIDS[i].bytes, SSIDS[i].len);
        eos_json_obj_close(&j);

        snprintf(msg, sizeof msg, "ssid [%s]: output is valid JSON", SSIDS[i].why);
        CK(json_valid(f.b, j.len), msg);

        snprintf(msg, sizeof msg, "ssid [%s]: output is valid UTF-8", SSIDS[i].why);
        CK(utf8_valid(f.b, j.len), msg);

        snprintf(msg, sizeof msg, "ssid [%s]: buffer intact", SSIDS[i].why);
        CK(frame_intact(&f), msg);

        // The predicted length is what the scan handler budgets on, so it has
        // to be exact, not an upper bound.
        want_len = eos_json_escaped_len(SSIDS[i].bytes, SSIDS[i].len);
        {
            frame_t g;
            eos_json_t k;
            frame_init(&g);
            eos_json_init(&k, g.b, FRAME);
            eos_json_strn(&k, SSIDS[i].bytes, SSIDS[i].len);
            got_len = k.len - 2;             // the two quotes
        }
        snprintf(msg, sizeof msg, "ssid [%s]: escaped_len predicts the exact length", SSIDS[i].why);
        CKI(got_len, want_len, msg);

        // The hex must be the raw bytes, untouched, so the client can hand back
        // exactly the network it picked.
        {
            const char *hx = strstr(f.b, "\"ssid_hex\":\"");
            snprintf(msg, sizeof msg, "ssid [%s]: hex is present", SSIDS[i].why);
            CK(hx != NULL, msg);
            if (hx) {
                char want[80];
                int n = 0;
                for (int b = 0; b < SSIDS[i].len; b++)
                    n += snprintf(want + n, sizeof want - (size_t)n, "%02x",
                                  (unsigned char)SSIDS[i].bytes[b]);
                want[n] = 0;
                snprintf(msg, sizeof msg, "ssid [%s]: hex is the raw bytes", SSIDS[i].why);
                CK(strncmp(hx + 12, want, (size_t)n) == 0, msg);
            }
        }
    }

    // Named escapes, spelled out.
    frame_init(&f);
    eos_json_init(&j, f.b, 256);
    eos_json_strn(&j, "\"\\\b\f\n\r\t", 7);
    CKS(f.b, "\"\\\"\\\\\\b\\f\\n\\r\\t\"", "the seven short escapes");

    // Every control byte becomes something a JSON parser accepts.
    for (int c = 0; c < 0x20; c++) {
        char in = (char)c;
        char msg[80];
        frame_init(&f);
        eos_json_init(&j, f.b, 64);
        eos_json_strn(&j, &in, 1);
        snprintf(msg, sizeof msg, "control byte 0x%02x escapes to valid JSON", c);
        CK(json_valid(f.b, j.len), msg);
        snprintf(msg, sizeof msg, "control byte 0x%02x is never emitted raw", c);
        CK(memchr(f.b, c, (size_t)j.len) == NULL, msg);
    }

    // NUL is a byte like any other in a length-counted SSID, and must survive
    // the writer as an escape rather than ending the string.
    frame_init(&f);
    eos_json_init(&j, f.b, 64);
    eos_json_strn(&j, "a\0b", 3);
    CKS(f.b, "\"a\\u0000b\"", "an embedded NUL is escaped, not a terminator");
    CKI(j.len, 10, "the NUL escape is six bytes wide");

    // Every single byte value, on its own.
    for (int c = 0; c < 256; c++) {
        char in = (char)c;
        char msg[80];
        int predicted;
        frame_init(&f);
        eos_json_init(&j, f.b, 64);
        eos_json_strn(&j, &in, 1);
        predicted = eos_json_escaped_len(&in, 1);
        snprintf(msg, sizeof msg, "byte 0x%02x alone is valid JSON", c);
        CK(json_valid(f.b, j.len), msg);
        snprintf(msg, sizeof msg, "byte 0x%02x alone is valid UTF-8", c);
        CK(utf8_valid(f.b, j.len), msg);
        snprintf(msg, sizeof msg, "byte 0x%02x alone: predicted length", c);
        CKI(j.len - 2, predicted, msg);
    }

    // A 32-byte SSID with no NUL in it, which is what the 802.11 field actually
    // is. Anything that reaches for strlen here reads off the end of the array.
    {
        unsigned char raw[32];
        char msg[80];
        for (int i = 0; i < 32; i++) raw[i] = (unsigned char)('A' + (i % 26));
        frame_init(&f);
        eos_json_init(&j, f.b, 256);
        eos_json_strn(&j, (const char *)raw, 32);
        CKI(j.len, 34, "a 32-byte SSID with no NUL emits 32 bytes and two quotes");
        CK(json_valid(f.b, j.len), "a 32-byte SSID with no NUL is valid JSON");

        // The same, filled with the bytes most likely to break something.
        for (int fillv = 0; fillv < 256; fillv += 17) {
            memset(raw, fillv, 32);
            frame_init(&f);
            eos_json_init(&j, f.b, 512);
            eos_json_strn(&j, (const char *)raw, 32);
            snprintf(msg, sizeof msg, "32 bytes of 0x%02x is valid JSON", fillv);
            CK(json_valid(f.b, j.len), msg);
            snprintf(msg, sizeof msg, "32 bytes of 0x%02x is valid UTF-8", fillv);
            CK(utf8_valid(f.b, j.len), msg);
            snprintf(msg, sizeof msg, "32 bytes of 0x%02x: predicted length", fillv);
            CKI(j.len - 2, eos_json_escaped_len((const char *)raw, 32), msg);
        }
    }

    // Random bytes: the property has to hold for input nobody designed.
    {
        unsigned char raw[64];
        unsigned long seed = 20260830UL;
        for (int trial = 0; trial < 400; trial++) {
            int n = (int)(seed % 33UL);
            for (int i = 0; i < 64; i++) {
                seed = seed * 1103515245UL + 12345UL;
                raw[i] = (unsigned char)(seed >> 16);
            }
            frame_init(&f);
            eos_json_init(&j, f.b, 1024);
            eos_json_obj_open(&j);
            eos_json_kv_strn(&j, "ssid", (const char *)raw, n);
            eos_json_obj_close(&j);
            if (!json_valid(f.b, j.len)) { CK(false, "random SSID is valid JSON"); break; }
            if (!utf8_valid(f.b, j.len)) { CK(false, "random SSID is valid UTF-8"); break; }
            if (!frame_intact(&f))       { CK(false, "random SSID stayed in its buffer"); break; }
            if (eos_json_escaped_len((const char *)raw, n) != j.len - 11) {
                CK(false, "random SSID: escaped_len predicts the exact length");
                break;
            }
            seed = seed * 1103515245UL + 12345UL;
        }
        CK(true, "400 random SSIDs: valid JSON, valid UTF-8, exact predicted length");
    }
}

static void test_json_overflow(void)
{
    printf("  json writer: overflow at every buffer size\n");

    // The same document into every buffer size from 1 byte up to two past what
    // it needs. Below the exact size it must overflow; at or above it must not,
    // and it must never write outside the frame or leave the buffer unterminated.
    for (int cap = 1; cap <= 80; cap++) {
        frame_t f;
        eos_json_t j;
        char msg[96];
        static const char WANT[] = "{\"ssid\":\"ab\\\"c\",\"rssi\":-58,\"ok\":true}";
        int need = (int)sizeof WANT - 1;

        frame_init(&f);
        eos_json_init(&j, f.b, cap);
        eos_json_obj_open(&j);
        eos_json_kv_strn(&j, "ssid", "ab\"c", 4);
        eos_json_kv_int(&j, "rssi", -58);
        eos_json_kv_bool(&j, "ok", true);
        eos_json_obj_close(&j);

        snprintf(msg, sizeof msg, "cap %d: buffer intact", cap);
        CK(frame_intact(&f), msg);

        snprintf(msg, sizeof msg, "cap %d: NUL-terminated", cap);
        CK(memchr(f.b, 0, (size_t)cap) != NULL, msg);

        snprintf(msg, sizeof msg, "cap %d: overflow reported iff it did not fit", cap);
        CK(eos_json_ok(&j) == (cap >= need + 1), msg);

        if (cap >= need + 1) {
            snprintf(msg, sizeof msg, "cap %d: exact document", cap);
            CKS(f.b, WANT, msg);
        } else {
            snprintf(msg, sizeof msg, "cap %d: never wrote past cap-1", cap);
            CK((int)strlen(f.b) <= cap - 1, msg);
        }
    }

    // A zero-length and a NULL buffer must be inert rather than a crash.
    {
        eos_json_t j;
        char one[1];
        eos_json_init(&j, one, 0);
        eos_json_obj_open(&j);
        eos_json_kv_int(&j, "x", 1);
        eos_json_obj_close(&j);
        CK(!eos_json_ok(&j), "a zero-capacity writer overflows immediately");

        eos_json_init(&j, NULL, 64);
        eos_json_obj_open(&j);
        CK(!eos_json_ok(&j), "a NULL buffer overflows immediately");
    }

    // Overflow is sticky: a write that fits after one that did not must still
    // leave the document marked bad, because the bytes in between are gone.
    {
        frame_t f;
        eos_json_t j;
        frame_init(&f);
        eos_json_init(&j, f.b, 12);
        eos_json_obj_open(&j);
        eos_json_kv_str(&j, "long_key_here", "value");
        eos_json_kv_int(&j, "n", 1);
        eos_json_obj_close(&j);
        CK(!eos_json_ok(&j), "overflow is sticky across later writes that would fit");
    }
}

// ==========================================================================
// The JSON reader
// ==========================================================================

static void test_json_read_basics(void)
{
    char out[64];
    int n;
    long v;
    bool b;

    printf("  json reader: values and absence\n");


    {
        static const char B[] = "{\"ssid\":\"home\",\"psk\":\"secret12\"}";
        CKI(eos_json_get_str(B, (int)strlen(B), "ssid", out, sizeof out, &n),
            EOS_JSON_FOUND, "ssid found");
        CKS(out, "home", "ssid value");
        CKI(n, 4, "ssid length");
        CKI(eos_json_get_str(B, (int)strlen(B), "psk", out, sizeof out, &n),
            EOS_JSON_FOUND, "psk found");
        CKS(out, "secret12", "psk value");
        CKI(eos_json_get_str(B, (int)strlen(B), "nope", out, sizeof out, &n),
            EOS_JSON_ABSENT, "a missing key is absent, not an error");
        CKI(n, 0, "a missing key clears the length");
        CKS(out, "", "a missing key clears the buffer");
    }

    // Whitespace everywhere a parser might not expect it.
    {
        static const char B[] = "  {  \"ssid\"  :  \"a b\"  ,  \"n\" : 42  }  ";
        CKI(eos_json_get_str(B, (int)strlen(B), "ssid", out, sizeof out, &n),
            EOS_JSON_FOUND, "whitespace: string found");
        CKS(out, "a b", "whitespace: string value");
        CKI(eos_json_get_int(B, (int)strlen(B), "n", &v), EOS_JSON_FOUND, "whitespace: int found");
        CKI(v, 42, "whitespace: int value");
    }

    // Numbers and booleans.
    {
        static const char B[] = "{\"a\":0,\"b\":-17,\"c\":2147483647,\"t\":true,\"f\":false}";
        CKI(eos_json_get_int(B, (int)strlen(B), "a", &v), EOS_JSON_FOUND, "zero found");
        CKI(v, 0, "zero value");
        CKI(eos_json_get_int(B, (int)strlen(B), "b", &v), EOS_JSON_FOUND, "negative found");
        CKI(v, -17, "negative value");
        CKI(eos_json_get_int(B, (int)strlen(B), "c", &v), EOS_JSON_FOUND, "int max found");
        CKI(v, 2147483647L, "int max value");
        CKI(eos_json_get_bool(B, (int)strlen(B), "t", &b), EOS_JSON_FOUND, "true found");
        CK(b, "true value");
        CKI(eos_json_get_bool(B, (int)strlen(B), "f", &b), EOS_JSON_FOUND, "false found");
        CK(!b, "false value");
    }

    // Wrong type is its own answer, not "absent" and not "malformed".
    {
        static const char B[] = "{\"a\":5,\"b\":\"x\",\"c\":true,\"d\":null}";
        CKI(eos_json_get_str(B, (int)strlen(B), "a", out, sizeof out, &n),
            EOS_JSON_TYPE, "a number asked for as a string is a type error");
        CKI(eos_json_get_int(B, (int)strlen(B), "b", &v),
            EOS_JSON_TYPE, "a string asked for as an int is a type error");
        CKI(eos_json_get_bool(B, (int)strlen(B), "a", &b),
            EOS_JSON_TYPE, "a number asked for as a bool is a type error");
        CKI(eos_json_get_str(B, (int)strlen(B), "d", out, sizeof out, &n),
            EOS_JSON_TYPE, "null asked for as a string is a type error");
    }

    // A float is a number but it is not an integer, and rounding it silently is
    // how a port becomes 0.
    {
        static const char B[] = "{\"a\":1.5,\"b\":1e3,\"c\":12345678901234}";
        CKI(eos_json_get_int(B, (int)strlen(B), "a", &v), EOS_JSON_TYPE, "1.5 is not an integer");
        CKI(eos_json_get_int(B, (int)strlen(B), "b", &v), EOS_JSON_TYPE, "1e3 is not an integer");
        CKI(eos_json_get_int(B, (int)strlen(B), "c", &v), EOS_JSON_TOOBIG, "a huge integer is too big");
    }
}

static void test_json_read_hostile(void)
{
    char out[64];
    int n;

    printf("  json reader: hostile bodies\n");

    // Nested values are skipped, never descended into. A key at depth 2 with
    // the name the handler wants must not be mistaken for the real one.
    {
        static const char B[] = "{\"outer\":{\"ssid\":\"decoy\"},\"ssid\":\"real\"}";
        CKI(eos_json_get_str(B, (int)strlen(B), "ssid", out, sizeof out, &n),
            EOS_JSON_FOUND, "nested decoy: the top-level key is found");
        CKS(out, "real", "nested decoy: the top-level value wins");
    }
    {
        static const char B[] = "{\"a\":[1,{\"ssid\":\"decoy\"},[\"ssid\"]],\"ssid\":\"real\"}";
        CKI(eos_json_get_str(B, (int)strlen(B), "ssid", out, sizeof out, &n),
            EOS_JSON_FOUND, "decoy inside an array: found");
        CKS(out, "real", "decoy inside an array: the top-level value wins");
    }
    {
        static const char B[] = "{\"a\":{\"ssid\":\"decoy\"}}";
        CKI(eos_json_get_str(B, (int)strlen(B), "ssid", out, sizeof out, &n),
            EOS_JSON_ABSENT, "a key that exists only at depth 2 is absent");
    }

    // A brace or bracket inside a string must not move the depth counter.
    {
        static const char B[] = "{\"a\":\"}{][\\\"\",\"ssid\":\"real\"}";
        CKI(eos_json_get_str(B, (int)strlen(B), "ssid", out, sizeof out, &n),
            EOS_JSON_FOUND, "braces inside a string do not confuse the skipper");
        CKS(out, "real", "braces inside a string: correct value");
    }

    // Duplicate keys: the first wins, deterministically.
    {
        static const char B[] = "{\"ssid\":\"first\",\"ssid\":\"second\"}";
        CKI(eos_json_get_str(B, (int)strlen(B), "ssid", out, sizeof out, &n),
            EOS_JSON_FOUND, "duplicate keys: found");
        CKS(out, "first", "duplicate keys: the first one wins");
    }

    // Malformed documents.
    {
        struct { const char *b; const char *why; } BAD[] = {
            { "",                          "empty body" },
            { "   ",                       "whitespace only" },
            { "[]",                        "an array, not an object" },
            { "\"ssid\"",                  "a bare string" },
            { "null",                      "a bare null" },
            { "{",                         "an unclosed object" },
            { "{\"ssid\"",                 "a key with no colon" },
            { "{\"ssid\":",                "a colon with no value" },
            { "{\"ssid\":\"unterminated",  "an unterminated string value" },
            { "{\"unterminated:1}",        "an unterminated key" },
            { "{\"a\":1,}",                "a trailing comma" },
            { "{\"a\":1 \"b\":2}",         "a missing comma" },
            { "{ssid:\"x\"}",              "an unquoted key" },
            { "{\"a\":\"\\q\"}",           "an unknown escape" },
            { "{\"a\":\"\\u12\"}",         "a short \\u escape" },
            { "{\"a\":\"\\u12zz\"}",       "a non-hex \\u escape" },
            { "{\"a\":\"\t\"}",            "a raw tab inside a string" },
            { "{\"a\":\"\x01\"}",          "a raw control byte inside a string" },
            { "{\"a\":@}",                 "junk where a value belongs" },
            { "{\"a\":,}",                 "a comma where a value belongs" },
        };
        for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
            char msg[120];
            eos_json_find_t s = eos_json_get_str(BAD[i].b, (int)strlen(BAD[i].b),
                                                 "ssid", out, sizeof out, &n);
            snprintf(msg, sizeof msg, "malformed: %s is rejected", BAD[i].why);
            CK(s == EOS_JSON_BAD, msg);
        }
    }

    // A body that is not NUL-terminated: the length is authoritative. Cutting a
    // valid document short must be BAD, never a read past the end.
    {
        static const char FULL[] = "{\"ssid\":\"home\",\"psk\":\"secret12\"}";
        int len = (int)strlen(FULL);
        for (int cut = 0; cut < len; cut++) {
            char msg[64];
            char *copy = (char *)malloc((size_t)cut ? (size_t)cut : 1);
            eos_json_find_t s;
            memcpy(copy, FULL, (size_t)cut);
            s = eos_json_get_str(copy, cut, "psk", out, sizeof out, &n);
            free(copy);
            // Either the value was not reached, or it was reached whole. What
            // must never happen is a short value reported as the real one.
            snprintf(msg, sizeof msg, "truncated at %d: never a partial value", cut);
            CK(s != EOS_JSON_FOUND || (n == 8 && strcmp(out, "secret12") == 0), msg);
        }
        CK(eos_json_get_str(FULL, len, "psk", out, sizeof out, &n) == EOS_JSON_FOUND,
           "the untruncated body still parses");
    }

    // A nesting bomb must be refused, not recursed into.
    {
        char bomb[600];
        int p = 0;
        p += snprintf(bomb + p, sizeof bomb - (size_t)p, "{\"a\":");
        for (int i = 0; i < 200; i++) bomb[p++] = '[';
        for (int i = 0; i < 200; i++) bomb[p++] = ']';
        p += snprintf(bomb + p, sizeof bomb - (size_t)p, ",\"ssid\":\"x\"}");
        CKI(eos_json_get_str(bomb, p, "ssid", out, sizeof out, &n),
            EOS_JSON_BAD, "200 levels of nesting is refused");
    }

    // Escapes decode, including surrogate pairs.
    {
        static const char B[] =
            "{\"a\":\"q\\\"b\",\"b\":\"s\\\\l\",\"c\":\"t\\tn\\n\","
             "\"d\":\"\\u0041\\u00e9\\u2615\",\"e\":\"\\ud83d\\udcf6\","
             "\"f\":\"\\/\"}";
        int len = (int)strlen(B);
        CKI(eos_json_get_str(B, len, "a", out, sizeof out, &n), EOS_JSON_FOUND, "escaped quote");
        CKS(out, "q\"b", "escaped quote decodes");
        CKI(eos_json_get_str(B, len, "b", out, sizeof out, &n), EOS_JSON_FOUND, "escaped backslash");
        CKS(out, "s\\l", "escaped backslash decodes");
        CKI(eos_json_get_str(B, len, "c", out, sizeof out, &n), EOS_JSON_FOUND, "escaped tab and nl");
        CKS(out, "t\tn\n", "escaped tab and newline decode");
        CKI(eos_json_get_str(B, len, "d", out, sizeof out, &n), EOS_JSON_FOUND, "\\u escapes");
        CKS(out, "A\xc3\xa9\xe2\x98\x95", "\\u escapes become UTF-8");
        CKI(n, 6, "\\u escapes: byte length, not character count");
        CKI(eos_json_get_str(B, len, "e", out, sizeof out, &n), EOS_JSON_FOUND, "surrogate pair");
        CKS(out, "\xf0\x9f\x93\xb6", "a surrogate pair becomes one four-byte sequence");
        CKI(n, 4, "surrogate pair: four bytes");
        CKI(eos_json_get_str(B, len, "f", out, sizeof out, &n), EOS_JSON_FOUND, "escaped slash");
        CKS(out, "/", "escaped slash decodes");
    }

    // Lone surrogates become U+FFFD rather than a broken UTF-8 sequence handed
    // on to esp_wifi, and \u0000 is refused outright.
    {
        static const char B[] = "{\"hi\":\"\\ud83d\",\"lo\":\"\\udcf6\",\"pair\":\"\\ud83dx\"}";
        int len = (int)strlen(B);
        CKI(eos_json_get_str(B, len, "hi", out, sizeof out, &n), EOS_JSON_FOUND, "lone high surrogate");
        CKS(out, "\xef\xbf\xbd", "a lone high surrogate becomes U+FFFD");
        CKI(eos_json_get_str(B, len, "lo", out, sizeof out, &n), EOS_JSON_FOUND, "lone low surrogate");
        CKS(out, "\xef\xbf\xbd", "a lone low surrogate becomes U+FFFD");
        CKI(eos_json_get_str(B, len, "pair", out, sizeof out, &n), EOS_JSON_FOUND, "high surrogate then text");
        CKS(out, "\xef\xbf\xbdx", "an unpaired high surrogate is replaced and the rest survives");
    }
    {
        static const char B[] = "{\"a\":\"x\\u0000y\"}";
        CKI(eos_json_get_str(B, (int)strlen(B), "a", out, sizeof out, &n),
            EOS_JSON_BAD, "an escaped NUL is refused, not silently truncating the value");
    }

    // Raw bytes inside a string come through untouched: an SSID is bytes, and
    // repairing it here would change which network the caller asked for.
    {
        static const char B[] = "{\"ssid\":\"a\xff\xc3 b\"}";
        CKI(eos_json_get_str(B, (int)strlen(B), "ssid", out, sizeof out, &n),
            EOS_JSON_FOUND, "raw invalid UTF-8 in a body value is accepted");
        CKI(n, 5, "raw invalid UTF-8: exact byte count");
        CK(memcmp(out, "a\xff\xc3 b", 5) == 0, "raw invalid UTF-8: bytes are unchanged");
    }

    // The output buffer, at exactly the right size and one byte short.
    {
        static const char B[] = "{\"psk\":\"12345678\"}";
        char small[16];
        for (int cap = 1; cap <= 12; cap++) {
            char msg[80];
            eos_json_find_t s;
            memset(small, 0x7E, sizeof small);
            s = eos_json_get_str(B, (int)strlen(B), "psk", small, cap, &n);
            snprintf(msg, sizeof msg, "out_cap %d: fits iff cap > 8", cap);
            CK((s == EOS_JSON_FOUND) == (cap >= 9), msg);
            snprintf(msg, sizeof msg, "out_cap %d: never wrote past the cap", cap);
            CK(small[cap] == 0x7E, msg);
            if (s == EOS_JSON_FOUND) {
                snprintf(msg, sizeof msg, "out_cap %d: exact value", cap);
                CKS(small, "12345678", msg);
            }
        }
    }

    // A 63-character passphrase, which is the WPA maximum, must fit a 64-byte
    // buffer exactly. Off by one here is a password nobody can type in.
    {
        char body[160], psk[64], want[64];
        int len;
        memset(want, 'p', 63);
        want[63] = 0;
        len = snprintf(body, sizeof body, "{\"psk\":\"%s\"}", want);
        CKI(eos_json_get_str(body, len, "psk", psk, sizeof psk, &n),
            EOS_JSON_FOUND, "a 63-character passphrase fits a 64-byte buffer");
        CKI(n, 63, "a 63-character passphrase is 63 bytes");
        CKS(psk, want, "a 63-character passphrase round-trips");
    }
    {
        char body[160], psk[64], want[80];
        int len;
        memset(want, 'p', 64);
        want[64] = 0;
        len = snprintf(body, sizeof body, "{\"psk\":\"%s\"}", want);
        CKI(eos_json_get_str(body, len, "psk", psk, sizeof psk, &n),
            EOS_JSON_TOOBIG, "a 64-character passphrase does not fit and is not truncated");
    }

    // A key longer than the reader's own key buffer must be skipped, not matched
    // and not fatal.
    {
        char body[256];
        int len = snprintf(body, sizeof body,
            "{\"%0*d\":1,\"ssid\":\"real\"}", 200, 0);
        CKI(eos_json_get_str(body, len, "ssid", out, sizeof out, &n),
            EOS_JSON_FOUND, "a 200-byte key is skipped rather than matched");
        CKS(out, "real", "a 200-byte key does not hide the real one");
    }
}

// ==========================================================================
// URIs
// ==========================================================================

static void test_uri(void)
{
    char out[64];

    printf("  uri: paths, queries, percent escapes\n");

    CKI(eos_httpd_path_of("/api/wifi/scan", out, sizeof out), 14, "plain path length");
    CKS(out, "/api/wifi/scan", "plain path");

    CKI(eos_httpd_path_of("/api/wifi/scan?rescan=1", out, sizeof out), 14, "path stops at the query");
    CKS(out, "/api/wifi/scan", "path without the query");

    CKI(eos_httpd_path_of("/a#frag", out, sizeof out), 2, "path stops at the fragment");
    CKS(out, "/a", "path without the fragment");

    CKI(eos_httpd_path_of("/a%20b", out, sizeof out), 4, "percent escape decodes");
    CKS(out, "/a b", "percent-decoded path");

    CKI(eos_httpd_path_of("/%2e%2e/secret", out, sizeof out), 10, "%2e decodes to a dot");
    CKS(out, "/../secret", "an encoded traversal decodes to a visible one");

    CK(eos_httpd_path_of("/a%zz", out, sizeof out) < 0, "a non-hex escape is refused");
    CK(eos_httpd_path_of("/a%2", out, sizeof out) < 0, "a truncated escape is refused");
    CK(eos_httpd_path_of("/a%00b", out, sizeof out) < 0, "%00 is refused, never a silent truncation");
    CK(eos_httpd_path_of("/aaaaaaaaaa", out, 5) < 0, "a path that does not fit is refused");
    CK(eos_httpd_path_of(NULL, out, sizeof out) < 0, "a NULL uri is refused");

    // A '+' is a space in a query and a literal plus in a path.
    CKI(eos_httpd_path_of("/a+b", out, sizeof out), 4, "plus in a path");
    CKS(out, "/a+b", "a plus in a path stays a plus");

    CKI(eos_httpd_query_get("/x?a=1&b=2", "a", out, sizeof out), 1, "first query value");
    CKS(out, "1", "first query value text");
    CKI(eos_httpd_query_get("/x?a=1&b=2", "b", out, sizeof out), 1, "second query value");
    CKS(out, "2", "second query value text");
    CK(eos_httpd_query_get("/x?a=1&b=2", "c", out, sizeof out) < 0, "a missing query key");
    CK(eos_httpd_query_get("/x", "a", out, sizeof out) < 0, "no query string at all");
    CKI(eos_httpd_query_get("/x?rescan", "rescan", out, sizeof out), 0, "a bare flag is present and empty");
    CKS(out, "", "a bare flag has an empty value");
    CKI(eos_httpd_query_get("/x?a=hello+world", "a", out, sizeof out), 11, "plus in a query value");
    CKS(out, "hello world", "a plus in a query is a space");
    CKI(eos_httpd_query_get("/x?a=%41%42", "a", out, sizeof out), 2, "percent in a query value");
    CKS(out, "AB", "percent-decoded query value");
    CK(eos_httpd_query_get("/x?ab=1", "a", out, sizeof out) < 0, "a key prefix is not a match");
    CK(eos_httpd_query_get("/x?a=1", "ab", out, sizeof out) < 0, "a longer key is not a match");
    CKI(eos_httpd_query_get("/x?a=&b=2", "a", out, sizeof out), 0, "an explicitly empty value");

    CK(eos_httpd_flag("1"), "flag: 1");
    CK(eos_httpd_flag("true"), "flag: true");
    CK(eos_httpd_flag("yes"), "flag: yes");
    CK(eos_httpd_flag("on"), "flag: on");
    CK(eos_httpd_flag(""), "flag: present with no value");
    CK(!eos_httpd_flag("0"), "flag: 0 is not a yes");
    CK(!eos_httpd_flag("false"), "flag: false is not a yes");
    CK(!eos_httpd_flag(NULL), "flag: NULL is not a yes");
}

// ==========================================================================
// Routing
// ==========================================================================

static void test_routes(void)
{
    printf("  routes: the eight endpoints and everything that is not one\n");

    struct { const char *m, *u; eos_route_t want; const char *why; } T[] = {
        { "GET",  "/api/wifi/scan",             EOS_ROUTE_WIFI_SCAN,    "wifi scan" },
        { "GET",  "/api/wifi/scan?rescan=1",    EOS_ROUTE_WIFI_SCAN,    "wifi scan with a query" },
        { "POST", "/api/wifi/connect",          EOS_ROUTE_WIFI_CONNECT, "wifi connect" },
        { "POST", "/api/wifi/forget",           EOS_ROUTE_WIFI_FORGET,  "wifi forget" },
        { "GET",  "/api/net/status",            EOS_ROUTE_NET_STATUS,   "net status" },
        { "GET",  "/api/ble/scan",              EOS_ROUTE_BLE_SCAN,     "ble scan" },
        { "POST", "/api/ble/pair",              EOS_ROUTE_BLE_PAIR,     "ble pair" },
        { "GET",  "/api/ble/status",            EOS_ROUTE_BLE_STATUS,   "ble status" },
        { "POST", "/api/ble/forget",            EOS_ROUTE_BLE_FORGET,   "ble forget" },

        { "POST", "/api/wifi/scan",             EOS_ROUTE_METHOD,       "POST to a GET endpoint" },
        { "GET",  "/api/wifi/connect",          EOS_ROUTE_METHOD,       "GET to a POST endpoint" },
        { "GET",  "/api/ble/pair",              EOS_ROUTE_METHOD,       "GET to ble pair" },
        { "PUT",  "/api/wifi/scan",             EOS_ROUTE_METHOD,       "PUT anywhere" },
        { "DELETE","/api/wifi/forget",          EOS_ROUTE_METHOD,       "DELETE anywhere" },
        { "HEAD", "/index.html",                EOS_ROUTE_METHOD,       "HEAD is not answered" },
        { "get",  "/api/wifi/scan",             EOS_ROUTE_METHOD,       "the method is case sensitive" },

        { "GET",  "/api/wifi/scans",            EOS_ROUTE_NONE,         "a typo under /api/" },
        { "GET",  "/api/wifi/scan/",            EOS_ROUTE_NONE,         "a trailing slash" },
        { "GET",  "/api/",                      EOS_ROUTE_NONE,         "the api root" },
        { "GET",  "/api/../index.html",         EOS_ROUTE_NONE,         "traversal out of /api/" },
        { "GET",  "/API/WIFI/SCAN",             EOS_ROUTE_STATIC,       "paths are case sensitive" },

        { "GET",  "/",                          EOS_ROUTE_STATIC,       "the root" },
        { "GET",  "/index.html",                EOS_ROUTE_STATIC,       "a page" },
        { "GET",  "/app.js",                    EOS_ROUTE_STATIC,       "a script" },
        { "POST", "/index.html",                EOS_ROUTE_METHOD,       "POST to a file" },
        { "GET",  "/../etc/passwd",             EOS_ROUTE_NONE,         "a traversal" },
        { "GET",  "/a/../../b",                 EOS_ROUTE_NONE,         "a traversal in the middle" },
        { "GET",  "/%2e%2e/etc/passwd",         EOS_ROUTE_NONE,         "an encoded traversal" },
        { "GET",  "/a\\b",                      EOS_ROUTE_NONE,         "a backslash" },
        { "GET",  "index.html",                 EOS_ROUTE_NONE,         "a path with no leading slash" },

        { "GET",  "/generate_204",              EOS_ROUTE_CAPTIVE,      "the Android probe" },
        { "GET",  "/gen_204",                   EOS_ROUTE_CAPTIVE,      "the Chrome probe" },
        { "GET",  "/hotspot-detect.html",       EOS_ROUTE_CAPTIVE,      "the iOS probe" },
        { "GET",  "/library/test/success.html", EOS_ROUTE_CAPTIVE,      "the macOS probe" },
        { "GET",  "/ncsi.txt",                  EOS_ROUTE_CAPTIVE,      "the Windows probe" },
        { "GET",  "/connecttest.txt",           EOS_ROUTE_CAPTIVE,      "the Windows 10 probe" },
        { "GET",  "/success.txt",               EOS_ROUTE_CAPTIVE,      "the Firefox probe" },
        { "GET",  "/canonical.html",            EOS_ROUTE_CAPTIVE,      "the GNOME probe" },
        { "GET",  "/Generate_204",              EOS_ROUTE_CAPTIVE,      "probes match case-insensitively" },
        { "GET",  "/generate_204?x=1",          EOS_ROUTE_CAPTIVE,      "a probe with a query" },
        { "GET",  "/generate_205",              EOS_ROUTE_STATIC,       "a near miss is not a probe" },

        /* A camera node - a screenless board running these same services and
           serving frames instead of a desktop. On a board with no sensor these
           routes still RESOLVE and then answer 501, which says "this image
           cannot do that" rather than 404's "no such thing". Only the first is
           true, and the difference is what someone debugging will act on. */
        { "GET",  "/api/cam/status",            EOS_ROUTE_CAM_STATUS,   "camera status" },
        { "GET",  "/api/cam/frame",             EOS_ROUTE_CAM_FRAME,    "a raw RGB565 frame" },
        { "GET",  "/api/cam/snap",              EOS_ROUTE_CAM_SNAP,     "the same frame as jpeg" },
        { "GET",  "/api/cam/frame?w=240&h=320", EOS_ROUTE_CAM_FRAME,    "a frame request carries its size in the query" },
        /* METHOD, not NONE: the router knows this path and only the verb is
           wrong, so it can answer 405 instead of 404. "you asked the wrong
           way" is a different problem from "there is no such thing", and the
           router keeps them apart. */
        { "POST", "/api/cam/frame",             EOS_ROUTE_METHOD,       "a frame is fetched, not posted - 405, not 404" },
        /* NONE, not STATIC: anything unmatched under /api/ is a 404 rather
           than falling through to the file handler. /api/ is a namespace, and
           a typo there must not be answered with a web page. */
        { "GET",  "/api/cam/framed",            EOS_ROUTE_NONE,         "a near miss under /api/ is a 404, not a static file" },
    };

    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        char msg[120];
        snprintf(msg, sizeof msg, "route: %s (%s %s)", T[i].why, T[i].m, T[i].u);
        CKI(eos_httpd_route(T[i].m, T[i].u), T[i].want, msg);
    }

    CKI(eos_httpd_route(NULL, "/"), EOS_ROUTE_NONE, "route: a NULL method");
    CKI(eos_httpd_route("GET", NULL), EOS_ROUTE_NONE, "route: a NULL uri");

    // A URI longer than the router's own buffer must not match anything.
    {
        char big[EOS_HTTPD_URI_MAX + 64];
        memset(big, 'a', sizeof big - 1);
        big[0] = '/';
        big[sizeof big - 1] = 0;
        CKI(eos_httpd_route("GET", big), EOS_ROUTE_NONE, "route: an over-long uri matches nothing");
    }
}

static void test_mime(void)
{
    printf("  mime: extensions, gzip twins\n");
    CKS(eos_httpd_mime("/int/web/index.html"), "text/html; charset=utf-8", "html");
    CKS(eos_httpd_mime("/int/web/index.html.gz"), "text/html; charset=utf-8", "html.gz is still html");
    CKS(eos_httpd_mime("/style.css.gz"), "text/css; charset=utf-8", "css.gz");
    CKS(eos_httpd_mime("/app.js.gz"), "application/javascript; charset=utf-8", "js.gz");
    CKS(eos_httpd_mime("/voxel-editor.js"), "application/javascript; charset=utf-8", "js");
    CKS(eos_httpd_mime("/a.json"), "application/json; charset=utf-8", "json");
    CKS(eos_httpd_mime("/a.svg"), "image/svg+xml", "svg");
    CKS(eos_httpd_mime("/a.png"), "image/png", "png");
    CKS(eos_httpd_mime("/a.txt"), "text/plain; charset=utf-8", "txt");
    CKS(eos_httpd_mime("/a.woff2"), "font/woff2", "woff2");
    CKS(eos_httpd_mime("/a.bin"), "application/octet-stream", "an unknown extension");
    CKS(eos_httpd_mime("/noext"), "application/octet-stream", "no extension");
    CKS(eos_httpd_mime("/a.gz"), "application/octet-stream", "a bare .gz names nothing");
    CKS(eos_httpd_mime(NULL), "application/octet-stream", "a NULL path");

    CKS(eos_httpd_auth_name(EOS_HTTPD_AUTH_OPEN), "open", "auth open");
    CKS(eos_httpd_auth_name(EOS_HTTPD_AUTH_WPA2), "wpa2", "auth wpa2");
    CKS(eos_httpd_auth_name(EOS_HTTPD_AUTH_WPA2_WPA3), "wpa2_wpa3", "auth wpa2_wpa3");
    CKS(eos_httpd_auth_name(99), "other", "an auth mode nobody has heard of");
}

// ==========================================================================
// The fake radios
// ==========================================================================

typedef struct {
    int      wifi_state, ble_state;
    int      wifi_n, ble_n;
    eos_httpd_ap_t      aps[24];
    eos_httpd_ble_dev_t devs[24];
    eos_httpd_net_t     net;
    eos_httpd_ble_status_t ble;

    // What the handlers actually did.
    int      wifi_scan_starts, ble_scan_starts;
    int      joins, forgets, pairs, ble_forgets;
    uint8_t  join_ssid[32];
    int      join_ssid_len;
    char     join_psk[80];
    int      join_psk_len;
    char     pair_addr[24];
    int      wifi_scan_start_rc, join_rc, forget_rc, pair_rc, ble_forget_rc, ble_scan_start_rc;

    // Files: a tiny in-memory filesystem.
    struct { const char *path; const char *data; int len; } files[8];
    int      nfiles;
    int      opens, closes;
} fake_t;

static fake_t FK;

typedef struct { const char *data; int len, pos; } fake_fh_t;
static fake_fh_t FH[4];
static int fh_used;

static int      f_wifi_state(void *c) { (void)c; return FK.wifi_state; }
static int      f_wifi_count(void *c) { (void)c; return FK.wifi_n; }
static bool     f_wifi_get(void *c, int i, eos_httpd_ap_t *o)
{ (void)c; if (i < 0 || i >= FK.wifi_n) return false; *o = FK.aps[i]; return true; }
static uint32_t f_wifi_age(void *c) { (void)c; return 1234; }
static int      f_wifi_start(void *c)
{ (void)c; FK.wifi_scan_starts++; if (FK.wifi_scan_start_rc == 0) FK.wifi_state = EOS_HTTPD_SCAN_RUNNING;
  return FK.wifi_scan_start_rc; }

static int f_wifi_join(void *c, const uint8_t *s, int sl, const char *p, int pl)
{
    (void)c;
    FK.joins++;
    FK.join_ssid_len = sl;
    memcpy(FK.join_ssid, s, (size_t)(sl > 32 ? 32 : sl));
    FK.join_psk_len = pl;
    memcpy(FK.join_psk, p, (size_t)(pl > 79 ? 79 : pl));
    FK.join_psk[pl > 79 ? 79 : pl] = 0;
    if (FK.join_rc == 0) FK.net.state = EOS_HTTPD_NET_JOINING;
    return FK.join_rc;
}
static int  f_wifi_forget(void *c) { (void)c; FK.forgets++; return FK.forget_rc; }
static bool f_net_status(void *c, eos_httpd_net_t *o) { (void)c; *o = FK.net; return true; }

static int      f_ble_state(void *c) { (void)c; return FK.ble_state; }
static int      f_ble_count(void *c) { (void)c; return FK.ble_n; }
static bool     f_ble_get(void *c, int i, eos_httpd_ble_dev_t *o)
{ (void)c; if (i < 0 || i >= FK.ble_n) return false; *o = FK.devs[i]; return true; }
static uint32_t f_ble_age(void *c) { (void)c; return 4321; }
static int      f_ble_start(void *c)
{ (void)c; FK.ble_scan_starts++; if (FK.ble_scan_start_rc == 0) FK.ble_state = EOS_HTTPD_SCAN_RUNNING;
  return FK.ble_scan_start_rc; }
static int f_ble_pair(void *c, const char *a)
{ (void)c; FK.pairs++; snprintf(FK.pair_addr, sizeof FK.pair_addr, "%s", a); return FK.pair_rc; }
static int  f_ble_forget(void *c) { (void)c; FK.ble_forgets++; return FK.ble_forget_rc; }
static bool f_ble_status(void *c, eos_httpd_ble_status_t *o) { (void)c; *o = FK.ble; return true; }

static void *f_open(void *c, const char *path, long *size)
{
    (void)c;
    for (int i = 0; i < FK.nfiles; i++) {
        if (strcmp(FK.files[i].path, path) != 0) continue;
        if (fh_used >= 4) return NULL;
        FH[fh_used].data = FK.files[i].data;
        FH[fh_used].len  = FK.files[i].len;
        FH[fh_used].pos  = 0;
        if (size) *size = FK.files[i].len;
        FK.opens++;
        return &FH[fh_used++];
    }
    return NULL;
}
static int f_read(void *c, void *fh, void *buf, int n)
{
    fake_fh_t *f = (fake_fh_t *)fh;
    int left;
    (void)c;
    left = f->len - f->pos;
    if (n > left) n = left;
    memcpy(buf, f->data + f->pos, (size_t)n);
    f->pos += n;
    return n;
}
static void f_close(void *c, void *fh) { (void)c; (void)fh; FK.closes++; if (fh_used) fh_used--; }

// ==========================================================================
// The fake megabrain
// ==========================================================================
//
// A scripted stand-in for eos_brain_bridge. It records exactly what the ask
// handler passed down — which is the only way to prove the defaults, the
// clamps and the fallbacks reach the client the way web/README.md says they do
// — and it plays back a scripted reply so the drain contract can be walked
// without a socket, a task or a model.

typedef struct {
    eos_httpd_brain_t st;
    int  asks, cancels;
    char q[512], model[64], system[512];
    int  max;
    int  ask_rc;

    // The scripted reply: WAIT for `wait_first` reads, then `reply` a few bytes
    // at a time, then `end_with` (END or FAIL).
    const char *reply;
    int  reply_pos, per_read, wait_first, end_with;
    bool cancel_had;
} fbrain_t;

static fbrain_t FB;

static bool f_brain_status(void *c, eos_httpd_brain_t *o) { (void)c; *o = FB.st; return true; }

static int f_brain_ask(void *c, const eos_httpd_ask_t *a)
{
    (void)c;
    FB.asks++;
    snprintf(FB.q,      sizeof FB.q,      "%s", a->q      ? a->q      : "<null>");
    snprintf(FB.model,  sizeof FB.model,  "%s", a->model  ? a->model  : "<null>");
    snprintf(FB.system, sizeof FB.system, "%s", a->system ? a->system : "<null>");
    FB.max = a->max_tokens;
    return FB.ask_rc;
}

static int f_brain_read(void *c, char *buf, int cap)
{
    int left, n;
    (void)c;
    if (FB.wait_first > 0) { FB.wait_first--; return EOS_HTTPD_STREAM_WAIT; }
    if (!FB.reply) return FB.end_with;
    left = (int)strlen(FB.reply) - FB.reply_pos;
    if (left <= 0) return FB.end_with;
    n = FB.per_read < cap ? FB.per_read : cap;
    if (n > left) n = left;
    memcpy(buf, FB.reply + FB.reply_pos, (size_t)n);
    FB.reply_pos += n;
    return n;
}

static bool f_brain_cancel(void *c) { (void)c; FB.cancels++; return FB.cancel_had; }

static void fbrain_reset(void)
{
    memset(&FB, 0, sizeof FB);
    snprintf(FB.st.host,  sizeof FB.st.host,  "192.168.0.139");
    snprintf(FB.st.model, sizeof FB.st.model, "qwen3.5:2b");
    FB.st.port      = 80;
    FB.st.reachable = true;
    FB.per_read     = 8;
    FB.end_with     = EOS_HTTPD_STREAM_END;
}

// The three the mini is holding, as the bridge compiles them in.
static const char *const FB_MODELS[] = { "qwen3.5:2b", "gemma4:12b-it-qat", "ornith:9b" };

static void fake_ports(eos_httpd_ports_t *p)
{
    memset(p, 0, sizeof *p);
    p->brain_status    = f_brain_status;
    p->brain_ask       = f_brain_ask;
    p->brain_read      = f_brain_read;
    p->brain_cancel    = f_brain_cancel;
    p->wifi_scan_state = f_wifi_state;
    p->wifi_scan_count = f_wifi_count;
    p->wifi_scan_get   = f_wifi_get;
    p->wifi_scan_age_ms= f_wifi_age;
    p->wifi_scan_start = f_wifi_start;
    p->wifi_join       = f_wifi_join;
    p->wifi_forget     = f_wifi_forget;
    p->net_status      = f_net_status;
    p->ble_scan_state  = f_ble_state;
    p->ble_scan_count  = f_ble_count;
    p->ble_scan_get    = f_ble_get;
    p->ble_scan_age_ms = f_ble_age;
    p->ble_scan_start  = f_ble_start;
    p->ble_pair        = f_ble_pair;
    p->ble_forget      = f_ble_forget;
    p->ble_status      = f_ble_status;
    p->file_open       = f_open;
    p->file_read       = f_read;
    p->file_close      = f_close;
}

static void fake_reset(void)
{
    memset(&FK, 0, sizeof FK);
    fbrain_reset();
    fh_used = 0;
    FK.wifi_state = EOS_HTTPD_SCAN_DONE;
    FK.ble_state  = EOS_HTTPD_SCAN_DONE;
    FK.net.state  = EOS_HTTPD_NET_SETUP;
    FK.net.join   = EOS_HTTPD_JOIN_NONE;
    FK.ble.battery = -1;
    snprintf(FK.net.ap_ip, sizeof FK.net.ap_ip, "192.168.4.1");
    snprintf(FK.net.host, sizeof FK.net.host, "penguinos");
    memcpy(FK.net.ap_ssid, "penguinos-f048", 14);
    FK.net.ap_ssid_len = 14;
    FK.net.ap_up = true;
}

static void ap_set(int i, const char *ssid, int len, int8_t rssi, int ch, int auth)
{
    memset(&FK.aps[i], 0, sizeof FK.aps[i]);
    memcpy(FK.aps[i].ssid, ssid, (size_t)len);
    FK.aps[i].ssid_len = (uint8_t)len;
    FK.aps[i].rssi = rssi;
    FK.aps[i].channel = (uint8_t)ch;
    FK.aps[i].auth = (uint8_t)auth;
    for (int b = 0; b < 6; b++) FK.aps[i].bssid[b] = (uint8_t)(0x10 * i + b);
}

// The framed server, so a handler that walks off the end of any of its buffers
// fails a check instead of passing.
typedef struct {
    unsigned char pre[GUARD];
    eos_httpd_t   h;
    unsigned char post[GUARD];
} srv_frame_t;

static srv_frame_t SRV;

static void srv_init(uint8_t mode)
{
    eos_httpd_ports_t p;
    eos_httpd_cfg_t cfg;
    memset(SRV.pre, 0x5A, GUARD);
    memset(SRV.post, 0xA5, GUARD);
    fake_ports(&p);
    eos_httpd_cfg_default(&cfg);
    cfg.mode = mode;
    eos_httpd_init(&SRV.h, &p, NULL, &cfg);
}

static bool srv_intact(void)
{
    for (int i = 0; i < GUARD; i++) if (SRV.pre[i]  != 0x5A) return false;
    for (int i = 0; i < GUARD; i++) if (SRV.post[i] != 0xA5) return false;
    return true;
}

static int call(const char *method, const char *uri, const char *body, eos_httpd_resp_t *r)
{
    eos_httpd_req_t q;
    memset(&q, 0, sizeof q);
    q.method = method;
    q.uri = uri;
    q.body = body;
    q.body_len = body ? (int)strlen(body) : 0;
    return eos_httpd_dispatch(&SRV.h, &q, r);
}

// Every JSON response must be valid JSON and valid UTF-8, always. This wraps
// call() so no test can forget to assert it.
static int callj(const char *method, const char *uri, const char *body, eos_httpd_resp_t *r)
{
    int st = call(method, uri, body, r);
    if (r->kind == EOS_HTTPD_BODY_BUF && r->content_type &&
        strncmp(r->content_type, "application/json", 16) == 0) {
        if (!json_valid(r->body, r->body_len)) CK(false, "response is valid JSON");
        else if (!utf8_valid(r->body, r->body_len)) CK(false, "response is valid UTF-8");
    }
    if (!srv_intact()) CK(false, "the server stayed inside its own struct");
    return st;
}

// ==========================================================================
// Handlers
// ==========================================================================

static void test_wifi_scan(void)
{
    eos_httpd_resp_t r;

    printf("  handlers: GET /api/wifi/scan\n");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    ap_set(0, "weak",   4, -88, 1,  EOS_HTTPD_AUTH_WPA2);
    ap_set(1, "strong", 6, -31, 6,  EOS_HTTPD_AUTH_WPA2_WPA3);
    ap_set(2, "middle", 6, -60, 11, EOS_HTTPD_AUTH_OPEN);
    FK.aps[1].saved = true;
    FK.wifi_n = 3;

    CKI(callj("GET", "/api/wifi/scan", NULL, &r), 200, "a finished scan is 200");
    CKS(r.content_type, "application/json; charset=utf-8", "scan is JSON");
    CKS(r.cache_control, "no-store", "scan is never cached");
    CK(has_kv(r.body, "total", "3"), "scan reports the true total");
    CK(has_kv(r.body, "scanning", "false"), "a finished scan is not scanning");
    CK(has_kv(r.body, "age_ms", "1234"), "scan reports the cache age");

    // Strongest first, which is the only order that is any use to a person, and
    // the only thing that makes a short list the right short list. The list is
    // capped at EOS_HTTPD_SCAN_MAX, which a tier-0 board turns down.
    if (EOS_HTTPD_SCAN_POOL >= 3) {
        const char *sg = strstr(r.body, "\"strong\"");
        const char *m  = strstr(r.body, "\"middle\"");
        const char *w  = strstr(r.body, "\"weak\"");
        CK(sg != NULL, "the strongest network is always in the list");
        if (EOS_HTTPD_SCAN_MAX >= 2) {
            CK(m && sg < m, "sorted: strong before middle");
        }
        if (EOS_HTTPD_SCAN_MAX >= 3) {
            CK(has_kv(r.body, "shown", "3"), "scan shows all three");
            CK(has_kv(r.body, "truncated", "false"), "a short list is not truncated");
            CK(w && m < w, "sorted: middle before weak");
            CK(strstr(r.body, "\"saved\":true") != NULL, "a stored network is flagged");
            CK(strstr(r.body, "\"secure\":false") != NULL, "an open network is flagged insecure");
            CK(strstr(r.body, "\"auth\":\"wpa2_wpa3\"") != NULL, "auth mode is named");
        } else {
            CK(has_kv(r.body, "truncated", "true"), "a capped list says it was cut");
            CK(w == NULL, "the weakest network is the one dropped, not an arbitrary one");
        }
        CK(strstr(r.body, "\"bssid\":\"10:11:12:13:14:15\"") != NULL, "bssid is lowercase colon hex");
        CK(strstr(r.body, "\"ssid_hex\":\"7374726f6e67\"") != NULL, "ssid_hex is the raw bytes");
    }
    CKI(FK.wifi_scan_starts, 0, "reading the cache never starts a scan");

    // A scan in flight is 202 and does not start a second one.
    FK.wifi_state = EOS_HTTPD_SCAN_RUNNING;
    CKI(callj("GET", "/api/wifi/scan", NULL, &r), 202, "a scan in flight is 202");
    CK(has_kv(r.body, "scanning", "true"), "a scan in flight says so");
    CKI(callj("GET", "/api/wifi/scan?rescan=1", NULL, &r), 202, "rescan while scanning is 202");
    CKI(FK.wifi_scan_starts, 0, "rescan while scanning does not start a second scan");

    // An explicit rescan starts one.
    FK.wifi_state = EOS_HTTPD_SCAN_DONE;
    CKI(callj("GET", "/api/wifi/scan?rescan=1", NULL, &r), 202, "an explicit rescan is 202");
    CKI(FK.wifi_scan_starts, 1, "an explicit rescan starts exactly one scan");
    CK(has_kv(r.body, "scanning", "true"), "a started rescan says it is scanning");

    // The radio interlock. WiFi and BLE share one antenna.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.ble_state = EOS_HTTPD_SCAN_RUNNING;
    CKI(callj("GET", "/api/wifi/scan?rescan=1", NULL, &r), 409,
        "a wifi rescan during a BLE scan is refused");
    CK(has_kv(r.body, "error", "\"busy\""), "the refusal is busy");
    CK(strstr(r.body, "one antenna") != NULL, "the refusal explains why");
    CKI(FK.wifi_scan_starts, 0, "the refused rescan never reached the radio");
    CKI(callj("GET", "/api/wifi/scan", NULL, &r), 200,
        "reading the cache during a BLE scan is still fine");

    // A port that refuses.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.wifi_scan_start_rc = -8;
    CKI(callj("GET", "/api/wifi/scan?rescan=1", NULL, &r), 409, "a refused scan start is 409");

    // Truncation: more networks than the response can carry, with the worst
    // possible names. The list gets shorter; the document stays valid.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    for (int i = 0; i < 24; i++) {
        unsigned char raw[32];
        memset(raw, 0xFF, 32);                       // three bytes out for each byte in
        ap_set(i, (const char *)raw, 32, (int8_t)(-30 - i), 6, EOS_HTTPD_AUTH_WPA2);
    }
    FK.wifi_n = 24;
    CKI(callj("GET", "/api/wifi/scan", NULL, &r), 200, "an overlong list still answers 200");
    CK(json_valid(r.body, r.body_len), "an overlong list is still valid JSON");
    CK(has_kv(r.body, "total", "24"), "an overlong list reports the true total");
    if (EOS_HTTPD_SCAN_MAX < 24 || EOS_HTTPD_RESP_MAX < 8000)
        CK(has_kv(r.body, "truncated", "true"), "an overlong list says it was cut");
    CK(r.body_len <= EOS_HTTPD_RESP_MAX, "the response never exceeds the buffer");
    CK(strstr(r.body, "\"shown\":0") == NULL, "at least one network still made it");

    // No wifi on this board at all.
    {
        eos_httpd_ports_t p;
        eos_httpd_cfg_t cfg;
        memset(&p, 0, sizeof p);
        eos_httpd_cfg_default(&cfg);
        eos_httpd_init(&SRV.h, &p, NULL, &cfg);
        CKI(callj("GET", "/api/wifi/scan", NULL, &r), 501, "no wifi port is 501");
        CK(has_kv(r.body, "error", "\"unsupported\""), "no wifi port says unsupported");
    }
}

static void test_wifi_connect(void)
{
    eos_httpd_resp_t r;

    printf("  handlers: POST /api/wifi/connect\n");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);

    CKI(callj("POST", "/api/wifi/connect", "{\"ssid\":\"home\",\"psk\":\"hunter22\"}", &r),
        202, "a good connect is 202, not 200");
    CKI(FK.joins, 1, "the join reached the radio exactly once");
    CKI(FK.join_ssid_len, 4, "the ssid length");
    CK(memcmp(FK.join_ssid, "home", 4) == 0, "the ssid bytes");
    CKI(FK.join_psk_len, 8, "the psk length");
    CKS(FK.join_psk, "hunter22", "the psk bytes");
    CK(has_kv(r.body, "state", "\"trying\""), "the reply says the join is only trying");
    CK(field(r.body, "ok") == NULL,
       "the reply carries no ok:true, which the web app reads as a finished join");
    CK(has_kv(r.body, "persist", "\"on-success\""), "the reply states the persistence rule");
    CK(strstr(r.body, "/api/net/status") != NULL, "the reply says where to poll");
    CK(strstr(r.body, "only after") != NULL, "the reply spells out that nothing is saved yet");

    // An open network: no psk at all.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    CKI(callj("POST", "/api/wifi/connect", "{\"ssid\":\"cafe\"}", &r),
        202, "an open network needs no psk");
    CKI(FK.join_psk_len, 0, "an open network joins with an empty psk");
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    CKI(callj("POST", "/api/wifi/connect", "{\"ssid\":\"cafe\",\"psk\":\"\"}", &r),
        202, "an explicitly empty psk is an open network");

    // ssid_hex is how an SSID that is not valid UTF-8 gets back to the radio
    // exactly as it left it.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    CKI(callj("POST", "/api/wifi/connect", "{\"ssid_hex\":\"61ffc320\",\"psk\":\"12345678\"}", &r),
        202, "ssid_hex is accepted");
    CKI(FK.join_ssid_len, 4, "ssid_hex length");
    CK(memcmp(FK.join_ssid, "a\xff\xc3 ", 4) == 0, "ssid_hex bytes reach the radio unchanged");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    CKI(callj("POST", "/api/wifi/connect",
              "{\"ssid\":\"ignored\",\"ssid_hex\":\"6162\",\"psk\":\"12345678\"}", &r),
        202, "ssid_hex wins over ssid");
    CK(memcmp(FK.join_ssid, "ab", 2) == 0 && FK.join_ssid_len == 2, "ssid_hex is the one used");

    // A 32-byte SSID, the maximum, with no NUL anywhere in it.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    {
        char body[200];
        int p = snprintf(body, sizeof body, "{\"ssid_hex\":\"");
        for (int i = 0; i < 32; i++) p += snprintf(body + p, sizeof body - (size_t)p, "41");
        snprintf(body + p, sizeof body - (size_t)p, "\",\"psk\":\"12345678\"}");
        CKI(callj("POST", "/api/wifi/connect", body, &r), 202, "a 32-byte SSID is accepted");
        CKI(FK.join_ssid_len, 32, "a 32-byte SSID keeps all 32 bytes");
    }
    // 33 bytes is not an SSID.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    {
        char body[200];
        int p = snprintf(body, sizeof body, "{\"ssid_hex\":\"");
        for (int i = 0; i < 33; i++) p += snprintf(body + p, sizeof body - (size_t)p, "41");
        snprintf(body + p, sizeof body - (size_t)p, "\"}");
        CKI(callj("POST", "/api/wifi/connect", body, &r), 400, "a 33-byte SSID is refused");
        CKI(FK.joins, 0, "an over-long SSID never reaches the radio");
    }

    // The rejections, each of which must cost the radio nothing.
    {
        struct { const char *body; int status; const char *why; } BAD[] = {
            { NULL,                                    400, "no body" },
            { "",                                      400, "an empty body" },
            { "not json",                              400, "a body that is not JSON" },
            { "[]",                                    400, "an array body" },
            { "{}",                                    400, "an object with no ssid" },
            { "{\"psk\":\"12345678\"}",                400, "a psk with no ssid" },
            { "{\"ssid\":\"\"}",                       400, "an empty ssid" },
            { "{\"ssid\":123}",                        400, "a numeric ssid" },
            { "{\"ssid\":null}",                       400, "a null ssid" },
            { "{\"ssid\":\"a\",\"psk\":\"short\"}",    400, "a five-character psk" },
            { "{\"ssid\":\"a\",\"psk\":\"1234567\"}",  400, "a seven-character psk" },
            { "{\"ssid\":\"a\",\"psk\":123}",          400, "a numeric psk" },
            { "{\"ssid_hex\":\"41424\"}",              400, "an odd-length ssid_hex" },
            { "{\"ssid_hex\":\"41zz\"}",               400, "a non-hex ssid_hex" },
            { "{\"ssid_hex\":\"\"}",                   400, "an empty ssid_hex" },
        };
        for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
            char msg[120];
            fake_reset();
            srv_init(EOS_HTTPD_MODE_SETUP);
            snprintf(msg, sizeof msg, "connect: %s is %d", BAD[i].why, BAD[i].status);
            CKI(callj("POST", "/api/wifi/connect", BAD[i].body, &r), BAD[i].status, msg);
            snprintf(msg, sizeof msg, "connect: %s never reaches the radio", BAD[i].why);
            CKI(FK.joins, 0, msg);
        }
    }

    // Exactly 8 and exactly 63 characters are the WPA bounds and both must work.
    {
        char body[200], psk[80];
        fake_reset();
        srv_init(EOS_HTTPD_MODE_SETUP);
        memset(psk, 'x', 8); psk[8] = 0;
        snprintf(body, sizeof body, "{\"ssid\":\"a\",\"psk\":\"%s\"}", psk);
        CKI(callj("POST", "/api/wifi/connect", body, &r), 202, "an 8-character psk is accepted");
        CKI(FK.join_psk_len, 8, "an 8-character psk arrives whole");

        fake_reset();
        srv_init(EOS_HTTPD_MODE_SETUP);
        memset(psk, 'x', 63); psk[63] = 0;
        snprintf(body, sizeof body, "{\"ssid\":\"a\",\"psk\":\"%s\"}", psk);
        CKI(callj("POST", "/api/wifi/connect", body, &r), 202, "a 63-character psk is accepted");
        CKI(FK.join_psk_len, 63, "a 63-character psk arrives whole");

        fake_reset();
        srv_init(EOS_HTTPD_MODE_SETUP);
        memset(psk, 'x', 64); psk[64] = 0;
        snprintf(body, sizeof body, "{\"ssid\":\"a\",\"psk\":\"%s\"}", psk);
        CKI(callj("POST", "/api/wifi/connect", body, &r), 400, "a 64-character psk is refused");
        CKI(FK.joins, 0, "a 64-character psk never reaches the radio");
    }

    // A body the transport already refused for size.
    {
        eos_httpd_req_t q;
        fake_reset();
        srv_init(EOS_HTTPD_MODE_SETUP);
        memset(&q, 0, sizeof q);
        q.method = "POST";
        q.uri = "/api/wifi/connect";
        q.body_truncated = true;
        CKI(eos_httpd_dispatch(&SRV.h, &q, &r), 413, "an over-long body is 413");
        CK(has_kv(r.body, "error", "\"too_big\""), "an over-long body says too_big");
        CKI(FK.joins, 0, "an over-long body never reaches the radio");
    }

    // The interlocks.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.ble_state = EOS_HTTPD_SCAN_RUNNING;
    CKI(callj("POST", "/api/wifi/connect", "{\"ssid\":\"a\",\"psk\":\"12345678\"}", &r),
        409, "a join during a BLE scan is refused");
    CKI(FK.joins, 0, "a refused join never reaches the radio");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.net.state = EOS_HTTPD_NET_JOINING;
    CKI(callj("POST", "/api/wifi/connect", "{\"ssid\":\"a\",\"psk\":\"12345678\"}", &r),
        409, "a second join while one is in flight is refused");
    CKI(FK.joins, 0, "the second join never reaches the radio");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.wifi_state = EOS_HTTPD_SCAN_RUNNING;
    CKI(callj("POST", "/api/wifi/connect", "{\"ssid\":\"a\",\"psk\":\"12345678\"}", &r),
        409, "a join during a wifi scan is refused");

    // A radio that refuses the start.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.join_rc = -3;
    CKI(callj("POST", "/api/wifi/connect", "{\"ssid\":\"a\",\"psk\":\"12345678\"}", &r),
        500, "an io error from the radio is 500");
}

static void test_wifi_forget_and_status(void)
{
    eos_httpd_resp_t r;

    printf("  handlers: POST /api/wifi/forget, GET /api/net/status\n");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    CKI(callj("POST", "/api/wifi/forget", NULL, &r), 200, "forget is 200");
    CKI(FK.forgets, 1, "forget reached the radio once");
    CK(strstr(r.body, "\"stored\":false") != NULL, "forget reports nothing stored");
    CK(has_kv(r.body, "next", "\"setup\""), "forget says the board returns to setup");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.forget_rc = -3;
    CKI(callj("POST", "/api/wifi/forget", NULL, &r), 500, "a failed forget is 500");

    // Setup mode.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    CKI(callj("GET", "/api/net/status", NULL, &r), 200, "status is 200");
    CK(has_kv(r.body, "mode", "\"setup\""), "status: setup mode");
    CK(has_kv(r.body, "state", "\"setup\""), "status: setup state");
    CK(strstr(r.body, "\"rssi\":null") != NULL, "status: no rssi without a link");
    CK(strstr(r.body, "\"mdns\":\"penguinos.local\"") != NULL, "status: the mDNS name");
    CK(strstr(r.body, "\"ip\":\"192.168.4.1\"") != NULL, "status: the AP address");
    CK(strstr(r.body, "penguinos-f048") != NULL, "status: the AP name");
    CK(has_kv_after(r.body, "\"join\":", "state", "\"none\""),
       "status: nothing has been tried yet");

    // Run mode with a link.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    FK.net.state = EOS_HTTPD_NET_UP;
    FK.net.join  = EOS_HTTPD_JOIN_OK;
    FK.net.rssi  = -47;
    FK.net.ssid_stored = true;
    memcpy(FK.net.ssid, "WavvyWorld", 10);
    FK.net.ssid_len = 10;
    snprintf(FK.net.ip, sizeof FK.net.ip, "192.168.0.51");
    CKI(callj("GET", "/api/net/status", NULL, &r), 200, "status in run mode is 200");
    CK(has_kv(r.body, "mode", "\"run\""), "status: run mode");
    CK(has_kv(r.body, "state", "\"up\""), "status: up");
    CK(strstr(r.body, "\"rssi\":-47") != NULL, "status: the rssi is reported when there is one");
    CK(strstr(r.body, "\"ip\":\"192.168.0.51\"") != NULL, "status: the station address");
    CK(strstr(r.body, "\"stored\":true") != NULL, "status: credentials are stored");
    CK(strstr(r.body, "\"ssid_hex\":\"5761767679576f726c64\"") != NULL, "status: the ssid in hex");

    // Every join outcome must name itself and say what to do about it.
    {
        int OUT[] = { EOS_HTTPD_JOIN_NONE, EOS_HTTPD_JOIN_OK, EOS_HTTPD_JOIN_RUNNING,
                      EOS_HTTPD_JOIN_AUTH, EOS_HTTPD_JOIN_NOTFOUND, EOS_HTTPD_JOIN_TIMEOUT,
                      EOS_HTTPD_JOIN_FAILED };
        // The coarse state the page branches on, and the reason it turns into
        // a sentence. web/README.md's table is the other half of this contract.
        const char *STATE[]  = { "none", "ok", "trying", "failed", "failed",
                                 "failed", "failed" };
        const char *REASON[] = { NULL, NULL, NULL, "bad_auth", "no_ap",
                                 "ip_fail", "failed" };
        for (size_t i = 0; i < sizeof OUT / sizeof OUT[0]; i++) {
            char msg[96], want[40];
            fake_reset();
            srv_init(EOS_HTTPD_MODE_SETUP);
            FK.net.join = (uint8_t)OUT[i];
            callj("GET", "/api/net/status", NULL, &r);
            snprintf(want, sizeof want, "\"%s\"", STATE[i]);
            snprintf(msg, sizeof msg, "status: join %d has state %s", OUT[i], STATE[i]);
            CK(has_kv_after(r.body, "\"join\":", "state", want), msg);
            if (REASON[i]) snprintf(want, sizeof want, "\"%s\"", REASON[i]);
            else           snprintf(want, sizeof want, "null");
            snprintf(msg, sizeof msg, "status: join %d has reason %s", OUT[i],
                     REASON[i] ? REASON[i] : "null");
            CK(has_kv(r.body, "reason", want), msg);
            snprintf(msg, sizeof msg, "status: join %d carries a detail", OUT[i]);
            CK(strstr(r.body, "\"detail\":\"") != NULL, msg);
            CKS(eos_httpd_join_state(OUT[i]), STATE[i], "join state name");
            CK(REASON[i] ? (eos_httpd_join_reason(OUT[i]) &&
                            strcmp(eos_httpd_join_reason(OUT[i]), REASON[i]) == 0)
                         : eos_httpd_join_reason(OUT[i]) == NULL,
               "join reason name");
        }
        // The three failure outcomes must all say that nothing was saved. This
        // is the whole promise of the flow, restated where the user can see it.
        int FAILED[] = { EOS_HTTPD_JOIN_AUTH, EOS_HTTPD_JOIN_NOTFOUND,
                         EOS_HTTPD_JOIN_TIMEOUT, EOS_HTTPD_JOIN_FAILED };
        for (size_t i = 0; i < sizeof FAILED / sizeof FAILED[0]; i++) {
            fake_reset();
            srv_init(EOS_HTTPD_MODE_SETUP);
            FK.net.join = (uint8_t)FAILED[i];
            callj("GET", "/api/net/status", NULL, &r);
            CK(strstr(r.body, "nothing was saved") != NULL,
               "a failed join says that nothing was saved");
        }
    }

    // An SSID that is not valid UTF-8, reported by the status endpoint.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    memcpy(FK.net.ssid, "b\xff" "d", 3);
    FK.net.ssid_len = 3;
    CKI(callj("GET", "/api/net/status", NULL, &r), 200, "status with a broken SSID is 200");
    CK(strstr(r.body, "\"ssid_hex\":\"62ff64\"") != NULL, "status: the raw bytes survive as hex");
}

static void test_ble(void)
{
    eos_httpd_resp_t r;

    printf("  handlers: the four /api/ble endpoints\n");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    memcpy(FK.devs[0].name, "K809", 4);
    FK.devs[0].name_len = 4;
    FK.devs[0].rssi = -44;
    FK.devs[0].is_hid = true;
    snprintf(FK.devs[0].addr, sizeof FK.devs[0].addr, "aa:bb:cc:dd:ee:ff");
    FK.ble_n = 1;

    CKI(callj("GET", "/api/ble/scan", NULL, &r), 200, "a finished BLE scan is 200");
    CK(strstr(r.body, "\"name\":\"K809\"") != NULL, "the keyboard is listed");
    CK(strstr(r.body, "\"hid\":true") != NULL, "the HID flag is reported");
    CK(strstr(r.body, "\"addr\":\"aa:bb:cc:dd:ee:ff\"") != NULL, "the address is reported");

    FK.wifi_state = EOS_HTTPD_SCAN_RUNNING;
    CKI(callj("GET", "/api/ble/scan?rescan=1", NULL, &r), 409,
        "a BLE rescan during a wifi scan is refused");
    CKI(FK.ble_scan_starts, 0, "the refused BLE rescan never reached the radio");
    FK.wifi_state = EOS_HTTPD_SCAN_DONE;
    CKI(callj("GET", "/api/ble/scan?rescan=1", NULL, &r), 202, "a BLE rescan is 202");
    CKI(FK.ble_scan_starts, 1, "the BLE rescan started exactly one scan");

    // Pairing.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    CKI(callj("POST", "/api/ble/pair", "{\"addr\":\"AA:BB:CC:DD:EE:FF\"}", &r),
        202, "pair is 202");
    CKS(FK.pair_addr, "aa:bb:cc:dd:ee:ff", "an uppercase address is normalised");
    CK(strstr(r.body, "one board at a time") != NULL,
       "the reply carries the one-board-at-a-time warning at the moment of pairing");
    CK(strstr(r.body, "/api/ble/status") != NULL, "the reply says where to poll");
    CK(has_kv(r.body, "expect", "\"passkey\""), "the reply says a passkey is coming");

    {
        struct { const char *b; const char *why; } BAD[] = {
            { NULL,                          "no body" },
            { "{}",                          "no addr" },
            { "{\"addr\":\"\"}",             "an empty addr" },
            { "{\"addr\":\"aa:bb:cc:dd:ee\"}", "a five-octet addr" },
            { "{\"addr\":\"aa:bb:cc:dd:ee:ff:00\"}", "a seven-octet addr" },
            { "{\"addr\":\"aa-bb-cc-dd-ee-ff\"}", "dashes instead of colons" },
            { "{\"addr\":\"zz:bb:cc:dd:ee:ff\"}", "a non-hex octet" },
            { "{\"addr\":\"aabbccddeeff\"}", "no separators" },
            { "{\"addr\":123}",              "a numeric addr" },
            { "{\"addr\":\"aa:bb:cc:dd:ee:f\"}", "a short last octet" },
        };
        for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
            char msg[120];
            fake_reset();
            srv_init(EOS_HTTPD_MODE_SETUP);
            snprintf(msg, sizeof msg, "pair: %s is 400", BAD[i].why);
            CKI(callj("POST", "/api/ble/pair", BAD[i].b, &r), 400, msg);
            snprintf(msg, sizeof msg, "pair: %s never reaches the radio", BAD[i].why);
            CKI(FK.pairs, 0, msg);
        }
    }

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.wifi_state = EOS_HTTPD_SCAN_RUNNING;
    CKI(callj("POST", "/api/ble/pair", "{\"addr\":\"aa:bb:cc:dd:ee:ff\"}", &r),
        409, "pairing during a wifi scan is refused");
    CKI(FK.pairs, 0, "the refused pair never reached the radio");

    // Status, and the passkey, which is the reason this endpoint exists.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.ble.pairing = true;
    FK.ble.passkey_shown = true;
    FK.ble.passkey = 1234;                 // four digits, and it must show as six
    CKI(callj("GET", "/api/ble/status", NULL, &r), 200, "ble status is 200");
    CK(strstr(r.body, "\"passkey\":\"001234\"") != NULL,
       "a passkey is a zero-padded six-digit string, never a number");
    CK(strstr(r.body, "\"battery\":null") != NULL, "an unknown battery level is null");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.ble.passkey_shown = true;
    FK.ble.passkey = 0;
    callj("GET", "/api/ble/status", NULL, &r);
    CK(strstr(r.body, "\"passkey\":\"000000\"") != NULL, "a passkey of zero is six zeroes");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.ble.bonded = true;
    FK.ble.connected = true;
    FK.ble.battery = 87;
    FK.ble.state = EOS_HTTPD_BLE_READY;
    memcpy(FK.ble.name, "K8\xff""09", 5);
    FK.ble.name_len = 5;
    snprintf(FK.ble.addr, sizeof FK.ble.addr, "aa:bb:cc:dd:ee:ff");
    callj("GET", "/api/ble/status", NULL, &r);
    CK(strstr(r.body, "\"bonded\":{") != NULL,
       "a bond is reported as an object, so bonded.addr and bonded.name exist");
    CK(strstr(r.body, "\"state\":\"ready\"") != NULL, "the BLE state is named");
    CK(strstr(r.body, "\"battery\":87") != NULL, "a known battery level is a number");
    CK(strstr(r.body, "\"passkey\":null") != NULL, "no passkey pending is null");
    CK(strstr(r.body, "\"name_hex\":\"4b38ff3039\"") != NULL,
       "a device name that is not UTF-8 survives as hex");

    // Forget.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    CKI(callj("POST", "/api/ble/forget", NULL, &r), 200, "ble forget is 200");
    CKI(FK.ble_forgets, 1, "ble forget reached the radio once");
    CK(strstr(r.body, "clear it there too") != NULL,
       "ble forget says the keyboard still holds its side");

    // A board with no Bluetooth.
    {
        eos_httpd_ports_t p;
        eos_httpd_cfg_t cfg;
        memset(&p, 0, sizeof p);
        eos_httpd_cfg_default(&cfg);
        eos_httpd_init(&SRV.h, &p, NULL, &cfg);
        CKI(callj("GET", "/api/ble/scan", NULL, &r), 501, "no BLE: scan is 501");
        CKI(callj("POST", "/api/ble/pair", "{\"addr\":\"aa:bb:cc:dd:ee:ff\"}", &r), 501,
            "no BLE: pair is 501");
        CKI(callj("GET", "/api/ble/status", NULL, &r), 501, "no BLE: status is 501");
        CKI(callj("POST", "/api/ble/forget", NULL, &r), 501, "no BLE: forget is 501");
    }
}

// ==========================================================================
// Static files, the portal, and the built-in page
// ==========================================================================

static const char PAGE[] = "<!doctype html><title>app</title>";
static const char CSSGZ[] = "\x1f\x8b\x08 not really gzip but it does not matter";

static void test_static(void)
{
    eos_httpd_resp_t r;

    printf("  static: document roots, gzip twins, the portal\n");

    // SETUP with nothing on flash: the built-in page, so an unprovisioned board
    // is still a board you can get onto a network.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    CKI(call("GET", "/", NULL, &r), 200, "setup with no files serves something at /");
    CKI(r.kind, EOS_HTTPD_BODY_BUF, "the built-in page comes from the buffer, not a file");
    CKS(r.content_type, "text/html; charset=utf-8", "the built-in page is HTML");
    CK(r.content_encoding == NULL, "the built-in page is not gzipped");
    CK(strstr(r.body, "/api/wifi/scan") != NULL, "the built-in page calls the scan endpoint");
    CK(strstr(r.body, "/api/wifi/connect") != NULL, "the built-in page calls the connect endpoint");
    CK(strstr(r.body, "only after") != NULL, "the built-in page states the persistence rule");
    CKI(call("GET", "/index.html", NULL, &r), 200, "the built-in page is also at /index.html");

    {
        int n = 0;
        const char *p = eos_httpd_builtin_setup(&n);
        CK(p != NULL && n > 500, "the built-in page is a real page, not a placeholder");
        CKI((int)strlen(p), n, "the built-in page's length matches its bytes");
        CK(strstr(p, "http://") == NULL, "the built-in page fetches nothing off-board");
        CK(strstr(p, "//cdn") == NULL, "the built-in page has no CDN reference");
    }

    // Anything else in SETUP goes to the portal rather than to a 404 the phone
    // renders as "no internet".
    CKI(call("GET", "/whatever", NULL, &r), 302, "an unknown path in setup redirects");
    CKI(r.kind, EOS_HTTPD_BODY_REDIRECT, "the redirect is a redirect");
    CKS(r.location, "http://192.168.4.1/", "the redirect points at the portal");
    CK(r.body_len > 0, "the redirect carries a body for clients that render it");

    // The probes.
    {
        const char *P[] = { "/generate_204", "/hotspot-detect.html", "/ncsi.txt",
                            "/success.txt", "/canonical.html", "/connecttest.txt" };
        for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
            char msg[100];
            snprintf(msg, sizeof msg, "setup: %s redirects to the portal", P[i]);
            CKI(call("GET", P[i], NULL, &r), 302, msg);
        }
    }
    CKI(SRV.h.req_portal, 7, "the portal counter counted every redirect");

    // Files on flash win over the built-in page, and the .gz twin wins over the
    // plain file.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.files[0].path = "/int/setup/index.html";
    FK.files[0].data = PAGE;
    FK.files[0].len  = (int)sizeof PAGE - 1;
    FK.nfiles = 1;
    CKI(call("GET", "/", NULL, &r), 200, "a real setup page is served when there is one");
    CKI(r.kind, EOS_HTTPD_BODY_FILE, "a real setup page is streamed from flash");
    CKS(r.path, "/int/setup/index.html", "the resolved path");
    CK(r.content_encoding == NULL, "a plain file is not marked gzip");
    CK(r.file != NULL, "the file handle comes back with the response");
    CKI(r.file_size, (int)sizeof PAGE - 1, "the file size is reported");
    if (r.file) SRV.h.ports.file_close(NULL, r.file);

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.files[0].path = "/int/setup/index.html.gz";
    FK.files[0].data = PAGE;
    FK.files[0].len  = (int)sizeof PAGE - 1;
    FK.files[1].path = "/int/setup/index.html";
    FK.files[1].data = PAGE;
    FK.files[1].len  = (int)sizeof PAGE - 1;
    FK.nfiles = 2;
    CKI(call("GET", "/", NULL, &r), 200, "the gzip twin is served");
    CKS(r.path, "/int/setup/index.html.gz", "the gzip twin is the one opened");
    CKS(r.content_encoding, "gzip", "the gzip twin is marked Content-Encoding: gzip");
    CKS(r.content_type, "text/html; charset=utf-8", "the gzip twin keeps the real type");
    CKI(FK.opens, 1, "the gzip twin was opened exactly once");
    if (r.file) SRV.h.ports.file_close(NULL, r.file);

    // RUN mode: the full app, out of a different root, and a 404 rather than a
    // portal redirect, because the board is a host on somebody's LAN now.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    FK.files[0].path = "/int/web/index.html.gz";
    FK.files[0].data = PAGE;
    FK.files[0].len  = (int)sizeof PAGE - 1;
    FK.files[1].path = "/int/web/style.css.gz";
    FK.files[1].data = CSSGZ;
    FK.files[1].len  = (int)sizeof CSSGZ - 1;
    FK.nfiles = 2;

    CKI(call("GET", "/", NULL, &r), 200, "run mode serves the app at /");
    CKS(r.path, "/int/web/index.html.gz", "run mode uses the run root");
    CKS(r.cache_control, "public, max-age=600", "the app is cacheable in run mode");
    if (r.file) SRV.h.ports.file_close(NULL, r.file);

    CKI(call("GET", "/style.css", NULL, &r), 200, "run mode serves the stylesheet");
    CKS(r.content_type, "text/css; charset=utf-8", "the stylesheet is CSS");
    CKS(r.content_encoding, "gzip", "the stylesheet is gzipped");
    if (r.file) SRV.h.ports.file_close(NULL, r.file);

    CKI(call("GET", "/app.js", NULL, &r), 404, "a file that is not there is 404 in run mode");
    CK(has_kv(r.body, "error", "\"not_found\""), "the 404 names itself");
    CKI(call("GET", "/generate_204", NULL, &r), 404,
        "run mode does not answer a probe with a portal redirect");
    CKI(SRV.h.req_portal, 0, "run mode never redirected anything to a portal");

    // A path that would escape the document root never reaches the filesystem.
    {
        const char *T[] = { "/../../etc/passwd", "/%2e%2e/%2e%2e/etc/passwd",
                            "/a/../../b", "/a\\b" };
        for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
            char msg[100];
            int before = FK.opens;
            snprintf(msg, sizeof msg, "traversal %s never opens a file", T[i]);
            call("GET", T[i], NULL, &r);
            CKI(FK.opens, before, msg);
        }
    }

    // A path longer than the staged buffer must be refused, not truncated onto
    // a different file.
    {
        char uri[EOS_HTTPD_URI_MAX];
        int p = 1;
        uri[0] = '/';
        while (p < EOS_HTTPD_URI_MAX - 2) uri[p++] = 'a';
        uri[p] = 0;
        CKI(call("GET", uri, NULL, &r), 404, "an over-long path is 404 in run mode");
    }

    // No file ports at all: static serving is off, and in SETUP the built-in
    // page still works, which is the entire point of it existing.
    {
        eos_httpd_ports_t p;
        eos_httpd_cfg_t cfg;
        fake_ports(&p);
        p.file_open = NULL;
        p.file_read = NULL;
        p.file_close = NULL;
        eos_httpd_cfg_default(&cfg);
        cfg.mode = EOS_HTTPD_MODE_SETUP;
        eos_httpd_init(&SRV.h, &p, NULL, &cfg);
        CKI(call("GET", "/", NULL, &r), 200, "no filesystem: the built-in page still serves");
        CKI(r.kind, EOS_HTTPD_BODY_BUF, "no filesystem: from the buffer");
        cfg.mode = EOS_HTTPD_MODE_RUN;
        eos_httpd_init(&SRV.h, &p, NULL, &cfg);
        CKI(call("GET", "/", NULL, &r), 404, "no filesystem in run mode is a 404");
    }

    // A custom root, because the CYD serves the app from the card.
    {
        eos_httpd_ports_t p;
        eos_httpd_cfg_t cfg;
        fake_reset();
        fake_ports(&p);
        eos_httpd_cfg_default(&cfg);
        cfg.mode = EOS_HTTPD_MODE_RUN;
        cfg.root_run = "/sd/web";
        eos_httpd_init(&SRV.h, &p, NULL, &cfg);
        FK.files[0].path = "/sd/web/index.html.gz";
        FK.files[0].data = PAGE;
        FK.files[0].len  = (int)sizeof PAGE - 1;
        FK.nfiles = 1;
        CKI(call("GET", "/", NULL, &r), 200, "a custom document root is honoured");
        CKS(r.path, "/sd/web/index.html.gz", "the custom root is the one used");
        if (r.file) SRV.h.ports.file_close(NULL, r.file);
    }
}

static void test_errors_and_counters(void)
{
    eos_httpd_resp_t r;

    printf("  dispatch: methods, counters, malformed requests\n");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);

    CKI(callj("POST", "/api/wifi/scan", NULL, &r), 405, "POST to a GET endpoint is 405");
    CKI(callj("PUT", "/api/wifi/scan", NULL, &r), 405, "PUT is 405");
    CKI(callj("GET", "/api/nope", NULL, &r), 404, "an unknown api path is 404");
    CK(has_kv(r.body, "error", "\"not_found\""), "the unknown api path names itself");

    {
        eos_httpd_req_t q;
        memset(&q, 0, sizeof q);
        q.uri = "/";
        CKI(eos_httpd_dispatch(&SRV.h, &q, &r), 400, "a request with no method is 400");
        memset(&q, 0, sizeof q);
        q.method = "GET";
        CKI(eos_httpd_dispatch(&SRV.h, &q, &r), 400, "a request with no uri is 400");
    }

    {
        char big[EOS_HTTPD_URI_MAX + 64];
        memset(big, 'a', sizeof big - 1);
        big[0] = '/';
        big[sizeof big - 1] = 0;
        CKI(callj("GET", big, NULL, &r), 414, "an over-long request target is 414");
    }

    CKI(callj("GET", "/api/%zz", NULL, &r), 400, "a malformed escape in the target is 400");

    // The counters, which are how a portal that is misbehaving is visible from
    // the other side of a SoftAP.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    call("GET", "/api/net/status", NULL, &r);
    call("GET", "/api/net/status", NULL, &r);
    call("GET", "/generate_204", NULL, &r);
    call("GET", "/whatever", NULL, &r);
    call("GET", "/api/nope", NULL, &r);
    CKI(SRV.h.req_total, 5, "every request is counted");
    CKI(SRV.h.req_api, 2, "api requests are counted");
    CKI(SRV.h.req_portal, 2, "the probe and the redirected page are both portal hits");
    CKI(SRV.h.req_rejected, 1, "the rejection is counted");

    call("GET", "/api/net/status", NULL, &r);
    CK(strstr(r.body, "\"requests\":") != NULL, "status reports the counters");

    // Init with no config at all must still be a working server.
    {
        eos_httpd_ports_t p;
        fake_reset();
        fake_ports(&p);
        eos_httpd_init(&SRV.h, &p, NULL, NULL);
        CKI(call("GET", "/", NULL, &r), 200, "a default-configured server serves the setup page");
        CKI(SRV.h.cfg.port, 80, "the default port");
        CKS(SRV.h.cfg.portal_ip, "192.168.4.1", "the default portal address");
        CKS(SRV.h.cfg.root_setup, "/int/setup", "the default setup root");
        CKS(SRV.h.cfg.root_run, "/int/web", "the default run root");
    }

    // A config with NULL strings must fall back rather than dereference them.
    {
        eos_httpd_ports_t p;
        eos_httpd_cfg_t cfg;
        fake_reset();
        fake_ports(&p);
        memset(&cfg, 0, sizeof cfg);
        cfg.mode = EOS_HTTPD_MODE_SETUP;
        eos_httpd_init(&SRV.h, &p, NULL, &cfg);
        CKS(SRV.h.cfg.root_setup, "/int/setup", "a NULL root falls back to the default");
        CKS(SRV.h.cfg.portal_ip, "192.168.4.1", "a NULL portal address falls back");
        CKI(call("GET", "/nope", NULL, &r), 302, "a zeroed config still redirects to the portal");
    }

    CK(srv_intact(), "the server never wrote outside its own struct");
}

// The fields web/README.md says setup.js reads, asserted one at a time. This is
// a contract between two files written by two people who never spoke, and the
// failure mode — a page that renders blank where an address should be — does
// not show up in either half on its own.
static void test_web_contract(void)
{
    eos_httpd_resp_t r;

    printf("  contract: the fields the web app actually reads\n");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    FK.net.state = EOS_HTTPD_NET_UP;
    FK.net.join  = EOS_HTTPD_JOIN_OK;
    FK.net.rssi  = -47;
    FK.net.ssid_stored = true;
    memcpy(FK.net.ssid, "WavvyWorld", 10);
    FK.net.ssid_len = 10;
    snprintf(FK.net.ip, sizeof FK.net.ip, "192.168.0.51");
    callj("GET", "/api/net/status", NULL, &r);

    // Top level, because that is where the page looks. They are also inside
    // "sta"; both spellings ship rather than one being argued about later.
    CK(has_kv(r.body, "mode", "\"run\""),            "net/status: mode at the top level");
    CK(has_kv(r.body, "ip", "\"192.168.0.51\""),     "net/status: ip at the top level");
    CK(has_kv(r.body, "ssid", "\"WavvyWorld\""),     "net/status: ssid at the top level");
    CK(has_kv(r.body, "rssi", "-47"),                "net/status: rssi at the top level");
    CK(has_kv(r.body, "hostname", "\"penguinos\""),     "net/status: hostname");
    CK(has_kv(r.body, "host", "\"penguinos\""),         "net/status: host, the other spelling");
    CK(has_kv(r.body, "mdns", "\"penguinos.local\""),   "net/status: mdns");
    CK(strstr(r.body, "\"ap\":{") != NULL,           "net/status: an ap object");
    CK(has_kv_after(r.body, "\"ap\":", "ssid", "\""), "net/status: ap.ssid");
    CK(strstr(r.body, "\"join\":{") != NULL,         "net/status: a join object");
    CK(has_kv_after(r.body, "\"join\":", "state", "\"ok\""), "net/status: join.state");
    CK(has_kv_after(r.body, "\"join\":", "reason", "null"),  "net/status: join.reason");

    // The scan list, field for field.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    ap_set(0, "net", 3, -55, 6, EOS_HTTPD_AUTH_WPA2);
    memset(FK.aps[0].bssid, 0, 6);   // eos_net's reduced scan does not keep one
    FK.wifi_n = 1;
    callj("GET", "/api/wifi/scan", NULL, &r);
    CK(strstr(r.body, "\"networks\":[") != NULL, "wifi/scan: a networks array");
    CK(strstr(r.body, "\"ssid\":\"net\"") != NULL, "wifi/scan: ssid");
    CK(strstr(r.body, "\"rssi\":-55") != NULL,    "wifi/scan: rssi");
    CK(strstr(r.body, "\"auth\":\"wpa2\"") != NULL, "wifi/scan: auth as a string");
    CK(strstr(r.body, "\"channel\":6") != NULL,   "wifi/scan: channel");
    CK(strstr(r.body, "\"hidden\":false") != NULL, "wifi/scan: hidden");
    // eos_net's reduced scan carries no BSSID, and a plausible-looking address
    // nobody can connect to is worse than an absent one.
    CK(strstr(r.body, "\"bssid\":null") != NULL,  "wifi/scan: an absent bssid is null");

    // The device list.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    memcpy(FK.devs[0].name, "K809", 4);
    FK.devs[0].name_len = 4;
    FK.devs[0].rssi = -40;
    FK.devs[0].is_hid = true;
    FK.devs[0].bonded = true;
    snprintf(FK.devs[0].addr, sizeof FK.devs[0].addr, "de:ad:be:ef:00:01");
    FK.ble_n = 1;
    callj("GET", "/api/ble/scan", NULL, &r);
    CK(strstr(r.body, "\"devices\":[") != NULL,   "ble/scan: a devices array");
    CK(strstr(r.body, "\"addr\":\"de:ad:be:ef:00:01\"") != NULL, "ble/scan: addr");
    CK(strstr(r.body, "\"name\":\"K809\"") != NULL, "ble/scan: name");
    CK(strstr(r.body, "\"rssi\":-40") != NULL,    "ble/scan: rssi");
    CK(strstr(r.body, "\"bonded\":true") != NULL, "ble/scan: the bonded flag");
    CK(has_kv(r.body, "scanning", "false"),       "ble/scan: scanning");

    // The pairing status.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.ble.state = EOS_HTTPD_BLE_PAIRING;
    FK.ble.pairing = true;
    FK.ble.passkey_shown = true;
    FK.ble.passkey = 7;
    FK.ble.battery = -1;
    callj("GET", "/api/ble/status", NULL, &r);
    CK(has_kv(r.body, "state", "\"pairing\""),     "ble/status: state");
    CK(has_kv(r.body, "passkey", "\"000007\""),    "ble/status: a six-digit passkey");
    CK(has_kv(r.body, "connected", "false"),      "ble/status: connected");
    CK(has_kv(r.body, "battery", "null"),         "ble/status: battery");
    CK(has_kv(r.body, "bonded", "null"),          "ble/status: bonded is null when there is none");
    CK(has_kv(r.body, "reason", "null"),          "ble/status: reason");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    FK.ble.state = EOS_HTTPD_BLE_READY;
    FK.ble.bonded = true;
    FK.ble.connected = true;
    memcpy(FK.ble.name, "K809", 4);
    FK.ble.name_len = 4;
    snprintf(FK.ble.addr, sizeof FK.ble.addr, "de:ad:be:ef:00:01");
    callj("GET", "/api/ble/status", NULL, &r);
    CK(has_kv_after(r.body, "\"bonded\":", "name", "\"K809\""), "ble/status: bonded.name");
    CK(has_kv_after(r.body, "\"bonded\":", "addr", "\"de:ad:be:ef:00:01\""),
       "ble/status: bonded.addr");
}

// The whole flow, once, in the order a person actually does it. This is the
// test that would have caught a contract change between the web app and the
// board, which is the failure mode that costs an evening.
// ==========================================================================
// Megabrain
// ==========================================================================
//
// The three endpoints, the shapes web/README.md's "Megabrain" section spells,
// and the two things about the streaming answer that are invariants rather than
// output: that dispatch stages it without touching the response buffer, and
// that a staged stream leaves the server able to answer the next request. Both
// are what makes it safe for the ESP-IDF responder to drop the dispatch lock
// before it starts draining, which is the whole reason one chat request does
// not park the other three workers.

static void brain_off(void)
{
    SRV.h.ports.brain_status = NULL;
    SRV.h.ports.brain_ask    = NULL;
    SRV.h.ports.brain_read   = NULL;
    SRV.h.ports.brain_cancel = NULL;
}

static void test_brain_status(void)
{
    eos_httpd_resp_t r;

    printf("  handlers: GET /api/brain/status\n");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    FB.st.models      = FB_MODELS;
    FB.st.model_count = 3;

    CKI(callj("GET", "/api/brain/status", NULL, &r), 200, "status is 200");
    CKS(r.content_type, "application/json; charset=utf-8", "status is JSON");
    CKS(r.cache_control, "no-store", "status is never cached");
    CK(has_kv(r.body, "host", "\"192.168.0.139\""), "status reports the host");
    CK(has_kv(r.body, "port", "80"), "status reports the port");
    CK(has_kv(r.body, "model", "\"qwen3.5:2b\""), "status reports the default model");
    CK(has_kv(r.body, "reachable", "true"), "a probed link reads as reachable");
    CK(has_kv(r.body, "busy", "false"), "an idle client is not busy");
    CK(has_kv(r.body, "last_error", "null"),
       "no failure yet is null, not an empty string - the web app branches on it");
    CK(strstr(r.body, "\"models\":[\"qwen3.5:2b\",\"gemma4:12b-it-qat\",\"ornith:9b\"]") != NULL,
       "the model list is an array in order");

    // A board that cannot reach the mini still answers, and says so. The bar
    // reads the same fact and prints "no brain".
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    FB.st.reachable  = false;
    FB.st.last_error = "megabrain: EOS_BRAIN_ERR_CONNECT";
    CKI(callj("GET", "/api/brain/status", NULL, &r), 200, "unreachable is still 200");
    CK(has_kv(r.body, "reachable", "false"), "unreachable says so");
    CK(strstr(r.body, "EOS_BRAIN_ERR_CONNECT") != NULL, "and carries the last error");

    // No model list is legal: the board cannot discover one, and a binding that
    // was given none must still answer something the picker can render.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    CKI(callj("GET", "/api/brain/status", NULL, &r), 200, "no model list is still 200");
    CK(strstr(r.body, "\"models\":[]") != NULL, "and an empty array, never a null");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    FB.st.busy = true;
    CKI(callj("GET", "/api/brain/status", NULL, &r), 200, "busy is 200");
    CK(has_kv(r.body, "busy", "true"), "a request in flight reads as busy");

    // A board with no megabrain client at all. 501, not 500 and not an empty
    // object: web/README.md's `unsupported` is "valid call, not on this tier".
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    brain_off();
    CKI(callj("GET", "/api/brain/status", NULL, &r), 501, "no client is 501");
    CK(has_kv(r.body, "error", "\"unsupported\""), "and names it unsupported");
    CKI(callj("POST", "/api/brain/cancel", NULL, &r), 501, "cancel with no client is 501");
    CKI(callj("POST", "/api/brain/ask", "{\"q\":\"hi\"}", &r), 501, "ask with no client is 501");
}

static void test_brain_ask(void)
{
    eos_httpd_resp_t r;
    char body[1200], q[512];
    int i;

    printf("  handlers: POST /api/brain/ask\n");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);

    // The whole documented body, and every field arriving intact.
    CKI(call("POST", "/api/brain/ask",
             "{\"q\":\"what is a tiling wm\",\"model\":\"ornith:9b\","
             "\"max\":128,\"system\":\"be terse\"}", &r), 200, "a full ask is 200");
    CKI(r.kind, EOS_HTTPD_BODY_STREAM, "and is staged as a stream");
    CKS(r.content_type, "text/plain; charset=utf-8", "the reply is plain text");
    CKS(r.cache_control, "no-store", "and is never cached");
    CK(r.body == NULL && r.body_len == 0,
       "a stream stages no body - there is nothing to send yet");
    CKI(FB.asks, 1, "the ask reached the client exactly once");
    CKS(FB.q, "what is a tiling wm", "the question arrived verbatim");
    CKS(FB.model, "ornith:9b", "the model arrived");
    CKS(FB.system, "be terse", "the system prompt arrived");
    CKI(FB.max, 128, "max arrived");

    // Absent optional fields are empty and zero, NEVER invented here: the
    // defaults live with the settings, and a handler that guessed one would be
    // a second place brain.model is decided.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    CKI(call("POST", "/api/brain/ask", "{\"q\":\"hi\"}", &r), 200, "q alone is enough");
    CKS(FB.model, "", "an absent model is empty, not guessed");
    CKS(FB.system, "", "an absent system prompt is empty, not guessed");
    CKI(FB.max, 0, "an absent max is zero, not 256");

    // max is clamped rather than refused. It is a number input on a settings
    // page, and a 400 on a typo in the field that is not the question is a
    // worse answer than the nearest legal one.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    CKI(call("POST", "/api/brain/ask", "{\"q\":\"hi\",\"max\":1}", &r), 200, "a tiny max still runs");
    CKI(FB.max, 16, "and is clamped up to the floor");
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    CKI(call("POST", "/api/brain/ask", "{\"q\":\"hi\",\"max\":999999}", &r), 200, "a huge max still runs");
    CKI(FB.max, 2048, "and is clamped down to the ceiling");
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    CKI(call("POST", "/api/brain/ask", "{\"q\":\"hi\",\"max\":-9}", &r), 200, "a negative max still runs");
    CKI(FB.max, 16, "and clamps up, never through zero");

    // The refusals.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    CKI(callj("POST", "/api/brain/ask", NULL, &r), 400, "no body is 400");
    CK(has_kv(r.body, "error", "\"bad_argument\""), "and says bad_argument");
    CKI(callj("POST", "/api/brain/ask", "{}", &r), 400, "an empty object is 400");
    CKI(callj("POST", "/api/brain/ask", "{\"q\":\"\"}", &r), 400, "an empty question is 400");
    CKI(callj("POST", "/api/brain/ask", "{\"q\":42}", &r), 400, "a question that is not a string is 400");
    CKI(callj("POST", "/api/brain/ask", "not json", &r), 400, "a body that is not JSON is 400");
    CKI(callj("POST", "/api/brain/ask", "[\"q\"]", &r), 400, "an array body is 400");
    CKI(FB.asks, 0, "not one of those reached the client");

    // The prompt ceiling is EOS_BRAIN_PROMPT_MAX with the NUL, so 383 bytes fit
    // and 384 do not. A truncated prompt is a different question.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    for (i = 0; i < EOS_HTTPD_ASK_MAX - 1; i++) q[i] = 'a';
    q[EOS_HTTPD_ASK_MAX - 1] = 0;
    snprintf(body, sizeof body, "{\"q\":\"%s\"}", q);
    CKI(call("POST", "/api/brain/ask", body, &r), 200, "a 383-byte question fits");
    CKI((int)strlen(FB.q), EOS_HTTPD_ASK_MAX - 1, "and arrives whole");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    q[EOS_HTTPD_ASK_MAX - 1] = 'a';
    q[EOS_HTTPD_ASK_MAX] = 0;
    snprintf(body, sizeof body, "{\"q\":\"%s\"}", q);
    CKI(callj("POST", "/api/brain/ask", body, &r), 413, "one byte more is 413");
    CK(has_kv(r.body, "error", "\"too_big\""), "and says too_big, never truncates");
    CKI(FB.asks, 0, "and never reaches the client");

    // The same rule for the two optional strings.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    for (i = 0; i < EOS_HTTPD_MODEL_MAX; i++) q[i] = 'm';
    q[EOS_HTTPD_MODEL_MAX] = 0;
    snprintf(body, sizeof body, "{\"q\":\"hi\",\"model\":\"%s\"}", q);
    CKI(callj("POST", "/api/brain/ask", body, &r), 413, "an oversize model name is 413");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    for (i = 0; i < EOS_HTTPD_SYSTEM_MAX; i++) q[i] = 's';
    q[EOS_HTTPD_SYSTEM_MAX] = 0;
    snprintf(body, sizeof body, "{\"q\":\"hi\",\"system\":\"%s\"}", q);
    CKI(callj("POST", "/api/brain/ask", body, &r), 413, "an oversize system prompt is 413");

    // A body the transport refused before reading it. This is the path a
    // prompt over EOS_HTTPD_BODY_MAX takes, and it must not be read as "no q".
    {
        eos_httpd_req_t rq;
        fake_reset();
        srv_init(EOS_HTTPD_MODE_RUN);
        memset(&rq, 0, sizeof rq);
        rq.method = "POST";
        rq.uri = "/api/brain/ask";
        rq.body_truncated = true;
        CKI(eos_httpd_dispatch(&SRV.h, &rq, &r), 413, "a refused body is 413, not 400");
        CK(has_kv(r.body, "error", "\"too_big\""), "and says too_big");
    }

    // eos_brain holds one request. web/README.md: a second ask is 409 busy.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    FB.ask_rc = -8;
    CKI(callj("POST", "/api/brain/ask", "{\"q\":\"hi\"}", &r), 409, "a second ask is 409");
    CK(has_kv(r.body, "error", "\"busy\""), "and says busy");
    CKI(r.kind, EOS_HTTPD_BODY_BUF, "a refused ask is a document, not a stream");

    // Anything else the client refuses with maps through the same table.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    FB.ask_rc = -1;
    CKI(callj("POST", "/api/brain/ask", "{\"q\":\"hi\"}", &r), 400, "a refused argument is 400");
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    FB.ask_rc = -9;
    CKI(callj("POST", "/api/brain/ask", "{\"q\":\"hi\"}", &r), 413, "a refused length is 413");

    // Non-ASCII survives the round trip: the whole parser exists because the
    // reply is UTF-8, and the question is too.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    CKI(call("POST", "/api/brain/ask",
             "{\"q\":\"\\u00e9t\\u00e9 \\u4f60\\u597d\"}", &r), 200, "an escaped question is 200");
    CKS(FB.q, "\xc3\xa9t\xc3\xa9 \xe4\xbd\xa0\xe5\xa5\xbd", "and is decoded to UTF-8");
    CK(utf8_valid(FB.q, (int)strlen(FB.q)), "which is well formed");
}

static void test_brain_stream_and_cancel(void)
{
    eos_httpd_resp_t r;
    char drained[128];
    int n, total = 0, waits = 0, ended = 0;

    printf("  handlers: the stream contract and POST /api/brain/cancel\n");

    // The invariant the lock release rests on: staging a stream does not write
    // the response buffer, so the buffer is not shared with the worker that
    // drains it. Checked by sentinel rather than by reading the code.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    memset(SRV.h.resp, '#', sizeof SRV.h.resp);
    CKI(call("POST", "/api/brain/ask", "{\"q\":\"hi\"}", &r), 200, "the ask is staged");
    {
        size_t k, untouched = 1;
        for (k = 0; k < sizeof SRV.h.resp; k++) if (SRV.h.resp[k] != '#') { untouched = 0; break; }
        CK(untouched, "a staged stream never writes the shared response buffer");
    }

    // And the server answers the next request immediately, which is what a
    // worker holding a stream must not prevent.
    CKI(callj("GET", "/api/net/status", NULL, &r), 200, "another request still works mid-stream");

    // The drain a worker performs: WAIT while the model thinks, then bytes,
    // then END. Nothing here can block.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    FB.reply      = "hello from the mini";
    FB.wait_first = 3;
    FB.per_read   = 5;
    CKI(call("POST", "/api/brain/ask", "{\"q\":\"hi\"}", &r), 200, "the ask is staged");
    for (n = 0; n < 64; n++) {
        int got = SRV.h.ports.brain_read(SRV.h.ctx, drained + total,
                                         (int)sizeof drained - total);
        if (got == EOS_HTTPD_STREAM_WAIT) { waits++; continue; }
        if (got == EOS_HTTPD_STREAM_END)  { ended = 1; break; }
        if (got < 0) break;
        total += got;
    }
    CKI(waits, 3, "the worker sees WAIT while the model is thinking");
    CK(ended, "and END when the reply finishes");
    CKI(total, (int)strlen("hello from the mini"), "every byte of the reply came through");
    CK(memcmp(drained, "hello from the mini", (size_t)total) == 0, "in order and unaltered");

    // The failure path. A stream that has already sent a 200 has no status code
    // left, so the sentence the "! " line carries comes from brain_status.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    FB.reply      = "half an ans";
    FB.per_read   = 64;
    FB.end_with   = EOS_HTTPD_STREAM_FAIL;
    FB.st.last_error = "megabrain: EOS_BRAIN_ERR_TRUNCATED";
    CKI(call("POST", "/api/brain/ask", "{\"q\":\"hi\"}", &r), 200, "the ask is staged");
    n = SRV.h.ports.brain_read(SRV.h.ctx, drained, (int)sizeof drained);
    CKI(n, 11, "the text that did arrive is delivered first");
    CKI(SRV.h.ports.brain_read(SRV.h.ctx, drained, (int)sizeof drained),
        EOS_HTTPD_STREAM_FAIL, "and only then does it fail");
    {
        eos_httpd_brain_t st;
        memset(&st, 0, sizeof st);
        CK(SRV.h.ports.brain_status(SRV.h.ctx, &st), "status answers after a failure");
        CK(st.last_error && strstr(st.last_error, "TRUNCATED") != NULL,
           "and carries the sentence the \"! \" line needs");
    }

    // Cancel. Never an error: a stop pressed a moment after the reply landed is
    // the normal case, and a 409 there would show a failure for something that
    // worked.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    CKI(callj("POST", "/api/brain/cancel", NULL, &r), 200, "cancelling nothing is 200");
    CK(has_kv(r.body, "cancelled", "false"), "and says it cancelled nothing");
    CKI(FB.cancels, 1, "the client was still asked");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    FB.cancel_had = true;
    CKI(callj("POST", "/api/brain/cancel", NULL, &r), 200, "cancelling a live request is 200");
    CK(has_kv(r.body, "cancelled", "true"), "and says so");
    CKS(r.content_type, "application/json; charset=utf-8", "cancel is JSON");

    // A body on cancel is ignored rather than refused - the web app sends none,
    // and a fetch that adds one must not be a failure the user sees.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_RUN);
    CKI(callj("POST", "/api/brain/cancel", "{\"anything\":1}", &r), 200, "a body on cancel is ignored");

    CK(srv_intact(), "the brain handlers stayed inside the server struct");
}

static void test_brain_routes(void)
{
    eos_httpd_resp_t r;

    printf("  routes: /api/brain/*\n");

    CKI(eos_httpd_route("GET",  "/api/brain/status"), EOS_ROUTE_BRAIN_STATUS, "GET status");
    CKI(eos_httpd_route("POST", "/api/brain/ask"),    EOS_ROUTE_BRAIN_ASK,    "POST ask");
    CKI(eos_httpd_route("POST", "/api/brain/cancel"), EOS_ROUTE_BRAIN_CANCEL, "POST cancel");

    // The verb table is exactly the three above. web/README.md: the board
    // answers GET and POST and nothing else, and each path takes only one.
    CKI(eos_httpd_route("POST", "/api/brain/status"), EOS_ROUTE_METHOD, "POST status is 405");
    CKI(eos_httpd_route("GET",  "/api/brain/ask"),    EOS_ROUTE_METHOD, "GET ask is 405");
    CKI(eos_httpd_route("GET",  "/api/brain/cancel"), EOS_ROUTE_METHOD, "GET cancel is 405");
    CKI(eos_httpd_route("DELETE", "/api/brain/cancel"), EOS_ROUTE_METHOD, "DELETE is 405");

    // A typo under /api/brain/ is a 404 and never a file: falling through to
    // the document root would turn a mistyped endpoint into a path surface.
    CKI(eos_httpd_route("GET", "/api/brain"),         EOS_ROUTE_NONE, "the bare prefix is 404");
    CKI(eos_httpd_route("GET", "/api/brain/asks"),    EOS_ROUTE_NONE, "a typo is 404");
    CKI(eos_httpd_route("POST", "/api/brain/ask/x"),  EOS_ROUTE_NONE, "and never STATIC");

    // The query string is not part of the path.
    CKI(eos_httpd_route("GET", "/api/brain/status?x=1"), EOS_ROUTE_BRAIN_STATUS,
        "a query string does not change the route");

    // SETUP has no megabrain, but the endpoints must still answer as endpoints:
    // a portal redirect on /api/ would be JSON the setup page cannot parse.
    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);
    CKI(callj("GET", "/api/brain/status", NULL, &r), 200, "status answers in SETUP too");
    CKI(r.kind, EOS_HTTPD_BODY_BUF, "and is a document, not a 302 to the portal");
    CKI(callj("GET", "/api/brain/asks", NULL, &r), 404, "a typo in SETUP is still 404");
    CK(has_kv(r.body, "error", "\"not_found\""), "and JSON, not the portal");
}

static void test_the_walkthrough(void)
{
    eos_httpd_resp_t r;

    printf("  walkthrough: power on, scan, join, pair\n");

    fake_reset();
    srv_init(EOS_HTTPD_MODE_SETUP);

    // The phone joins the AP and its OS probes for internet.
    CKI(call("GET", "/generate_204", NULL, &r), 302, "1. the probe pops the portal");

    // The portal loads the setup page.
    CKI(call("GET", "/", NULL, &r), 200, "2. the portal serves the setup page");

    // The page reads the cached scan taken when SETUP started.
    ap_set(0, "WavvyWorld", 10, -47, 6, EOS_HTTPD_AUTH_WPA2);
    FK.wifi_n = 1;
    CKI(callj("GET", "/api/wifi/scan", NULL, &r), 200, "3. the cached scan is there immediately");
    CK(strstr(r.body, "WavvyWorld") != NULL, "3. the network is in the list");

    // The user types the wrong password.
    CKI(callj("POST", "/api/wifi/connect",
              "{\"ssid\":\"WavvyWorld\",\"psk\":\"wrongpass\"}", &r), 202,
        "4. the first attempt is accepted");
    FK.net.state = EOS_HTTPD_NET_SETUP;
    FK.net.join  = EOS_HTTPD_JOIN_AUTH;
    CKI(callj("GET", "/api/net/status", NULL, &r), 200, "5. the outcome is readable");
    CK(has_kv(r.body, "reason", "\"bad_auth\""), "5. the password was refused");
    CK(strstr(r.body, "nothing was saved") != NULL,
       "5. and the board says nothing was written to flash");
    CK(has_kv(r.body, "mode", "\"setup\""), "5. the board is still in setup mode");
    CK(strstr(r.body, "\"stored\":false") != NULL,
       "5. no credentials are stored after a failed join");

    // Second try, right password.
    CKI(callj("POST", "/api/wifi/connect",
              "{\"ssid\":\"WavvyWorld\",\"psk\":\"rightpass\"}", &r), 202,
        "6. the second attempt is accepted");
    CKI(FK.joins, 2, "6. exactly two joins were attempted");
    CKS(FK.join_psk, "rightpass", "6. the second password reached the radio");

    FK.net.state = EOS_HTTPD_NET_UP;
    FK.net.join  = EOS_HTTPD_JOIN_OK;
    FK.net.ssid_stored = true;
    memcpy(FK.net.ssid, "WavvyWorld", 10);
    FK.net.ssid_len = 10;
    snprintf(FK.net.ip, sizeof FK.net.ip, "192.168.0.51");
    CKI(callj("GET", "/api/net/status", NULL, &r), 200, "7. the board is up");
    CK(strstr(r.body, "\"state\":\"ok\"") != NULL, "7. the join succeeded");
    CK(strstr(r.body, "\"stored\":true") != NULL, "7. and only now are credentials stored");
    CK(strstr(r.body, "192.168.0.51") != NULL, "7. the board reports its address");

    // Then the keyboard.
    memcpy(FK.devs[0].name, "K809", 4);
    FK.devs[0].name_len = 4;
    FK.devs[0].is_hid = true;
    FK.devs[0].rssi = -40;
    snprintf(FK.devs[0].addr, sizeof FK.devs[0].addr, "de:ad:be:ef:00:01");
    FK.ble_n = 1;
    CKI(callj("GET", "/api/ble/scan?rescan=1", NULL, &r), 202, "8. the BLE scan starts");
    FK.ble_state = EOS_HTTPD_SCAN_DONE;
    CKI(callj("GET", "/api/ble/scan", NULL, &r), 200, "9. the keyboard is found");
    CK(strstr(r.body, "K809") != NULL, "9. by name");

    CKI(callj("POST", "/api/ble/pair", "{\"addr\":\"de:ad:be:ef:00:01\"}", &r), 202,
        "10. pairing starts");
    CK(strstr(r.body, "one board at a time") != NULL, "10. with the warning attached");

    FK.ble.pairing = true;
    FK.ble.passkey_shown = true;
    FK.ble.passkey = 42;
    CKI(callj("GET", "/api/ble/status", NULL, &r), 200, "11. the passkey is readable");
    CK(strstr(r.body, "\"passkey\":\"000042\"") != NULL, "11. as six digits");

    FK.ble.pairing = false;
    FK.ble.bonded = true;
    FK.ble.connected = true;
    FK.ble.passkey_shown = false;
    FK.ble.state = EOS_HTTPD_BLE_READY;
    CKI(callj("GET", "/api/ble/status", NULL, &r), 200, "12. the keyboard is bonded");
    CK(strstr(r.body, "\"bonded\":{") != NULL, "12. and stays bonded");

    CK(srv_intact(), "the walkthrough never wrote outside the server struct");
}


// ==========================================================================
// Every endpoint in web/README.md, in one table
// ==========================================================================
//
// Three agents added routes to this file's ROUTES[] in parallel and each
// guessed at the others'. This is the reconciliation, written down: the
// complete list the web app calls, checked against the real route scan and
// then driven through the real dispatch on a server with NOTHING bound.
//
// Two properties, and the second is the one that matters. Every path must
// RESOLVE — a 404 here means the page gets "not found" for an endpoint the
// firmware believes it implements, which is the failure the whole run was
// about. And an unwired endpoint must answer 501 UNSUPPORTED, not 404 and not
// 500: web/README.md's error table says unsupported means "valid call, not
// available on this tier", and that is the honest thing for a board whose
// ports were never assigned to say.

static const struct { const char *method, *uri; } CONTRACT[] = {
    // Setup mode, and the one endpoint it shares with the rest of the app.
    { "GET",  "/api/net/status" },
    { "GET",  "/api/wifi/scan" },
    { "POST", "/api/wifi/connect" },
    { "POST", "/api/wifi/forget" },
    { "GET",  "/api/ble/scan" },
    { "POST", "/api/ble/pair" },
    { "GET",  "/api/ble/status" },
    { "POST", "/api/ble/forget" },
    // System
    { "GET",  "/api/system" },
    { "GET",  "/api/system/health" },
    { "POST", "/api/system/reboot" },
    // Files
    { "GET",  "/api/fs/list?path=%2Fint" },
    { "GET",  "/api/fs/stat?path=%2Fint%2Fa" },
    { "GET",  "/api/fs/read?path=%2Fint%2Fa" },
    { "GET",  "/api/fs/usage?point=%2Fint" },
    { "POST", "/api/fs/write?path=%2Fint%2Fa&offset=0&final=1" },
    { "POST", "/api/fs/upload/abort?path=%2Fint%2Fa" },
    { "POST", "/api/fs/mkdir?path=%2Fint%2Fd" },
    { "POST", "/api/fs/remove?path=%2Fint%2Fa" },
    { "POST", "/api/fs/rename?from=%2Fint%2Fa&to=%2Fint%2Fb" },
    // Settings, themes, apps
    { "GET",  "/api/settings" },
    { "POST", "/api/settings" },
    { "GET",  "/api/themes" },
    { "GET",  "/api/apps" },
    // Buddy, and the gallery
    { "GET",  "/api/buddy" },
    { "POST", "/api/buddy/reload" },
    { "GET",  "/api/buddy/gallery" },
    { "POST", "/api/buddy/gallery/select" },
    { "POST", "/api/buddy/gallery/remove" },
    // Console
    { "GET",  "/api/console/log" },
    { "POST", "/api/console/exec" },
    // Megabrain
    { "GET",  "/api/brain/status" },
    { "POST", "/api/brain/ask" },
    { "POST", "/api/brain/cancel" },
};

static void test_every_endpoint(void)
{
    eos_httpd_cfg_t cfg;
    eos_httpd_resp_t r;
    char path[96];
    int i, n = (int)(sizeof CONTRACT / sizeof CONTRACT[0]);
    int resolved = 0, unsupported = 0;

    printf("  the contract: all %d endpoints resolve and answer honestly\n", n);

    // 1. The route scan. The path is taken without its query string, the way
    //    eos_httpd_route() is called for real.
    for (i = 0; i < n; i++) {
        const char *q = strchr(CONTRACT[i].uri, '?');
        size_t len = q ? (size_t)(q - CONTRACT[i].uri) : strlen(CONTRACT[i].uri);
        eos_route_t rt;
        if (len >= sizeof path) len = sizeof path - 1;
        memcpy(path, CONTRACT[i].uri, len);
        path[len] = 0;
        rt = eos_httpd_route(CONTRACT[i].method, path);
        if (rt != EOS_ROUTE_NONE && rt != EOS_ROUTE_METHOD) resolved++;
        else printf("    %s %s does not resolve\n", CONTRACT[i].method, path);
    }
    CKI(resolved, n, "every endpoint in web/README.md is in the route table");

    // 2. Dispatch, with no ports bound at all.
    memset(&SRV.h, 0, sizeof SRV.h);
    memset(SRV.pre, 0x5A, GUARD);
    memset(SRV.post, 0xA5, GUARD);
    eos_httpd_cfg_default(&cfg);
    cfg.mode = EOS_HTTPD_MODE_RUN;
    eos_httpd_init(&SRV.h, NULL, NULL, &cfg);
    eos_httpd_set_api(NULL);

    for (i = 0; i < n; i++) {
        const char *body = CONTRACT[i].method[0] == 'P' ? "{}" : NULL;
        callj(CONTRACT[i].method, CONTRACT[i].uri, body, &r);
        if (r.status == 501) unsupported++;
        else printf("    %s %s answered %d, not 501\n",
                    CONTRACT[i].method, CONTRACT[i].uri, r.status);
    }
    CKI(unsupported, n, "an unwired board answers 501 to all of them, never 404");
    CK(srv_intact(), "and never wrote outside the server struct doing it");

    // The two ends of the table that are NOT in the contract and must stay
    // where they are: an unknown /api path is a 404, and the wrong method on a
    // path that exists is a 405. /api/settings is the only path in the whole
    // contract with two methods, which is why the scan keeps looking after a
    // path match instead of settling for the first row.
    CKI(eos_httpd_route("GET", "/api/nope"), EOS_ROUTE_NONE, "an unknown api path is no route");
    CKI(eos_httpd_route("DELETE", "/api/settings"), EOS_ROUTE_METHOD, "a wrong method is 405");
    CK(eos_httpd_route("GET", "/api/settings") == EOS_ROUTE_SETTINGS_GET,
       "and /api/settings still finds its GET");
    CK(eos_httpd_route("POST", "/api/settings") == EOS_ROUTE_SETTINGS_SET,
       "and its POST, which is the row a first-match scan would have lost");
}

int main(void)
{
    printf("\n=== eos_httpd ===\n");

    test_json_shapes();
    test_json_escaping();
    test_json_overflow();
    test_json_read_basics();
    test_json_read_hostile();
    test_uri();
    test_routes();
    test_mime();
    test_wifi_scan();
    test_wifi_connect();
    test_wifi_forget_and_status();
    test_ble();
    test_static();
    test_errors_and_counters();
    test_web_contract();
    test_brain_routes();
    test_brain_status();
    test_brain_ask();
    test_brain_stream_and_cancel();
    test_the_walkthrough();
    test_every_endpoint();

    printf("\n=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
