// eos_ble — see eos_ble.h for why this exists. This file is in two halves and
// the split is deliberate.
//
// Everything above the ESP_PLATFORM guard is plain C99 with no radio in it:
// address formatting, the name sanitiser, the scan-table merge, the bond
// record codec and the radio lock. That half is what kernel/svc/test/test_ble.c
// runs on a laptop, and it is the half where the bugs that matter live —
// records read back from NVS and names read off the air are the two places a
// bad byte can walk out of bounds, and neither of them needs a radio to test.
//
// Everything below the guard is NimBLE. It is a state machine driven entirely
// from ble_gap_event(), because every GATT operation here is asynchronous and
// the alternative — blocking a task on each step — is what makes BLE bring-ups
// hang forever waiting for a peripheral that walked out of range.
//
// The one non-obvious constraint in the NimBLE half: a keyboard reconnecting
// after sleep does NOT need a scan. It is bonded, we know its address, and a
// direct connect finds it without touching the radio lock. Scanning is only
// for pairing something new. Getting that wrong means the board scans every
// few seconds forever and WiFi never gets the antenna.

#include "eos_ble.h"

#include <string.h>

// ============================================================== observers

static eos_ble_passkey_fn s_pk_cb;
static void              *s_pk_user;
static eos_ble_state_fn   s_st_cb;
static void              *s_st_user;

void eos_ble_on_passkey(eos_ble_passkey_fn fn, void *user)
{
    s_pk_cb   = fn;
    s_pk_user = user;
}

void eos_ble_on_state(eos_ble_state_fn fn, void *user)
{
    s_st_cb   = fn;
    s_st_user = user;
}

// This sentence is the entire user interface for a failure mode that is
// otherwise invisible: the keyboard keeps working here and silently stops
// working on the board it used to live on, with no error anywhere.
const char *eos_ble_pair_warning(void)
{
    return "This keyboard bonds to one board at a time. Pairing it here will "
           "stop it working on the board it was paired to before.";
}

// The radio lock now lives in eos_radio.h / eos_radio.c, which belongs to
// neither stack. eos_ble.h includes it, so every caller that used to reach
// eos_radio_lock() through this header still does.

// ========================================================== addresses

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int eos_ble_addr_str(char *out, int max, const uint8_t addr[6])
{
    static const char H[] = "0123456789ABCDEF";
    int i, j = 0;

    if (!out || !addr) return (int)EOS_ERR_ARG;
    if (max < EOS_BLE_ADDR_STR) return (int)EOS_ERR_TOOBIG;

    for (i = 0; i < 6; i++) {
        if (i) out[j++] = ':';
        out[j++] = H[(addr[i] >> 4) & 0x0F];
        out[j++] = H[addr[i] & 0x0F];
    }
    out[j] = '\0';
    return j;
}

bool eos_ble_addr_parse(const char *s, uint8_t out[6])
{
    uint8_t tmp[6];
    int i;

    if (!s || !out) return false;

    // Each byte is checked before the next character is even read, so a short
    // string stops at its terminator instead of running off the end of it.
    for (i = 0; i < 6; i++) {
        int hi, lo;

        hi = hexval(s[i * 3]);
        if (hi < 0) return false;
        lo = hexval(s[i * 3 + 1]);
        if (lo < 0) return false;
        tmp[i] = (uint8_t)((hi << 4) | lo);

        if (i < 5) {
            char sep = s[i * 3 + 2];
            if (sep != ':' && sep != '-') return false;
        }
    }
    if (s[17] != '\0') return false;

    memcpy(out, tmp, 6);
    return true;
}

// ============================================================== names

int eos_ble_name_sanitize(char *dst, int dstmax, const char *src, int srclen)
{
    int i = 0;

    if (!dst || dstmax <= 0) return 0;

    if (src && srclen > 0) {
        for (; i < srclen && i < dstmax - 1; i++) {
            unsigned char c = (unsigned char)src[i];
            if (c == 0) break;                       // a padded field ends here
            dst[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    dst[i] = '\0';
    return i;
}

// ========================================================== scan table

bool eos_ble_adv_is_hid(uint16_t appearance, const uint16_t *uuid16, int n)
{
    int i;

    if (appearance == EOS_BLE_APPEARANCE_KBD ||
        appearance == EOS_BLE_APPEARANCE_HID) return true;
    if (!uuid16 || n <= 0) return false;
    for (i = 0; i < n; i++) if (uuid16[i] == EOS_BLE_UUID_HID) return true;
    return false;
}

// Higher is better. A HID advertiser outranks anything else whatever the
// signal, and a keyboard outranks a HID device that has not said what it is,
// because the table is eight entries and a room can hold more than eight
// beacons. RSSI is a negative dBm, so adding it orders correctly.
static int dev_rank(const eos_ble_dev_t *d)
{
    int r = 0;

    if (d->flags & EOS_BLE_F_HID)      r += 1000;
    if (d->flags & EOS_BLE_F_KEYBOARD) r += 2000;
    if (d->flags & EOS_BLE_F_BONDED)   r += 4000;
    return r + (int)d->rssi;
}

int eos_ble_devlist_find(const eos_ble_dev_t *tbl, int n, const uint8_t addr[6])
{
    int i;

    if (!tbl || !addr || n <= 0) return -1;
    for (i = 0; i < n; i++) if (eos_ble_addr_eq(tbl[i].addr, addr)) return i;
    return -1;
}

// Defined once in each half: the target reads the live scan table, the host has
// no table to read. Splitting it here is what keeps eos_ble_pair_addr() itself
// portable, so its argument checking is exercised by the host suite rather than
// only ever running on a board.
static uint8_t scan_addr_type(const uint8_t addr[6]);

eos_err_t eos_ble_pair_addr(const char *addr)
{
    uint8_t a[6];

    if (!eos_ble_addr_parse(addr, a)) return EOS_ERR_ARG;
    return eos_ble_pair(a, scan_addr_type(a));
}

int eos_ble_devlist_add(eos_ble_dev_t *tbl, int *n, int max, const eos_ble_dev_t *d)
{
    int i, victim;

    if (!tbl || !n || !d || max <= 0) return -1;
    if (*n < 0 || *n > max) return -1;

    // Merge. An active scan reports the advertisement and the scan response
    // separately, milliseconds apart, and the name is usually only in the
    // second one - so a device seen twice is one device with more known about
    // it, not two devices.
    for (i = 0; i < *n; i++) {
        if (tbl[i].addr_type != d->addr_type) continue;
        if (!eos_ble_addr_eq(tbl[i].addr, d->addr)) continue;

        tbl[i].flags = (uint8_t)(tbl[i].flags | d->flags);
        if (d->rssi != 0)   tbl[i].rssi = d->rssi;
        if (d->appearance)  tbl[i].appearance = d->appearance;
        if (d->name[0]) {
            memcpy(tbl[i].name, d->name, EOS_BLE_NAME_MAX);
            tbl[i].name[EOS_BLE_NAME_MAX - 1] = '\0';
        }
        return i;
    }

    if (*n < max) {
        tbl[*n] = *d;
        tbl[*n].name[EOS_BLE_NAME_MAX - 1] = '\0';
        return (*n)++;
    }

    victim = 0;
    for (i = 1; i < max; i++)
        if (dev_rank(&tbl[i]) < dev_rank(&tbl[victim])) victim = i;

    if (dev_rank(d) <= dev_rank(&tbl[victim])) return -1;
    tbl[victim] = *d;
    tbl[victim].name[EOS_BLE_NAME_MAX - 1] = '\0';
    return victim;
}

// ========================================================== bond record
//
// 48 bytes, fixed. A record is either exactly right or it is rejected: there is
// no partial parse and no "version 2 with extra fields at the end", because
// the thing on the other side of this codec is a flash page that survived a
// power cut and the only safe reading of a damaged one is to pair again.
//
//   0     'E'
//   1     'B'
//   2     format version
//   3     address type
//   4..9  address, display order
//   10    name length, 0..31
//   11..42 name, NUL padded
//   43    reserved, zero
//   44..45 appearance, little endian
//   46    reserved, zero
//   47    checksum: 0xA5 xor bytes 0..46

#define BOND_VER 1

int eos_ble_bond_encode(uint8_t *out, int max, const eos_ble_bond_t *b)
{
    uint8_t sum = 0xA5;
    int i, nl = 0;

    if (!out || !b) return (int)EOS_ERR_ARG;
    if (max < EOS_BLE_BOND_BYTES) return (int)EOS_ERR_TOOBIG;

    while (nl < EOS_BLE_NAME_MAX - 1 && b->name[nl]) nl++;

    memset(out, 0, EOS_BLE_BOND_BYTES);
    out[0] = 'E';
    out[1] = 'B';
    out[2] = BOND_VER;
    out[3] = b->addr_type;
    memcpy(out + 4, b->addr, 6);
    out[10] = (uint8_t)nl;
    memcpy(out + 11, b->name, (size_t)nl);
    out[44] = (uint8_t)(b->appearance & 0xFFu);
    out[45] = (uint8_t)((b->appearance >> 8) & 0xFFu);

    for (i = 0; i < EOS_BLE_BOND_BYTES - 1; i++) sum ^= out[i];
    out[EOS_BLE_BOND_BYTES - 1] = sum;
    return EOS_BLE_BOND_BYTES;
}

bool eos_ble_bond_decode(eos_ble_bond_t *out, const uint8_t *buf, int len)
{
    uint8_t sum = 0xA5;
    int i, nl;

    if (!out || !buf) return false;
    if (len != EOS_BLE_BOND_BYTES) return false;
    if (buf[0] != 'E' || buf[1] != 'B' || buf[2] != BOND_VER) return false;

    for (i = 0; i < EOS_BLE_BOND_BYTES - 1; i++) sum ^= buf[i];
    if (sum != buf[EOS_BLE_BOND_BYTES - 1]) return false;

    nl = buf[10];
    if (nl > EOS_BLE_NAME_MAX - 1) return false;
    if (buf[3] > EOS_BLE_ADDR_RANDOM) return false;

    memset(out, 0, sizeof *out);
    out->addr_type  = buf[3];
    memcpy(out->addr, buf + 4, 6);
    out->appearance = (uint16_t)(buf[44] | ((uint16_t)buf[45] << 8));
    eos_ble_name_sanitize(out->name, EOS_BLE_NAME_MAX, (const char *)(buf + 11), nl);
    return true;
}

// ========================================================== the radio half

#ifndef ESP_PLATFORM

// Host build. The portable half above is the whole point of the split; these
// exist so that code which calls the service links and runs on a laptop with
// the radio simply reported absent, exactly as the display backend does.

eos_err_t eos_ble_init(const eos_ble_cfg_t *cfg) { (void)cfg; return EOS_ERR_NODEV; }
void      eos_ble_tick(uint32_t now_ms)          { (void)now_ms; }
bool      eos_ble_connected(void)                { return false; }
uint32_t  eos_ble_passkey(void)                  { return 0; }
bool      eos_ble_scanning(void)                 { return false; }
eos_err_t eos_ble_scan_start(uint16_t ms)        { (void)ms; return EOS_ERR_NODEV; }
eos_err_t eos_ble_scan_stop(void)                { return EOS_ERR_NODEV; }
int       eos_ble_scan_results(eos_ble_dev_t *out, int max) { (void)out; (void)max; return 0; }
eos_err_t eos_ble_forget(void)                   { return EOS_ERR_NODEV; }
uint32_t  eos_ble_scan_age_ms(void)              { return EOS_BLE_SCAN_NEVER; }

static uint8_t scan_addr_type(const uint8_t addr[6])
{
    (void)addr;
    return EOS_BLE_ADDR_RANDOM;
}

eos_err_t eos_ble_pair(const uint8_t addr[6], uint8_t addr_type)
{
    (void)addr; (void)addr_type;
    return EOS_ERR_NODEV;
}

void eos_ble_status(eos_ble_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->state   = EOS_BLE_OFF;
    out->battery = EOS_BLE_BATTERY_UNKNOWN;
}

#else   // ESP_PLATFORM

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#if CONFIG_IDF_TARGET_ESP32
#include "esp_bt.h"     // only the original ESP32 has classic-BT memory to give back
#endif

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "eos_input.h"

static const char *TAG = "eos_ble";

#define NVS_NS  "eos_ble"
#define NVS_KEY "bond"

#define UUID_HID_SVC   0x1812
#define UUID_PROTO     0x2A4E   // Protocol Mode: write 0 for boot reports
#define UUID_REPORT    0x2A4D
#define UUID_BOOT_KBD  0x2A22
#define UUID_CCCD      0x2902
#define UUID_BATTERY   0x2A19

#define CHR_MAX  10
#define SUB_MAX   4

// A boot keyboard report is eight bytes: modifiers, reserved, six usages. HOGP
// notifications carry no report id - the characteristic identity is the id -
// so anything eight bytes or longer off an input report is the keyboard, and
// anything shorter is a mouse or a consumer-control key we do not bind.
#define KBD_REPORT_BYTES 8

typedef struct {
    uint16_t uuid;
    uint16_t val_handle;
    uint8_t  props;
} chr_t;

static eos_ble_cfg_t s_cfg;
static bool          s_inited;
static bool          s_synced;
static uint8_t       s_own_addr_type;
static uint8_t       s_state = EOS_BLE_OFF;

static bool          s_scanning;
static uint32_t      s_scan_deadline;
static uint32_t      s_scan_done_ms;
static bool          s_scan_ever;
static eos_ble_dev_t s_devs[EOS_BLE_SCAN_MAX];
static int           s_ndev;

static uint16_t      s_conn = BLE_HS_CONN_HANDLE_NONE;
static eos_ble_dev_t s_target;      // who the current attempt is aimed at
static bool          s_have_target;
static bool          s_repaired;    // a stale bond was already dropped this attempt

static bool           s_bonded;
static eos_ble_bond_t s_bond;

static uint32_t s_passkey;
static bool     s_passkey_shown;

static uint32_t s_retry_at;
static uint8_t  s_battery = EOS_BLE_BATTERY_UNKNOWN;
static int8_t   s_rssi;

// An initiator is a receiver. ble_gap_connect() puts the link layer into
// initiating state, which listens on the advertising channels exactly the way a
// scan does, and on one antenna that is the same conflict docs/provisioning.md
// forbids between a BLE scan and a WiFi scan. So a connect attempt holds the
// radio lock for as long as it is listening, and gives it back the moment the
// link is up or the attempt ends — an ESTABLISHED connection is scheduled
// traffic that coexistence handles, and holding the lock across it would mean
// WiFi never scanned again while a keyboard was attached.
//
// The window is short and the backoff is not, deliberately. Reconnect used to
// pass reconnect_ms as BOTH the listening duration and the delay before the
// next attempt, which is a 100% duty cycle: a board with a bond whose keyboard
// is out of the room was initiating continuously, and the frame loop starts a
// reconnect and then a WiFi scan on the same pass. 1200 ms in every 3000 leaves
// eos_net's radio_take() a gap it always wins within about a second.
#define BLE_CONNECT_WINDOW_MS  1200u    // reconnect: listen this long per attempt
#define BLE_PAIR_WINDOW_MS    10000u    // a human-initiated pair may wait longer

static bool     s_connecting;        // an initiator is up and holds the lock
static uint32_t s_connect_deadline;

static chr_t    s_chr[CHR_MAX];
static int      s_nchr;
static uint16_t s_svc_start, s_svc_end;
static uint16_t s_sub[SUB_MAX];     // value handles we accept notifications from
static int      s_nsub;
static uint16_t s_cccd[SUB_MAX];    // CCCDs still to be written
static int      s_ncccd, s_cccd_i;
static bool     s_subscribed;

// The owner string both stacks name themselves by. Taken from the enum so
// that eos_net's "wifi" and this "ble" cannot drift into two spellings the
// lock would read as two different owners.
#define RADIO_BLE (eos_radio_user_name(EOS_RADIO_BLE))

static int ble_gap_event(struct ble_gap_event *ev, void *arg);

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void set_state(uint8_t st)
{
    if (s_state == st) return;
    s_state = st;
    if (s_st_cb) s_st_cb(st, s_st_user);
}

// NimBLE addresses are little endian on the wire; everything above this file
// is display order. This is the only place the two meet.
static void addr_from_ble(uint8_t out[6], const ble_addr_t *a)
{
    int i;
    for (i = 0; i < 6; i++) out[i] = a->val[5 - i];
}

static void addr_to_ble(ble_addr_t *out, const uint8_t in[6], uint8_t type)
{
    int i;
    out->type = type;
    for (i = 0; i < 6; i++) out->val[i] = in[5 - i];
}

// ------------------------------------------------------------------- NVS

static bool bond_load(eos_ble_bond_t *b)
{
    nvs_handle_t h;
    uint8_t rec[EOS_BLE_BOND_BYTES];
    size_t len = sizeof rec;

    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    if (nvs_get_blob(h, NVS_KEY, rec, &len) != ESP_OK) { nvs_close(h); return false; }
    nvs_close(h);
    return eos_ble_bond_decode(b, rec, (int)len);
}

static void bond_save(const eos_ble_bond_t *b)
{
    nvs_handle_t h;
    uint8_t rec[EOS_BLE_BOND_BYTES];

    if (eos_ble_bond_encode(rec, (int)sizeof rec, b) != EOS_BLE_BOND_BYTES) return;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, NVS_KEY, rec, sizeof rec) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void bond_erase(void)
{
    nvs_handle_t h;

    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_KEY);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------- subscription

static void gatt_reset(void)
{
    s_nchr = s_nsub = s_ncccd = s_cccd_i = 0;
    s_svc_start = s_svc_end = 0;
    s_subscribed = false;
}

static bool is_subscribed_handle(uint16_t h)
{
    int i;
    for (i = 0; i < s_nsub; i++) if (s_sub[i] == h) return true;
    return false;
}

static void link_ready(void)
{
    eos_ble_bond_t b;

    s_subscribed = true;
    set_state(EOS_BLE_READY);

    // The bond record is written only now, when a keyboard has actually
    // delivered a usable subscription. A record written at connect time would
    // name a device that turned out not to be a keyboard at all, and the board
    // would then chase it forever on every boot.
    memset(&b, 0, sizeof b);
    memcpy(b.addr, s_target.addr, 6);
    b.addr_type  = s_target.addr_type;
    b.appearance = s_target.appearance;
    memcpy(b.name, s_target.name, EOS_BLE_NAME_MAX);
    b.name[EOS_BLE_NAME_MAX - 1] = '\0';

    if (!s_bonded || memcmp(&b, &s_bond, sizeof b) != 0) {
        s_bond   = b;
        s_bonded = true;
        bond_save(&s_bond);
    }

    ESP_LOGI(TAG, "keyboard ready: %s", s_bond.name[0] ? s_bond.name : "(unnamed)");
    eos_input_inject_conn(EOS_SRC_KEYBOARD, true, now_ms());
}

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err,
                           struct ble_gatt_attr *attr, void *arg);

// One CCCD write at a time. GATT has no pipelining here and issuing four
// writes at once gets three of them refused with BLE_HS_EBUSY.
static void cccd_write_next(void)
{
    static const uint8_t enable[2] = { 0x01, 0x00 };

    if (s_cccd_i >= s_ncccd) {
        if (s_nsub > 0) link_ready();
        else {
            ESP_LOGW(TAG, "no input report to subscribe to; dropping the link");
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return;
    }
    if (ble_gattc_write_flat(s_conn, s_cccd[s_cccd_i], enable, sizeof enable,
                             on_cccd_written, NULL) != 0) {
        ESP_LOGW(TAG, "cccd write failed on handle %u", (unsigned)s_cccd[s_cccd_i]);
        s_cccd_i++;
        cccd_write_next();
    }
}

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err,
                           struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err && err->status != 0)
        ESP_LOGW(TAG, "cccd write status %d", err->status);
    s_cccd_i++;
    cccd_write_next();
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn; (void)arg;

    if (err && err->status == BLE_HS_EDONE) { cccd_write_next(); return 0; }
    if (err && err->status != 0)            { cccd_write_next(); return 0; }
    if (!dsc) return 0;

    if (ble_uuid_u16(&dsc->uuid.u) != UUID_CCCD) return 0;
    if (!is_subscribed_handle(chr_val_handle)) return 0;
    if (s_ncccd < SUB_MAX) s_cccd[s_ncccd++] = dsc->handle;
    return 0;
}

static int on_proto_written(uint16_t conn, const struct ble_gatt_error *err,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err && err->status != 0)
        ESP_LOGW(TAG, "protocol mode write status %d (staying in report mode)", err->status);
    ble_gattc_disc_all_dscs(s_conn, s_svc_start, s_svc_end, on_dsc, NULL);
    return 0;
}

// Which characteristics to listen to is decided once, here, and the two cases
// are exclusive on purpose. Boot protocol mode gives a keyboard whose report
// layout is fixed by the spec at eight bytes, which is the only layout this
// host understands; asking for it and then also subscribing to the report-mode
// characteristics would double every keystroke on a keyboard that ignored the
// request.
static void choose_reports(void)
{
    uint16_t proto = 0, boot = 0;
    int i;

    s_nsub = 0;
    for (i = 0; i < s_nchr; i++) {
        if (s_chr[i].uuid == UUID_PROTO)    proto = s_chr[i].val_handle;
        if (s_chr[i].uuid == UUID_BOOT_KBD) boot  = s_chr[i].val_handle;
    }

    if (boot) {
        s_sub[s_nsub++] = boot;
        if (proto) {
            static const uint8_t boot_mode = 0x00;
            ESP_LOGI(TAG, "using boot protocol mode");
            if (ble_gattc_write_flat(s_conn, proto, &boot_mode, 1,
                                     on_proto_written, NULL) == 0) return;
        }
    } else {
        for (i = 0; i < s_nchr && s_nsub < SUB_MAX; i++) {
            if (s_chr[i].uuid != UUID_REPORT) continue;
            if (!(s_chr[i].props & BLE_GATT_CHR_PROP_NOTIFY)) continue;
            s_sub[s_nsub++] = s_chr[i].val_handle;
        }
        ESP_LOGI(TAG, "using report protocol mode, %d input reports", s_nsub);
    }

    ble_gattc_disc_all_dscs(s_conn, s_svc_start, s_svc_end, on_dsc, NULL);
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;

    if (err && err->status == BLE_HS_EDONE) { choose_reports(); return 0; }
    if (err && err->status != 0) {
        ESP_LOGW(TAG, "characteristic discovery failed: %d", err->status);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    if (!chr || s_nchr >= CHR_MAX) return 0;

    s_chr[s_nchr].uuid       = ble_uuid_u16(&chr->uuid.u);
    s_chr[s_nchr].val_handle = chr->val_handle;
    s_chr[s_nchr].props      = chr->properties;
    s_nchr++;
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn; (void)arg;

    if (err && err->status == BLE_HS_EDONE) {
        if (!s_svc_start) {
            ESP_LOGW(TAG, "peer has no HID service; not a keyboard");
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        ble_gattc_disc_all_chrs(s_conn, s_svc_start, s_svc_end, on_chr, NULL);
        return 0;
    }
    if (err && err->status != 0) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    if (svc) {
        s_svc_start = svc->start_handle;
        s_svc_end   = svc->end_handle;
    }
    return 0;
}

static int on_battery(uint16_t conn, const struct ble_gatt_error *err,
                      struct ble_gatt_attr *attr, void *arg)
{
    uint8_t pct = 0;
    uint16_t got = 0;

    (void)conn; (void)arg;
    if (err && err->status != 0) return 0;
    if (!attr || !attr->om) return 0;
    if (ble_hs_mbuf_to_flat(attr->om, &pct, 1, &got) != 0 || got != 1) return 0;
    if (pct <= 100) s_battery = pct;
    return 0;
}

static void start_discovery(void)
{
    gatt_reset();
    set_state(EOS_BLE_CONNECTING);
    ble_gattc_read_by_uuid(s_conn, 1, 0xFFFF, BLE_UUID16_DECLARE(UUID_BATTERY),
                           on_battery, NULL);
    ble_gattc_disc_svc_by_uuid(s_conn, BLE_UUID16_DECLARE(UUID_HID_SVC), on_svc, NULL);
}

// ------------------------------------------------------------ GAP events

// Claim the antenna for an initiator. wait 0: a WiFi scan or join that already
// holds it is the one thing this must not queue behind, because the caller is
// either the frame loop (which would stall) or an HTTP worker (which would hold
// the server's mutex while it waited).
static bool connect_begin(uint32_t window_ms)
{
    if (!eos_radio_lock(RADIO_BLE, 0)) return false;
    s_connecting       = true;
    s_connect_deadline = now_ms() + window_ms;
    return true;
}

// Idempotent, and safe to call when we never held the lock: eos_radio_unlock()
// checks the owner, so releasing one WiFi is holding does nothing.
static void connect_end(void)
{
    if (!s_connecting) return;
    s_connecting = false;
    eos_radio_unlock(RADIO_BLE);
}

static void drop_link(void)
{
    connect_end();
    if (s_subscribed) eos_input_inject_conn(EOS_SRC_KEYBOARD, false, now_ms());
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_passkey = 0;
    s_passkey_shown = false;
    s_battery = EOS_BLE_BATTERY_UNKNOWN;
    s_rssi = 0;
    gatt_reset();
    set_state(s_synced ? EOS_BLE_IDLE : EOS_BLE_OFF);
    s_retry_at = now_ms() + (s_cfg.reconnect_ms ? s_cfg.reconnect_ms : 3000u);
}

static void scan_finished(void)
{
    if (!s_scanning) return;
    s_scanning     = false;
    s_scan_done_ms = now_ms();
    s_scan_ever    = true;
    eos_radio_unlock(RADIO_BLE);
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) set_state(EOS_BLE_IDLE);
}

static void note_advert(struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    eos_ble_dev_t dev;
    uint16_t uuids[8];
    int nu = 0, i;

    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return;

    memset(&dev, 0, sizeof dev);
    addr_from_ble(dev.addr, &d->addr);
    dev.addr_type = (d->addr.type == BLE_ADDR_PUBLIC) ? EOS_BLE_ADDR_PUBLIC
                                                      : EOS_BLE_ADDR_RANDOM;
    dev.rssi = d->rssi;

    for (i = 0; i < f.num_uuids16 && nu < (int)(sizeof uuids / sizeof uuids[0]); i++)
        uuids[nu++] = ble_uuid_u16(&f.uuids16[i].u);

    if (f.appearance_is_present) dev.appearance = f.appearance;
    if (f.name && f.name_len) {
        eos_ble_name_sanitize(dev.name, EOS_BLE_NAME_MAX, (const char *)f.name, f.name_len);
        if (dev.name[0]) dev.flags |= EOS_BLE_F_NAMED;
    }
    if (eos_ble_adv_is_hid(dev.appearance, uuids, nu)) dev.flags |= EOS_BLE_F_HID;
    if (dev.appearance == EOS_BLE_APPEARANCE_KBD)      dev.flags |= EOS_BLE_F_KEYBOARD;
    if (s_bonded && s_bond.addr_type == dev.addr_type &&
        eos_ble_addr_eq(s_bond.addr, dev.addr))        dev.flags |= EOS_BLE_F_BONDED;

    // Everything seen is merged, HID or not, because the service list and the
    // name arrive in different reports and a device that only shows 0x1812 in
    // its scan response would otherwise be thrown away before it could say so.
    // The filtering happens on the way out, in eos_ble_scan_results().
    eos_ble_devlist_add(s_devs, &s_ndev, EOS_BLE_SCAN_MAX, &dev);
}

static void show_passkey(uint32_t pk)
{
    s_passkey = pk;
    s_passkey_shown = true;
    set_state(EOS_BLE_PAIRING);
    ESP_LOGW(TAG, "PAIRING: type %06u on the keyboard, then Enter", (unsigned)pk);
    ESP_LOGW(TAG, "%s", eos_ble_pair_warning());
    if (s_pk_cb) s_pk_cb(pk, s_have_target ? &s_target : NULL, s_pk_user);
}

static int ble_gap_event(struct ble_gap_event *ev, void *arg)
{
    struct ble_gap_conn_desc desc;

    (void)arg;

    switch (ev->type) {

    case BLE_GAP_EVENT_DISC:
        note_advert(&ev->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        scan_finished();
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        // The one terminal event for an initiator, whichever way it went: the
        // link layer has stopped listening, so the antenna goes back either
        // way. NimBLE delivers this with BLE_HS_ETIMEOUT when the window we
        // asked for expires, so there is no path that leaves the lock held.
        connect_end();
        if (ev->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed: %d", ev->connect.status);
            drop_link();
            return 0;
        }
        s_conn = ev->connect.conn_handle;
        set_state(EOS_BLE_CONNECTING);
        // Encryption first. Reading a HID service before the link is encrypted
        // gets every characteristic refused with insufficient authentication,
        // and the errors look like a broken keyboard rather than a missing
        // pairing.
        if (ble_gap_security_initiate(s_conn) != 0) {
            ESP_LOGW(TAG, "security initiate failed");
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason 0x%x", ev->disconnect.reason);
        drop_link();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        s_passkey = 0;
        s_passkey_shown = false;
        if (ev->enc_change.status == 0) {
            s_repaired = false;
            start_discovery();
            return 0;
        }
        // The keyboard was re-paired to some other board, wiped its side of the
        // bond, and now rejects the key we still hold. Ours is the stale copy;
        // drop it and pair fresh. Once only - a repair loop would sit there
        // showing passkeys forever.
        ESP_LOGW(TAG, "encryption failed: %d", ev->enc_change.status);
        if (!s_repaired && ble_gap_conn_find(s_conn, &desc) == 0) {
            s_repaired = true;
            ESP_LOGW(TAG, "dropping a stale bond and pairing again");
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io;

        memset(&io, 0, sizeof io);
        if (ev->passkey.params.action == BLE_SM_IOACT_DISP) {
            // The host picks the number because the keyboard has no screen to
            // show one. Six digits, never leading-zero-short: %06u on the LCD.
            io.action  = BLE_SM_IOACT_DISP;
            io.passkey = 100000u + (esp_random() % 900000u);
            show_passkey(io.passkey);
            ble_sm_inject_io(ev->passkey.conn_handle, &io);
        } else if (ev->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            io.action = BLE_SM_IOACT_NUMCMP;
            io.numcmp_accept = 1;
            show_passkey(ev->passkey.params.numcmp);
            ble_sm_inject_io(ev->passkey.conn_handle, &io);
        } else {
            ESP_LOGW(TAG, "unsupported pairing action %d", ev->passkey.params.action);
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // NimBLE asks before pairing with a peer it already has a bond for.
        // Deleting and retrying is the same stale-bond repair as above, caught
        // one step earlier.
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t rep[KBD_REPORT_BYTES];
        uint16_t got = 0;

        if (!is_subscribed_handle(ev->notify_rx.attr_handle)) return 0;
        if (!ev->notify_rx.om) return 0;
        if (ble_hs_mbuf_to_flat(ev->notify_rx.om, rep, sizeof rep, &got) != 0) return 0;
        // Shorter than a keyboard report means a mouse, a consumer-control key
        // or a vendor report. The HAL would survive it - it bounds-checks
        // everything - but it would read the first byte as modifiers and
        // invent a chord out of a volume key.
        if (got < KBD_REPORT_BYTES) return 0;
        eos_input_hid_report(rep, (uint8_t)got, now_ms());
        return 0;
    }

    default:
        return 0;
    }
}

// --------------------------------------------------------------- host task

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable identity address");
        return;
    }
    s_synced = true;
    set_state(EOS_BLE_IDLE);
    // A bonded keyboard is chased immediately rather than after the first
    // backoff, so a board that reboots with the keyboard awake reconnects in
    // about a second instead of four.
    s_retry_at = now_ms();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "controller reset, reason %d", reason);
    s_synced = false;
    s_scanning = false;
    s_connecting = false;
    eos_radio_unlock(RADIO_BLE);
    drop_link();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ------------------------------------------------------------ public API

eos_err_t eos_ble_init(const eos_ble_cfg_t *cfg)
{
    const eos_board_t *b = eos_board_get();
    esp_err_t err;

    if (s_inited) return EOS_OK;
    if (b && !b->input.ble_keyboard) return EOS_ERR_NODEV;

    s_cfg = cfg ? *cfg : eos_ble_defaults();
    if (!s_cfg.host_name) s_cfg.host_name = "esp-os";

    // NimBLE keeps its bonds in NVS and so do we. A partition that will not
    // open is recoverable exactly once, by erasing it, and that costs the WiFi
    // credentials too - so it is done only for the two errors that mean the
    // partition is unusable rather than merely empty.
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs unusable (%s); erasing it", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return EOS_ERR_IO;
    }

#if CONFIG_IDF_TARGET_ESP32
    // Classic Bluetooth is never coming up in this image. Handing its
    // controller memory back is tens of KB on the tier-0 board and it can only
    // be done before the controller starts.
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
#endif

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return EOS_ERR_IO;
    }

    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // DISPLAY_ONLY is the truth about this board and it is what selects passkey
    // entry: the host displays six digits, the keyboard - which has keys and no
    // screen - is the one that types them. Bonding and MITM are both on because
    // an unauthenticated pairing over the air to a device that will then send
    // us every keystroke is not worth the two seconds it saves.
    ble_hs_cfg.sm_io_cap         = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 1;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gap_device_name_set(s_cfg.host_name);

    s_bonded = bond_load(&s_bond);
    if (s_bonded) {
        char a[EOS_BLE_ADDR_STR];
        eos_ble_addr_str(a, sizeof a, s_bond.addr);
        ESP_LOGI(TAG, "bonded to %s (%s)", s_bond.name[0] ? s_bond.name : "?", a);
    }

    nimble_port_freertos_init(host_task);
    s_inited = true;
    return EOS_OK;
}

void eos_ble_status(eos_ble_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->state         = s_state;
    out->bonded        = s_bonded;
    out->connected     = s_subscribed;
    out->scanning      = s_scanning;
    out->passkey       = s_passkey;
    out->passkey_shown = s_passkey_shown;
    out->battery       = s_battery;
    out->rssi          = s_rssi;
    if (s_bonded) out->bond = s_bond;
}

bool     eos_ble_connected(void) { return s_subscribed; }
uint32_t eos_ble_passkey(void)   { return s_passkey_shown ? s_passkey : 0; }
bool     eos_ble_scanning(void)  { return s_scanning; }

eos_err_t eos_ble_scan_start(uint16_t ms)
{
    struct ble_gap_disc_params p;
    uint16_t dur = ms ? ms : (s_cfg.scan_ms ? s_cfg.scan_ms : 6000);
    int rc;

    if (!s_inited || !s_synced) return EOS_ERR_STATE;
    if (s_scanning) return EOS_ERR_BUSY;
    if (!eos_radio_lock(RADIO_BLE, 0)) return EOS_ERR_BUSY;

    memset(&p, 0, sizeof p);
    // 30 ms of listening in every 60 ms. The prior Arduino build used a 99%
    // duty cycle, which finds keyboards a little faster and starves WiFi
    // completely while it does it; on one antenna that is not a trade worth
    // making.
    p.itvl              = 96;   // 60 ms in 0.625 ms units
    p.window            = 48;   // 30 ms
    p.passive           = 0;    // active: the name is in the scan response
    p.filter_duplicates = 0;    // merged in software, so both reports arrive
    p.limited           = 0;

    s_ndev = 0;
    rc = ble_gap_disc(s_own_addr_type, (int32_t)dur, &p, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc failed: %d", rc);
        eos_radio_unlock(RADIO_BLE);
        return EOS_ERR_IO;
    }
    s_scanning = true;
    s_scan_deadline = now_ms() + dur;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) set_state(EOS_BLE_SCANNING);
    return EOS_OK;
}

eos_err_t eos_ble_scan_stop(void)
{
    if (!s_scanning) return EOS_OK;
    ble_gap_disc_cancel();
    scan_finished();
    return EOS_OK;
}

int eos_ble_scan_results(eos_ble_dev_t *out, int max)
{
    int i, n = 0;

    if (!out || max <= 0) return 0;

    // Only HID, strongest first. Insertion sort over at most eight entries:
    // a qsort call would cost more flash than the loop it replaced.
    for (i = 0; i < s_ndev; i++) {
        int j;

        if (!(s_devs[i].flags & EOS_BLE_F_HID)) continue;
        if (n >= max) break;
        for (j = n; j > 0 && out[j - 1].rssi < s_devs[i].rssi; j--) out[j] = out[j - 1];
        out[j] = s_devs[i];
        n++;
    }
    return n;
}

uint32_t eos_ble_scan_age_ms(void)
{
    if (!s_scan_ever) return EOS_BLE_SCAN_NEVER;
    if (s_scanning)   return 0;
    return now_ms() - s_scan_done_ms;
}

static uint8_t scan_addr_type(const uint8_t addr[6])
{
    int i = eos_ble_devlist_find(s_devs, s_ndev, addr);
    return i >= 0 ? s_devs[i].addr_type : EOS_BLE_ADDR_RANDOM;
}

eos_err_t eos_ble_pair(const uint8_t addr[6], uint8_t addr_type)
{
    ble_addr_t peer;
    int i, rc;

    if (!addr) return EOS_ERR_ARG;
    if (!s_inited || !s_synced) return EOS_ERR_STATE;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) return EOS_ERR_BUSY;

    // A scan and a connect cannot both own the radio, and the connect is the
    // one the human is waiting on.
    eos_ble_scan_stop();

    memset(&s_target, 0, sizeof s_target);
    memcpy(s_target.addr, addr, 6);
    s_target.addr_type = addr_type;
    for (i = 0; i < s_ndev; i++) {
        if (s_devs[i].addr_type != addr_type) continue;
        if (!eos_ble_addr_eq(s_devs[i].addr, addr)) continue;
        s_target = s_devs[i];
        break;
    }
    s_have_target = true;
    s_repaired = false;

    // Initiating is listening, so it takes the antenna. Refusing rather than
    // waiting is what keeps this call short: it runs on an HTTP worker holding
    // the server's mutex, and the handler turns BUSY into the 409 that already
    // says wifi and bluetooth share one antenna.
    if (!connect_begin(BLE_PAIR_WINDOW_MS)) {
        set_state(EOS_BLE_IDLE);
        return EOS_ERR_BUSY;
    }

    addr_to_ble(&peer, addr, addr_type == EOS_BLE_ADDR_RANDOM ? BLE_ADDR_RANDOM
                                                              : BLE_ADDR_PUBLIC);
    set_state(EOS_BLE_CONNECTING);
    rc = ble_gap_connect(s_own_addr_type, &peer, (int32_t)BLE_PAIR_WINDOW_MS,
                         NULL, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect failed: %d", rc);
        connect_end();
        set_state(EOS_BLE_IDLE);
        return EOS_ERR_IO;
    }
    return EOS_OK;
}

eos_err_t eos_ble_forget(void)
{
    ble_addr_t peer;

    if (!s_inited) return EOS_ERR_STATE;

    if (s_conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);

    if (s_bonded) {
        addr_to_ble(&peer, s_bond.addr,
                    s_bond.addr_type == EOS_BLE_ADDR_RANDOM ? BLE_ADDR_RANDOM
                                                            : BLE_ADDR_PUBLIC);
        ble_store_util_delete_peer(&peer);
    }
    bond_erase();
    memset(&s_bond, 0, sizeof s_bond);
    s_bonded = false;
    s_have_target = false;
    ESP_LOGI(TAG, "bond forgotten");
    return EOS_OK;
}

void eos_ble_tick(uint32_t now)
{
    if (!s_inited || !s_synced) return;

    // Belt and braces: if DISC_COMPLETE were ever missed the radio lock would
    // be held forever and WiFi would never scan again.
    if (s_scanning && due(now, s_scan_deadline + 1000u)) {
        ESP_LOGW(TAG, "scan overran its duration; releasing the radio");
        ble_gap_disc_cancel();
        scan_finished();
    }

    // The same guarantee for an initiator. BLE_GAP_EVENT_CONNECT always
    // arrives, timeout included, so this should never fire — but a lock held
    // by a connect attempt nobody ever closed is a board whose WiFi scan
    // returns nothing forever, which is not a failure worth trusting an
    // upstream event for.
    if (s_connecting && s_conn == BLE_HS_CONN_HANDLE_NONE &&
        due(now, s_connect_deadline + 1000u)) {
        ESP_LOGW(TAG, "connect overran its window; releasing the radio");
        ble_gap_conn_cancel();
        connect_end();
        set_state(EOS_BLE_IDLE);
    }

    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        if (s_subscribed) (void)ble_gap_conn_rssi(s_conn, &s_rssi);
        return;
    }
    if (!s_bonded || !s_cfg.auto_reconnect) return;
    if (s_scanning || s_connecting) return;
    if (!due(now, s_retry_at)) return;

    s_retry_at = now + (s_cfg.reconnect_ms ? s_cfg.reconnect_ms : 3000u);
    {
        ble_addr_t peer;
        int rc;

        // Chasing a keyboard that is not in the room must never cost WiFi a
        // scan. This runs on the frame loop one call before eos_httpd_pump()
        // starts one, so without this check the two overlapped on every pass.
        if (!connect_begin(BLE_CONNECT_WINDOW_MS)) {
            // Try again soon rather than at the full backoff: the radio is
            // busy for seconds, not minutes, and a keyboard waking up while
            // WiFi scanned should not wait three seconds to be noticed.
            s_retry_at = now + 500u;
            return;
        }

        memset(&s_target, 0, sizeof s_target);
        memcpy(s_target.addr, s_bond.addr, 6);
        s_target.addr_type = s_bond.addr_type;
        s_target.appearance = s_bond.appearance;
        memcpy(s_target.name, s_bond.name, EOS_BLE_NAME_MAX);
        s_target.name[EOS_BLE_NAME_MAX - 1] = '\0';
        s_have_target = true;

        addr_to_ble(&peer, s_bond.addr,
                    s_bond.addr_type == EOS_BLE_ADDR_RANDOM ? BLE_ADDR_RANDOM
                                                            : BLE_ADDR_PUBLIC);
        // A bonded keyboard needs no scan: we know where it lives. The connect
        // listens for it to advertise, which it does the moment a key is
        // pressed. The window is BLE_CONNECT_WINDOW_MS and not reconnect_ms:
        // the two are the listening time and the gap between listens, and
        // making them equal left the antenna claimed continuously.
        rc = ble_gap_connect(s_own_addr_type, &peer, (int32_t)BLE_CONNECT_WINDOW_MS,
                             NULL, ble_gap_event, NULL);
        if (rc != 0) {
            connect_end();
            if (rc != BLE_HS_EALREADY && rc != BLE_HS_EBUSY)
                ESP_LOGD(TAG, "reconnect attempt failed: %d", rc);
        }
    }
}

#endif  // ESP_PLATFORM
