// eos_net — the network service, and specifically the answer to "this board is
// in a room whose WiFi it has never heard of".
//
// The web app is how you set the WiFi and the web app is served over the WiFi,
// so a board carried somewhere new is unreachable. The way out is that every
// board in this fleet has a screen. Boot takes one of three shapes: credentials
// in NVS and a join that lands inside a 15 s budget means RUN; anything else
// means SETUP — a WPA2 SoftAP named esp-os-<last4 of MAC> whose password was
// generated at first boot and is PRINTED ON THE PANEL, plus a captive-portal
// DNS responder that answers every query with 192.168.4.1 so joining the AP
// opens the page by itself. The screen is the out-of-band channel; that is the
// whole trick, and it is what lets the AP be closed rather than open.
//
// The one non-obvious constraint, and the most expensive thing in this file to
// get wrong: CREDENTIALS ARE PERSISTED ONLY AFTER A JOIN HAS SUCCEEDED. Saving
// first and trying second means one typo leaves the board booting forever into
// a network it cannot reach, recoverable only with a serial cable. That is why
// eos_net_try() and eos_net_commit() are two functions and not one — the
// ordering is a type-level fact a caller cannot get backwards, not a comment.
//
// Everything that touches hardware is behind eos_net_driver_t, the same trick
// eos_brain plays with its transport, so the entire state machine runs on the
// host with no radio, no lwIP and no NVS.

#ifndef EOS_NET_H
#define EOS_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---------------------------------------------------------------- tunables

#ifndef EOS_NET_SCAN_MAX
#define EOS_NET_SCAN_MAX 16        // networks kept from one scan, strongest first
#endif

#define EOS_NET_SSID_MAX     33    // 802.11 SSID is 32 octets; +1 for the NUL
#define EOS_NET_PSK_MAX      64    // WPA2 passphrase is 8..63 chars; +1
#define EOS_NET_AP_NAME_MAX  16    // "esp-os-" + 4 hex + NUL = 12
#define EOS_NET_AP_PSK_MAX   16
#define EOS_NET_HOSTNAME_MAX 32    // mDNS label, <name>.local
#define EOS_NET_QR_MAX       224   // a fully escaped WIFI: URI never exceeds this

// 12 characters out of a 32-symbol alphabet: 60 bits. Well past WPA2's 8-char
// minimum and still short enough to read off a 240x240 panel and type once.
#define EOS_NET_AP_PSK_LEN   12

// The SoftAP's own address, and the address the captive DNS responder hands
// back for every name. esp_netif's AP default; stated here so the QR payload,
// the portal and the status endpoint cannot disagree about it.
#define EOS_NET_AP_IP        0xC0A80401u   /* 192.168.4.1, host order */
#define EOS_NET_AP_IP_STR    "192.168.4.1"

#define EOS_NET_AP_PREFIX    "esp-os-"

// NVS. The namespace and all three keys are <= 15 characters, which is the
// hard NVS key limit; eos_net_key_ok() is the assertion of that.
#define EOS_NET_NVS_NS       "eos_net"
#define EOS_NET_KEY_SSID     "ssid"
#define EOS_NET_KEY_PSK      "psk"
#define EOS_NET_KEY_AP_PSK   "appsk"
#define EOS_NET_NVS_KEY_LIMIT 15

#if EOS_NET_SCAN_MAX < 1 || EOS_NET_SCAN_MAX > 4096
#error "EOS_NET_SCAN_MAX must fit eos_net_t.scan_n, an int16_t, and be non-empty"
#endif
#if EOS_NET_AP_PSK_LEN < 8 || EOS_NET_AP_PSK_LEN >= EOS_NET_AP_PSK_MAX
#error "EOS_NET_AP_PSK_LEN must be a legal WPA2 passphrase and fit EOS_NET_AP_PSK_MAX"
#endif

// ------------------------------------------------------------- the radio lock
//
// There is one radio. On the C6, WiFi, BLE and 802.15.4 all share it, and a BLE
// scan overlapping a WiFi scan is a hardware conflict rather than a data race —
// no amount of careful ordering inside either service fixes it. eos_net
// therefore takes a single process-wide lock around every scan and every join.
//
// That lock is not this file's to own; it belongs to whoever owns the radio,
// which is the BLE service. eos_net.c binds to it in one of two ways, in this
// order:
//
//   1. kernel/svc/include/eos_radio.h, if it exists — picked up automatically
//      by __has_include, and expected to declare
//
//          typedef enum { EOS_RADIO_WIFI = 0, EOS_RADIO_BLE } eos_radio_user_t;
//          bool eos_radio_acquire(eos_radio_user_t who, uint32_t timeout_ms);
//          void eos_radio_release(eos_radio_user_t who);
//
//   2. the lock the BLE service actually shipped, which lives at the tail of
//      eos_ble.h as eos_radio_lock(owner, wait_ms) / eos_radio_unlock(owner).
//      Compile eos_net.c with -DEOS_NET_BIND_RADIO_LOCK to use it. That one is
//      an opt-in and not a bare __has_include on purpose: eos_ble.h is on the
//      same include path as this header, so auto-detecting it would drag
//      eos_ble.c into the link of every host suite that wanted nothing but the
//      network state machine.
//
// With neither in play the calls compile out and eos_net serialises against
// nothing. That is a real gap and not a placeholder — harmless with the BLE
// service down, wrong the first time NimBLE scans — so
// eos_net_radio_serialised() reports the truth at runtime and the host test
// prints which of the three cases it built in.
#ifndef EOS_NET_RADIO_WAIT_MS
#define EOS_NET_RADIO_WAIT_MS 20000   // a blocking scan plus slack
#endif

bool eos_net_radio_serialised(void);

// ------------------------------------------------------------------- errors

typedef enum {
    EOS_NET_OK = 0,
    EOS_NET_ERR_ARG,        // caller passed something impossible
    EOS_NET_ERR_BUSY,       // a join is already in flight
    EOS_NET_ERR_TOO_LONG,   // ssid or psk does not fit the 802.11 limits
    EOS_NET_ERR_NO_CRED,    // nothing stored to join with
    EOS_NET_ERR_JOIN,       // the AP refused, or never answered inside the budget
    EOS_NET_ERR_NOT_TRIED,  // commit with no successful try behind it
    EOS_NET_ERR_STORE,      // NVS refused a read or a write
    EOS_NET_ERR_RADIO,      // could not take the radio lock
    EOS_NET_ERR_DRIVER,     // the platform said no: scan, ap_start, mac
    EOS_NET_ERR_STATE       // wrong mode for this call
} eos_net_err_t;

const char *eos_net_err_name(eos_net_err_t e);

// -------------------------------------------------------------------- modes

// The same four states the status bar already models. eos_net_bar_wifi() maps
// onto eos_bar_wifi_t numerically; the mapping is asserted in the host test
// rather than being enforced by an include, because kernel/shell must stay
// buildable with no service layer present and vice versa.
typedef enum {
    EOS_NET_OFF = 0,   // nothing started
    EOS_NET_JOINING,   // a join is in flight, or STA has lost its address
    EOS_NET_STA,       // on a real network, mDNS up
    EOS_NET_SETUP      // SoftAP + captive portal
} eos_net_mode_t;

const char *eos_net_mode_name(eos_net_mode_t m);

// Credential lifecycle. The whole point of this enum existing in the public
// header is that a caller can see, and a test can assert, that TRIED_OK is the
// only state from which anything reaches NVS.
typedef enum {
    EOS_NET_CRED_NONE = 0,   // nothing stored, nothing attempted
    EOS_NET_CRED_TRIED_OK,   // the last try joined; commit is legal exactly once
    EOS_NET_CRED_TRIED_FAIL, // the last try failed; commit must refuse
    EOS_NET_CRED_SAVED       // committed, or loaded from NVS at boot
} eos_net_cred_t;

const char *eos_net_cred_name(eos_net_cred_t c);

// -------------------------------------------------------------------- scans

typedef enum {
    EOS_NET_AUTH_OPEN = 0,
    EOS_NET_AUTH_WEP,
    EOS_NET_AUTH_WPA,
    EOS_NET_AUTH_WPA2,
    EOS_NET_AUTH_WPA_WPA2,
    EOS_NET_AUTH_WPA3,
    EOS_NET_AUTH_WPA2_WPA3,
    EOS_NET_AUTH_WAPI,
    EOS_NET_AUTH_ENTERPRISE,
    EOS_NET_AUTH_OTHER
} eos_net_auth_t;

const char *eos_net_auth_name(eos_net_auth_t a);
bool        eos_net_auth_needs_psk(eos_net_auth_t a);

typedef struct {
    char           ssid[EOS_NET_SSID_MAX];
    int8_t         rssi;      // dBm
    uint8_t        channel;
    eos_net_auth_t auth;
} eos_net_ap_t;

// Deduplicates by SSID keeping the strongest sighting, drops hidden networks
// (an empty SSID is not something a person can pick out of a list), and sorts
// by RSSI descending with SSID as the tie-break so the order is deterministic
// and a rescan does not shuffle the list under the user's thumb. In place.
// Returns the surviving count.
int eos_net_scan_reduce(eos_net_ap_t *aps, int count);

// ------------------------------------------------------------------- events
//
// The status bar and the setup screen both want to know the moment something
// changes, and neither should be polling a radio to find out.

typedef enum {
    EOS_NET_EV_MODE = 0,    // mode changed
    EOS_NET_EV_IP,          // address gained or lost
    EOS_NET_EV_RSSI,        // signal moved enough to redraw
    EOS_NET_EV_SCAN,        // the scan cache was refreshed
    EOS_NET_EV_TRY,         // an eos_net_try() finished; check eos_net_cred()
    EOS_NET_EV_COMMIT,      // credentials reached NVS
    EOS_NET_EV_FORGET,      // credentials were cleared
    EOS_NET_EV_AP_PSK       // the SoftAP password was generated or loaded
} eos_net_event_t;

const char *eos_net_event_name(eos_net_event_t e);

typedef struct eos_net_s eos_net_t;
typedef void (*eos_net_cb_t)(eos_net_event_t ev, const eos_net_t *n, void *ud);

// ---------------------------------------------------------------- the driver
//
// Ten function pointers. Everything below this line that touches silicon lives
// behind them, which is why the state machine is host-testable with no radio.
// A NULL entry is treated as "this platform cannot do that" and the caller gets
// EOS_NET_ERR_DRIVER rather than a crash — except sta_join, ap_start and mac,
// which eos_net_init() requires.
//
// sta_join blocks, bounded by budget_ms, and returns 0 only when the join
// completed AND an address was obtained. Half a join is a failure here.

typedef struct {
    int  (*sta_join)(void *ud, const char *ssid, const char *psk, uint32_t budget_ms);
    int  (*sta_leave)(void *ud);
    int  (*ap_start)(void *ud, const char *ssid, const char *psk);
    int  (*ap_stop)(void *ud);
    int  (*scan)(void *ud, eos_net_ap_t *out, int max);  // raw count, or <0
    int  (*mac)(void *ud, uint8_t out[6]);
    int  (*ip)(void *ud, uint32_t *out);                 // host order, 0 = none
    int  (*rssi)(void *ud, int8_t *out);
    int  (*mdns_start)(void *ud, const char *hostname);
    int  (*entropy)(void *ud, uint8_t *out, size_t n);
    void  *ud;
} eos_net_driver_t;

// ----------------------------------------------------------------- the store
//
// Three calls, because that is all NVS is being asked for. load returns the
// length written, or <0 when the key is absent — absent is not an error, it is
// the first-boot case and it is the common one.

typedef struct {
    int (*load)(void *ud, const char *key, char *out, size_t cap);
    int (*save)(void *ud, const char *key, const char *val);
    int (*erase)(void *ud, const char *key);
    void *ud;
} eos_net_store_t;

// True when `key` is something NVS will actually accept. Exposed because a key
// that is one character too long fails at runtime on the board and nowhere else.
bool eos_net_key_ok(const char *key);

// ----------------------------------------------------------------- the config

typedef struct {
    eos_net_driver_t drv;
    eos_net_store_t  store;

    uint32_t join_budget_ms;   // 15000, straight out of docs/provisioning.md
    uint32_t poll_ms;          // 2000: how often STA re-reads address and RSSI
    uint32_t rejoin_ms;        // 10000: gap between rejoin attempts after a drop
    uint32_t finish_delay_ms;  // 1500: see eos_net_commit()
    int8_t   rssi_hysteresis;  // 3 dBm; smaller moves do not raise an event

    // Empty means "derive it from the MAC", which gives esp-os-f048.local and
    // not six boards all claiming esp-os.local.
    char hostname[EOS_NET_HOSTNAME_MAX];

    eos_net_cb_t on_event;
    void        *cb_ud;
} eos_net_cfg_t;

void eos_net_cfg_init(eos_net_cfg_t *cfg);

// ------------------------------------------------------------------ the state
//
// Meant for BSS. About 1.2 KB, most of it the scan cache, and it allocates
// nothing — see the note on eos_net_idf_defaults() for the single exception on
// target, which is the captive-portal task's stack.

struct eos_net_s {
    eos_net_cfg_t  cfg;
    eos_net_mode_t mode;
    eos_net_cred_t cred;
    eos_net_err_t  last_err;

    char ssid[EOS_NET_SSID_MAX];       // the network in use, or last committed
    char psk[EOS_NET_PSK_MAX];         // kept for rejoin after a link drop
    char try_ssid[EOS_NET_SSID_MAX];   // the pair eos_net_try() attempted
    char try_psk[EOS_NET_PSK_MAX];

    char    ap_ssid[EOS_NET_AP_NAME_MAX];
    char    ap_psk[EOS_NET_AP_PSK_MAX];
    char    hostname[EOS_NET_HOSTNAME_MAX];
    uint8_t mac[6];

    uint32_t ip;
    int8_t   rssi;

    eos_net_ap_t scan[EOS_NET_SCAN_MAX];
    int16_t      scan_n;

    bool scan_valid;
    bool trying;        // a join is in flight; the bar shows JOINING regardless
    bool ap_up;
    bool mdns_up;
    bool started;
    bool ap_psk_saved;

    bool     finish_pending;
    uint32_t finish_at_ms;
    uint32_t next_poll_ms;
    uint32_t rejoin_at_ms;
};

// -------------------------------------------------------------------- the API

eos_net_err_t eos_net_init(eos_net_t *n, const eos_net_cfg_t *cfg);

// The three-state boot, exactly as docs/provisioning.md draws it. Blocks for at
// most cfg.join_budget_ms. Returns OK in both landing states — reaching SETUP
// because a join failed is a successful outcome, not an error — so read
// eos_net_mode() for where it ended up.
eos_net_err_t eos_net_start(eos_net_t *n);

// Attempts a join and reports the outcome. TOUCHES NOTHING PERSISTENT: whether
// this succeeds or fails, NVS is not written. Leaves the SoftAP up if it was
// up, because the phone that asked for this join is an AP client.
//
// IT BLOCKS, for up to cfg.join_budget_ms — fifteen seconds by default. Do not
// call it from inside an HTTP handler: the connection making the request comes
// through the same SoftAP the radio is about to re-mode, and a 15 s stall in a
// handler is a request the phone gives up on. Take the ssid and psk in the
// handler, answer immediately, and run the try from the OS loop.
eos_net_err_t eos_net_try(eos_net_t *n, const char *ssid, const char *psk);

// Persists the pair eos_net_try() just proved. Refuses with ERR_NOT_TRIED in
// every other case: no try, a failed try, or a second commit of the same
// success. Consuming the success is deliberate — it means a stale OK cannot be
// laundered into a write after a later attempt failed.
//
// Schedules, but does not perform, the drop to STA: cfg.finish_delay_ms later
// eos_net_pump() tears the AP down. Doing it inside this call would kill the
// HTTP response on its way to the phone that asked for it.
eos_net_err_t eos_net_commit(eos_net_t *n);

// Clears the stored network and drops back to SETUP. The SoftAP password is
// deliberately kept: it is on the panel and in NVS, and rerolling it would make
// the screen the user is looking at wrong.
eos_net_err_t eos_net_forget(eos_net_t *n);

// Cached unless `force`. The cache is filled once when SETUP starts, because a
// scan puts the radio in APSTA and briefly drops AP clients — and the phone
// doing the setup IS an AP client. Rescan is therefore an explicit act.
eos_net_err_t eos_net_scan(eos_net_t *n, bool force);

// Borrowed, valid until the next scan. Returns the count.
int eos_net_scan_results(const eos_net_t *n, const eos_net_ap_t **out);
bool eos_net_scan_cached(const eos_net_t *n);

// Drives the deferred AP teardown, the address and RSSI polling, and the rejoin
// after a link drop. Call it from the OS loop with a millisecond clock. It
// never blocks for longer than one sta_join budget, and only when rejoining.
void eos_net_pump(eos_net_t *n, uint32_t now_ms);

// Performs the AP teardown and the move to STA immediately, for a caller that
// would rather sequence it itself than wait for the pump.
eos_net_err_t eos_net_finish(eos_net_t *n);

eos_net_mode_t eos_net_mode(const eos_net_t *n);
eos_net_cred_t eos_net_cred(const eos_net_t *n);

// True when a network is stored — loaded at boot or committed since. Distinct
// from eos_net_cred(), which tracks the last try and is therefore TRIED_FAIL
// after a failed attempt at a second network even though the first one is still
// safely in NVS.
bool eos_net_has_credentials(const eos_net_t *n);

// True while a join is in flight. The mode does not change during a try from
// SETUP; this is what does.
bool eos_net_trying(const eos_net_t *n);
eos_net_err_t  eos_net_last_error(const eos_net_t *n);
uint32_t       eos_net_ip(const eos_net_t *n);
int8_t         eos_net_rssi(const eos_net_t *n);
const char    *eos_net_ssid(const eos_net_t *n);
const char    *eos_net_ap_ssid(const eos_net_t *n);
const char    *eos_net_ap_psk(const eos_net_t *n);
const char    *eos_net_hostname(const eos_net_t *n);

// Dotted quad into `out`. Returns out. Needs 16 bytes.
char *eos_net_ip_str(uint32_t ip, char *out, size_t cap);

// The eos_bar_wifi_t value for the current state, as a plain int so the service
// layer does not have to include the shell's header:
//   0 EOS_WIFI_OFF   1 EOS_WIFI_JOINING   2 EOS_WIFI_DOWN   3 EOS_WIFI_UP
// SETUP maps to DOWN: the radio is doing something, but the board is not on a
// network the user can reach it on.
int eos_net_bar_wifi(const eos_net_t *n);

// ------------------------------------------------------------------- helpers
//
// Pure, no state, and separately testable — which is the reason they are public
// rather than static.

// "esp-os-f048" from the last two octets of the MAC, lowercase hex. Returns out.
char *eos_net_ap_name(const uint8_t mac[6], char *out, size_t cap);

// EOS_NET_AP_PSK_LEN characters from a 32-symbol alphabet, one input byte per
// character, low 5 bits — 256 is a multiple of 32, so this is uniform without
// rejection sampling. Needs `n` >= EOS_NET_AP_PSK_LEN bytes of real entropy.
// Returns 0, or <0 if the entropy or the output buffer is short.
int eos_net_ap_psk_from_entropy(const uint8_t *entropy, size_t n,
                                char *out, size_t cap);

// True if every character came from that alphabet and the length is right.
bool eos_net_ap_psk_valid(const char *psk);

// The payload a phone camera joins an AP from:
//   WIFI:S:esp-os-f048;T:WPA;P:hqm4x7bt2fkd;;
// Backslash-escapes the four characters the format reserves. Returns the length
// written, or <0 if it will not fit. `out` wants EOS_NET_QR_MAX.
int eos_net_wifi_qr(const char *ssid, const char *psk, char *out, size_t cap);

// The same, for the AP this board is currently serving.
int eos_net_ap_qr(const eos_net_t *n, char *out, size_t cap);

// ------------------------------------------------- the captive-portal DNS
//
// Answers every question with EOS_NET_AP_IP so that joining the SoftAP opens
// the setup page by itself on iOS and Android. Without it the user has to know
// to type an IP, and they will not.
//
// Split out as a pure builder because DNS message parsing is exactly the kind
// of code that is wrong on a malformed packet and untested on the board. Feed
// it the received datagram; it writes the reply. A question of a type other
// than A gets a well-formed reply with no answers rather than a lie, which is
// what stops an AAAA lookup from being told 192.168.4.1.
//
// Returns the reply length, 0 for "do not answer this", or <0 on a buffer
// problem. `out` wants EOS_NET_DNS_MAX.
#define EOS_NET_DNS_MAX 512
int eos_net_dns_reply(const uint8_t *query, size_t qlen,
                      uint8_t *out, size_t cap, uint32_t ip);

// --------------------------------------------------------- platform bindings
//
// ESP-IDF only; the host test never sees them. eos_net_idf_defaults() fills in
// both the driver and the NVS store.
//
// THE ONE ALLOCATION IN THIS FILE, declared as the rules require: on target,
// bringing up the SoftAP also starts a captive-portal task (one 3 KB stack,
// created once, torn down with the AP) and esp_netif's DHCP server. Nothing
// else here allocates, ever.
#ifdef ESP_PLATFORM
void eos_net_idf_driver(eos_net_driver_t *out);
void eos_net_idf_store(eos_net_store_t *out);
void eos_net_idf_defaults(eos_net_cfg_t *cfg);
#endif

#endif
