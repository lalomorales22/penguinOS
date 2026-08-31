// Host test for eos_net. No radio, no sockets, no NVS: the driver and the
// store are both scripted fakes, which is the whole reason they are structs of
// function pointers.
//
// The centre of this file is the save-only-after-success rule. Every path that
// could reach the store is walked and the store's own write counter is asserted
// directly, so "a failed try never persists" is a fact about the code under
// test and not a claim about it. If that rule ever regresses, the board it
// regresses on needs a serial cable to recover, so it is tested harder than
// anything else here.
//
// Build:
//   cc -std=c99 -Wall -Wextra -O1 -Ikernel/svc/include -Ikernel/shell/include \
//      kernel/svc/eos_net.c kernel/svc/test/test_net.c -o /tmp/test_net && /tmp/test_net
//
// -Ikernel/shell/include is optional: it lets the test pin eos_net_bar_wifi()
// against the real eos_bar_wifi_t rather than against repeated literals.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "eos_net.h"

#if defined(__has_include)
#  if __has_include("eos_bar.h")
#    include "eos_bar.h"
#    define HAVE_BAR 1
#  endif
#endif
#ifndef HAVE_BAR
#define HAVE_BAR 0
#endif

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
        printf("    FAIL: %s\n      got  %ld\n      want %ld\n", msg, (long)(got), (long)(want)); \
    } \
} while (0)

// =============================================================== fake store

#define FS_SLOTS 8
#define FS_KEY   20
#define FS_VAL   80

typedef struct {
    char key[FS_SLOTS][FS_KEY];
    char val[FS_SLOTS][FS_VAL];
    bool used[FS_SLOTS];
    int  saves, erases, loads;
    bool fail_save;              // every write refused
    char fail_key[FS_KEY];       // only this key's write refused
} fake_store_t;

static int fs_find(fake_store_t *s, const char *key)
{
    for (int i = 0; i < FS_SLOTS; i++)
        if (s->used[i] && strcmp(s->key[i], key) == 0) return i;
    return -1;
}

static int fs_load(void *ud, const char *key, char *out, size_t cap)
{
    fake_store_t *s = (fake_store_t *)ud;
    int i;
    s->loads++;
    if (cap) out[0] = 0;
    i = fs_find(s, key);
    if (i < 0) return -1;
    if (strlen(s->val[i]) + 1 > cap) return -1;
    memcpy(out, s->val[i], strlen(s->val[i]) + 1);
    return (int)strlen(out);
}

static int fs_save(void *ud, const char *key, const char *val)
{
    fake_store_t *s = (fake_store_t *)ud;
    int i;
    if (s->fail_save) return -1;
    if (s->fail_key[0] && strcmp(s->fail_key, key) == 0) return -1;
    s->saves++;
    i = fs_find(s, key);
    if (i < 0) {
        for (i = 0; i < FS_SLOTS && s->used[i]; i++) { }
        if (i == FS_SLOTS) return -1;
        s->used[i] = true;
        snprintf(s->key[i], FS_KEY, "%s", key);
    }
    snprintf(s->val[i], FS_VAL, "%s", val);
    return 0;
}

static int fs_erase(void *ud, const char *key)
{
    fake_store_t *s = (fake_store_t *)ud;
    int i = fs_find(s, key);
    s->erases++;
    if (i >= 0) s->used[i] = false;
    return 0;
}

static void fs_bind(eos_net_store_t *st, fake_store_t *s)
{
    memset(s, 0, sizeof *s);
    st->load = fs_load; st->save = fs_save; st->erase = fs_erase; st->ud = s;
}

static const char *fs_get(fake_store_t *s, const char *key)
{
    int i = fs_find(s, key);
    return i < 0 ? NULL : s->val[i];
}

// ============================================================== fake driver

#define FD_SCAN 24

typedef struct {
    uint8_t mac[6];
    // A join succeeds iff the ssid/psk pair matches one of these.
    char good_ssid[4][EOS_NET_SSID_MAX];
    char good_psk[4][EOS_NET_PSK_MAX];
    int  ngood;

    int joins, leaves, ap_starts, ap_stops, scans, mdns;
    char last_join_ssid[EOS_NET_SSID_MAX];
    char last_join_psk[EOS_NET_PSK_MAX];
    char last_ap_ssid[EOS_NET_AP_NAME_MAX];
    char last_ap_psk[EOS_NET_AP_PSK_MAX];
    char last_mdns[EOS_NET_HOSTNAME_MAX];
    uint32_t last_budget;

    uint32_t ip;          // handed back by drv.ip once joined
    int8_t   rssi;
    bool     joined;

    eos_net_ap_t scan_list[FD_SCAN];
    int          scan_n;
    bool         scan_fails;
    bool         all_fail;     // the router is off: nothing joins
    bool         ap_fails;
    bool         no_entropy;
    uint8_t      entropy_seed;
} fake_drv_t;

static int fd_mac(void *ud, uint8_t out[6])
{
    memcpy(out, ((fake_drv_t *)ud)->mac, 6);
    return 0;
}

static int fd_entropy(void *ud, uint8_t *out, size_t n)
{
    fake_drv_t *d = (fake_drv_t *)ud;
    if (d->no_entropy) return -1;
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(d->entropy_seed + i * 7u);
    return 0;
}

static int fd_join(void *ud, const char *ssid, const char *psk, uint32_t budget_ms)
{
    fake_drv_t *d = (fake_drv_t *)ud;
    d->joins++;
    d->last_budget = budget_ms;
    if (d->all_fail) { d->joined = false; d->ip = 0; d->rssi = 0; return -1; }
    snprintf(d->last_join_ssid, sizeof d->last_join_ssid, "%s", ssid);
    snprintf(d->last_join_psk, sizeof d->last_join_psk, "%s", psk ? psk : "");
    for (int i = 0; i < d->ngood; i++) {
        if (strcmp(d->good_ssid[i], ssid) == 0 &&
            strcmp(d->good_psk[i], psk ? psk : "") == 0) {
            d->joined = true;
            d->ip = 0xC0A80042u;   /* 192.168.0.66 */
            d->rssi = -47;
            return 0;
        }
    }
    d->joined = false;
    d->ip = 0;
    d->rssi = 0;
    return -1;
}

static int fd_leave(void *ud)
{
    fake_drv_t *d = (fake_drv_t *)ud;
    d->leaves++;
    d->joined = false;
    d->ip = 0;
    return 0;
}

static int fd_ap_start(void *ud, const char *ssid, const char *psk)
{
    fake_drv_t *d = (fake_drv_t *)ud;
    if (d->ap_fails) return -1;
    d->ap_starts++;
    snprintf(d->last_ap_ssid, sizeof d->last_ap_ssid, "%s", ssid);
    snprintf(d->last_ap_psk, sizeof d->last_ap_psk, "%s", psk);
    return 0;
}

static int fd_ap_stop(void *ud) { ((fake_drv_t *)ud)->ap_stops++; return 0; }

static int fd_scan(void *ud, eos_net_ap_t *out, int max)
{
    fake_drv_t *d = (fake_drv_t *)ud;
    int n;
    d->scans++;
    if (d->scan_fails) return -1;
    n = d->scan_n < max ? d->scan_n : max;
    for (int i = 0; i < n; i++) out[i] = d->scan_list[i];
    return n;
}

static int fd_ip(void *ud, uint32_t *out)   { *out = ((fake_drv_t *)ud)->ip;   return 0; }
static int fd_rssi(void *ud, int8_t *out)   { *out = ((fake_drv_t *)ud)->rssi; return 0; }

static int fd_mdns(void *ud, const char *h)
{
    fake_drv_t *d = (fake_drv_t *)ud;
    d->mdns++;
    snprintf(d->last_mdns, sizeof d->last_mdns, "%s", h);
    return 0;
}

static void fd_bind(eos_net_driver_t *drv, fake_drv_t *d)
{
    memset(d, 0, sizeof *d);
    d->mac[0] = 0x40; d->mac[1] = 0x4C; d->mac[2] = 0xCA;
    d->mac[3] = 0x11; d->mac[4] = 0xF0; d->mac[5] = 0x48;
    d->entropy_seed = 0x11;
    memset(drv, 0, sizeof *drv);
    drv->sta_join   = fd_join;
    drv->sta_leave  = fd_leave;
    drv->ap_start   = fd_ap_start;
    drv->ap_stop    = fd_ap_stop;
    drv->scan       = fd_scan;
    drv->mac        = fd_mac;
    drv->ip         = fd_ip;
    drv->rssi       = fd_rssi;
    drv->mdns_start = fd_mdns;
    drv->entropy    = fd_entropy;
    drv->ud         = d;
}

static void fd_good(fake_drv_t *d, const char *ssid, const char *psk)
{
    snprintf(d->good_ssid[d->ngood], EOS_NET_SSID_MAX, "%s", ssid);
    snprintf(d->good_psk[d->ngood], EOS_NET_PSK_MAX, "%s", psk);
    d->ngood++;
}

// ================================================================= harness

// The state lives inside a canary frame, so a write past the end of the scan
// cache or a string field shows up as a failed check rather than as luck.
#define GUARD 32

typedef struct {
    char      pre[GUARD];
    eos_net_t n;
    char      post[GUARD];
} framed_t;

typedef struct {
    framed_t         f;
    fake_drv_t       d;
    fake_store_t     s;
    eos_net_cfg_t    cfg;
    eos_net_event_t  events[64];
    int              nev;
} rig_t;

static rig_t *g_rig;

static void on_event(eos_net_event_t ev, const eos_net_t *n, void *ud)
{
    rig_t *r = (rig_t *)ud;
    (void)n;
    if (r->nev < (int)(sizeof r->events / sizeof r->events[0])) r->events[r->nev++] = ev;
}

static void rig_init(rig_t *r)
{
    memset(&r->f, 0xA5, sizeof r->f);
    r->nev = 0;
    fd_bind(&r->cfg.drv, &r->d);
    fs_bind(&r->cfg.store, &r->s);
    eos_net_cfg_init(&r->cfg);
    fd_bind(&r->cfg.drv, &r->d);
    fs_bind(&r->cfg.store, &r->s);
    r->cfg.on_event = on_event;
    r->cfg.cb_ud    = r;
    g_rig = r;
}

static void guards_ok(rig_t *r, const char *where)
{
    int bad = 0;
    for (int i = 0; i < GUARD; i++)
        if ((unsigned char)r->f.pre[i] != 0xA5 || (unsigned char)r->f.post[i] != 0xA5) bad++;
    checks++;
    if (bad) { fails++; printf("    FAIL: canary frame damaged after %s (%d bytes)\n", where, bad); }
}

static int ev_count(rig_t *r, eos_net_event_t ev)
{
    int c = 0;
    for (int i = 0; i < r->nev; i++) if (r->events[i] == ev) c++;
    return c;
}

// ================================================================== helpers

static void test_names(void)
{
    printf("  names and enums\n");

    CKS(eos_net_err_name(EOS_NET_OK), "ok", "err name ok");
    CKS(eos_net_err_name(EOS_NET_ERR_NOT_TRIED), "not_tried", "err name not_tried");
    CKS(eos_net_mode_name(EOS_NET_SETUP), "setup", "mode name setup");
    CKS(eos_net_cred_name(EOS_NET_CRED_TRIED_OK), "tried_ok", "cred name tried_ok");
    CKS(eos_net_event_name(EOS_NET_EV_COMMIT), "commit", "event name commit");
    CKS(eos_net_auth_name(EOS_NET_AUTH_WPA2_WPA3), "wpa2/wpa3", "auth name");

    for (int i = 0; i <= (int)EOS_NET_ERR_STATE; i++)
        CK(strcmp(eos_net_err_name((eos_net_err_t)i), "?") != 0, "every error has a name");
    for (int i = 0; i <= (int)EOS_NET_SETUP; i++)
        CK(strcmp(eos_net_mode_name((eos_net_mode_t)i), "?") != 0, "every mode has a name");
    for (int i = 0; i <= (int)EOS_NET_AUTH_OTHER; i++)
        CK(strcmp(eos_net_auth_name((eos_net_auth_t)i), "?") != 0, "every auth mode has a name");
    for (int i = 0; i <= (int)EOS_NET_EV_AP_PSK; i++)
        CK(strcmp(eos_net_event_name((eos_net_event_t)i), "?") != 0, "every event has a name");
    for (int i = 0; i <= (int)EOS_NET_CRED_SAVED; i++)
        CK(strcmp(eos_net_cred_name((eos_net_cred_t)i), "?") != 0, "every cred state has a name");

    CK(!eos_net_auth_needs_psk(EOS_NET_AUTH_OPEN), "an open network wants no passphrase");
    CK(!eos_net_auth_needs_psk(EOS_NET_AUTH_ENTERPRISE), "enterprise does not take a plain psk");
    CK(eos_net_auth_needs_psk(EOS_NET_AUTH_WPA2), "wpa2 wants a passphrase");
    CK(eos_net_auth_needs_psk(EOS_NET_AUTH_WPA3), "wpa3 wants a passphrase");
}

static void test_ap_name(void)
{
    static const struct { uint8_t mac[6]; const char *want; } V[] = {
        { { 0x40, 0x4C, 0xCA, 0x11, 0xF0, 0x48 }, "penguinos-f048" },
        { { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, "penguinos-0000" },
        { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, "penguinos-ffff" },
        { { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC }, "penguinos-9abc" },
        { { 0xDE, 0xAD, 0xBE, 0xEF, 0x0A, 0x0B }, "penguinos-0a0b" },
    };
    char buf[EOS_NET_AP_NAME_MAX];

    printf("  ap name from mac\n");

    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) {
        eos_net_ap_name(V[i].mac, buf, sizeof buf);
        CKS(buf, V[i].want, "ap name derives from the last two mac octets");
        CK(strlen(buf) == 14, "ap name is 14 characters, well inside the 32-octet SSID limit");
    }

    // Only the last two octets matter, which is what makes two boards from the
    // same production run distinguishable.
    {
        uint8_t a[6] = { 1, 2, 3, 4, 0xAB, 0xCD }, b[6] = { 9, 9, 9, 9, 0xAB, 0xCD };
        char x[EOS_NET_AP_NAME_MAX], y[EOS_NET_AP_NAME_MAX];
        eos_net_ap_name(a, x, sizeof x);
        eos_net_ap_name(b, y, sizeof y);
        CKS(x, y, "only the last two octets feed the name");
        b[4] = 0xAC;
        eos_net_ap_name(b, y, sizeof y);
        CK(strcmp(x, y) != 0, "a different fifth octet gives a different name");
    }

    // Short buffers truncate rather than overrun.
    for (size_t cap = 1; cap <= 12; cap++) {
        char small[EOS_NET_AP_NAME_MAX];
        memset(small, 0x7E, sizeof small);
        eos_net_ap_name(V[0].mac, small, cap);
        CK(strlen(small) == cap - 1, "a short buffer truncates to cap-1");
        CK((unsigned char)small[cap] == 0x7E, "nothing is written past cap");
    }
    eos_net_ap_name(V[0].mac, NULL, 16);
    eos_net_ap_name(V[0].mac, buf, 0);
    CK(1, "NULL and zero-cap are survivable");
}

static void test_ap_psk(void)
{
    uint8_t ent[64];
    char psk[EOS_NET_AP_PSK_MAX], other[EOS_NET_AP_PSK_MAX];
    int seen[256];

    printf("  ap password generation\n");

    for (size_t i = 0; i < sizeof ent; i++) ent[i] = (uint8_t)(i * 13u + 5u);
    CKI(eos_net_ap_psk_from_entropy(ent, sizeof ent, psk, sizeof psk), 0, "generation succeeds");
    CKI(strlen(psk), EOS_NET_AP_PSK_LEN, "the password is exactly EOS_NET_AP_PSK_LEN");
    CK(eos_net_ap_psk_valid(psk), "a generated password validates");
    CK(strlen(psk) >= 8, "long enough to be a legal WPA2 passphrase");
    CK(strlen(psk) <= 63, "short enough to be a legal WPA2 passphrase");

    // Nothing in the alphabet can be misread off a panel: no 0/O, no 1/l/I,
    // and no uppercase at all so there is no case to get wrong when typing.
    for (size_t i = 0; psk[i]; i++) {
        CK(psk[i] != '0' && psk[i] != 'O' && psk[i] != 'o', "no zero-vs-oh confusion");
        CK(psk[i] != '1' && psk[i] != 'l' && psk[i] != 'I', "no one-vs-ell confusion");
        CK(!(psk[i] >= 'A' && psk[i] <= 'Z'), "lowercase only");
        CK((psk[i] >= 'a' && psk[i] <= 'z') || (psk[i] >= '2' && psk[i] <= '9'),
           "every character is a digit 2-9 or a lowercase letter");
    }

    // The mapping must cover the whole 32-symbol alphabet and nothing else,
    // and every one of the 256 byte values must land inside it.
    memset(seen, 0, sizeof seen);
    for (int b = 0; b < 256; b++) {
        uint8_t e[EOS_NET_AP_PSK_LEN];
        char out[EOS_NET_AP_PSK_MAX];
        memset(e, (uint8_t)b, sizeof e);
        CKI(eos_net_ap_psk_from_entropy(e, sizeof e, out, sizeof out), 0, "any byte generates");
        CK(eos_net_ap_psk_valid(out), "every byte value maps into the alphabet");
        seen[(unsigned char)out[0]] = 1;
    }
    {
        int distinct = 0;
        for (int i = 0; i < 256; i++) distinct += seen[i];
        CKI(distinct, 32, "the 256 byte values cover exactly 32 symbols, uniformly");
    }

    // Different entropy, different password. Same entropy, same password.
    ent[0] ^= 0xFF;
    eos_net_ap_psk_from_entropy(ent, sizeof ent, other, sizeof other);
    CK(strcmp(psk, other) != 0, "different entropy gives a different password");
    ent[0] ^= 0xFF;
    eos_net_ap_psk_from_entropy(ent, sizeof ent, other, sizeof other);
    CKS(other, psk, "the generator is a pure function of its entropy");

    // Refusals, not truncations. A short password on a SoftAP is a real
    // exposure and silently shortening one would be the worst outcome here.
    CK(eos_net_ap_psk_from_entropy(ent, EOS_NET_AP_PSK_LEN - 1, psk, sizeof psk) < 0,
       "short entropy is refused, not stretched");
    CK(eos_net_ap_psk_from_entropy(ent, sizeof ent, psk, EOS_NET_AP_PSK_LEN) < 0,
       "a buffer one byte short is refused, not truncated");
    CK(eos_net_ap_psk_from_entropy(NULL, sizeof ent, psk, sizeof psk) < 0, "NULL entropy refused");
    CK(eos_net_ap_psk_from_entropy(ent, sizeof ent, NULL, sizeof psk) < 0, "NULL output refused");

    CK(!eos_net_ap_psk_valid(NULL), "NULL is not a valid password");
    CK(!eos_net_ap_psk_valid(""), "empty is not a valid password");
    CK(!eos_net_ap_psk_valid("short"), "too short is not valid");
    CK(!eos_net_ap_psk_valid("abcdefghijklm"), "too long is not valid");
    CK(!eos_net_ap_psk_valid("abcdefghijk1"), "a character outside the alphabet is not valid");
    CK(!eos_net_ap_psk_valid("ABCDEFGHIJKM"), "uppercase is not valid");
    CK(eos_net_ap_psk_valid("abcdefghijkm"), "a hand-built in-alphabet password validates");
}

static void test_keys(void)
{
    printf("  nvs key handling\n");

    CK(eos_net_key_ok(EOS_NET_KEY_SSID), "the ssid key is a legal nvs key");
    CK(eos_net_key_ok(EOS_NET_KEY_PSK), "the psk key is a legal nvs key");
    CK(eos_net_key_ok(EOS_NET_KEY_AP_PSK), "the ap psk key is a legal nvs key");
    CK(strlen(EOS_NET_NVS_NS) <= EOS_NET_NVS_KEY_LIMIT, "the namespace fits the nvs limit");
    CK(strcmp(EOS_NET_KEY_SSID, EOS_NET_KEY_PSK) != 0 &&
       strcmp(EOS_NET_KEY_SSID, EOS_NET_KEY_AP_PSK) != 0 &&
       strcmp(EOS_NET_KEY_PSK, EOS_NET_KEY_AP_PSK) != 0, "the three keys are distinct");

    CK(!eos_net_key_ok(NULL), "NULL is not a key");
    CK(!eos_net_key_ok(""), "empty is not a key");
    CK(eos_net_key_ok("123456789012345"), "fifteen characters is the limit and is accepted");
    CK(!eos_net_key_ok("1234567890123456"), "sixteen characters is refused");
}

static void test_ip_str(void)
{
    char b[16];
    printf("  address formatting\n");
    CKS(eos_net_ip_str(EOS_NET_AP_IP, b, sizeof b), EOS_NET_AP_IP_STR,
        "EOS_NET_AP_IP and EOS_NET_AP_IP_STR agree");
    CKS(eos_net_ip_str(0, b, sizeof b), "0.0.0.0", "zero formats");
    CKS(eos_net_ip_str(0xFFFFFFFFu, b, sizeof b), "255.255.255.255", "broadcast formats");
    CKS(eos_net_ip_str(0xC0A80042u, b, sizeof b), "192.168.0.66", "a normal address formats");
}

// =============================================================== scan reduce

static eos_net_ap_t mk(const char *ssid, int rssi, int ch, eos_net_auth_t a)
{
    eos_net_ap_t p;
    memset(&p, 0, sizeof p);
    snprintf(p.ssid, sizeof p.ssid, "%s", ssid);
    p.rssi = (int8_t)rssi;
    p.channel = (uint8_t)ch;
    p.auth = a;
    return p;
}

static void test_scan_reduce(void)
{
    eos_net_ap_t a[24];
    int n;

    printf("  scan dedup and sort\n");

    CKI(eos_net_scan_reduce(NULL, 4), 0, "NULL is empty");
    CKI(eos_net_scan_reduce(a, 0), 0, "zero is empty");
    CKI(eos_net_scan_reduce(a, -3), 0, "a negative count is empty");

    a[0] = mk("solo", -50, 6, EOS_NET_AUTH_WPA2);
    CKI(eos_net_scan_reduce(a, 1), 1, "one network survives");
    CKS(a[0].ssid, "solo", "and is unchanged");

    // Sorted strongest first.
    a[0] = mk("weak",   -88, 1, EOS_NET_AUTH_WPA2);
    a[1] = mk("strong", -30, 6, EOS_NET_AUTH_WPA2);
    a[2] = mk("mid",    -60, 11, EOS_NET_AUTH_OPEN);
    n = eos_net_scan_reduce(a, 3);
    CKI(n, 3, "nothing is dropped when there are no duplicates");
    CKS(a[0].ssid, "strong", "strongest first");
    CKS(a[1].ssid, "mid", "then the middle");
    CKS(a[2].ssid, "weak", "weakest last");
    CKI(a[0].channel, 6, "the channel travels with its network");
    CKI(a[1].auth, EOS_NET_AUTH_OPEN, "so does the auth mode");

    // Dedup keeps the strongest sighting WHOLE: the surviving row's channel and
    // auth must come from the same beacon its RSSI came from, not be spliced.
    a[0] = mk("mesh", -70, 1,  EOS_NET_AUTH_WPA2);
    a[1] = mk("mesh", -41, 11, EOS_NET_AUTH_WPA3);
    a[2] = mk("mesh", -55, 6,  EOS_NET_AUTH_OPEN);
    n = eos_net_scan_reduce(a, 3);
    CKI(n, 1, "three sightings of one name collapse to one row");
    CKI(a[0].rssi, -41, "the strongest sighting wins");
    CKI(a[0].channel, 11, "and brings its own channel");
    CKI(a[0].auth, EOS_NET_AUTH_WPA3, "and its own auth mode");

    // Strongest first in the input, so the keep-strongest branch is exercised
    // in the other direction too.
    a[0] = mk("mesh", -41, 11, EOS_NET_AUTH_WPA3);
    a[1] = mk("mesh", -70, 1,  EOS_NET_AUTH_WPA2);
    n = eos_net_scan_reduce(a, 2);
    CKI(n, 1, "order of arrival does not change the dedup result");
    CKI(a[0].rssi, -41, "the strongest still wins");
    CKI(a[0].channel, 11, "still with its own channel");

    // Hidden networks are dropped: they have nothing to show in a picker and
    // every one of them would collapse onto the same empty row.
    a[0] = mk("",      -20, 1, EOS_NET_AUTH_WPA2);
    a[1] = mk("named", -80, 6, EOS_NET_AUTH_WPA2);
    a[2] = mk("",      -25, 11, EOS_NET_AUTH_WPA2);
    n = eos_net_scan_reduce(a, 3);
    CKI(n, 1, "hidden networks are dropped even when they are the strongest");
    CKS(a[0].ssid, "named", "the named network survives");

    // Ties break on the SSID, so two scans of the same room give the same list
    // and the row under the user's thumb does not move.
    a[0] = mk("zulu",  -55, 1, EOS_NET_AUTH_WPA2);
    a[1] = mk("alpha", -55, 6, EOS_NET_AUTH_WPA2);
    a[2] = mk("mike",  -55, 11, EOS_NET_AUTH_WPA2);
    n = eos_net_scan_reduce(a, 3);
    CKI(n, 3, "equal signals all survive");
    CKS(a[0].ssid, "alpha", "ties sort by name");
    CKS(a[1].ssid, "mike", "ties sort by name");
    CKS(a[2].ssid, "zulu", "ties sort by name");
    {
        eos_net_ap_t b[3];
        b[0] = mk("mike",  -55, 11, EOS_NET_AUTH_WPA2);
        b[1] = mk("zulu",  -55, 1, EOS_NET_AUTH_WPA2);
        b[2] = mk("alpha", -55, 6, EOS_NET_AUTH_WPA2);
        eos_net_scan_reduce(b, 3);
        for (int i = 0; i < 3; i++) CKS(b[i].ssid, a[i].ssid, "a shuffled scan reduces identically");
    }

    // Everything at once, the shape a real room produces.
    {
        int i = 0;
        a[i++] = mk("",         -33, 1, EOS_NET_AUTH_OPEN);
        a[i++] = mk("house",    -62, 6, EOS_NET_AUTH_WPA2);
        a[i++] = mk("house",    -44, 1, EOS_NET_AUTH_WPA2);
        a[i++] = mk("guest",    -71, 11, EOS_NET_AUTH_OPEN);
        a[i++] = mk("neighbour", -85, 6, EOS_NET_AUTH_WPA_WPA2);
        a[i++] = mk("guest",    -73, 1, EOS_NET_AUTH_OPEN);
        a[i++] = mk("",         -20, 11, EOS_NET_AUTH_OPEN);
        a[i++] = mk("house",    -90, 11, EOS_NET_AUTH_WPA2);
        n = eos_net_scan_reduce(a, i);
        CKI(n, 3, "a realistic scan reduces to three networks");
        CKS(a[0].ssid, "house", "strongest first");
        CKI(a[0].rssi, -44, "with its strongest reading");
        CKS(a[1].ssid, "guest", "then guest");
        CKI(a[1].rssi, -71, "with its strongest reading");
        CKS(a[2].ssid, "neighbour", "then the neighbour");
        for (int k = 1; k < n; k++)
            CK(a[k - 1].rssi >= a[k].rssi, "the result is sorted descending throughout");
    }

    // A whole array of one name, and a whole array of hidden.
    for (int i = 0; i < 24; i++) a[i] = mk("same", -30 - i, (uint8_t)(i % 13 + 1), EOS_NET_AUTH_WPA2);
    CKI(eos_net_scan_reduce(a, 24), 1, "twenty-four sightings of one name is one row");
    CKI(a[0].rssi, -30, "and it is the strongest one");
    for (int i = 0; i < 24; i++) a[i] = mk("", -30, 1, EOS_NET_AUTH_OPEN);
    CKI(eos_net_scan_reduce(a, 24), 0, "an all-hidden scan reduces to nothing");

    // An SSID that fills the field exactly must survive with its NUL intact.
    {
        eos_net_ap_t long_ap;
        memset(&long_ap, 0, sizeof long_ap);
        memset(long_ap.ssid, 'x', EOS_NET_SSID_MAX - 1);
        long_ap.rssi = -40;
        a[0] = long_ap;
        CKI(eos_net_scan_reduce(a, 1), 1, "a 32-octet ssid survives");
        CKI(strlen(a[0].ssid), EOS_NET_SSID_MAX - 1, "at full length");
    }
}

// ======================================================================= QR

static void test_qr(void)
{
    char b[EOS_NET_QR_MAX];
    int n;

    printf("  wifi QR payload\n");

    n = eos_net_wifi_qr("penguinos-f048", "hqm4x7bt2fkd", b, sizeof b);
    CK(n > 0, "a normal payload builds");
    CKS(b, "WIFI:S:penguinos-f048;T:WPA;P:hqm4x7bt2fkd;;",
        "the payload is exactly the form docs/provisioning.md specifies");
    CKI(n, (int)strlen(b), "the return value is the written length");

    // The four reserved characters are escaped, in both fields.
    n = eos_net_wifi_qr("my;net", "a\\b,c:d", b, sizeof b);
    CK(n > 0, "a payload with reserved characters builds");
    CKS(b, "WIFI:S:my\\;net;T:WPA;P:a\\\\b\\,c\\:d;;", "reserved characters are backslash-escaped");

    n = eos_net_wifi_qr("open-net", NULL, b, sizeof b);
    CKS(b, "WIFI:S:open-net;T:nopass;;", "an open network has no P field");
    n = eos_net_wifi_qr("open-net", "", b, sizeof b);
    CKS(b, "WIFI:S:open-net;T:nopass;;", "an empty password is the same as none");
    (void)n;

    CK(eos_net_wifi_qr(NULL, "x", b, sizeof b) < 0, "no ssid, no payload");
    CK(eos_net_wifi_qr("", "x", b, sizeof b) < 0, "an empty ssid is refused");
    CK(eos_net_wifi_qr("a", "b", NULL, sizeof b) < 0, "NULL output is refused");
    CK(eos_net_wifi_qr("a", "b", b, 0) < 0, "a zero-cap buffer is refused");

    // Truncation is a refusal, never a short payload. A QR that encodes half a
    // password is worse than no QR: the phone joins nothing and says nothing.
    {
        const char *ssid = "penguinos-f048", *psk = "hqm4x7bt2fkd";
        int full = eos_net_wifi_qr(ssid, psk, b, sizeof b);
        for (int cap = 1; cap <= full; cap++) {
            char small[EOS_NET_QR_MAX];
            memset(small, 0x5A, sizeof small);
            CK(eos_net_wifi_qr(ssid, psk, small, (size_t)cap) < 0,
               "any buffer that cannot hold the whole payload is refused");
            CK((unsigned char)small[cap] == 0x5A, "and nothing is written past cap");
        }
        CK(eos_net_wifi_qr(ssid, psk, b, (size_t)full + 1) == full,
           "exactly enough room succeeds");
    }

    // The worst case the board can produce must fit EOS_NET_QR_MAX.
    {
        char ssid[EOS_NET_SSID_MAX], psk[EOS_NET_PSK_MAX];
        memset(ssid, ';', sizeof ssid - 1); ssid[sizeof ssid - 1] = 0;
        memset(psk, ';', sizeof psk - 1);   psk[sizeof psk - 1] = 0;
        CK(eos_net_wifi_qr(ssid, psk, b, sizeof b) > 0,
           "EOS_NET_QR_MAX holds a fully escaped maximum-length pair");
    }
}

// ================================================== the credential machine

// The rule this whole file exists for. Every one of these asserts the store's
// own write counter, not a return code.
static void test_try_never_persists(void)
{
    rig_t r;

    printf("  try never writes, commit does  [THE RULE]\n");

    // --- a failed try writes nothing -------------------------------------
    rig_init(&r);
    fd_good(&r.d, "house", "correct-horse");
    eos_net_init(&r.f.n, &r.cfg);
    eos_net_start(&r.f.n);
    CKI(eos_net_mode(&r.f.n), EOS_NET_SETUP, "an empty store boots to SETUP");
    r.s.saves = 0;   // discard the ap password write, which is not a credential

    CKI(eos_net_try(&r.f.n, "house", "wrong"), EOS_NET_ERR_JOIN, "a wrong password fails to join");
    CKI(r.s.saves, 0, "A FAILED TRY WRITES NOTHING TO THE STORE");
    CK(fs_get(&r.s, EOS_NET_KEY_SSID) == NULL, "no ssid was persisted");
    CK(fs_get(&r.s, EOS_NET_KEY_PSK) == NULL, "no psk was persisted");
    CKI(eos_net_cred(&r.f.n), EOS_NET_CRED_TRIED_FAIL, "the failure is recorded in RAM only");
    CK(!eos_net_has_credentials(&r.f.n), "and there are still no credentials");

    // --- a SUCCESSFUL try also writes nothing ----------------------------
    CKI(eos_net_try(&r.f.n, "house", "correct-horse"), EOS_NET_OK, "the right password joins");
    CKI(r.s.saves, 0, "A SUCCESSFUL TRY ALSO WRITES NOTHING — commit is a separate act");
    CK(fs_get(&r.s, EOS_NET_KEY_SSID) == NULL, "still nothing persisted");
    CKI(eos_net_cred(&r.f.n), EOS_NET_CRED_TRIED_OK, "the success is staged, not stored");

    // --- commit is what writes -------------------------------------------
    CKI(eos_net_commit(&r.f.n), EOS_NET_OK, "commit after a success is accepted");
    CKI(r.s.saves, 2, "commit writes exactly the ssid and the psk");
    CKS(fs_get(&r.s, EOS_NET_KEY_SSID), "house", "the ssid that was proved is the ssid stored");
    CKS(fs_get(&r.s, EOS_NET_KEY_PSK), "correct-horse", "and the psk that was proved");
    CKI(eos_net_cred(&r.f.n), EOS_NET_CRED_SAVED, "cred moves to SAVED");
    CK(eos_net_has_credentials(&r.f.n), "there are now credentials");
    CKI(ev_count(&r, EOS_NET_EV_COMMIT), 1, "commit raises exactly one event");

    // --- a second commit is refused --------------------------------------
    CKI(eos_net_commit(&r.f.n), EOS_NET_ERR_NOT_TRIED, "a second commit is refused");
    CKI(r.s.saves, 2, "and writes nothing");
    guards_ok(&r, "try/commit");

    // --- commit with no try at all ---------------------------------------
    rig_init(&r);
    fd_good(&r.d, "house", "pw");
    eos_net_init(&r.f.n, &r.cfg);
    eos_net_start(&r.f.n);
    r.s.saves = 0;
    CKI(eos_net_commit(&r.f.n), EOS_NET_ERR_NOT_TRIED, "commit with no try is refused");
    CKI(r.s.saves, 0, "and writes nothing");
    CKI(eos_net_last_error(&r.f.n), EOS_NET_ERR_NOT_TRIED, "and says why");

    // --- commit immediately after a failure ------------------------------
    eos_net_try(&r.f.n, "house", "nope");
    CKI(eos_net_commit(&r.f.n), EOS_NET_ERR_NOT_TRIED, "commit after a failed try is refused");
    CKI(r.s.saves, 0, "and writes nothing");

    // --- a success followed by a failure cannot be laundered -------------
    // The trap this consumes-on-commit design exists to close: try good, try
    // bad, commit. If the OK were sticky the board would persist the network it
    // just proved it cannot join.
    CKI(eos_net_try(&r.f.n, "house", "pw"), EOS_NET_OK, "the good pair joins");
    CKI(eos_net_try(&r.f.n, "house", "nope"), EOS_NET_ERR_JOIN, "the bad pair does not");
    CKI(eos_net_commit(&r.f.n), EOS_NET_ERR_NOT_TRIED,
        "a stale success cannot be committed after a later failure");
    CKI(r.s.saves, 0, "and writes nothing");

    // --- and the pair committed is the pair proved, not the last typed ---
    CKI(eos_net_try(&r.f.n, "house", "pw"), EOS_NET_OK, "prove the good pair again");
    CKI(eos_net_commit(&r.f.n), EOS_NET_OK, "commit takes it");
    CKS(fs_get(&r.s, EOS_NET_KEY_PSK), "pw", "the stored psk is the one that joined");
    guards_ok(&r, "launder");
}

static void test_commit_store_failure(void)
{
    rig_t r;

    printf("  a store that refuses\n");

    // Both writes refused: nothing lands, and the caller is told.
    rig_init(&r);
    fd_good(&r.d, "house", "pw");
    eos_net_init(&r.f.n, &r.cfg);
    eos_net_start(&r.f.n);
    eos_net_try(&r.f.n, "house", "pw");
    r.s.fail_save = true;
    CKI(eos_net_commit(&r.f.n), EOS_NET_ERR_STORE, "a refusing store fails the commit");
    CK(fs_get(&r.s, EOS_NET_KEY_SSID) == NULL, "and stores nothing");
    CKI(eos_net_cred(&r.f.n), EOS_NET_CRED_TRIED_OK,
        "the proved pair is still committable once the store recovers");
    r.s.fail_save = false;
    CKI(eos_net_commit(&r.f.n), EOS_NET_OK, "and it commits on the retry");
    CKS(fs_get(&r.s, EOS_NET_KEY_SSID), "house", "landing this time");

    // The psk write fails after the ssid write landed. Half a credential is a
    // boot failure the user cannot read, so the ssid must be taken back out.
    rig_init(&r);
    fd_good(&r.d, "house", "pw");
    eos_net_init(&r.f.n, &r.cfg);
    eos_net_start(&r.f.n);
    eos_net_try(&r.f.n, "house", "pw");
    snprintf(r.s.fail_key, sizeof r.s.fail_key, "%s", EOS_NET_KEY_PSK);
    CKI(eos_net_commit(&r.f.n), EOS_NET_ERR_STORE, "a half-failing store fails the commit");
    CK(fs_get(&r.s, EOS_NET_KEY_SSID) == NULL,
       "AND ROLLS THE SSID BACK — half a credential must not survive");
    CK(fs_get(&r.s, EOS_NET_KEY_PSK) == NULL, "with no psk either");
    guards_ok(&r, "store failure");
}

static void test_forget(void)
{
    rig_t r;
    char psk_before[EOS_NET_AP_PSK_MAX];

    printf("  forget\n");

    rig_init(&r);
    fd_good(&r.d, "house", "pw");
    fs_save(&r.s, EOS_NET_KEY_SSID, "house");
    fs_save(&r.s, EOS_NET_KEY_PSK, "pw");
    eos_net_init(&r.f.n, &r.cfg);
    CKI(eos_net_start(&r.f.n), EOS_NET_OK, "stored credentials boot");
    CKI(eos_net_mode(&r.f.n), EOS_NET_STA, "straight to STA");
    CKS(eos_net_ap_psk(&r.f.n), "",
        "a board that never needed SETUP never generated an AP password");

    r.nev = 0;
    CKI(eos_net_forget(&r.f.n), EOS_NET_OK, "forget succeeds");
    CK(fs_get(&r.s, EOS_NET_KEY_SSID) == NULL, "the ssid is gone from the store");
    CK(fs_get(&r.s, EOS_NET_KEY_PSK) == NULL, "the psk is gone from the store");
    CKI(eos_net_cred(&r.f.n), EOS_NET_CRED_NONE, "cred is back to NONE");
    CK(!eos_net_has_credentials(&r.f.n), "and there are no credentials");
    CKS(eos_net_ssid(&r.f.n), "", "the reported network is cleared");
    CKI(eos_net_mode(&r.f.n), EOS_NET_SETUP, "and the board drops to SETUP");
    CKI(ev_count(&r, EOS_NET_EV_FORGET), 1, "one forget event");
    CK(r.d.ap_starts >= 1, "the SoftAP came up");

    snprintf(psk_before, sizeof psk_before, "%s", eos_net_ap_psk(&r.f.n));
    CK(eos_net_ap_psk_valid(psk_before), "dropping to SETUP generated an AP password");
    CKS(fs_get(&r.s, EOS_NET_KEY_AP_PSK), psk_before, "and stored it");

    // And the board is now provisionable again.
    CKI(eos_net_try(&r.f.n, "house", "pw"), EOS_NET_OK, "a fresh try works after a forget");
    CKI(eos_net_commit(&r.f.n), EOS_NET_OK, "and commits");
    CKS(fs_get(&r.s, EOS_NET_KEY_SSID), "house", "restoring the credentials");
    eos_net_pump(&r.f.n, 1000);
    eos_net_pump(&r.f.n, 4000);
    CKI(eos_net_mode(&r.f.n), EOS_NET_STA, "and the board settles on the network");

    // A second forget must not reroll the AP password: it is on the panel and
    // may already be typed into a phone. Rerolling would make the screen the
    // user is looking at wrong.
    CKI(eos_net_forget(&r.f.n), EOS_NET_OK, "a second forget succeeds");
    CKS(eos_net_ap_psk(&r.f.n), psk_before, "THE AP PASSWORD SURVIVES A FORGET");
    CKS(r.d.last_ap_psk, psk_before, "and the AP is raised with the same one");
    CKS(fs_get(&r.s, EOS_NET_KEY_AP_PSK), psk_before, "and it is still the stored one");
    guards_ok(&r, "forget");
}

// ============================================================ the boot paths

static void test_boot(void)
{
    rig_t r;

    printf("  three-state boot\n");

    // --- no credentials -> SETUP -----------------------------------------
    rig_init(&r);
    fd_good(&r.d, "house", "pw");
    r.d.scan_list[0] = mk("house", -40, 6, EOS_NET_AUTH_WPA2);
    r.d.scan_list[1] = mk("guest", -70, 1, EOS_NET_AUTH_OPEN);
    r.d.scan_n = 2;
    CKI(eos_net_init(&r.f.n, &r.cfg), EOS_NET_OK, "init succeeds");
    CKI(eos_net_mode(&r.f.n), EOS_NET_OFF, "before start, the mode is OFF");
    CKI(eos_net_bar_wifi(&r.f.n), 0, "and the bar shows OFF");
    CKS(eos_net_ap_ssid(&r.f.n), "penguinos-f048", "the AP name is derived at init");
    CKS(eos_net_hostname(&r.f.n), "penguinos-f048", "and so is the mDNS name, so boards do not collide");

    CKI(eos_net_start(&r.f.n), EOS_NET_OK, "an empty store still starts cleanly");
    CKI(eos_net_mode(&r.f.n), EOS_NET_SETUP, "with no credentials the board goes to SETUP");
    CKI(r.d.joins, 0, "and does not attempt a join it has nothing for");
    CKI(r.d.ap_starts, 1, "the SoftAP is up");
    CKS(r.d.last_ap_ssid, "penguinos-f048", "under the derived name");
    CK(eos_net_ap_psk_valid(r.d.last_ap_psk), "with a generated WPA2 password");
    CK(strlen(r.d.last_ap_psk) >= 8, "THE SOFTAP IS NOT OPEN");
    CKS(r.d.last_ap_psk, eos_net_ap_psk(&r.f.n),
        "and the password on the panel is the password on the air");
    CKI(eos_net_ip(&r.f.n), EOS_NET_AP_IP, "the address is the portal address");
    CKI(eos_net_bar_wifi(&r.f.n), 2, "the bar shows DOWN in SETUP");
    CKI(r.d.scans, 1, "one scan runs at SETUP entry");
    CK(eos_net_scan_cached(&r.f.n), "and the result is cached");
    CKI(r.d.mdns, 0, "mDNS is not started on a SoftAP nobody can resolve names on");

    {
        const eos_net_ap_t *list = NULL;
        int n = eos_net_scan_results(&r.f.n, &list);
        CKI(n, EOS_NET_SCAN_MAX < 2 ? EOS_NET_SCAN_MAX : 2, "the cached scan holds both networks");
        CKS(list[0].ssid, "house", "strongest first");
    }
    {
        char qr[EOS_NET_QR_MAX];
        int n = eos_net_ap_qr(&r.f.n, qr, sizeof qr);
        CK(n > 0, "the AP's QR payload builds");
        CK(strstr(qr, "WIFI:S:penguinos-f048;T:WPA;P:") == qr, "and names this board's AP with WPA");
    }
    guards_ok(&r, "boot setup");

    // --- credentials that join -> STA ------------------------------------
    rig_init(&r);
    fd_good(&r.d, "house", "pw");
    fs_save(&r.s, EOS_NET_KEY_SSID, "house");
    fs_save(&r.s, EOS_NET_KEY_PSK, "pw");
    eos_net_init(&r.f.n, &r.cfg);
    CKI(eos_net_start(&r.f.n), EOS_NET_OK, "a stored network boots");
    CKI(eos_net_mode(&r.f.n), EOS_NET_STA, "straight to STA");
    CKI(r.d.joins, 1, "with exactly one join attempt");
    CKS(r.d.last_join_ssid, "house", "using the stored ssid");
    CKS(r.d.last_join_psk, "pw", "and the stored psk");
    CKI(r.d.last_budget, 15000, "inside the 15 s budget from docs/provisioning.md");
    CKI(r.d.ap_starts, 0, "the SoftAP never comes up");
    CKI(r.d.scans, 0, "and nothing is scanned");
    CKI(r.d.mdns, 1, "mDNS starts");
    CKS(r.d.last_mdns, "penguinos-f048", "under the derived hostname");
    CKI(eos_net_ip(&r.f.n), 0xC0A80042u, "the address is the one the driver reported");
    CKI(eos_net_rssi(&r.f.n), -47, "and so is the signal");
    CKI(eos_net_bar_wifi(&r.f.n), 3, "the bar shows UP");
    CKS(eos_net_ssid(&r.f.n), "house", "the joined network is reported");
    CKI(eos_net_cred(&r.f.n), EOS_NET_CRED_SAVED, "cred is SAVED");
    guards_ok(&r, "boot sta");

    // --- credentials that do not join -> SETUP, credentials KEPT ---------
    rig_init(&r);
    fd_good(&r.d, "elsewhere", "pw");
    fs_save(&r.s, EOS_NET_KEY_SSID, "house");
    fs_save(&r.s, EOS_NET_KEY_PSK, "pw");
    r.s.saves = 0; r.s.erases = 0;
    eos_net_init(&r.f.n, &r.cfg);
    CKI(eos_net_start(&r.f.n), EOS_NET_OK, "a stored network that will not join still starts");
    CKI(r.d.joins, 1, "one attempt is made");
    CKI(eos_net_mode(&r.f.n), EOS_NET_SETUP, "and the board falls back to SETUP");
    CKI(r.d.ap_starts, 1, "with the SoftAP up");
    CKS(fs_get(&r.s, EOS_NET_KEY_SSID), "house",
        "THE CREDENTIALS ARE KEPT — out of range is not the same as wrong");
    CKS(fs_get(&r.s, EOS_NET_KEY_PSK), "pw", "both of them");
    CKI(r.s.erases, 0, "nothing was erased");
    CK(eos_net_has_credentials(&r.f.n), "and the board still knows it has a network");

    // Carried back into range, the same credentials work again.
    fd_good(&r.d, "house", "pw");
    CKI(eos_net_try(&r.f.n, "house", "pw"), EOS_NET_OK, "the same pair joins once in range");
    guards_ok(&r, "boot fallback");

    // --- an ssid with no psk in the store --------------------------------
    rig_init(&r);
    fd_good(&r.d, "open", "");
    fs_save(&r.s, EOS_NET_KEY_SSID, "open");
    eos_net_init(&r.f.n, &r.cfg);
    CKI(eos_net_start(&r.f.n), EOS_NET_OK, "an ssid with no psk is an open network, not a fault");
    CKI(eos_net_mode(&r.f.n), EOS_NET_STA, "and it joins");
    CKS(r.d.last_join_psk, "", "with an empty passphrase");

    // --- start is once ---------------------------------------------------
    CKI(eos_net_start(&r.f.n), EOS_NET_ERR_STATE, "a second start is refused");

    // --- init refuses a driver that cannot do the job --------------------
    {
        eos_net_cfg_t bad = r.cfg;
        eos_net_t tmp;
        bad.drv.mac = NULL;
        CKI(eos_net_init(&tmp, &bad), EOS_NET_ERR_ARG, "init needs a mac");
        bad = r.cfg; bad.drv.sta_join = NULL;
        CKI(eos_net_init(&tmp, &bad), EOS_NET_ERR_ARG, "init needs a way to join");
        bad = r.cfg; bad.drv.ap_start = NULL;
        CKI(eos_net_init(&tmp, &bad), EOS_NET_ERR_ARG, "init needs a way to raise the AP");
        CKI(eos_net_init(NULL, &r.cfg), EOS_NET_ERR_ARG, "init needs a state");
        CKI(eos_net_init(&tmp, NULL), EOS_NET_ERR_ARG, "init needs a config");
    }
    guards_ok(&r, "boot edges");
}

static void test_ap_psk_persistence(void)
{
    rig_t r;
    char first[EOS_NET_AP_PSK_MAX];

    printf("  the SoftAP password is generated once\n");

    rig_init(&r);
    fd_good(&r.d, "x", "y");
    eos_net_init(&r.f.n, &r.cfg);
    eos_net_start(&r.f.n);
    snprintf(first, sizeof first, "%s", eos_net_ap_psk(&r.f.n));
    CK(eos_net_ap_psk_valid(first), "first boot generates one");
    CKS(fs_get(&r.s, EOS_NET_KEY_AP_PSK), first, "and stores it");
    CKI(ev_count(&r, EOS_NET_EV_AP_PSK), 1, "and says so once");

    // Second boot with the same store, and a driver whose entropy would give a
    // different answer: the stored password must win, because it is the one on
    // the sticker the user is reading.
    {
        fake_store_t keep = r.s;
        rig_init(&r);
        r.s = keep;
        r.d.entropy_seed = 0x99;
        fd_good(&r.d, "x", "y");
        eos_net_init(&r.f.n, &r.cfg);
        eos_net_start(&r.f.n);
        CKS(eos_net_ap_psk(&r.f.n), first, "the second boot reuses the stored password");
        CKS(r.d.last_ap_psk, first, "and raises the AP with it");
    }

    // A store that will not keep it is a degradation, not a failure: the AP
    // still comes up closed, with the password on the panel.
    rig_init(&r);
    r.s.fail_save = true;
    fd_good(&r.d, "x", "y");
    eos_net_init(&r.f.n, &r.cfg);
    CKI(eos_net_start(&r.f.n), EOS_NET_OK, "an unwritable store still reaches SETUP");
    CK(eos_net_ap_psk_valid(eos_net_ap_psk(&r.f.n)), "with a valid password");
    CKI(r.d.ap_starts, 1, "and the AP up");

    // No entropy source at all is a hard failure: an open AP is not a fallback.
    rig_init(&r);
    r.d.no_entropy = true;
    fd_good(&r.d, "x", "y");
    eos_net_init(&r.f.n, &r.cfg);
    CKI(eos_net_start(&r.f.n), EOS_NET_ERR_DRIVER,
        "with no entropy the AP is refused rather than opened");
    CKI(r.d.ap_starts, 0, "AN OPEN AP IS NEVER A FALLBACK");
    guards_ok(&r, "ap psk");
}

// ==================================================== scan cache and rescan

static void test_scan_cache(void)
{
    rig_t r;
    const eos_net_ap_t *list = NULL;

    printf("  scan caching\n");

    rig_init(&r);
    fd_good(&r.d, "house", "pw");
    r.d.scan_list[0] = mk("house", -55, 6, EOS_NET_AUTH_WPA2);
    r.d.scan_list[1] = mk("house", -40, 1, EOS_NET_AUTH_WPA2);
    r.d.scan_list[2] = mk("guest", -70, 11, EOS_NET_AUTH_OPEN);
    r.d.scan_n = 3;
    eos_net_init(&r.f.n, &r.cfg);
    eos_net_start(&r.f.n);

    CKI(r.d.scans, 1, "SETUP entry scans once");
    CKS(list ? list[0].ssid : "", "", "the borrowed pointer starts unset");
    if (EOS_NET_SCAN_MAX >= 3) {
        // Three raw sightings, two names. Only meaningful when the cache is
        // big enough to have received all three from the driver.
        CKI(eos_net_scan_results(&r.f.n, &list), 2, "and the cache is already reduced");
        CKS(list[0].ssid, "house", "strongest first");
        CKI(list[0].rssi, -40, "with the strongest sighting");
    } else {
        int n = eos_net_scan_results(&r.f.n, &list);
        CK(n >= 1 && n <= EOS_NET_SCAN_MAX,
           "a cache smaller than the scan still holds a legal, reduced result");
        CKS(list[0].ssid, "house", "of the networks it did see, the strongest first");
    }

    // The phone doing the setup IS an AP client, and a scan drops it. So an
    // unforced scan must not touch the radio.
    CKI(eos_net_scan(&r.f.n, false), EOS_NET_OK, "an unforced scan is satisfied from the cache");
    CKI(r.d.scans, 1, "AND DOES NOT TOUCH THE RADIO");
    CKI(eos_net_scan(&r.f.n, false), EOS_NET_OK, "however many times it is called");
    CKI(eos_net_scan(&r.f.n, false), EOS_NET_OK, "however many times it is called");
    CKI(r.d.scans, 1, "still one scan");

    // Rescan is explicit, and it does reach the radio.
    r.d.scan_list[0] = mk("moved", -33, 3, EOS_NET_AUTH_WPA3);
    r.d.scan_n = 1;
    CKI(eos_net_scan(&r.f.n, true), EOS_NET_OK, "a forced scan runs");
    CKI(r.d.scans, 2, "and reaches the radio");
    CKI(eos_net_scan_results(&r.f.n, &list), 1, "the cache is replaced, not appended to");
    CKS(list[0].ssid, "moved", "with the new result");

    // More networks than the cache holds.
    for (int i = 0; i < FD_SCAN; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "net%02d", i);
        r.d.scan_list[i] = mk(nm, -30 - i, 1, EOS_NET_AUTH_WPA2);
    }
    r.d.scan_n = FD_SCAN;
    CKI(eos_net_scan(&r.f.n, true), EOS_NET_OK, "an overfull scan runs");
    CKI(eos_net_scan_results(&r.f.n, &list),
        FD_SCAN < EOS_NET_SCAN_MAX ? FD_SCAN : EOS_NET_SCAN_MAX,
        "and is clamped to the cache size");
    CKS(list[0].ssid, "net00", "keeping the strongest");
    guards_ok(&r, "scan cache");

    // A driver that cannot scan is reported, and does not poison the cache.
    rig_init(&r);
    fd_good(&r.d, "house", "pw");
    r.d.scan_fails = true;
    eos_net_init(&r.f.n, &r.cfg);
    CKI(eos_net_start(&r.f.n), EOS_NET_OK, "a failing scan does not stop SETUP");
    CKI(eos_net_mode(&r.f.n), EOS_NET_SETUP, "the AP is still up and the page still loads");
    CK(!eos_net_scan_cached(&r.f.n), "and the cache stays empty rather than half-filled");
    CKI(eos_net_scan(&r.f.n, false), EOS_NET_ERR_DRIVER, "a retry re-attempts and reports");
    CKI(eos_net_scan_results(&r.f.n, &list), 0, "with nothing to show");

    CKI(eos_net_scan(NULL, false), EOS_NET_ERR_ARG, "a NULL state is an argument error");
    CKI(eos_net_scan_results(NULL, &list), 0, "and has no results");
    CK(list == NULL, "with a NULL list");
}

// ============================================================== pump, events

static void test_pump(void)
{
    rig_t r;
    uint32_t t = 100000;

    printf("  pump: deferred teardown, polling, rejoin\n");

    // The AP must outlive the HTTP response that reports the join, so commit
    // schedules the teardown rather than doing it.
    rig_init(&r);
    fd_good(&r.d, "house", "pw");
    eos_net_init(&r.f.n, &r.cfg);
    eos_net_start(&r.f.n);
    CKI(eos_net_mode(&r.f.n), EOS_NET_SETUP, "in SETUP");
    CKI(eos_net_try(&r.f.n, "house", "pw"), EOS_NET_OK, "the join works");
    CKI(eos_net_mode(&r.f.n), EOS_NET_SETUP,
        "AND THE MODE DOES NOT CHANGE — the panel keeps showing the instructions");
    CKI(r.d.ap_stops, 0, "the AP is still up");
    CKI(eos_net_commit(&r.f.n), EOS_NET_OK, "commit lands");
    CKI(r.d.ap_stops, 0, "AND STILL DOES NOT DROP THE AP — the reply has to get out");

    eos_net_pump(&r.f.n, t);
    CKI(r.d.ap_stops, 0, "the first pump only starts the clock");
    eos_net_pump(&r.f.n, t + 1000);
    CKI(r.d.ap_stops, 0, "1 s later it is still up");
    eos_net_pump(&r.f.n, t + 1600);
    CKI(r.d.ap_stops, 1, "past the delay the AP comes down");
    CKI(eos_net_mode(&r.f.n), EOS_NET_STA, "and the board is on the network");
    CKI(r.d.mdns, 1, "with mDNS started");
    eos_net_pump(&r.f.n, t + 5000);
    CKI(r.d.ap_stops, 1, "and it is only torn down once");
    guards_ok(&r, "deferred finish");

    // A link that drops means JOINING and a retry, never a SoftAP. Opening a
    // provisioning page every time the router reboots is not acceptable.
    rig_init(&r);
    fd_good(&r.d, "house", "pw");
    fs_save(&r.s, EOS_NET_KEY_SSID, "house");
    fs_save(&r.s, EOS_NET_KEY_PSK, "pw");
    eos_net_init(&r.f.n, &r.cfg);
    eos_net_start(&r.f.n);
    CKI(eos_net_mode(&r.f.n), EOS_NET_STA, "up");
    t = 500000;
    r.nev = 0;

    r.d.ip = 0;                      // the router went away
    r.d.joined = false;
    r.d.all_fail = true;
    eos_net_pump(&r.f.n, t + 3000);
    CKI(eos_net_mode(&r.f.n), EOS_NET_JOINING, "a lost address means JOINING");
    CKI(eos_net_bar_wifi(&r.f.n), 1, "and the bar says JOINING");
    CK(r.d.ap_starts == 0, "AND NOT A SOFTAP");
    CKI(ev_count(&r, EOS_NET_EV_IP), 1, "the address loss is an event");

    // It retries on a cadence, with the stored credentials.
    {
        int joins_before = r.d.joins;
        eos_net_pump(&r.f.n, t + 4000);
        CKI(r.d.joins, joins_before, "and does not hammer the radio");
        eos_net_pump(&r.f.n, t + 14000);
        CKI(r.d.joins, joins_before + 1, "it retries after the rejoin gap");
        CKS(r.d.last_join_ssid, "house", "with the stored network");
        CKS(r.d.last_join_psk, "pw", "and the stored passphrase");
    }
    CKI(eos_net_mode(&r.f.n), EOS_NET_JOINING, "still down while the router is off");

    // The router comes back.
    r.d.all_fail = false;
    eos_net_pump(&r.f.n, t + 30000);
    CKI(eos_net_mode(&r.f.n), EOS_NET_STA, "and the board comes back by itself");
    CKI(eos_net_ip(&r.f.n), 0xC0A80042u, "with an address");
    guards_ok(&r, "rejoin");

    // A failed try from STA costs the working connection — the platform tears
    // the old association down to attempt the new one, whatever the outcome.
    // The board must come back by itself, on the credentials still in NVS,
    // without a SoftAP and without a human.
    rig_init(&r);
    fd_good(&r.d, "house", "pw");
    fs_save(&r.s, EOS_NET_KEY_SSID, "house");
    fs_save(&r.s, EOS_NET_KEY_PSK, "pw");
    eos_net_init(&r.f.n, &r.cfg);
    eos_net_start(&r.f.n);
    CKI(eos_net_mode(&r.f.n), EOS_NET_STA, "on the network");
    t = 700000;
    r.s.saves = 0;   // discount the two writes that seeded the store
    CKI(eos_net_try(&r.f.n, "cafe", "guessed"), EOS_NET_ERR_JOIN, "a try at a new network fails");
    CKI(r.s.saves, 0, "writing nothing, as always");
    CKS(eos_net_ssid(&r.f.n), "house", "and not changing the network on record");
    CKS(fs_get(&r.s, EOS_NET_KEY_SSID), "house", "nor the one in the store");
    eos_net_pump(&r.f.n, t + 3000);
    CKI(eos_net_mode(&r.f.n), EOS_NET_JOINING, "the lost link shows as JOINING");
    CK(r.d.ap_starts == 0, "and still does not raise a SoftAP");
    eos_net_pump(&r.f.n, t + 20000);
    CKI(eos_net_mode(&r.f.n), EOS_NET_STA, "and the board rejoins the old network by itself");
    CKS(r.d.last_join_ssid, "house", "using the credentials that are still stored");
    guards_ok(&r, "failed try recovery");

    // RSSI hysteresis: small movements do not wake the renderer.
    rig_init(&r);
    fd_good(&r.d, "house", "pw");
    fs_save(&r.s, EOS_NET_KEY_SSID, "house");
    fs_save(&r.s, EOS_NET_KEY_PSK, "pw");
    eos_net_init(&r.f.n, &r.cfg);
    eos_net_start(&r.f.n);
    t = 900000;
    r.nev = 0;
    r.d.rssi = -48;
    eos_net_pump(&r.f.n, t + 3000);
    CKI(ev_count(&r, EOS_NET_EV_RSSI), 0, "a 1 dBm wobble raises nothing");
    r.d.rssi = -60;
    eos_net_pump(&r.f.n, t + 6000);
    CKI(ev_count(&r, EOS_NET_EV_RSSI), 1, "a real change does");
    CKI(eos_net_rssi(&r.f.n), -60, "and the value follows");

    eos_net_pump(NULL, t);
    CK(1, "a NULL pump is survivable");
    guards_ok(&r, "hysteresis");
}

static void test_bar_mapping(void)
{
    rig_t r;

    printf("  status bar mapping\n");

#if HAVE_BAR
    CKI(EOS_WIFI_OFF, 0, "eos_bar_wifi_t OFF is still 0");
    CKI(EOS_WIFI_JOINING, 1, "eos_bar_wifi_t JOINING is still 1");
    CKI(EOS_WIFI_DOWN, 2, "eos_bar_wifi_t DOWN is still 2");
    CKI(EOS_WIFI_UP, 3, "eos_bar_wifi_t UP is still 3");
    printf("    (pinned against the real eos_bar_wifi_t)\n");
#else
    printf("    (eos_bar.h not on the include path; values pinned as literals)\n");
#endif

    CKI(eos_net_bar_wifi(NULL), 0, "no service at all reads as OFF");

    rig_init(&r);
    fd_good(&r.d, "house", "pw");
    eos_net_init(&r.f.n, &r.cfg);
    CKI(eos_net_bar_wifi(&r.f.n), 0, "before start: OFF");
    eos_net_start(&r.f.n);
    CKI(eos_net_bar_wifi(&r.f.n), 2, "SETUP reads as DOWN");
    eos_net_try(&r.f.n, "house", "pw");
    eos_net_commit(&r.f.n);
    eos_net_pump(&r.f.n, 1000);
    eos_net_pump(&r.f.n, 3000);
    CKI(eos_net_bar_wifi(&r.f.n), 3, "STA with an address reads as UP");
    r.d.ip = 0;
    eos_net_pump(&r.f.n, 10000);
    CKI(eos_net_bar_wifi(&r.f.n), 1, "STA with no address reads as JOINING");
    guards_ok(&r, "bar mapping");
}

// =================================================== the captive-portal DNS

// A query for `name`, one label per dot, with the given qtype.
static size_t mk_query(uint8_t *b, const char *name, uint16_t qtype, uint16_t qclass)
{
    size_t at = 12;
    const char *p = name;

    memset(b, 0, 12);
    b[0] = 0x12; b[1] = 0x34;      // transaction id
    b[2] = 0x01; b[3] = 0x00;      // standard query, recursion desired
    b[4] = 0x00; b[5] = 0x01;      // QDCOUNT 1

    while (*p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        b[at++] = (uint8_t)len;
        memcpy(b + at, p, len);
        at += len;
        p = dot ? dot + 1 : p + len;
    }
    b[at++] = 0;
    b[at++] = (uint8_t)(qtype >> 8);   b[at++] = (uint8_t)(qtype & 0xFF);
    b[at++] = (uint8_t)(qclass >> 8);  b[at++] = (uint8_t)(qclass & 0xFF);
    return at;
}

static void test_dns(void)
{
    uint8_t q[EOS_NET_DNS_MAX], out[EOS_NET_DNS_MAX];
    size_t qlen;
    int n;

    printf("  captive portal DNS\n");

    // The whole point: any name at all resolves to the portal.
    {
        static const char *NAMES[] = {
            "captive.apple.com", "connectivitycheck.gstatic.com", "www.msftconnecttest.com",
            "penguinos-f048.local", "a", "example.com", "detectportal.firefox.com"
        };
        for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
            qlen = mk_query(q, NAMES[i], 1, 1);
            memset(out, 0, sizeof out);
            n = eos_net_dns_reply(q, qlen, out, sizeof out, EOS_NET_AP_IP);
            CK(n > 0, "an A query is answered");
            CKI(n, (int)qlen + 16, "the reply is the query plus one 16-byte A record");
            CK(out[0] == q[0] && out[1] == q[1], "the transaction id is echoed");
            CK((out[2] & 0x80) != 0, "the response bit is set");
            CK((out[2] & 0x04) != 0, "and the authoritative bit");
            CKI(out[3] & 0x0F, 0, "with rcode 0");
            CKI((out[4] << 8) | out[5], 1, "one question echoed");
            CKI((out[6] << 8) | out[7], 1, "and one answer");
            CK(memcmp(out + 12, q + 12, qlen - 12) == 0, "the question is echoed byte for byte");
            CK(out[qlen] == 0xC0 && out[qlen + 1] == 0x0C,
               "the answer names the question by compression pointer");
            CKI((out[qlen + 2] << 8) | out[qlen + 3], 1, "type A");
            CKI((out[qlen + 4] << 8) | out[qlen + 5], 1, "class IN");
            CKI((out[qlen + 10] << 8) | out[qlen + 11], 4, "rdlength 4");
            CK(out[qlen + 12] == 192 && out[qlen + 13] == 168 &&
               out[qlen + 14] == 4 && out[qlen + 15] == 1, "answering 192.168.4.1");
        }
    }

    // A different portal address comes back verbatim.
    qlen = mk_query(q, "a.b", 1, 1);
    n = eos_net_dns_reply(q, qlen, out, sizeof out, 0x0A000001u);
    CK(n > 0 && out[qlen + 12] == 10 && out[qlen + 15] == 1, "the answer address is the one passed in");

    // An AAAA lookup told 192.168.4.1 is how a phone ends up with a broken
    // route instead of a portal. Well formed, no answers.
    qlen = mk_query(q, "captive.apple.com", 28, 1);
    n = eos_net_dns_reply(q, qlen, out, sizeof out, EOS_NET_AP_IP);
    CKI(n, (int)qlen, "an AAAA query gets a reply with no answer section");
    CKI((out[6] << 8) | out[7], 0, "ANCOUNT is zero");
    CK((out[2] & 0x80) != 0, "and it is still a well-formed response");

    qlen = mk_query(q, "x.y", 15, 1);   // MX
    n = eos_net_dns_reply(q, qlen, out, sizeof out, EOS_NET_AP_IP);
    CKI((out[6] << 8) | out[7], 0, "so does an MX query");
    qlen = mk_query(q, "x.y", 255, 1);  // ANY
    n = eos_net_dns_reply(q, qlen, out, sizeof out, EOS_NET_AP_IP);
    CKI((out[6] << 8) | out[7], 1, "an ANY query is answered");
    qlen = mk_query(q, "x.y", 1, 3);    // class CHAOS
    n = eos_net_dns_reply(q, qlen, out, sizeof out, EOS_NET_AP_IP);
    CKI((out[6] << 8) | out[7], 0, "a non-IN class is not answered");

    // Malformed input. This code only ever sees packets from a stranger's
    // phone, so every one of these must be a refusal and not a read past the
    // end of the datagram.
    qlen = mk_query(q, "captive.apple.com", 1, 1);

    CKI(eos_net_dns_reply(NULL, qlen, out, sizeof out, EOS_NET_AP_IP), -1, "NULL query refused");
    CKI(eos_net_dns_reply(q, qlen, NULL, sizeof out, EOS_NET_AP_IP), -1, "NULL output refused");
    for (size_t i = 0; i < 12; i++)
        CKI(eos_net_dns_reply(q, i, out, sizeof out, EOS_NET_AP_IP), 0,
            "anything shorter than a header is ignored");
    CKI(eos_net_dns_reply(q, EOS_NET_DNS_MAX + 1, out, sizeof out, EOS_NET_AP_IP), 0,
        "an oversized datagram is ignored");

    // Every truncation of a valid query: no crash, no answer built from
    // whatever happened to be in the buffer.
    for (size_t cut = 12; cut < qlen; cut++) {
        n = eos_net_dns_reply(q, cut, out, sizeof out, EOS_NET_AP_IP);
        CK(n <= 0, "a truncated query is refused at every cut point");
    }

    {
        uint8_t bad[EOS_NET_DNS_MAX];

        memcpy(bad, q, qlen);
        bad[2] |= 0x80;   // already a response
        CKI(eos_net_dns_reply(bad, qlen, out, sizeof out, EOS_NET_AP_IP), 0,
            "a response is not answered");

        memcpy(bad, q, qlen);
        bad[5] = 2;       // QDCOUNT 2
        CKI(eos_net_dns_reply(bad, qlen, out, sizeof out, EOS_NET_AP_IP), 0,
            "a multi-question query is not answered");
        bad[4] = 0; bad[5] = 0;
        CKI(eos_net_dns_reply(bad, qlen, out, sizeof out, EOS_NET_AP_IP), 0,
            "and neither is one with no question");

        // A compression pointer in a question is the classic way to walk a
        // parser off the end of a buffer. Refused outright.
        memcpy(bad, q, qlen);
        bad[12] = 0xC0; bad[13] = 0x0C;
        CKI(eos_net_dns_reply(bad, qlen, out, sizeof out, EOS_NET_AP_IP), 0,
            "a compression pointer in the question is refused");
        bad[12] = 0x80;   // reserved label type
        CKI(eos_net_dns_reply(bad, qlen, out, sizeof out, EOS_NET_AP_IP), 0,
            "and so is a reserved label type");

        // A label claiming to run past the end of the datagram.
        memcpy(bad, q, qlen);
        bad[12] = 60;
        CKI(eos_net_dns_reply(bad, qlen, out, sizeof out, EOS_NET_AP_IP), 0,
            "a label that overruns the datagram is refused");

        // A name with no terminating zero: 400 one-byte labels, no root.
        memset(bad, 0, sizeof bad);
        bad[2] = 0x01; bad[5] = 0x01;
        for (size_t i = 12; i < 400; i += 2) { bad[i] = 1; bad[i + 1] = 'a'; }
        CKI(eos_net_dns_reply(bad, 400, out, sizeof out, EOS_NET_AP_IP), 0,
            "an unterminated name is refused");

        // A name longer than 255 bytes, correctly terminated.
        memset(bad, 0, sizeof bad);
        bad[2] = 0x01; bad[5] = 0x01;
        {
            size_t at = 12;
            for (int i = 0; i < 8; i++) { bad[at] = 40; memset(bad + at + 1, 'z', 40); at += 41; }
            bad[at++] = 0;
            bad[at++] = 0; bad[at++] = 1; bad[at++] = 0; bad[at++] = 1;
            CKI(eos_net_dns_reply(bad, at, out, sizeof out, EOS_NET_AP_IP), 0,
                "a name past 255 bytes is refused");
        }
    }

    // A short output buffer is refused, never half-written.
    qlen = mk_query(q, "captive.apple.com", 1, 1);
    {
        int full = eos_net_dns_reply(q, qlen, out, sizeof out, EOS_NET_AP_IP);
        for (int cap = 0; cap < full; cap++) {
            uint8_t small[EOS_NET_DNS_MAX];
            memset(small, 0x77, sizeof small);
            CKI(eos_net_dns_reply(q, qlen, small, (size_t)cap, EOS_NET_AP_IP), -1,
                "a short output buffer is refused");
            CKI(small[cap], 0x77, "and nothing is written past it");
        }
        CKI(eos_net_dns_reply(q, qlen, out, (size_t)full, EOS_NET_AP_IP), full,
            "exactly enough room succeeds");
    }

    // Random garbage: 20,000 datagrams that are not DNS. None may crash, none
    // may produce a reply longer than the buffer.
    {
        unsigned seed = 12345u, bad_len = 0;
        for (int i = 0; i < 20000; i++) {
            uint8_t junk[EOS_NET_DNS_MAX];
            size_t len;
            seed = seed * 1103515245u + 12345u;
            len = 1 + (seed >> 16) % (EOS_NET_DNS_MAX - 1);
            for (size_t k = 0; k < len; k++) {
                seed = seed * 1103515245u + 12345u;
                junk[k] = (uint8_t)(seed >> 20);
            }
            memset(out, 0, sizeof out);
            n = eos_net_dns_reply(junk, len, out, sizeof out, EOS_NET_AP_IP);
            if (n > (int)sizeof out) bad_len++;
        }
        CKI(bad_len, 0, "20,000 random datagrams never produce an over-long reply");
    }
}

// ========================================================== radio and misc

static void test_radio_contract(void)
{
    printf("  radio lock\n");
    if (eos_net_radio_serialised()) {
        printf("    bound: every scan and every join takes the shared radio lock\n");
        CK(1, "the radio lock is wired in");
    } else {
        printf("    NOT bound: nothing here is serialised against a BLE scan.\n");
        printf("    Build with -DEOS_NET_BIND_RADIO_LOCK against eos_ble.h, or land\n");
        printf("    eos_radio.h, before NimBLE and WiFi are up at the same time.\n");
        CK(!eos_net_radio_serialised(), "and eos_net_radio_serialised() says so honestly");
    }
}

int main(void)
{
    printf("\neos_net host test\n\n");
    printf("  sizeof(eos_net_t)    = %u bytes (BSS)\n", (unsigned)sizeof(eos_net_t));
    printf("  sizeof(eos_net_ap_t) = %u bytes x %d cached\n\n",
           (unsigned)sizeof(eos_net_ap_t), EOS_NET_SCAN_MAX);

    test_names();
    test_ap_name();
    test_ap_psk();
    test_keys();
    test_ip_str();
    test_scan_reduce();
    test_qr();
    test_try_never_persists();
    test_commit_store_failure();
    test_forget();
    test_boot();
    test_ap_psk_persistence();
    test_scan_cache();
    test_pump();
    test_bar_mapping();
    test_dns();
    test_radio_contract();

    printf("\n=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
