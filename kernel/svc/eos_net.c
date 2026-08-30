// eos_net — implementation. See eos_net.h for why any of this exists.
//
// Everything above the ESP_PLATFORM section is portable C99 with no radio, no
// sockets and no NVS, which is how the credential state machine — the part
// that must not be wrong — is tested exhaustively on the host. The one rule
// this file exists to enforce is that nothing reaches the store except through
// eos_net_commit(), and eos_net_commit() refuses unless a join actually landed.

#include "eos_net.h"

#include <string.h>
#include <stdio.h>

// The shared radio lock. See the long note in eos_net.h for the two spellings
// this binds to and why the eos_ble.h one is an explicit opt-in rather than a
// bare __has_include: eos_ble.h sits on the same include path as this file, so
// detecting it automatically would drag eos_ble.c into the link of every host
// suite that only wanted the network state machine.
#if defined(__has_include)
#  if __has_include("eos_radio.h")
#    include "eos_radio.h"
#    define EOS_NET_HAVE_RADIO 1
#    define EOS_NET_RADIO_VIA_BLE 0
#  endif
#endif
#if !defined(EOS_NET_HAVE_RADIO) && defined(EOS_NET_BIND_RADIO_LOCK)
#  include "eos_ble.h"
#  define EOS_NET_HAVE_RADIO 1
#  define EOS_NET_RADIO_VIA_BLE 1
#endif
#ifndef EOS_NET_HAVE_RADIO
#define EOS_NET_HAVE_RADIO 0
#define EOS_NET_RADIO_VIA_BLE 0
#endif

#define EOS_NET_RADIO_OWNER "wifi"

bool eos_net_radio_serialised(void) { return EOS_NET_HAVE_RADIO ? true : false; }

static bool radio_take(void)
{
#if EOS_NET_HAVE_RADIO && EOS_NET_RADIO_VIA_BLE
    return eos_radio_lock(EOS_NET_RADIO_OWNER, EOS_NET_RADIO_WAIT_MS);
#elif EOS_NET_HAVE_RADIO
    return eos_radio_acquire(EOS_RADIO_WIFI, EOS_NET_RADIO_WAIT_MS);
#else
    return true;
#endif
}

static void radio_give(void)
{
#if EOS_NET_HAVE_RADIO && EOS_NET_RADIO_VIA_BLE
    eos_radio_unlock(EOS_NET_RADIO_OWNER);
#elif EOS_NET_HAVE_RADIO
    eos_radio_release(EOS_RADIO_WIFI);
#endif
}

// ------------------------------------------------------------------- names

const char *eos_net_err_name(eos_net_err_t e)
{
    switch (e) {
    case EOS_NET_OK:             return "ok";
    case EOS_NET_ERR_ARG:        return "arg";
    case EOS_NET_ERR_BUSY:       return "busy";
    case EOS_NET_ERR_TOO_LONG:   return "too_long";
    case EOS_NET_ERR_NO_CRED:    return "no_cred";
    case EOS_NET_ERR_JOIN:       return "join";
    case EOS_NET_ERR_NOT_TRIED:  return "not_tried";
    case EOS_NET_ERR_STORE:      return "store";
    case EOS_NET_ERR_RADIO:      return "radio";
    case EOS_NET_ERR_DRIVER:     return "driver";
    case EOS_NET_ERR_STATE:      return "state";
    }
    return "?";
}

const char *eos_net_mode_name(eos_net_mode_t m)
{
    switch (m) {
    case EOS_NET_OFF:     return "off";
    case EOS_NET_JOINING: return "joining";
    case EOS_NET_STA:     return "sta";
    case EOS_NET_SETUP:   return "setup";
    }
    return "?";
}

const char *eos_net_cred_name(eos_net_cred_t c)
{
    switch (c) {
    case EOS_NET_CRED_NONE:       return "none";
    case EOS_NET_CRED_TRIED_OK:   return "tried_ok";
    case EOS_NET_CRED_TRIED_FAIL: return "tried_fail";
    case EOS_NET_CRED_SAVED:      return "saved";
    }
    return "?";
}

const char *eos_net_auth_name(eos_net_auth_t a)
{
    switch (a) {
    case EOS_NET_AUTH_OPEN:       return "open";
    case EOS_NET_AUTH_WEP:        return "wep";
    case EOS_NET_AUTH_WPA:        return "wpa";
    case EOS_NET_AUTH_WPA2:       return "wpa2";
    case EOS_NET_AUTH_WPA_WPA2:   return "wpa/wpa2";
    case EOS_NET_AUTH_WPA3:       return "wpa3";
    case EOS_NET_AUTH_WPA2_WPA3:  return "wpa2/wpa3";
    case EOS_NET_AUTH_WAPI:       return "wapi";
    case EOS_NET_AUTH_ENTERPRISE: return "enterprise";
    case EOS_NET_AUTH_OTHER:      return "other";
    }
    return "?";
}

// OPEN is the only one that takes no passphrase. ENTERPRISE takes a great deal
// more than one and this service does not do it, but it does not take a plain
// PSK either, so the form must not offer one.
bool eos_net_auth_needs_psk(eos_net_auth_t a)
{
    return a != EOS_NET_AUTH_OPEN && a != EOS_NET_AUTH_ENTERPRISE;
}

const char *eos_net_event_name(eos_net_event_t e)
{
    switch (e) {
    case EOS_NET_EV_MODE:   return "mode";
    case EOS_NET_EV_IP:     return "ip";
    case EOS_NET_EV_RSSI:   return "rssi";
    case EOS_NET_EV_SCAN:   return "scan";
    case EOS_NET_EV_TRY:    return "try";
    case EOS_NET_EV_COMMIT: return "commit";
    case EOS_NET_EV_FORGET: return "forget";
    case EOS_NET_EV_AP_PSK: return "ap_psk";
    }
    return "?";
}

// ----------------------------------------------------------------- helpers

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!cap) return;
    if (src) for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void emit(eos_net_t *n, eos_net_event_t ev)
{
    if (n->cfg.on_event) n->cfg.on_event(ev, n, n->cfg.cb_ud);
}

static void set_mode(eos_net_t *n, eos_net_mode_t m)
{
    if (n->mode == m) return;
    n->mode = m;
    emit(n, EOS_NET_EV_MODE);
}

static void set_ip(eos_net_t *n, uint32_t ip)
{
    if (n->ip == ip) return;
    n->ip = ip;
    emit(n, EOS_NET_EV_IP);
}

bool eos_net_key_ok(const char *key)
{
    size_t len;
    if (!key) return false;
    len = strlen(key);
    return len > 0 && len <= EOS_NET_NVS_KEY_LIMIT;
}

char *eos_net_ip_str(uint32_t ip, char *out, size_t cap)
{
    if (!out || cap == 0) return out;
    snprintf(out, cap, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFFu), (unsigned)((ip >> 16) & 0xFFu),
             (unsigned)((ip >> 8) & 0xFFu),  (unsigned)(ip & 0xFFu));
    return out;
}

// ------------------------------------------------------------- ap identity

char *eos_net_ap_name(const uint8_t mac[6], char *out, size_t cap)
{
    static const char HEX[] = "0123456789abcdef";
    const char *p = EOS_NET_AP_PREFIX;
    size_t i = 0;

    if (!out || cap == 0) return out;
    for (; *p && i + 1 < cap; p++) out[i++] = *p;
    if (mac) {
        const char nib[4] = { HEX[(mac[4] >> 4) & 0xF], HEX[mac[4] & 0xF],
                              HEX[(mac[5] >> 4) & 0xF], HEX[mac[5] & 0xF] };
        for (int k = 0; k < 4 && i + 1 < cap; k++) out[i++] = nib[k];
    }
    out[i] = 0;
    return out;
}

// 32 symbols: digits 2-9 and a-z without l and o. 1, 0, I and O are absent
// entirely, so nothing here can be misread off a 240x240 panel. Exactly 32
// means the low 5 bits of a uniform byte are a uniform symbol, with no
// rejection loop that could spin on a bad entropy source.
static const char AP_PSK_ALPHABET[] = "23456789abcdefghijkmnpqrstuvwxyz";

int eos_net_ap_psk_from_entropy(const uint8_t *entropy, size_t n,
                                char *out, size_t cap)
{
    size_t i;
    if (!entropy || !out) return -1;
    if (n < (size_t)EOS_NET_AP_PSK_LEN) return -1;
    if (cap < (size_t)EOS_NET_AP_PSK_LEN + 1) return -1;
    for (i = 0; i < (size_t)EOS_NET_AP_PSK_LEN; i++)
        out[i] = AP_PSK_ALPHABET[entropy[i] & 0x1Fu];
    out[i] = 0;
    return 0;
}

bool eos_net_ap_psk_valid(const char *psk)
{
    size_t i;
    if (!psk) return false;
    if (strlen(psk) != (size_t)EOS_NET_AP_PSK_LEN) return false;
    for (i = 0; psk[i]; i++)
        if (!strchr(AP_PSK_ALPHABET, psk[i])) return false;
    return true;
}

// ---------------------------------------------------------------- the QR
//
// The WIFI: URI reserves \ ; , : and escapes them with a backslash. Our own
// generated SSID and password contain none of them; a hand-set hostname or a
// third-party AP could, so it is done properly rather than assumed.

static int qr_put(char *out, size_t cap, size_t at, const char *s, bool escape)
{
    size_t i = at;
    for (; *s; s++) {
        if (escape && (*s == '\\' || *s == ';' || *s == ',' || *s == ':' || *s == '"')) {
            if (i + 1 >= cap) return -1;
            out[i++] = '\\';
        }
        if (i + 1 >= cap) return -1;
        out[i++] = *s;
    }
    return (int)i;
}

int eos_net_wifi_qr(const char *ssid, const char *psk, char *out, size_t cap)
{
    int at;
    if (!ssid || !out || cap == 0) return -1;
    if (!ssid[0]) return -1;

    at = qr_put(out, cap, 0, "WIFI:S:", false);
    if (at < 0) return -1;
    at = qr_put(out, cap, (size_t)at, ssid, true);
    if (at < 0) return -1;

    // An open AP is T:nopass with no P field. The board never serves one, but
    // the encoder is also what the setup page uses to show a joined network.
    if (psk && psk[0]) {
        at = qr_put(out, cap, (size_t)at, ";T:WPA;P:", false);
        if (at < 0) return -1;
        at = qr_put(out, cap, (size_t)at, psk, true);
        if (at < 0) return -1;
        at = qr_put(out, cap, (size_t)at, ";;", false);
    } else {
        at = qr_put(out, cap, (size_t)at, ";T:nopass;;", false);
    }
    if (at < 0) return -1;
    out[at] = 0;
    return at;
}

int eos_net_ap_qr(const eos_net_t *n, char *out, size_t cap)
{
    if (!n) return -1;
    return eos_net_wifi_qr(n->ap_ssid, n->ap_psk, out, cap);
}

// ------------------------------------------------------------ scan reduce

// Strongest first; SSID as the tie-break so two APs on the same reading do not
// swap places between two scans and make the list jump under a thumb.
static int ap_cmp(const eos_net_ap_t *a, const eos_net_ap_t *b)
{
    if (a->rssi != b->rssi) return a->rssi > b->rssi ? -1 : 1;
    return strcmp(a->ssid, b->ssid);
}

int eos_net_scan_reduce(eos_net_ap_t *aps, int count)
{
    int keep = 0, i, j;

    if (!aps || count <= 0) return 0;

    for (i = 0; i < count; i++) {
        aps[i].ssid[EOS_NET_SSID_MAX - 1] = 0;

        // A hidden network has nothing to show in a picker and every hidden
        // network would collapse into the same row. Drop them.
        if (!aps[i].ssid[0]) continue;

        for (j = 0; j < keep; j++) {
            if (strcmp(aps[j].ssid, aps[i].ssid) != 0) continue;
            // Same name, seen twice. Keep the stronger sighting whole — its
            // channel and auth mode go with its RSSI, not with the other one's.
            if (aps[i].rssi > aps[j].rssi) aps[j] = aps[i];
            break;
        }
        if (j < keep) continue;
        if (keep != i) aps[keep] = aps[i];
        keep++;
    }

    // Insertion sort: keep is at most EOS_NET_SCAN_MAX, and this is the whole
    // reason there is no qsort and no comparison callback in a kernel file.
    for (i = 1; i < keep; i++) {
        eos_net_ap_t t = aps[i];
        for (j = i; j > 0 && ap_cmp(&aps[j - 1], &t) > 0; j--) aps[j] = aps[j - 1];
        aps[j] = t;
    }
    return keep;
}

// ------------------------------------------------------------------- config

void eos_net_cfg_init(eos_net_cfg_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->join_budget_ms  = 15000;   // docs/provisioning.md
    cfg->poll_ms         = 2000;
    cfg->rejoin_ms       = 10000;
    cfg->finish_delay_ms = 1500;
    cfg->rssi_hysteresis = 3;
}

// -------------------------------------------------------------------- store

static int store_load(eos_net_t *n, const char *key, char *out, size_t cap)
{
    if (cap) out[0] = 0;
    if (!n->cfg.store.load || !eos_net_key_ok(key)) return -1;
    return n->cfg.store.load(n->cfg.store.ud, key, out, cap);
}

static bool store_save(eos_net_t *n, const char *key, const char *val)
{
    if (!n->cfg.store.save || !eos_net_key_ok(key)) return false;
    return n->cfg.store.save(n->cfg.store.ud, key, val) >= 0;
}

static void store_erase(eos_net_t *n, const char *key)
{
    if (n->cfg.store.erase && eos_net_key_ok(key))
        n->cfg.store.erase(n->cfg.store.ud, key);
}

// ------------------------------------------------------------- the SoftAP

// Generated once, at the first boot that needs it, and kept. Rerolling it on
// every entry to SETUP would make the password already printed on the panel —
// and already typed into a phone — wrong.
static eos_net_err_t ensure_ap_psk(eos_net_t *n)
{
    uint8_t ent[EOS_NET_AP_PSK_LEN];
    char stored[EOS_NET_AP_PSK_MAX];

    if (eos_net_ap_psk_valid(n->ap_psk)) return EOS_NET_OK;

    if (store_load(n, EOS_NET_KEY_AP_PSK, stored, sizeof stored) >= 0 &&
        eos_net_ap_psk_valid(stored)) {
        copy_str(n->ap_psk, sizeof n->ap_psk, stored);
        n->ap_psk_saved = true;
        emit(n, EOS_NET_EV_AP_PSK);
        return EOS_NET_OK;
    }

    if (!n->cfg.drv.entropy) return EOS_NET_ERR_DRIVER;
    if (n->cfg.drv.entropy(n->cfg.drv.ud, ent, sizeof ent) < 0) return EOS_NET_ERR_DRIVER;
    if (eos_net_ap_psk_from_entropy(ent, sizeof ent, n->ap_psk, sizeof n->ap_psk) < 0)
        return EOS_NET_ERR_DRIVER;

    // A store that refuses is not fatal: the AP still comes up with a password
    // the panel is showing. It is only next boot that gets a different one.
    n->ap_psk_saved = store_save(n, EOS_NET_KEY_AP_PSK, n->ap_psk);
    emit(n, EOS_NET_EV_AP_PSK);
    return EOS_NET_OK;
}

static eos_net_err_t enter_setup(eos_net_t *n)
{
    eos_net_err_t e;

    if (n->cfg.drv.sta_leave) n->cfg.drv.sta_leave(n->cfg.drv.ud);

    e = ensure_ap_psk(n);
    if (e != EOS_NET_OK) { n->last_err = e; return e; }

    if (!n->ap_up) {
        if (!n->cfg.drv.ap_start) { n->last_err = EOS_NET_ERR_DRIVER; return EOS_NET_ERR_DRIVER; }
        if (n->cfg.drv.ap_start(n->cfg.drv.ud, n->ap_ssid, n->ap_psk) < 0) {
            n->last_err = EOS_NET_ERR_DRIVER;
            return EOS_NET_ERR_DRIVER;
        }
        n->ap_up = true;
    }

    set_ip(n, EOS_NET_AP_IP);
    n->rssi = 0;
    set_mode(n, EOS_NET_SETUP);

    // One scan, cached, at the moment SETUP starts. Doing it here rather than
    // when the page asks means the list is already there when the phone loads,
    // and the APSTA blip that a scan costs happens before the phone is on.
    eos_net_scan(n, false);
    return EOS_NET_OK;
}

// ---------------------------------------------------------------------- init

eos_net_err_t eos_net_init(eos_net_t *n, const eos_net_cfg_t *cfg)
{
    if (!n || !cfg) return EOS_NET_ERR_ARG;
    if (!cfg->drv.sta_join || !cfg->drv.ap_start || !cfg->drv.mac) return EOS_NET_ERR_ARG;

    memset(n, 0, sizeof *n);
    n->cfg  = *cfg;
    n->mode = EOS_NET_OFF;
    n->cred = EOS_NET_CRED_NONE;

    if (!n->cfg.join_budget_ms)  n->cfg.join_budget_ms  = 15000;
    if (!n->cfg.poll_ms)         n->cfg.poll_ms         = 2000;
    if (!n->cfg.rejoin_ms)       n->cfg.rejoin_ms       = 10000;
    if (!n->cfg.finish_delay_ms) n->cfg.finish_delay_ms = 1500;
    if (n->cfg.rssi_hysteresis <= 0) n->cfg.rssi_hysteresis = 3;

    if (n->cfg.drv.mac(n->cfg.drv.ud, n->mac) < 0) {
        memset(n->mac, 0, sizeof n->mac);
        n->last_err = EOS_NET_ERR_DRIVER;
        return EOS_NET_ERR_DRIVER;
    }

    eos_net_ap_name(n->mac, n->ap_ssid, sizeof n->ap_ssid);

    // An empty configured hostname means derive it, so six boards on one LAN
    // are six distinct .local names and not one collision.
    if (n->cfg.hostname[0]) copy_str(n->hostname, sizeof n->hostname, n->cfg.hostname);
    else                    copy_str(n->hostname, sizeof n->hostname, n->ap_ssid);

    return EOS_NET_OK;
}

// --------------------------------------------------------------- the join

static void note_link(eos_net_t *n)
{
    uint32_t ip = 0;
    int8_t r = 0;
    if (n->cfg.drv.ip   && n->cfg.drv.ip(n->cfg.drv.ud, &ip)  < 0) ip = 0;
    if (n->cfg.drv.rssi && n->cfg.drv.rssi(n->cfg.drv.ud, &r) < 0) r  = 0;
    set_ip(n, ip);
    n->rssi = r;
}

static eos_net_err_t do_join(eos_net_t *n, const char *ssid, const char *psk)
{
    int r;

    if (!radio_take()) { n->last_err = EOS_NET_ERR_RADIO; return EOS_NET_ERR_RADIO; }
    r = n->cfg.drv.sta_join(n->cfg.drv.ud, ssid, psk ? psk : "", n->cfg.join_budget_ms);
    radio_give();

    if (r < 0) { n->last_err = EOS_NET_ERR_JOIN; return EOS_NET_ERR_JOIN; }
    return EOS_NET_OK;
}

static void start_mdns(eos_net_t *n)
{
    if (n->mdns_up || !n->cfg.drv.mdns_start) return;
    if (n->cfg.drv.mdns_start(n->cfg.drv.ud, n->hostname) >= 0) n->mdns_up = true;
}

eos_net_err_t eos_net_start(eos_net_t *n)
{
    char ssid[EOS_NET_SSID_MAX], psk[EOS_NET_PSK_MAX];

    if (!n) return EOS_NET_ERR_ARG;
    if (n->started) return EOS_NET_ERR_STATE;
    n->started = true;

    if (store_load(n, EOS_NET_KEY_SSID, ssid, sizeof ssid) >= 0 && ssid[0]) {
        if (store_load(n, EOS_NET_KEY_PSK, psk, sizeof psk) < 0) psk[0] = 0;

        copy_str(n->ssid, sizeof n->ssid, ssid);
        copy_str(n->psk,  sizeof n->psk,  psk);
        n->cred = EOS_NET_CRED_SAVED;

        n->trying = true;
        set_mode(n, EOS_NET_JOINING);
        if (do_join(n, ssid, psk) == EOS_NET_OK) {
            n->trying = false;
            note_link(n);
            start_mdns(n);
            set_mode(n, EOS_NET_STA);
            n->next_poll_ms = n->cfg.poll_ms;
            return EOS_NET_OK;
        }
        n->trying = false;

        // The join failed. The credentials STAY: out of range is not the same
        // as wrong, and wiping a good password because the router was off is
        // not a recovery, it is a second failure. SETUP is how the user
        // corrects it, and eos_net_forget() is how they clear it deliberately.
    }

    return enter_setup(n);
}

// ------------------------------------------------------- try, then commit

eos_net_err_t eos_net_try(eos_net_t *n, const char *ssid, const char *psk)
{
    eos_net_err_t e;

    if (!n || !ssid || !ssid[0]) return EOS_NET_ERR_ARG;
    if (n->trying) return EOS_NET_ERR_BUSY;
    if (strlen(ssid) >= sizeof n->try_ssid) return EOS_NET_ERR_TOO_LONG;
    if (psk && strlen(psk) >= sizeof n->try_psk) return EOS_NET_ERR_TOO_LONG;

    copy_str(n->try_ssid, sizeof n->try_ssid, ssid);
    copy_str(n->try_psk,  sizeof n->try_psk,  psk);

    // Mode is deliberately NOT changed here. In SETUP the panel is showing the
    // AP name, the password and the QR, and the phone reading them is still an
    // AP client — flipping the screen to JOINING mid-attempt would take the
    // instructions away from the person following them. eos_net_bar_wifi()
    // reports JOINING from the `trying` flag instead.
    n->trying = true;
    e = do_join(n, n->try_ssid, n->try_psk);
    n->trying = false;

    if (e == EOS_NET_OK) {
        n->cred = EOS_NET_CRED_TRIED_OK;
    } else {
        n->cred = EOS_NET_CRED_TRIED_FAIL;
        // Stop the platform quietly retrying a network we have just proved is
        // not joinable; it would fight the SoftAP for the radio forever.
        if (n->cfg.drv.sta_leave) n->cfg.drv.sta_leave(n->cfg.drv.ud);
    }

    // Not one byte has reached the store on either path. That is the rule.
    emit(n, EOS_NET_EV_TRY);
    return e;
}

eos_net_err_t eos_net_commit(eos_net_t *n)
{
    if (!n) return EOS_NET_ERR_ARG;

    // The only gate. NONE, TRIED_FAIL and SAVED all land here, which means a
    // commit with no try behind it, a commit after a failure, and a second
    // commit of an already-consumed success are the same refusal.
    if (n->cred != EOS_NET_CRED_TRIED_OK) {
        n->last_err = EOS_NET_ERR_NOT_TRIED;
        return EOS_NET_ERR_NOT_TRIED;
    }
    if (!n->cfg.store.save) { n->last_err = EOS_NET_ERR_STORE; return EOS_NET_ERR_STORE; }

    if (!store_save(n, EOS_NET_KEY_SSID, n->try_ssid)) {
        n->last_err = EOS_NET_ERR_STORE;
        return EOS_NET_ERR_STORE;
    }
    if (!store_save(n, EOS_NET_KEY_PSK, n->try_psk)) {
        // Half-written credentials are worse than none: an SSID with no
        // passphrase is a boot that fails in a way the user cannot read. Take
        // the SSID back out.
        store_erase(n, EOS_NET_KEY_SSID);
        n->last_err = EOS_NET_ERR_STORE;
        return EOS_NET_ERR_STORE;
    }

    copy_str(n->ssid, sizeof n->ssid, n->try_ssid);
    copy_str(n->psk,  sizeof n->psk,  n->try_psk);
    n->cred = EOS_NET_CRED_SAVED;   // consumes the success; a second commit refuses

    // The AP does not come down here. finish_at_ms stays 0, meaning "deadline
    // not set"; the first eos_net_pump() after this reads the clock and sets
    // it. Tearing the AP down inside this call would kill the HTTP response on
    // its way back to the phone that asked for the join.
    n->finish_pending = true;
    n->finish_at_ms   = 0;

    emit(n, EOS_NET_EV_COMMIT);
    return EOS_NET_OK;
}

eos_net_err_t eos_net_forget(eos_net_t *n)
{
    if (!n) return EOS_NET_ERR_ARG;

    store_erase(n, EOS_NET_KEY_SSID);
    store_erase(n, EOS_NET_KEY_PSK);
    // EOS_NET_KEY_AP_PSK is deliberately kept. See ensure_ap_psk().

    n->ssid[0] = n->psk[0] = 0;
    n->try_ssid[0] = n->try_psk[0] = 0;
    n->cred = EOS_NET_CRED_NONE;
    n->finish_pending = false;
    n->mdns_up = false;

    emit(n, EOS_NET_EV_FORGET);
    return enter_setup(n);
}

eos_net_err_t eos_net_finish(eos_net_t *n)
{
    if (!n) return EOS_NET_ERR_ARG;
    // Gated on there being a stored network rather than on cred == SAVED: a
    // failed try at a second network moves cred to TRIED_FAIL while the first
    // network is still in NVS, and that must not make finishing illegal.
    if (!n->ssid[0]) return EOS_NET_ERR_STATE;

    n->finish_pending = false;

    if (n->ap_up) {
        if (n->cfg.drv.ap_stop) n->cfg.drv.ap_stop(n->cfg.drv.ud);
        n->ap_up = false;
    }

    note_link(n);
    start_mdns(n);
    set_mode(n, n->ip ? EOS_NET_STA : EOS_NET_JOINING);
    return EOS_NET_OK;
}

// --------------------------------------------------------------- scanning

eos_net_err_t eos_net_scan(eos_net_t *n, bool force)
{
    int raw;

    if (!n) return EOS_NET_ERR_ARG;
    if (n->scan_valid && !force) return EOS_NET_OK;
    if (!n->cfg.drv.scan) { n->last_err = EOS_NET_ERR_DRIVER; return EOS_NET_ERR_DRIVER; }

    if (!radio_take()) { n->last_err = EOS_NET_ERR_RADIO; return EOS_NET_ERR_RADIO; }
    raw = n->cfg.drv.scan(n->cfg.drv.ud, n->scan, EOS_NET_SCAN_MAX);
    radio_give();

    if (raw < 0) { n->last_err = EOS_NET_ERR_DRIVER; return EOS_NET_ERR_DRIVER; }
    if (raw > EOS_NET_SCAN_MAX) raw = EOS_NET_SCAN_MAX;

    n->scan_n     = (int16_t)eos_net_scan_reduce(n->scan, raw);
    n->scan_valid = true;
    emit(n, EOS_NET_EV_SCAN);
    return EOS_NET_OK;
}

int eos_net_scan_results(const eos_net_t *n, const eos_net_ap_t **out)
{
    if (!n) { if (out) *out = NULL; return 0; }
    if (out) *out = n->scan;
    return n->scan_n;
}

bool eos_net_scan_cached(const eos_net_t *n) { return n && n->scan_valid; }

// ------------------------------------------------------------------- pump

void eos_net_pump(eos_net_t *n, uint32_t now_ms)
{
    if (!n) return;

    // The deferred teardown from eos_net_commit(). finish_at_ms of 0 means the
    // deadline has not been set yet: the first pump after a commit sets it, so
    // the delay is measured from a real clock reading and not from a zero.
    if (n->finish_pending) {
        if (!n->finish_at_ms) {
            n->finish_at_ms = now_ms + n->cfg.finish_delay_ms;
            if (!n->finish_at_ms) n->finish_at_ms = 1;   // 0 is the sentinel
        } else if ((int32_t)(now_ms - n->finish_at_ms) >= 0) {
            eos_net_finish(n);
            n->finish_at_ms = 0;
            // Rebase both timers off a real clock reading. Without this the
            // zero-initialised deadlines are already in the past and a board
            // whose address has not arrived yet would go straight into a
            // blocking 15 s rejoin one tick after the AP came down.
            n->next_poll_ms = now_ms + n->cfg.poll_ms;
            n->rejoin_at_ms = now_ms + n->cfg.rejoin_ms;
        }
    }

    if (n->mode != EOS_NET_STA && n->mode != EOS_NET_JOINING) return;
    if (n->trying) return;

    if ((int32_t)(now_ms - n->next_poll_ms) >= 0) {
        uint32_t was_ip = n->ip;
        int8_t   was_rssi = n->rssi;

        note_link(n);
        n->next_poll_ms = now_ms + n->cfg.poll_ms;

        if (n->rssi != was_rssi) {
            int d = (int)n->rssi - (int)was_rssi;
            if (d < 0) d = -d;
            if (d >= (int)n->cfg.rssi_hysteresis) emit(n, EOS_NET_EV_RSSI);
        }

        // A link that has gone means JOINING, not SETUP. Opening a SoftAP
        // because the router rebooted would put a provisioning page on the air
        // every time the house blinks; the board waits and retries instead.
        if (!n->ip && was_ip) {
            set_mode(n, EOS_NET_JOINING);
            n->rejoin_at_ms = now_ms + n->cfg.rejoin_ms;
        } else if (n->ip && n->mode == EOS_NET_JOINING) {
            start_mdns(n);
            set_mode(n, EOS_NET_STA);
        }
    }

    if (n->mode == EOS_NET_JOINING && !n->ip && n->ssid[0] &&
        (int32_t)(now_ms - n->rejoin_at_ms) >= 0) {
        n->trying = true;
        if (do_join(n, n->ssid, n->psk) == EOS_NET_OK) {
            note_link(n);
            start_mdns(n);
            if (n->ip) set_mode(n, EOS_NET_STA);
        }
        n->trying = false;
        n->rejoin_at_ms = now_ms + n->cfg.rejoin_ms;
        n->next_poll_ms = now_ms + n->cfg.poll_ms;
    }
}

// ------------------------------------------------------------- accessors

eos_net_mode_t eos_net_mode(const eos_net_t *n)       { return n ? n->mode : EOS_NET_OFF; }
eos_net_cred_t eos_net_cred(const eos_net_t *n)       { return n ? n->cred : EOS_NET_CRED_NONE; }
bool eos_net_has_credentials(const eos_net_t *n)      { return n && n->ssid[0] != 0; }
bool eos_net_trying(const eos_net_t *n)               { return n && n->trying; }
eos_net_err_t  eos_net_last_error(const eos_net_t *n) { return n ? n->last_err : EOS_NET_ERR_ARG; }
uint32_t       eos_net_ip(const eos_net_t *n)         { return n ? n->ip : 0; }
int8_t         eos_net_rssi(const eos_net_t *n)       { return n ? n->rssi : 0; }
const char    *eos_net_ssid(const eos_net_t *n)       { return n ? n->ssid : ""; }
const char    *eos_net_ap_ssid(const eos_net_t *n)    { return n ? n->ap_ssid : ""; }
const char    *eos_net_ap_psk(const eos_net_t *n)     { return n ? n->ap_psk : ""; }
const char    *eos_net_hostname(const eos_net_t *n)   { return n ? n->hostname : ""; }

// The numbers are eos_bar_wifi_t's. The test asserts they still are.
int eos_net_bar_wifi(const eos_net_t *n)
{
    if (!n) return 0;                 /* EOS_WIFI_OFF */
    if (n->trying) return 1;           /* EOS_WIFI_JOINING */
    switch (n->mode) {
    case EOS_NET_OFF:     return 0;
    case EOS_NET_JOINING: return 1;
    case EOS_NET_SETUP:   return 2;    /* EOS_WIFI_DOWN */
    case EOS_NET_STA:     return n->ip ? 3 : 1;
    }
    return 0;
}

// -------------------------------------------------------- captive-portal DNS
//
// Every question gets 192.168.4.1 so that joining the AP opens the setup page
// by itself. The parsing is bounded at every step because this is code that
// only ever sees packets from a stranger's phone.

#define DNS_HDR 12

int eos_net_dns_reply(const uint8_t *query, size_t qlen,
                      uint8_t *out, size_t cap, uint32_t ip)
{
    size_t at, name_start, name_len;
    uint16_t flags, qdcount, qtype, qclass;
    size_t need;

    if (!query || !out) return -1;
    if (qlen < DNS_HDR || qlen > EOS_NET_DNS_MAX) return 0;

    flags   = (uint16_t)((query[2] << 8) | query[3]);
    qdcount = (uint16_t)((query[4] << 8) | query[5]);

    if (flags & 0x8000u) return 0;      // already a response; not ours to answer
    if (qdcount != 1)    return 0;      // nobody sends more than one, and a
                                        // multi-question reply is not worth it

    // Walk the QNAME. Length-prefixed labels, 63 bytes each at most, ending in
    // a zero length. A compression pointer has no business in a question and is
    // the classic way to walk a parser off the end of the buffer, so refuse it.
    name_start = DNS_HDR;
    at = name_start;
    for (;;) {
        uint8_t l;
        if (at >= qlen) return 0;
        l = query[at];
        if ((l & 0xC0u) != 0) return 0;         // pointer or reserved: refuse
        at++;
        if (l == 0) break;
        if (l > 63) return 0;
        if (at + l > qlen) return 0;
        at += l;
    }
    name_len = at - name_start;
    if (name_len < 1 || name_len > 255) return 0;

    if (at + 4 > qlen) return 0;
    qtype  = (uint16_t)((query[at] << 8) | query[at + 1]);
    qclass = (uint16_t)((query[at + 2] << 8) | query[at + 3]);
    at += 4;

    // Header + the echoed question, plus an answer when we have one to give.
    // The answer's name is the 0xC00C compression pointer back at the question,
    // which is why it costs two bytes and not name_len.
    need = DNS_HDR + name_len + 4;
    {
        bool answer = (qclass == 1 && (qtype == 1 || qtype == 255));
        if (answer) need += 2 + 2 + 2 + 4 + 2 + 4;
        if (need > cap) return -1;

        out[0] = query[0]; out[1] = query[1];              // the transaction id
        out[2] = (uint8_t)(0x84u | (query[2] & 0x01u));    // QR + AA, keep RD
        out[3] = 0x00;                                     // RA clear, RCODE 0
        out[4] = 0; out[5] = 1;                            // QDCOUNT
        out[6] = 0; out[7] = answer ? 1 : 0;               // ANCOUNT
        out[8] = 0; out[9] = 0;                            // NSCOUNT
        out[10] = 0; out[11] = 0;                          // ARCOUNT

        memcpy(out + DNS_HDR, query + name_start, name_len + 4);
        at = DNS_HDR + name_len + 4;

        // A question we will not answer gets a well-formed empty reply rather
        // than a lie. An AAAA lookup told 192.168.4.1 is how a phone ends up
        // with a broken v6 route instead of a captive portal.
        if (!answer) return (int)at;

        out[at++] = 0xC0; out[at++] = 0x0C;                // name -> offset 12
        out[at++] = 0x00; out[at++] = 0x01;                // type A
        out[at++] = 0x00; out[at++] = 0x01;                // class IN
        out[at++] = 0; out[at++] = 0; out[at++] = 0; out[at++] = 0;  // TTL 0
        out[at++] = 0x00; out[at++] = 0x04;                // rdlength
        out[at++] = (uint8_t)((ip >> 24) & 0xFFu);
        out[at++] = (uint8_t)((ip >> 16) & 0xFFu);
        out[at++] = (uint8_t)((ip >> 8) & 0xFFu);
        out[at++] = (uint8_t)(ip & 0xFFu);
        return (int)at;
    }
}

// --------------------------------------------------------- platform bindings
//
// Only compiled into an ESP-IDF build; the host test sees none of it, which is
// the point of the driver indirection above.
//
// The single most important line down here is esp_wifi_set_storage(RAM). By
// default esp_wifi keeps the STA config in its own NVS namespace and reconnects
// to it at the next boot — which would persist a failed eos_net_try() behind
// this file's back and undo the entire try-then-commit rule. eos_net owns what
// is remembered, and it remembers nothing until a join has landed.

#ifdef ESP_PLATFORM

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "eos_net";

#define NET_BIT_GOT_IP  BIT0
#define NET_BIT_FAILED  BIT1

static esp_netif_t     *s_sta_if;
static esp_netif_t     *s_ap_if;
static EventGroupHandle_t s_events;
static bool              s_inited;
static bool              s_started;
static bool              s_want_sta;
static bool              s_want_ap;
static bool              s_joining;
static bool              s_have_ip;
static uint32_t          s_ip;
// Why the last association attempt ended, straight from the driver, so the
// panel can say "wrong password" instead of the useless "that network refused".
static uint8_t           s_last_reason;

// In BSS on purpose: 16 wifi_ap_record_t is about 1.3 KB and this runs on a
// task whose stack is not 1.3 KB bigger than it needs to be.
static wifi_ap_record_t  s_recs[EOS_NET_SCAN_MAX];

static void net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d =
            (const wifi_event_sta_disconnected_t *)data;
        uint8_t reason = d ? d->reason : 0;

        s_have_ip = false;
        s_ip = 0;

        // esp_wifi_disconnect() raises this event too, and it is dispatched on
        // the event task rather than inline. The old code cleared the failure
        // bit 50 ms after asking for the disconnect and hoped the event had
        // landed by then; on a cold boot it has not, because BLE comes up two
        // milliseconds earlier and the event task is busy. The stale event then
        // arrived after s_joining went true and aborted a join five
        // milliseconds after it had already associated at -53 dBm. Filter on
        // the reason instead of on timing: a leave is one we asked for.
        if (reason == WIFI_REASON_ASSOC_LEAVE ||
            reason == WIFI_REASON_AUTH_LEAVE ||
            reason == WIFI_REASON_STA_LEAVING) return;

        if (s_joining) {
            s_last_reason = reason;
            xEventGroupSetBits(s_events, NET_BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        s_ip = ntohl(e->ip_info.ip.addr);
        s_have_ip = true;
        xEventGroupSetBits(s_events, NET_BIT_GOT_IP);
    }
}

static esp_err_t net_apply_mode(void)
{
    wifi_mode_t want = WIFI_MODE_NULL;
    if (s_want_sta && s_want_ap) want = WIFI_MODE_APSTA;
    else if (s_want_sta)         want = WIFI_MODE_STA;
    else if (s_want_ap)          want = WIFI_MODE_AP;
    return esp_wifi_set_mode(want);
}

static esp_err_t net_ensure_init(void)
{
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err;

    if (s_inited) return ESP_OK;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    if ((err = esp_netif_init()) != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_sta_if = esp_netif_create_default_wifi_sta();
    s_ap_if  = esp_netif_create_default_wifi_ap();
    if (!s_sta_if || !s_ap_if) return ESP_FAIL;

    if ((err = esp_wifi_init(&ic)) != ESP_OK) return err;

    // See the note at the top of this section. Not optional.
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;

    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, net_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, net_event, NULL, NULL);

    s_inited = true;
    return ESP_OK;
}

static esp_err_t net_ensure_started(void)
{
    esp_err_t err;
    if (s_started) return ESP_OK;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_started = true;
    return ESP_OK;
}

// -------------------------------------------------- the captive-portal DNS
//
// One task, one UDP socket on port 53, answering every question with
// 192.168.4.1 (eos_net_dns_reply builds the packet; it is host-tested). The
// receive timeout exists so the task notices s_dns_run going false rather than
// blocking in recvfrom until the AP comes back up.
//
// Static buffers, not stack: this task runs on 3 KB and two 512-byte frames
// would be a third of it.

static volatile bool s_dns_run;
static TaskHandle_t  s_dns_task;
static int           s_dns_sock = -1;
static uint8_t       s_dns_q[EOS_NET_DNS_MAX];
static uint8_t       s_dns_r[EOS_NET_DNS_MAX];

static void dns_task(void *arg)
{
    struct sockaddr_in me, them;
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    socklen_t tlen;
    int fd;

    (void)arg;

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { ESP_LOGE(TAG, "portal dns: no socket"); s_dns_task = NULL; vTaskDelete(NULL); return; }

    memset(&me, 0, sizeof me);
    me.sin_family      = AF_INET;
    me.sin_port        = htons(53);
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&me, sizeof me) != 0) {
        ESP_LOGE(TAG, "portal dns: bind 53 failed");
        close(fd);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    s_dns_sock = fd;

    while (s_dns_run) {
        int n, r;
        tlen = sizeof them;
        n = (int)recvfrom(fd, s_dns_q, sizeof s_dns_q, 0, (struct sockaddr *)&them, &tlen);
        if (n <= 0) continue;
        r = eos_net_dns_reply(s_dns_q, (size_t)n, s_dns_r, sizeof s_dns_r, EOS_NET_AP_IP);
        if (r > 0) sendto(fd, s_dns_r, (size_t)r, 0, (struct sockaddr *)&them, tlen);
    }

    s_dns_sock = -1;
    close(fd);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void dns_start(void)
{
    if (s_dns_task) return;
    s_dns_run = true;
    // The one allocation this file makes on target, declared in eos_net.h.
    if (xTaskCreate(dns_task, "eos_portal", 3072, NULL, 4, &s_dns_task) != pdPASS) {
        s_dns_run = false;
        s_dns_task = NULL;
        ESP_LOGE(TAG, "portal dns: task create failed");
    }
}

static void dns_stop(void)
{
    int spins = 0;
    if (!s_dns_task) return;
    s_dns_run = false;
    // The socket's 1 s receive timeout bounds this; 3 s is four times the worst
    // case and then it is abandoned rather than deadlocking the caller.
    while (s_dns_task && spins++ < 30) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_dns_task) ESP_LOGW(TAG, "portal dns: task did not exit");
}

// ------------------------------------------------------------- driver ops

static int idf_mac(void *ud, uint8_t out[6])
{
    (void)ud;
    return esp_read_mac(out, ESP_MAC_WIFI_STA) == ESP_OK ? 0 : -1;
}

static int idf_entropy(void *ud, uint8_t *out, size_t n)
{
    (void)ud;
    if (!out || !n) return -1;
    esp_fill_random(out, n);
    return 0;
}

static int idf_sta_join(void *ud, const char *ssid, const char *psk, uint32_t budget_ms)
{
    wifi_config_t wc;
    EventBits_t bits;

    (void)ud;
    if (net_ensure_init() != ESP_OK) return -1;

    memset(&wc, 0, sizeof wc);
    // NOT copy_str. wifi_config_t.sta.ssid is 32 bytes and an SSID is 32 OCTETS,
    // so a full-length name fills the field with no terminator. copy_str
    // reserves one byte for a NUL and would hand esp_wifi 31 characters of a
    // 32-character network — a join that fails, that nothing saves (which is
    // right), and that the panel can only report as "that network refused".
    // The memset above is what terminates every shorter name.
    {
        size_t sl = ssid ? strlen(ssid) : 0;
        if (sl > sizeof wc.sta.ssid) sl = sizeof wc.sta.ssid;
        memcpy(wc.sta.ssid, ssid, sl);
    }
    // The passphrase field is 64 for a 63-character maximum, so one reserved
    // byte is exactly right here and this one stays a string copy.
    copy_str((char *)wc.sta.password, sizeof wc.sta.password, psk);
    // Accept whatever the AP offers. Leaving IDF's default threshold in place
    // silently refuses open and WEP networks, which is a join failure the user
    // cannot tell apart from a wrong password.
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable    = true;
    wc.sta.pmf_cfg.required   = false;

    s_want_sta = true;
    if (net_apply_mode() != ESP_OK) return -1;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return -1;
    if (net_ensure_started() != ESP_OK) return -1;

    // Drop any prior association first, and only then arm the failure bit, so
    // the disconnect we asked for is not read as this attempt failing.
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));
    xEventGroupClearBits(s_events, NET_BIT_GOT_IP | NET_BIT_FAILED);
    s_have_ip = false;
    s_ip = 0;
    s_joining = true;

    // Retry inside the budget rather than failing on the first refusal. A
    // first connect commonly bounces once on a busy 2.4 GHz band, and the
    // reasons that are actually final - no such network, bad key - are
    // recognised and returned immediately instead of being retried pointlessly.
    {
        uint32_t left = budget_ms;
        for (;;) {
            uint32_t slice = left > 6000u ? 6000u : left;
            if (esp_wifi_connect() != ESP_OK) { s_joining = false; return -1; }

            bits = xEventGroupWaitBits(s_events, NET_BIT_GOT_IP | NET_BIT_FAILED,
                                       pdFALSE, pdFALSE, pdMS_TO_TICKS(slice));
            if (bits & NET_BIT_GOT_IP) break;

            if (bits & NET_BIT_FAILED) {
                uint8_t r = s_last_reason;
                if (r == WIFI_REASON_NO_AP_FOUND       ||
                    r == WIFI_REASON_AUTH_FAIL         ||
                    r == WIFI_REASON_HANDSHAKE_TIMEOUT ||
                    r == WIFI_REASON_MIC_FAILURE       ||
                    r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) break;
                xEventGroupClearBits(s_events, NET_BIT_FAILED);
            }
            if (left <= slice) break;
            left -= slice;
        }
    }
    s_joining = false;

    // An association with no address is not a join. DHCP silence is exactly
    // what a captive hotel network looks like and the board must not call it
    // success and then persist it.
    if (bits & NET_BIT_GOT_IP) return 0;

    esp_wifi_disconnect();
    return -1;
}

static int idf_sta_leave(void *ud)
{
    (void)ud;
    if (!s_inited) return 0;
    s_joining = false;
    esp_wifi_disconnect();
    s_have_ip = false;
    s_ip = 0;
    // Only give the STA interface up entirely when the AP still needs the
    // radio; otherwise leave it enabled so a rejoin does not re-mode the chip.
    if (s_want_ap) { s_want_sta = false; net_apply_mode(); }
    return 0;
}

static int idf_ap_start(void *ud, const char *ssid, const char *psk)
{
    wifi_config_t ac;

    (void)ud;
    if (net_ensure_init() != ESP_OK) return -1;
    if (!ssid || !ssid[0] || !psk || strlen(psk) < 8) return -1;   // never open

    memset(&ac, 0, sizeof ac);
    // Same 32-octet field as the station side, and ssid_len is what makes it
    // unambiguous. The generated name is eleven characters so this is not
    // reachable today; it matches the station path so that it stays that way.
    {
        size_t sl = strlen(ssid);
        if (sl > sizeof ac.ap.ssid) sl = sizeof ac.ap.ssid;
        memcpy(ac.ap.ssid, ssid, sl);
        ac.ap.ssid_len = (uint8_t)sl;
    }
    copy_str((char *)ac.ap.password, sizeof ac.ap.password, psk);
    ac.ap.channel        = 1;
    ac.ap.max_connection = 4;
    ac.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ac.ap.pmf_cfg.required = false;

    s_want_ap = true;
    if (net_apply_mode() != ESP_OK) return -1;
    if (esp_wifi_set_config(WIFI_IF_AP, &ac) != ESP_OK) return -1;
    if (net_ensure_started() != ESP_OK) return -1;

    // esp_netif's default AP handler is already running the DHCP server on
    // 192.168.4.1. The captive portal is the part IDF does not give us.
    dns_start();
    ESP_LOGI(TAG, "setup ap up: %s on " EOS_NET_AP_IP_STR, ssid);
    return 0;
}

static int idf_ap_stop(void *ud)
{
    (void)ud;
    dns_stop();
    if (!s_inited) return 0;
    s_want_ap = false;
    net_apply_mode();
    return 0;
}

static eos_net_auth_t idf_auth(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:            return EOS_NET_AUTH_OPEN;
    case WIFI_AUTH_WEP:             return EOS_NET_AUTH_WEP;
    case WIFI_AUTH_WPA_PSK:         return EOS_NET_AUTH_WPA;
    case WIFI_AUTH_WPA2_PSK:        return EOS_NET_AUTH_WPA2;
    case WIFI_AUTH_WPA_WPA2_PSK:    return EOS_NET_AUTH_WPA_WPA2;
    case WIFI_AUTH_WPA3_PSK:        return EOS_NET_AUTH_WPA3;
    case WIFI_AUTH_WPA2_WPA3_PSK:   return EOS_NET_AUTH_WPA2_WPA3;
    case WIFI_AUTH_WAPI_PSK:        return EOS_NET_AUTH_WAPI;
    case WIFI_AUTH_WPA2_ENTERPRISE: return EOS_NET_AUTH_ENTERPRISE;
    default:                        return EOS_NET_AUTH_OTHER;
    }
}

static int idf_scan(void *ud, eos_net_ap_t *out, int max)
{
    wifi_scan_config_t sc;
    bool restore_sta;
    uint16_t num;
    int i, n;

    (void)ud;
    if (!out || max <= 0) return -1;
    if (net_ensure_init() != ESP_OK) return -1;
    if (max > EOS_NET_SCAN_MAX) max = EOS_NET_SCAN_MAX;

    // Scanning needs the STA interface, so in SETUP this is the APSTA blip
    // docs/provisioning.md warns about — the phone doing the setup is an AP
    // client and may drop for a second. That is why the caller caches.
    restore_sta = s_want_sta;
    s_want_sta = true;
    if (net_apply_mode() != ESP_OK) { s_want_sta = restore_sta; return -1; }
    if (net_ensure_started() != ESP_OK) { s_want_sta = restore_sta; net_apply_mode(); return -1; }

    memset(&sc, 0, sizeof sc);
    sc.show_hidden = false;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        s_want_sta = restore_sta;
        net_apply_mode();
        return -1;
    }

    num = (uint16_t)EOS_NET_SCAN_MAX;
    if (esp_wifi_scan_get_ap_records(&num, s_recs) != ESP_OK) num = 0;
    esp_wifi_clear_ap_list();

    n = (int)num;
    if (n > max) n = max;
    for (i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        copy_str(out[i].ssid, sizeof out[i].ssid, (const char *)s_recs[i].ssid);
        out[i].rssi    = s_recs[i].rssi;
        out[i].channel = s_recs[i].primary;
        out[i].auth    = idf_auth(s_recs[i].authmode);
    }

    if (!restore_sta) { s_want_sta = false; net_apply_mode(); }
    return n;
}

static int idf_ip(void *ud, uint32_t *out)
{
    esp_netif_ip_info_t info;

    (void)ud;
    if (!out) return -1;
    *out = 0;
    if (!s_inited) return -1;

    if (s_want_sta && s_have_ip && s_sta_if &&
        esp_netif_get_ip_info(s_sta_if, &info) == ESP_OK && info.ip.addr) {
        *out = ntohl(info.ip.addr);
        return 0;
    }
    if (s_want_ap && s_ap_if && esp_netif_get_ip_info(s_ap_if, &info) == ESP_OK) {
        *out = ntohl(info.ip.addr);
        return 0;
    }
    return 0;
}

static int idf_rssi(void *ud, int8_t *out)
{
    wifi_ap_record_t ap;

    (void)ud;
    if (!out) return -1;
    *out = 0;
    if (!s_inited || !s_have_ip) return -1;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return -1;
    *out = ap.rssi;
    return 0;
}

static bool s_mdns_up;

static int idf_mdns_start(void *ud, const char *hostname)
{
    (void)ud;
    if (!hostname || !hostname[0]) return -1;
    if (!s_mdns_up) {
        if (mdns_init() != ESP_OK) return -1;
        s_mdns_up = true;
    }
    if (mdns_hostname_set(hostname) != ESP_OK) return -1;
    mdns_instance_name_set("ESP-OS");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mdns: %s.local", hostname);
    return 0;
}

void eos_net_idf_driver(eos_net_driver_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->sta_join   = idf_sta_join;
    out->sta_leave  = idf_sta_leave;
    out->ap_start   = idf_ap_start;
    out->ap_stop    = idf_ap_stop;
    out->scan       = idf_scan;
    out->mac        = idf_mac;
    out->ip         = idf_ip;
    out->rssi       = idf_rssi;
    out->mdns_start = idf_mdns_start;
    out->entropy    = idf_entropy;
    out->ud         = NULL;
}

// --------------------------------------------------------------- NVS store

static int idf_load(void *ud, const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    size_t len = cap;

    (void)ud;
    if (!out || !cap) return -1;
    out[0] = 0;
    if (nvs_open(EOS_NET_NVS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) { nvs_close(h); out[0] = 0; return -1; }
    nvs_close(h);
    out[cap - 1] = 0;
    return (int)strlen(out);
}

static int idf_save(void *ud, const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err;

    (void)ud;
    if (!val) return -1;
    if (nvs_open(EOS_NET_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

static int idf_erase(void *ud, const char *key)
{
    nvs_handle_t h;
    esp_err_t err;

    (void)ud;
    if (nvs_open(EOS_NET_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    err = nvs_erase_key(h, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

void eos_net_idf_store(eos_net_store_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->load  = idf_load;
    out->save  = idf_save;
    out->erase = idf_erase;
    out->ud    = NULL;
}

void eos_net_idf_defaults(eos_net_cfg_t *cfg)
{
    if (!cfg) return;
    eos_net_cfg_init(cfg);
    eos_net_idf_driver(&cfg->drv);
    eos_net_idf_store(&cfg->store);
}

#endif  // ESP_PLATFORM
