// Host test for eos_settings and the four endpoints built on it. No radio, no
// panel, no flash — but a real filesystem: it links the real eos_storage
// backend against a sandbox under /tmp, so "the settings file is truncated"
// means a truncated file and not a string a test handed to a parser.
//
// Most of this is an attack on the load path, because that is the one that runs
// on every boot and the one whose failure mode is a board that will not come
// up. Truncated, empty, not an object, nested, trailing junk, every key at the
// wrong type, values far out of range, a value one byte too long, and a file
// full of random bytes — each of them has to leave a usable store behind.
//
// The other half is the rule that WiFi credentials are not in this file. It is
// checked twice and from both directions: the serialised document never
// contains them however they were set, and setting them calls the port that
// hands them to eos_net instead.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "eos_settings.h"
#include "eos_httpd.h"
#include "eos_storage.h"
#include "waveshare-c6-lcd-13.h"

// The suite links a real board so the real storage backend can route "/int" and
// declare "/sd" absent, exactly as it does on the bench.
const eos_board_t *eos_board_get(void) { return &EOS_BOARD; }

static int checks = 0, failed = 0;

#define CK(cond, msg) do { \
    checks++; \
    if (!(cond)) { failed++; printf("    FAIL: %s\n", msg); } \
} while (0)

#define CKS(got, want, msg) do { \
    checks++; \
    if (strcmp((got), (want)) != 0) { \
        failed++; \
        printf("    FAIL: %s\n      got  [%s]\n      want [%s]\n", msg, got, want); \
    } \
} while (0)

#define CKI(got, want, msg) do { \
    checks++; \
    if ((long)(got) != (long)(want)) { \
        failed++; \
        printf("    FAIL: %s (got %ld, want %ld)\n", msg, (long)(got), (long)(want)); \
    } \
} while (0)

#define HAS(hay, needle, msg) CK(strstr((hay), (needle)) != NULL, msg)
#define HASNT(hay, needle, msg) CK(strstr((hay), (needle)) == NULL, msg)

// The document length is always the literal's own length. Passing a hand-counted
// one is how a test ends up asserting that the parser rejects the bytes after it.
#define PARSE(lit) eos_settings_parse(&s, (lit), (int)strlen(lit), &bad)

// ---------------------------------------------------------------- sandbox

static char ROOT[128];

void eos_storage_host_reset(void);

static void rmtree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;
    char p[512];

    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", path, de->d_name);
        rmtree(p);
        unlink(p);
        rmdir(p);
    }
    closedir(d);
    rmdir(path);
}

static void sandbox_open(void)
{
    snprintf(ROOT, sizeof ROOT, "/tmp/eos-settings-test-%d", (int)getpid());
    rmtree(ROOT);
    setenv("EOS_STORAGE_HOST_ROOT", ROOT, 1);
    eos_storage_host_reset();
    CK(eos_storage_init() == EOS_OK, "the sandbox filesystem mounts");
}

// Writes raw bytes straight to the settings file, going around eos_storage so
// the test can produce things eos_settings_print() never would.
static void put_file(const void *bytes, int n)
{
    char p[256];
    FILE *f;
    snprintf(p, sizeof p, "%s/settings.json", ROOT);
    f = fopen(p, "wb");
    if (!f) { failed++; printf("    FAIL: cannot write %s\n", p); return; }
    if (n > 0) fwrite(bytes, 1, (size_t)n, f);
    fclose(f);
}

static void put_text(const char *s) { put_file(s, (int)strlen(s)); }

static void drop_file(void)
{
    char p[256];
    snprintf(p, sizeof p, "%s/settings.json", ROOT);
    unlink(p);
}

// ------------------------------------------------------------- fake ports

static struct {
    char ssid[64];
    char psk[80];
    bool ssid_null, psk_null;
    int  calls;
    int  ret;

    char cur_ssid[64];
    bool psk_stored;

    int  applied[EOS_SET_COUNT];
    eos_err_t apply_ret;
    int  apply_ret_key;      // only that key gets apply_ret; the rest get OK
} F;

static int f_wifi_set(void *ctx, const char *ssid, const char *psk)
{
    (void)ctx;
    F.calls++;
    F.ssid_null = (ssid == NULL);
    F.psk_null  = (psk  == NULL);
    snprintf(F.ssid, sizeof F.ssid, "%s", ssid ? ssid : "");
    snprintf(F.psk,  sizeof F.psk,  "%s", psk  ? psk  : "");
    return F.ret;
}

static bool f_wifi_ssid(void *ctx, char *out, int cap)
{
    (void)ctx;
    snprintf(out, (size_t)cap, "%s", F.cur_ssid);
    return true;
}

static bool f_wifi_psk_set(void *ctx) { (void)ctx; return F.psk_stored; }

static eos_err_t f_apply(void *ctx, int key, const eos_settings_t *s)
{
    (void)ctx; (void)s;
    if (key >= 0 && key < EOS_SET_COUNT) F.applied[key]++;
    if (key == F.apply_ret_key) return F.apply_ret;
    return EOS_OK;
}

static void store_open(eos_settings_store_t *st)
{
    eos_settings_ports_t p;
    memset(&F, 0, sizeof F);
    F.apply_ret_key = -1;
    snprintf(F.cur_ssid, sizeof F.cur_ssid, "WavvyWorld");

    memset(&p, 0, sizeof p);
    p.wifi_set     = f_wifi_set;
    p.wifi_ssid    = f_wifi_ssid;
    p.wifi_psk_set = f_wifi_psk_set;
    p.apply        = f_apply;
    eos_settings_init(st, &p);
}

// ==========================================================================
// The key table
// ==========================================================================

static void t_keys(void)
{
    int i;

    printf("  keys: the names, their types and their flags\n");

    /* Bumped from 13 when cam.host was added for the camera node. This check
       is deliberately brittle: the enum and the key TABLE must stay in step,
       and a mismatch between them silently maps one setting onto another's
       storage. A count that has to be updated on purpose is how that stays
       impossible to do by accident. */
    CKI(EOS_SET_COUNT, 14, "fourteen key slots");
    CKS(eos_settings_key_name(EOS_SET_WIFI_SSID),    "wifi.ssid",     "wifi.ssid");
    CKS(eos_settings_key_name(EOS_SET_WIFI_PSK_SET), "wifi.psk_set",  "wifi.psk_set");
    CKS(eos_settings_key_name(EOS_SET_SYS_AUTOSTART),"sys.autostart", "sys.autostart");
    CK(eos_settings_key_name(-1) == NULL,           "a negative key has no name");
    CK(eos_settings_key_name(EOS_SET_COUNT) == NULL,"one past the end has no name");

    CKI(eos_settings_key_of("ui.bright"), EOS_SET_UI_BRIGHT, "lookup by name");
    CKI(eos_settings_key_of("ui.brightness"), -1, "a near miss is not a match");
    CKI(eos_settings_key_of(""), -1, "the empty name is not a key");
    CKI(eos_settings_key_of(NULL), -1, "NULL is not a key");

    // web/README.md: "Every key is <= 15 bytes. Do not add a longer one - it
    // will not fit an NVS key and the failure appears at write time."
    for (i = 0; i < EOS_SET_COUNT; i++) {
        const char *n = eos_settings_key_name(i);
        CK(n && strlen(n) <= 15, "every key fits an NVS key");
    }

    CK(eos_settings_key_reboot(EOS_SET_WIFI_SSID),     "wifi.ssid is reboot-marked");
    CK(eos_settings_key_reboot(EOS_SET_NET_HOST),      "net.host is reboot-marked");
    CK(eos_settings_key_reboot(EOS_SET_SYS_AUTOSTART), "sys.autostart is reboot-marked");
    CK(!eos_settings_key_reboot(EOS_SET_UI_BRIGHT),    "ui.bright is live");
    CK(!eos_settings_key_reboot(EOS_SET_BRAIN_HOST),   "brain.host is live");

    CKI(eos_settings_key_type(EOS_SET_BRAIN_PORT),   EOS_SET_T_INT,  "brain.port is a number");
    CKI(eos_settings_key_type(EOS_SET_WIFI_PSK_SET), EOS_SET_T_BOOL, "wifi.psk_set is a bool");
    CKI(eos_settings_key_type(EOS_SET_SYS_TZ),       EOS_SET_T_STR,  "sys.tz is a string");

    CK((eos_settings_key_flags(EOS_SET_WIFI_SSID) & EOS_SET_F_ROUTED),
       "wifi.ssid routes to eos_net");
    CK(!(eos_settings_key_flags(EOS_SET_WIFI_SSID) & EOS_SET_F_PERSIST),
       "and is not in the file");
    CK(!(eos_settings_key_flags(EOS_SET_WIFI_PSK) & EOS_SET_F_PERSIST),
       "neither is wifi.psk");
    CK((eos_settings_key_flags(EOS_SET_WIFI_PSK) & EOS_SET_F_WRITEONLY),
       "wifi.psk is write-only");
    CK((eos_settings_key_flags(EOS_SET_WIFI_PSK_SET) & EOS_SET_F_READONLY),
       "wifi.psk_set is read-only");
}

// ==========================================================================
// Defaults and the round trip
// ==========================================================================

static void fill(eos_settings_t *s)
{
    snprintf(s->net_host,      sizeof s->net_host,      "%s", "pip");
    snprintf(s->brain_host,    sizeof s->brain_host,    "%s", "192.168.0.139");
    snprintf(s->brain_model,   sizeof s->brain_model,   "%s", "gemma4:12b-it-qat");
    snprintf(s->brain_system,  sizeof s->brain_system,  "%s",
             "terse, dry, helpful.\n\"never\" uses \\ or exclamation marks.");
    snprintf(s->ui_theme,      sizeof s->ui_theme,      "%s", "gruvbox");
    snprintf(s->sys_tz,        sizeof s->sys_tz,        "%s", "PST8PDT,M3.2.0,M11.1.0");
    snprintf(s->sys_autostart, sizeof s->sys_autostart, "%s", "term");
    s->brain_port = 8080;
    s->brain_max  = 1024;
    s->ui_bright  = 137;
}

static bool same(const eos_settings_t *a, const eos_settings_t *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static void t_defaults(void)
{
    eos_settings_t d;

    printf("  defaults: what an unconfigured board wears\n");

    eos_settings_defaults(&d);
    CKS(d.ui_theme, EOS_SETTINGS_THEME_DEFAULT, "the default theme is the embedded one");
    CKI(d.brain_port, 80,  "megabrain is on port 80");
    CKI(d.brain_max,  256, "and 256 tokens");
    CKI(d.ui_bright,  255, "the backlight starts full");
    CKS(d.net_host,      "", "net.host empty means derive it from the MAC");
    CKS(d.brain_host,    "", "brain.host empty means discover it");
    CKS(d.sys_tz,        "", "sys.tz empty means UTC");
    CKS(d.sys_autostart, "", "nothing autostarts");
    eos_settings_defaults(NULL);   // must not crash
    checks++;
}

static void t_roundtrip(void)
{
    eos_settings_t a, b;
    char doc[EOS_SETTINGS_DOC_MAX];
    uint16_t bad = 0xFFFF;
    int n;

    printf("  round trip: print then parse is the identity\n");

    eos_settings_defaults(&a);
    fill(&a);

    n = eos_settings_print(&a, doc, (int)sizeof doc);
    CK(n > 0, "a full store serialises");
    CKI((long)strlen(doc), n, "and the length is the string length");
    CK(doc[0] == '{' && doc[n - 1] == '}', "into one flat object");

    CKI(eos_settings_parse(&b, doc, n, &bad), EOS_SETTINGS_OK, "and parses back cleanly");
    CKI(bad, 0, "with no unusable keys");
    CK(same(&a, &b), "every field survives the round trip");

    // The escapes in the system prompt are the interesting half: a newline, a
    // quote and a backslash all have to come back as themselves.
    CKS(b.brain_system, a.brain_system, "escapes round trip byte for byte");

    // Defaults round trip too, which is the case that actually runs on a board
    // that has never been configured.
    eos_settings_defaults(&a);
    n = eos_settings_print(&a, doc, (int)sizeof doc);
    CK(n > 0, "so does a store of pure defaults");
    CKI(eos_settings_parse(&b, doc, n, &bad), EOS_SETTINGS_OK, "  and it parses");
    CK(same(&a, &b), "  identically");

    // A buffer one byte short is refused, never truncated.
    CKI(eos_settings_print(&a, doc, n), (int)EOS_ERR_TOOBIG,
        "a document that does not fit is refused, not truncated");
    CKI(eos_settings_print(&a, doc, 0), (int)EOS_ERR_ARG, "and a zero buffer is an argument error");
    CKI(eos_settings_print(NULL, doc, (int)sizeof doc), (int)EOS_ERR_ARG, "as is no store");
}

// ==========================================================================
// Corrupt input
// ==========================================================================

static void t_corrupt(void)
{
    eos_settings_t d, s;
    char doc[EOS_SETTINGS_DOC_MAX];
    char trunc[EOS_SETTINGS_DOC_MAX];
    uint16_t bad;
    int n, i;

    printf("  corrupt: every bad file leaves a usable store\n");

    eos_settings_defaults(&d);

    CKI(eos_settings_parse(&s, NULL, 0, &bad), EOS_SETTINGS_ERR_EMPTY, "no buffer is empty");
    CK(same(&s, &d), "  and leaves defaults");
    CKI(PARSE(""), EOS_SETTINGS_ERR_EMPTY, "a zero-byte file is empty");
    CK(same(&s, &d), "  and leaves defaults");
    CKI(PARSE("  \n\t\r "), EOS_SETTINGS_ERR_EMPTY,
        "whitespace only is empty");
    CK(same(&s, &d), "  and leaves defaults");

    CKI(PARSE("not json at all"), EOS_SETTINGS_ERR_SYNTAX,
        "prose is a syntax error");
    CK(same(&s, &d), "  and leaves defaults");
    CKI(PARSE("[1,2,3]"), EOS_SETTINGS_ERR_SYNTAX,
        "an array is not a settings document");
    CK(same(&s, &d), "  and leaves defaults");
    CKI(PARSE("{\"ui.bright\":200}x"), EOS_SETTINGS_ERR_SYNTAX,
        "trailing junk is a syntax error");
    CK(same(&s, &d), "  and leaves defaults");

    // Truncation at every length. This is the power-cut-mid-write case and the
    // one that must never leave a half-applied store.
    eos_settings_defaults(&s);
    fill(&s);
    n = eos_settings_print(&s, doc, (int)sizeof doc);
    for (i = 1; i < n; i++) {
        eos_settings_t t;
        memcpy(trunc, doc, (size_t)i);
        if (eos_settings_parse(&t, trunc, i, &bad) == EOS_SETTINGS_OK) {
            // The one truncation that is legal is none of them: every prefix of
            // a complete object is missing its brace.
            failed++;
            printf("    FAIL: a %d-byte truncation of a %d-byte document parsed clean\n", i, n);
        }
        if (!same(&t, &d)) {
            failed++;
            printf("    FAIL: a %d-byte truncation left something other than defaults\n", i);
        }
    }
    checks += 2;
    CK(1, "every truncation of a full document falls back to defaults");

    // Garbage bytes, including embedded NULs and high bytes. The parser reads a
    // length and never a terminator, so a NUL in the middle is data.
    {
        unsigned char junk[256];
        for (i = 0; i < 256; i++) junk[i] = (unsigned char)((i * 37u + 11u) & 0xFF);
        CKI(eos_settings_parse(&s, (const char *)junk, 256, &bad), EOS_SETTINGS_ERR_SYNTAX,
            "256 bytes of garbage is a syntax error");
        CK(same(&s, &d), "  and leaves defaults");
    }
    {
        const char nul_doc[] = "{\"ui.theme\":\"a\0b\",\"ui.bright\":9}";
        // An escaped NUL is refused by the reader; a raw one inside a string is
        // a string byte. Either way the store has to survive it.
        eos_settings_parse(&s, nul_doc, (int)sizeof nul_doc - 1, &bad);
        CK(s.ui_bright <= 255, "a raw NUL inside a value cannot corrupt a neighbour");
    }

    // Every key at the wrong type, one document. Each one keeps its default and
    // the document as a whole still applies.
    {
        const char *wrong =
            "{\"net.host\":123,\"brain.host\":true,\"brain.port\":\"eighty\","
            "\"brain.model\":[1],\"brain.max\":null,\"brain.system\":{},"
            "\"ui.theme\":0,\"ui.bright\":\"bright\",\"sys.tz\":false,"
            "\"sys.autostart\":1.5}";
        CKI(eos_settings_parse(&s, wrong, (int)strlen(wrong), &bad),
            EOS_SETTINGS_ERR_FIELD, "ten wrong types is a field error, not a syntax error");
        CK(same(&s, &d), "  and every one of them kept its default");
        CK(bad != 0, "  with the bad keys reported");
        CK((bad & (1u << EOS_SET_BRAIN_PORT)) != 0, "  brain.port among them");
        CK((bad & (1u << EOS_SET_SYS_TZ)) != 0,     "  sys.tz too");
    }

    // One bad key in an otherwise good document: the rest still applies.
    CKI(PARSE("{\"ui.theme\":\"ember\",\"ui.bright\":12,\"brain.port\":\"nope\"}"),
        EOS_SETTINGS_ERR_FIELD, "one bad key is a field error");
    CKS(s.ui_theme, "ember", "  the good string still applied");
    CKI(s.ui_bright, 12,     "  the good number still applied");
    CKI(s.brain_port, 80,    "  the bad one kept its default");
    CKI(bad, 1u << EOS_SET_BRAIN_PORT, "  and only it is reported");

    // Missing keys are not an error at all: that is a file written by an older
    // firmware, and it has to keep working.
    CKI(PARSE("{\"ui.bright\":5}"), EOS_SETTINGS_OK,
        "a document with one key is fine");
    CKI(s.ui_bright, 5, "  it applies");
    CKS(s.ui_theme, EOS_SETTINGS_THEME_DEFAULT, "  and everything absent is default");
    CKI(bad, 0, "  with nothing reported bad");
    CKI(PARSE("{}"), EOS_SETTINGS_OK, "an empty object is fine");
    CK(same(&s, &d), "  and is exactly the defaults");

    // Unknown keys are ignored, which is the same rule eos_theme applies: a
    // file from a newer build must not brick an older one.
    CKI(PARSE("{\"ui.gamma\":3,\"ui.bright\":7}"),
        EOS_SETTINGS_OK, "an unknown key is ignored");
    CKI(s.ui_bright, 7, "  and the known ones still apply");

    // A nested object where a flat one belongs. The reader skips subtrees
    // rather than descending, so {"a":{"ui.bright":1}} must not find it.
    CKI(PARSE("{\"a\":{\"ui.bright\":1},\"ui.bright\":9}"),
        EOS_SETTINGS_OK, "a nested object is skipped, not descended into");
    CKI(s.ui_bright, 9, "  the top-level value is the one that wins");

    // A value longer than its field is refused, not truncated: a truncated
    // system prompt is a different prompt.
    {
        char over[EOS_SETTINGS_DOC_MAX];
        int len = snprintf(over, sizeof over, "{\"ui.theme\":\"%.*s\"}",
                           EOS_SETTINGS_THEME_MAX + 8, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                                       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        CKI(eos_settings_parse(&s, over, len, &bad), EOS_SETTINGS_ERR_FIELD,
            "a value longer than its field is a field error");
        CKS(s.ui_theme, EOS_SETTINGS_THEME_DEFAULT, "  and the field keeps its default");
    }
}

// ==========================================================================
// Clamping
// ==========================================================================

static void t_clamp(void)
{
    eos_settings_t s;
    uint16_t bad;

    printf("  clamping: an out-of-range number is a typo, not a rejection\n");

    CKI(PARSE("{\"ui.bright\":4000}"), EOS_SETTINGS_OK,
        "a brightness of 4000 does not reject the file");
    CKI(s.ui_bright, 255, "  it clamps to 255");
    PARSE("{\"ui.bright\":-5}");
    CKI(s.ui_bright, 0, "a negative brightness clamps to 0");

    PARSE("{\"brain.port\":0}");
    CKI(s.brain_port, 1, "port 0 clamps to 1");
    PARSE("{\"brain.port\":99999}");
    CKI(s.brain_port, 65535, "port 99999 clamps to 65535");
    PARSE("{\"brain.port\":-1}");
    CKI(s.brain_port, 1, "a negative port clamps to 1");

    PARSE("{\"brain.max\":1}");
    CKI(s.brain_max, 16, "1 token clamps up to 16");
    PARSE("{\"brain.max\":999999}");
    CKI(s.brain_max, 2048, "999999 tokens clamps to 2048");

    // A number the reader itself refuses: past 2^31 it answers TOOBIG rather
    // than wrapping, which is a bad field and not a wrong value.
    CKI(PARSE("{\"brain.port\":99999999999}"),
        EOS_SETTINGS_ERR_FIELD, "a number past 2^31 is a bad field");
    CKI(s.brain_port, 80, "  and the port keeps its default");

    // The same clamps through the setter, which is the path a POST takes.
    {
        eos_settings_store_t st;
        bool rb = false;
        store_open(&st);
        CKI(eos_settings_set_num(&st, EOS_SET_UI_BRIGHT, 9999, &rb), EOS_OK,
            "the setter accepts an out-of-range brightness");
        CKI(st.v.ui_bright, 255, "  and clamps it");
        CK(!rb, "  and does not ask for a reboot");
        CKI(eos_settings_set_num(&st, EOS_SET_BRAIN_MAX, -3, &rb), EOS_OK, "and a negative max");
        CKI(st.v.brain_max, 16, "  clamped up to 16");
    }
}

// ==========================================================================
// The setters
// ==========================================================================

static void t_setters(void)
{
    eos_settings_store_t st;
    char big[EOS_SETTINGS_DOC_MAX];
    bool rb;

    printf("  setters: types, lengths, apply and rollback\n");

    store_open(&st);

    rb = false;
    CKI(eos_settings_set_str(&st, EOS_SET_UI_THEME, "gruvbox", &rb), EOS_OK, "a string sets");
    CKS(st.v.ui_theme, "gruvbox", "  and lands");
    CK(!rb, "  ui.theme does not ask for a reboot by itself");
    CK(st.dirty, "  and the store is now dirty");
    CKI(F.applied[EOS_SET_UI_THEME], 1, "  and ports.apply ran once");

    rb = false;
    CKI(eos_settings_set_str(&st, EOS_SET_NET_HOST, "pip", &rb), EOS_OK, "net.host sets");
    CK(rb, "  and asks for a reboot");

    // A number sent as a string, which is what a hand-written curl does.
    rb = false;
    CKI(eos_settings_set_str(&st, EOS_SET_BRAIN_PORT, "8080", &rb), EOS_OK,
        "a number as a string is accepted");
    CKI(st.v.brain_port, 8080, "  and parses");
    CKI(eos_settings_set_str(&st, EOS_SET_BRAIN_PORT, "80x", &rb), EOS_ERR_ARG,
        "but trailing junk is not a number");
    CKI(st.v.brain_port, 8080, "  and nothing changed");
    CKI(eos_settings_set_str(&st, EOS_SET_BRAIN_PORT, "", &rb), EOS_ERR_ARG,
        "nor is an empty string");

    // A string sent as a number is the store's 400.
    CKI(eos_settings_set_num(&st, EOS_SET_UI_THEME, 3, &rb), EOS_ERR_ARG,
        "a number where a string belongs is refused");

    // Read-only and out-of-range keys.
    CKI(eos_settings_set_str(&st, EOS_SET_WIFI_PSK_SET, "true", &rb), EOS_ERR_READONLY,
        "wifi.psk_set cannot be written");
    CKI(eos_settings_set_str(&st, -1, "x", &rb), EOS_ERR_ARG, "a negative key is refused");
    CKI(eos_settings_set_str(&st, EOS_SET_COUNT, "x", &rb), EOS_ERR_ARG, "so is one past the end");
    CKI(eos_settings_set_str(&st, EOS_SET_UI_THEME, NULL, &rb), EOS_ERR_ARG, "so is a NULL value");

    // Over-long strings are refused and change nothing.
    memset(big, 'z', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    CKI(eos_settings_set_str(&st, EOS_SET_UI_THEME, big, &rb), EOS_ERR_TOOBIG,
        "a value longer than its field is too_big");
    CKS(st.v.ui_theme, "gruvbox", "  and the field is untouched");

    // A value exactly at the documented limit fits; one byte more does not.
    {
        char at[EOS_SETTINGS_TZ_MAX + 1];
        memset(at, 'a', sizeof at - 1);
        at[sizeof at - 1] = '\0';
        CKI(eos_settings_set_str(&st, EOS_SET_SYS_TZ, at, &rb), EOS_ERR_TOOBIG,
            "a value one byte past the field is too_big");
        at[sizeof at - 2] = '\0';
        CKI(eos_settings_set_str(&st, EOS_SET_SYS_TZ, at, &rb), EOS_OK,
            "the documented maximum length fits exactly");
        CKI((long)strlen(st.v.sys_tz), EOS_SETTINGS_TZ_MAX - 1, "  all of it");
    }

    // ports.apply rejecting a value rolls it back. This is the live theme
    // switch that could not find its file.
    F.apply_ret_key = EOS_SET_UI_THEME;
    F.apply_ret     = EOS_ERR_NOTFOUND;
    CKI(eos_settings_set_str(&st, EOS_SET_UI_THEME, "nosuch", &rb), EOS_ERR_NOTFOUND,
        "a rejected apply is reported");
    CKS(st.v.ui_theme, "gruvbox", "  and the value is rolled back");

    F.apply_ret     = EOS_ERR_UNSUPPORTED;
    rb = false;
    CKI(eos_settings_set_str(&st, EOS_SET_UI_THEME, "carbon", &rb), EOS_OK,
        "an apply that says 'not until reboot' still saves");
    CKS(st.v.ui_theme, "carbon", "  the value is kept");
    CK(rb, "  and the key is reported as needing a reboot");

    // The numeric rollback path.
    F.apply_ret_key = EOS_SET_UI_BRIGHT;
    F.apply_ret     = EOS_ERR_IO;
    st.v.ui_bright = 100;
    CKI(eos_settings_set_num(&st, EOS_SET_UI_BRIGHT, 20, &rb), EOS_ERR_IO,
        "a rejected numeric apply is reported");
    CKI(st.v.ui_bright, 100, "  and the number is rolled back");
}

// ==========================================================================
// WiFi routes away
// ==========================================================================

static void t_wifi_routes(void)
{
    eos_settings_store_t st;
    char doc[EOS_SETTINGS_DOC_MAX];
    char val[64];
    bool rb;
    int n;

    printf("  wifi: the credentials route to eos_net and never reach the file\n");

    store_open(&st);

    rb = false;
    CKI(eos_settings_set_str(&st, EOS_SET_WIFI_SSID, "HiddenHouse", &rb), EOS_OK,
        "wifi.ssid is accepted");
    CK(rb, "  and is reboot-marked");
    CK(!st.dirty, "  and does NOT make the file dirty");
    CKI(F.calls, 0, "  and does not hand eos_net half a pair");

    CKI(eos_settings_set_str(&st, EOS_SET_WIFI_PSK, "hunter22", &rb), EOS_OK,
        "wifi.psk is accepted");
    CK(!st.dirty, "  and does not make the file dirty either");
    CKI(F.calls, 0, "  still nothing handed over");

    CKI(eos_settings_route_commit(&st), EOS_OK, "the commit hands the pair over");
    CKI(F.calls, 1, "  exactly once");
    CKS(F.ssid, "HiddenHouse", "  with the ssid");
    CKS(F.psk,  "hunter22",    "  and the passphrase");

    // The staged passphrase does not survive the commit.
    {
        int i, nz = 0;
        for (i = 0; i < (int)sizeof st.route_psk; i++) if (st.route_psk[i]) nz++;
        CKI(nz, 0, "the staged passphrase is wiped afterwards");
    }
    CKI(eos_settings_route_commit(&st), EOS_OK, "a second commit is a no-op");
    CKI(F.calls, 1, "  and hands nothing over");

    // The whole point: neither key is in the document, however it was set.
    n = eos_settings_print(&st.v, doc, (int)sizeof doc);
    CK(n > 0, "the store still serialises");
    HASNT(doc, "wifi.ssid",   "the document has no wifi.ssid");
    HASNT(doc, "wifi.psk",    "the document has no wifi.psk");
    HASNT(doc, "HiddenHouse", "and no trace of the network name");
    HASNT(doc, "hunter22",    "and no trace of the passphrase");

    // An empty passphrase means "leave the stored one alone", not "clear it".
    store_open(&st);
    rb = false;
    CKI(eos_settings_set_str(&st, EOS_SET_WIFI_PSK, "", &rb), EOS_OK,
        "an empty passphrase is accepted");
    CK(rb, "  and is still reboot-marked");
    CKI(eos_settings_route_commit(&st), EOS_OK, "  the commit is a no-op");
    CKI(F.calls, 0, "  and nothing is handed to eos_net");

    // A passphrase alone: eos_net is asked with a NULL ssid, which the board
    // binding reads as "the network already named".
    store_open(&st);
    eos_settings_set_str(&st, EOS_SET_WIFI_PSK, "corrected", &rb);
    eos_settings_route_commit(&st);
    CKI(F.calls, 1, "a passphrase on its own still commits");
    CK(F.ssid_null, "  with no ssid, meaning the current network");
    CKS(F.psk, "corrected", "  and the new passphrase");

    // Read-back goes through the ports, not the file.
    store_open(&st);
    CK(eos_settings_get(&st, EOS_SET_WIFI_SSID, val, (int)sizeof val) >= 0,
       "wifi.ssid reads back");
    CKS(val, "WavvyWorld", "  from eos_net, not from the store");
    F.psk_stored = true;
    eos_settings_get(&st, EOS_SET_WIFI_PSK_SET, val, (int)sizeof val);
    CKS(val, "true", "wifi.psk_set reports that eos_net holds one");
    F.psk_stored = false;
    eos_settings_get(&st, EOS_SET_WIFI_PSK_SET, val, (int)sizeof val);
    CKS(val, "false", "and reports when it does not");

    CKI(eos_settings_get(&st, EOS_SET_WIFI_PSK, val, (int)sizeof val), (int)EOS_ERR_READONLY,
        "the passphrase itself is never readable");
    CKS(val, "", "  and nothing is written into the buffer");

    // With no port bound at all, the WiFi keys degrade rather than crash.
    eos_settings_init(&st, NULL);
    CKI(eos_settings_get(&st, EOS_SET_WIFI_SSID, val, (int)sizeof val), 0,
        "with no network service the ssid reads empty");
    eos_settings_set_str(&st, EOS_SET_WIFI_SSID, "x", &rb);
    CKI(eos_settings_route_commit(&st), EOS_ERR_UNSUPPORTED,
        "and a commit says so rather than pretending");
}

// ==========================================================================
// The file
// ==========================================================================

static void t_file(void)
{
    eos_settings_store_t st;
    bool rb = false;

    printf("  file: load, save, and every way the file can be wrong\n");

    // No file at all. The first boot, and not an error.
    drop_file();
    store_open(&st);
    CKI(eos_settings_load(&st), EOS_SETTINGS_ERR_ABSENT, "a missing file is ABSENT");
    CK(!st.from_file, "  and did not come from a file");
    {
        eos_settings_t d;
        eos_settings_defaults(&d);
        CK(same(&st.v, &d), "  and the store is exactly the defaults");
    }

    // Write and read back.
    eos_settings_set_str(&st, EOS_SET_UI_THEME, "tokyonight", &rb);
    eos_settings_set_num(&st, EOS_SET_UI_BRIGHT, 40, &rb);
    eos_settings_set_str(&st, EOS_SET_SYS_TZ, "PST8PDT,M3.2.0,M11.1.0", &rb);
    CK(st.dirty, "three edits leave the store dirty");
    CKI(eos_settings_flush(&st), EOS_OK, "and the flush writes it");
    CK(!st.dirty, "  clearing dirty");
    CKI(st.saves, 1, "  and counting one save");
    CKI(eos_settings_flush(&st), EOS_OK, "a second flush with nothing to do succeeds");
    CKI(st.saves, 1, "  and does not touch the flash again");

    store_open(&st);
    CKI(eos_settings_load(&st), EOS_SETTINGS_OK, "the file loads back");
    CK(st.from_file, "  from a file");
    CKS(st.v.ui_theme, "tokyonight", "  with the theme");
    CKI(st.v.ui_bright, 40, "  the brightness");
    CKS(st.v.sys_tz, "PST8PDT,M3.2.0,M11.1.0", "  and the timezone");
    CK(!st.dirty, "  and nothing to write back");

    // Truncated on disk.
    put_text("{\"ui.theme\":\"tokyonight\",\"ui.bri");
    store_open(&st);
    CKI(eos_settings_load(&st), EOS_SETTINGS_ERR_SYNTAX, "a truncated file is a syntax error");
    CKS(st.v.ui_theme, EOS_SETTINGS_THEME_DEFAULT, "  and the store is back to defaults");
    CK(!st.from_file, "  and knows it did not come from a file");

    // Empty on disk.
    put_text("");
    store_open(&st);
    CKI(eos_settings_load(&st), EOS_SETTINGS_ERR_EMPTY, "an empty file is EMPTY");
    CKI(st.v.ui_bright, 255, "  and the store is defaults");

    // Random bytes on disk.
    {
        unsigned char junk[512];
        int i;
        for (i = 0; i < 512; i++) junk[i] = (unsigned char)((i * 173u + 7u) & 0xFF);
        put_file(junk, 512);
    }
    store_open(&st);
    CKI(eos_settings_load(&st), EOS_SETTINGS_ERR_SYNTAX, "512 random bytes is a syntax error");
    CKI(st.v.brain_port, 80, "  and the store is defaults");

    // A file bigger than the document buffer. Refused rather than parsed as its
    // first 768 bytes, which would be a truncated document presented as valid.
    {
        char huge[EOS_SETTINGS_DOC_MAX * 4];
        memset(huge, ' ', sizeof huge);
        huge[0] = '{';
        memcpy(huge + 1, "\"ui.bright\":9", 13);
        huge[sizeof huge - 1] = '}';
        put_file(huge, (int)sizeof huge);
    }
    store_open(&st);
    CKI(eos_settings_load(&st), EOS_SETTINGS_ERR_SYNTAX, "an oversized file is refused");
    CKI(st.v.ui_bright, 255, "  and the store is defaults");

    // Wrong types on disk: the file is kept, the bad keys are not.
    put_text("{\"ui.theme\":\"ember\",\"brain.port\":\"eighty\"}");
    store_open(&st);
    CKI(eos_settings_load(&st), EOS_SETTINGS_ERR_FIELD, "wrong types are a field error");
    CK(st.from_file, "  the file still counted");
    CKS(st.v.ui_theme, "ember", "  the good key applied");
    CKI(st.v.brain_port, 80, "  the bad one did not");
    CK(st.bad_fields != 0, "  and the store says which");
    CK(!st.dirty, "  and it is NOT rewritten on the spot");

    drop_file();
}

// ==========================================================================
// The debounce
// ==========================================================================

static void t_debounce(void)
{
    eos_settings_store_t st;
    bool rb = false;

    printf("  debounce: a slider drag is one flash erase, not sixty\n");

    drop_file();
    store_open(&st);
    eos_settings_pump(&st, 100000);          // establish the clock
    CKI(st.saves, 0, "an idle pump writes nothing");

    eos_settings_set_num(&st, EOS_SET_UI_BRIGHT, 10, &rb);
    eos_settings_pump(&st, 100100);
    CKI(st.saves, 0, "100 ms after an edit, nothing has been written");
    eos_settings_pump(&st, 100000 + EOS_SETTINGS_DEBOUNCE_MS - 1);
    CKI(st.saves, 0, "one millisecond short of the debounce, still nothing");

    // Sixty more edits, each one resetting the timer. The clock only ever goes
    // forward, which is the one thing the debounce assumes.
    {
        int i;
        uint32_t t = 100000 + EOS_SETTINGS_DEBOUNCE_MS;
        for (i = 0; i < 60; i++) {
            eos_settings_set_num(&st, EOS_SET_UI_BRIGHT, 10 + i, &rb);
            t += 30;
            eos_settings_pump(&st, t);
        }
        CKI(st.saves, 0, "sixty edits 30 ms apart write nothing at all");
        eos_settings_pump(&st, t + EOS_SETTINGS_DEBOUNCE_MS);
        CKI(st.saves, 1, "and the whole drag costs exactly one write");
    }
    CK(!st.dirty, "  leaving the store clean");

    store_open(&st);
    CKI(eos_settings_load(&st), EOS_SETTINGS_OK, "the debounced write is on disk");
    CKI(st.v.ui_bright, 69, "  holding the LAST value of the drag");

    eos_settings_pump(NULL, 0);   // must not crash
    checks++;
    drop_file();
}

// ==========================================================================
// The endpoints
// ==========================================================================
//
// The real store behind eos_httpd's real handlers, wired the same way
// firmware/main/eos_settings_bind.c wires them on the board. This is the half
// that checks the JSON the web app actually reads.

static eos_settings_store_t EP;

static bool ep_get(void *ctx, int i, eos_httpd_kv_t *out)
{
    char val[EOS_HTTPD_VALUE_MAX];
    const char *name;

    (void)ctx;
    if (i < 0 || i >= EOS_SET_COUNT) return false;
    name = eos_settings_key_name(i);
    if (!name) return false;
    memset(out, 0, sizeof *out);
    if (eos_settings_get(&EP, i, val, (int)sizeof val) < 0) return true;   // write-only
    snprintf(out->key, sizeof out->key, "%s", name);
    switch (eos_settings_key_type(i)) {
    case EOS_SET_T_INT:  out->type = EOS_HTTPD_VAL_INT;  out->n = strtol(val, NULL, 10); break;
    case EOS_SET_T_BOOL: out->type = EOS_HTTPD_VAL_BOOL; out->n = !strcmp(val, "true");  break;
    default:             out->type = EOS_HTTPD_VAL_STR;
                         snprintf(out->s, sizeof out->s, "%s", val);                     break;
    }
    return true;
}

static int ep_set(void *ctx, const char *key, const char *val,
                  bool is_num, long num, bool *reboot)
{
    int k;
    (void)ctx;
    k = eos_settings_key_of(key);
    if (k < 0) return (int)EOS_ERR_NOTFOUND;
    return is_num ? (int)eos_settings_set_num(&EP, k, num, reboot)
                  : (int)eos_settings_set_str(&EP, k, val, reboot);
}

static int ep_commits;
static void ep_commit(void *ctx) { (void)ctx; ep_commits++; (void)eos_settings_route_commit(&EP); }

static int ep_reboot_ms = -1;
static int ep_reboot(void *ctx, int in_ms) { (void)ctx; ep_reboot_ms = in_ms; return 0; }

static bool ep_sys(void *ctx, eos_httpd_sys_t *o)
{
    (void)ctx;
    memset(o, 0, sizeof *o);
    snprintf(o->board_id,        sizeof o->board_id,        "waveshare-c6-lcd-13");
    snprintf(o->board_name,      sizeof o->board_name,      "Waveshare ESP32-C6-LCD-1.3");
    snprintf(o->board_summary,   sizeof o->board_summary,   "st7789 240x240");
    snprintf(o->chip_target,     sizeof o->chip_target,     "esp32-c6");
    snprintf(o->chip_variant,    sizeof o->chip_variant,    "ESP32-C6FH4");
    snprintf(o->compositor,      sizeof o->compositor,      "indexed8");
    snprintf(o->disp_controller, sizeof o->disp_controller, "st7789");
    snprintf(o->disp_bus,        sizeof o->disp_bus,        "spi2");
    snprintf(o->fw_version,      sizeof o->fw_version,      "0.1.0");
    snprintf(o->fw_idf,          sizeof o->fw_idf,          "v5.5.5");
    snprintf(o->fw_built,        sizeof o->fw_built,        "2026-08-30T12:00:00Z");
    snprintf(o->tz,              sizeof o->tz,              "%s", EP.v.sys_tz);
    o->chip_cores = 1; o->chip_rev = 1; o->flash_mb = 4;
    o->mac[0] = 0xa0; o->mac[5] = 0x8e;
    o->disp_w = 240; o->disp_h = 240; o->disp_clock_hz = 40000000; o->backlight = true;
    o->heap_free = 173100; o->heap_largest = 155648; o->heap_min_free = 170000;
    o->heap_total = 480000;
    o->uptime_ms = 128394;
    o->epoch = 1756500000u; o->time_synced = true;
    o->chunk_max = EOS_HTTPD_BODY_MAX; o->path_max = 96; o->name_max = 40;
    o->list_max = 32; o->open_files = 4;
    return true;
}

static bool ep_fs(void *ctx, int i, eos_httpd_fs_t *o)
{
    (void)ctx;
    memset(o, 0, sizeof *o);
    if (i == 0) {
        snprintf(o->point, sizeof o->point, "/sd");
        snprintf(o->fs, sizeof o->fs, "none");
        o->removable = true;
        return true;
    }
    if (i == 1) {
        snprintf(o->point, sizeof o->point, "/int");
        snprintf(o->fs, sizeof o->fs, "littlefs");
        o->mounted = o->writable = true;
        o->total = 983040; o->used = 8192;
        return true;
    }
    return false;
}

static bool ep_net(void *ctx, eos_httpd_net_t *o)
{
    (void)ctx;
    memset(o, 0, sizeof *o);
    o->state = EOS_HTTPD_NET_UP;
    memcpy(o->ssid, "WavvyWorld", 10);
    o->ssid_len = 10;
    o->rssi = -58;
    snprintf(o->ip, sizeof o->ip, "192.168.0.160");
    snprintf(o->host, sizeof o->host, "penguinos");
    return true;
}

static bool ep_theme_active(void *ctx, eos_httpd_theme_t *o)
{
    int i;
    (void)ctx;
    memset(o, 0, sizeof *o);
    snprintf(o->name, sizeof o->name, "%s", EP.v.ui_theme);
    snprintf(o->path, sizeof o->path, "/int/themes/%s.json", EP.v.ui_theme);
    o->has_colors = true;
    for (i = 0; i < EOS_HTTPD_THEME_ROLES; i++)
        snprintf(o->color[i], sizeof o->color[i], "#%02x2624", (unsigned)(0x20 + i));
    o->gap = 2; o->border = 1; o->bar_h = 12; o->tab_h = 11; o->radius = 0;
    return true;
}

static const char *EP_THEMES[] = { "cyd-amber", "gruvbox", "ember", "carbon" };
static int ep_theme_n = 4;

static bool ep_theme_list(void *ctx, int i, eos_httpd_theme_t *o)
{
    (void)ctx;
    if (i < 0 || i >= ep_theme_n) return false;
    memset(o, 0, sizeof *o);
    snprintf(o->name, sizeof o->name, "%s", EP_THEMES[i]);
    snprintf(o->path, sizeof o->path, "/int/themes/%s.json", EP_THEMES[i]);
    return true;
}

static eos_httpd_t H;

static void ep_open(bool with_ports)
{
    eos_httpd_ports_t p;
    eos_httpd_cfg_t cfg;

    store_open(&EP);
    ep_commits = 0;
    ep_reboot_ms = -1;

    memset(&p, 0, sizeof p);
    if (with_ports) {
        p.settings_get    = ep_get;
        p.settings_set    = ep_set;
        p.settings_commit = ep_commit;
        p.sys_info        = ep_sys;
        p.fs_info         = ep_fs;
        p.net_status      = ep_net;
        p.reboot          = ep_reboot;
        p.theme_active    = ep_theme_active;
        p.theme_list      = ep_theme_list;
    }
    eos_httpd_cfg_default(&cfg);
    cfg.mode = EOS_HTTPD_MODE_RUN;
    eos_httpd_init(&H, &p, NULL, &cfg);
}

static int hit(const char *method, const char *uri, const char *body,
               eos_httpd_resp_t *r)
{
    eos_httpd_req_t q;
    memset(&q, 0, sizeof q);
    q.method = method;
    q.uri    = uri;
    q.body   = body;
    q.body_len = body ? (int)strlen(body) : 0;
    return eos_httpd_dispatch(&H, &q, r);
}

static void t_routes(void)
{
    printf("  routes: four endpoints, and the one path with two methods\n");

    CKI(eos_httpd_route("GET",  "/api/settings"),      EOS_ROUTE_SETTINGS_GET,  "GET /api/settings");
    CKI(eos_httpd_route("POST", "/api/settings"),      EOS_ROUTE_SETTINGS_SET,  "POST /api/settings");
    CKI(eos_httpd_route("GET",  "/api/system"),        EOS_ROUTE_SYSTEM,        "GET /api/system");
    CKI(eos_httpd_route("GET",  "/api/system/health"), EOS_ROUTE_SYSTEM_HEALTH, "GET /api/system/health");
    CKI(eos_httpd_route("POST", "/api/system/reboot"), EOS_ROUTE_SYSTEM_REBOOT, "POST /api/system/reboot");
    CKI(eos_httpd_route("GET",  "/api/themes"),        EOS_ROUTE_THEMES,        "GET /api/themes");

    // The dual-method path is the one that would break if the table scan
    // settled for the first row it matched.
    CKI(eos_httpd_route("PUT", "/api/settings"),      EOS_ROUTE_METHOD, "PUT /api/settings is 405");
    CKI(eos_httpd_route("POST", "/api/system"),       EOS_ROUTE_METHOD, "POST /api/system is 405");
    CKI(eos_httpd_route("GET",  "/api/system/reboot"), EOS_ROUTE_METHOD, "GET the reboot is 405");
    CKI(eos_httpd_route("POST", "/api/themes"),       EOS_ROUTE_METHOD, "POST /api/themes is 405");

    CKI(eos_httpd_route("GET", "/api/setting"),  EOS_ROUTE_NONE, "a typo is a 404, not a file");
    CKI(eos_httpd_route("GET", "/api/systemx"),  EOS_ROUTE_NONE, "and so is a near miss");
}

static void t_settings_endpoint(void)
{
    eos_httpd_resp_t r;

    printf("  GET/POST /api/settings\n");

    ep_open(true);
    CKI(hit("GET", "/api/settings", NULL, &r), 200, "GET answers 200");
    HAS(r.body, "\"settings\":",    "  with a settings object");
    HAS(r.body, "\"ui.theme\":\"cyd-amber\"", "  the default theme");
    HAS(r.body, "\"ui.bright\":255", "  the brightness as a number, not a string");
    HAS(r.body, "\"brain.port\":80", "  and the port as a number");
    HAS(r.body, "\"wifi.ssid\":\"WavvyWorld\"", "  the ssid, read from eos_net");
    HAS(r.body, "\"wifi.psk_set\":false", "  and psk_set as a JSON bool");
    HASNT(r.body, "\"wifi.psk\"",  "  and never the passphrase key itself");
    CKS(r.content_type, "application/json; charset=utf-8", "  as JSON");

    // A patch of exactly the keys that changed, which is what app.js sends.
    CKI(hit("POST", "/api/settings",
            "{\"ui.theme\":\"gruvbox\",\"ui.bright\":40}", &r), 200, "a two-key patch applies");
    HAS(r.body, "\"ui.theme\":\"gruvbox\"", "  and the response restates it");
    HAS(r.body, "\"ui.bright\":40",         "  and the number");
    HAS(r.body, "\"reboot_required\":[]",   "  with nothing needing a reboot");
    CKI(EP.v.ui_bright, 40, "  the store took the value");
    CK(EP.dirty, "  and is dirty, not written");
    CKI(ep_commits, 1, "  and the commit ran once");

    // Reboot-marked keys come back named.
    CKI(hit("POST", "/api/settings", "{\"net.host\":\"pip\"}", &r), 200, "net.host applies");
    HAS(r.body, "\"reboot_required\":[\"net.host\"]", "  and is named as needing a reboot");

    CKI(hit("POST", "/api/settings",
            "{\"wifi.ssid\":\"HiddenHouse\",\"wifi.psk\":\"hunter22\"}", &r), 200,
        "the WiFi pair applies");
    HAS(r.body, "\"wifi.ssid\"", "  and the response carries the ssid");
    HASNT(r.body, "hunter22",    "  but never the passphrase");
    CKI(F.calls, 1, "  and eos_net was handed the pair exactly once");
    CKS(F.ssid, "HiddenHouse", "  with the right network");
    HAS(r.body, "\"reboot_required\":[\"wifi.ssid\"]", "  and wifi.ssid needs a reboot");
    // The rule the whole provisioning flow rests on, visible at the API: the
    // reported ssid is still the committed one, because nothing is saved until
    // a join has actually succeeded. A board that reported the new name here
    // would be a board that had already written it.
    HAS(r.body, "\"wifi.ssid\":\"WavvyWorld\"",
        "  and the reported ssid is still the one eos_net has committed");

    // Errors.
    CKI(hit("POST", "/api/settings", "not json", &r), 400, "a non-JSON body is 400");
    HAS(r.body, "bad_argument", "  with the right code");
    CKI(hit("POST", "/api/settings", NULL, &r), 400, "an empty body is 400");
    CKI(hit("POST", "/api/settings", "{\"wifi.psk_set\":true}", &r), 403,
        "writing a read-only key is 403");
    HAS(r.body, "readonly",     "  with the readonly code");
    HAS(r.body, "wifi.psk_set", "  naming the key that was refused");

    {
        char big[600];
        int n = snprintf(big, sizeof big, "{\"ui.theme\":\"");
        memset(big + n, 'z', 300);
        snprintf(big + n + 300, sizeof big - (size_t)n - 300, "\"}");
        CKI(hit("POST", "/api/settings", big, &r), 413, "an over-long value is 413");
        HAS(r.body, "too_big", "  with the right code");
        CKS(EP.v.ui_theme, "gruvbox", "  and the field is untouched");
    }

    // An unknown key is ignored rather than refused: a newer page must not be
    // able to make an older board reject a whole save.
    CKI(hit("POST", "/api/settings", "{\"ui.gamma\":2,\"ui.bright\":9}", &r), 200,
        "an unknown key does not break the patch");
    CKI(EP.v.ui_bright, 9, "  and the known key applied");

    // A number where a string belongs.
    CKI(hit("POST", "/api/settings", "{\"ui.theme\":123}", &r), 400,
        "a number where a string belongs is 400");

    // No store bound at all.
    ep_open(false);
    CKI(hit("GET",  "/api/settings", NULL, &r), 501, "with no store, GET is 501");
    CKI(hit("POST", "/api/settings", "{}", &r), 501, "and POST is 501");
    HAS(r.body, "unsupported", "  with the right code");
}

static void t_system_endpoint(void)
{
    eos_httpd_resp_t r;

    printf("  GET /api/system, /api/system/health, POST /api/system/reboot\n");

    ep_open(true);
    CKI(hit("GET", "/api/system", NULL, &r), 200, "GET /api/system answers 200");

    // Every group the Settings page's read-only panel renders.
    HAS(r.body, "\"board\":",   "board group");
    HAS(r.body, "\"waveshare-c6-lcd-13\"", "  with the profile id");
    HAS(r.body, "\"chip\":",    "chip group");
    HAS(r.body, "\"target\":\"esp32-c6\"", "  naming the silicon");
    HAS(r.body, "\"mac\":\"a0:00:00:00:00:8e\"", "  and the MAC, colon separated");
    HAS(r.body, "\"psram\":{\"present\":false", "  psram reported absent, not omitted");
    HAS(r.body, "\"render\":",  "render group");
    HAS(r.body, "\"compositor\":\"indexed8\"", "  naming the compositor");
    HAS(r.body, "\"display\":", "display group");
    HAS(r.body, "\"controller\":\"st7789\"", "  naming the panel");
    HAS(r.body, "\"heap\":",    "heap group");
    HAS(r.body, "\"largest_block\":155648", "  with the largest block");
    HAS(r.body, "\"fs\":[",     "fs array");
    HAS(r.body, "\"point\":\"/sd\"",  "  the card, declared");
    HAS(r.body, "\"mounted\":false",  "  and absent");
    HAS(r.body, "\"point\":\"/int\"", "  and the internal filesystem");
    HAS(r.body, "\"net\":",     "net group");
    HAS(r.body, "\"ip\":\"192.168.0.160\"", "  with the address the panel shows");
    HAS(r.body, "\"mdns\":\"penguinos.local\"", "  and the mDNS name");
    HAS(r.body, "\"uptime_ms\":128394", "uptime");
    HAS(r.body, "\"time\":",    "time group");
    HAS(r.body, "\"fw\":",      "fw group");
    HAS(r.body, "\"idf\":\"v5.5.5\"", "  naming the IDF");
    HAS(r.body, "\"limits\":",  "limits group");
    HAS(r.body, "\"chunk_max\":", "  the one field the client changes behaviour on");
    CK(r.body_len < EOS_HTTPD_RESP_MAX, "the whole document fits the response buffer");

    CKI(hit("GET", "/api/system/health", NULL, &r), 200, "health answers 200");
    HAS(r.body, "\"ok\":true",  "  ok");
    HAS(r.body, "\"uptime_ms\":128394", "  uptime");
    HAS(r.body, "\"heap_free\":173100", "  and free heap");
    CK(r.body_len < 80, "  in under eighty bytes");

    CKI(hit("POST", "/api/system/reboot", NULL, &r), 200, "reboot answers 200");
    HAS(r.body, "\"ok\":true", "  with ok");
    HAS(r.body, "\"in_ms\":",  "  and the delay");
    CK(ep_reboot_ms > 0, "the restart is SCHEDULED, not performed");
    CK(ep_reboot_ms >= 200, "  far enough out that the response goes first");

    ep_open(false);
    CKI(hit("GET",  "/api/system", NULL, &r), 501, "with no board description, 501");
    CKI(hit("GET",  "/api/system/health", NULL, &r), 501, "health too");
    CKI(hit("POST", "/api/system/reboot", NULL, &r), 501, "and a board that cannot restart says so");
}

static void t_themes_endpoint(void)
{
    eos_httpd_resp_t r;

    printf("  GET /api/themes\n");

    ep_open(true);
    CKI(hit("GET", "/api/themes", NULL, &r), 200, "themes answers 200");
    HAS(r.body, "\"active\":\"cyd-amber\"", "the active theme is named");
    HAS(r.body, "\"themes\":[", "and a list follows");
    HAS(r.body, "\"colors\":",  "the active entry carries colours");
    HAS(r.body, "\"bg\":\"#202624\"", "  starting at the bg role");
    HAS(r.body, "\"tab_inactive\":", "  and ending at tab_inactive");
    HAS(r.body, "\"metrics\":", "and metrics");
    HAS(r.body, "\"bar_h\":12", "  with the bar height");
    HAS(r.body, "\"gruvbox\"",  "the other files are listed");
    HAS(r.body, "\"ember\"",    "  all of them");
    HAS(r.body, "\"carbon\"",   "  all of them");

    // The active theme appears exactly once even though the scan also finds it.
    {
        const char *p = r.body, *f;
        int n = 0;
        while ((f = strstr(p, "\"name\":\"cyd-amber\"")) != NULL) { n++; p = f + 4; }
        CKI(n, 1, "and the active theme is not listed twice");
    }

    // The sixteen role names, all present, in order.
    {
        int i, pos = 0, out_of_order = 0;
        for (i = 0; i < EOS_HTTPD_THEME_ROLES; i++) {
            char want[32];
            const char *f;
            snprintf(want, sizeof want, "\"%s\":\"#", eos_httpd_role_names[i]);
            f = strstr(r.body, want);
            if (!f) { failed++; printf("    FAIL: role %s missing\n", eos_httpd_role_names[i]); }
            else if ((int)(f - r.body) < pos) out_of_order++;
            else pos = (int)(f - r.body);
        }
        checks++;
        CKI(out_of_order, 0, "the sixteen roles are emitted in eos_role_t order");
    }

    // The picker is never empty: with no theme files at all, the active theme
    // is still offered, because it came out of the image.
    ep_theme_n = 0;
    CKI(hit("GET", "/api/themes", NULL, &r), 200, "with no theme files, still 200");
    HAS(r.body, "\"active\":\"cyd-amber\"", "  the active theme is named");
    HAS(r.body, "\"colors\":", "  with its colours");
    CK(strstr(r.body, "\"themes\":[{") != NULL, "  and the list is not empty");
    ep_theme_n = 4;

    // Switching theme through settings changes what /api/themes calls active,
    // which is what the page re-reads after a save.
    CKI(hit("POST", "/api/settings", "{\"ui.theme\":\"ember\"}", &r), 200, "a theme switch saves");
    CKI(hit("GET", "/api/themes", NULL, &r), 200, "and themes re-reads");
    HAS(r.body, "\"active\":\"ember\"", "  with the new theme active");

    ep_open(false);
    CKI(hit("GET", "/api/themes", NULL, &r), 501, "with no theme service, 501");
}

// A card full of theme files must produce a short list rather than a truncated
// document — the response is built in one pass into a fixed buffer.
static bool many_theme_list(void *ctx, int i, eos_httpd_theme_t *o)
{
    (void)ctx;
    if (i < 0 || i >= 400) return false;
    memset(o, 0, sizeof *o);
    snprintf(o->name, sizeof o->name, "theme-%024d", i);
    snprintf(o->path, sizeof o->path, "/int/themes/theme-%024d.json", i);
    return true;
}

static void t_themes_flood(void)
{
    eos_httpd_resp_t r;

    printf("  themes: four hundred files still produce one valid document\n");

    ep_open(true);
    H.ports.theme_list = many_theme_list;
    CKI(hit("GET", "/api/themes", NULL, &r), 200, "four hundred themes still answers 200");
    CK(r.body_len < EOS_HTTPD_RESP_MAX, "  inside the response buffer");
    CK(r.body[r.body_len - 1] == '}', "  and the document is closed, not cut off");
    HAS(r.body, "\"active\":", "  with the active theme still first");
}

// ==========================================================================

int main(void)
{
    sandbox_open();

    t_keys();
    t_defaults();
    t_roundtrip();
    t_corrupt();
    t_clamp();
    t_setters();
    t_wifi_routes();
    t_file();
    t_debounce();
    t_routes();
    t_settings_endpoint();
    t_system_endpoint();
    t_themes_endpoint();
    t_themes_flood();

    rmtree(ROOT);
    printf("\n=== %d checks, %d failed ===\n", checks, failed);
    return failed ? 1 : 0;
}
