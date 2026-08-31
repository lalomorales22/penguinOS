// eos_ble — the BLE HID host. The board is the central; a keyboard is the
// peripheral; keystrokes land in the eos_input ring and nothing above that
// line knows a radio was involved.
//
// This exists because these panels have no keys. A board in a strange room has
// no network and no way to type, and the only two channels out of that hole are
// the screen and a keyboard that pairs over the air. The pairing itself needs
// both: most BLE keyboards bond by passkey entry, where the HOST picks a
// six-digit number and the human types it ON THE KEYBOARD. The keyboard has no
// screen. So the number has to come off the board's LCD, which is why
// eos_ble_on_passkey() exists and why the passkey is also readable by polling —
// the panel shows it immediately and the web page shows the same digits.
//
// The one non-obvious constraint: WiFi and BLE share one radio on every SoC in
// this fleet (on the C6 they share it with 802.15.4 as well). A BLE scan and a
// WiFi scan running at the same time do not merely go slowly, they corrupt each
// other's results and can wedge the controller. So both go through the advisory
// lock at the bottom of this header, and every scan in penguinOS takes it. It is
// advisory: it works only because both callers use it.
//
// The second thing that is not obvious and costs an afternoon: a BLE keyboard
// bonds to ONE host. Pairing the K809 here silently destroys its bond with
// whatever board it was paired to before, and the only symptom on that other
// board is a keyboard that stops working. eos_ble_pair_warning() is the exact
// sentence the LCD and the web page must both show at the moment of pairing.
//
// NimBLE only. Bluedroid is 83 KB against NimBLE's 19 KB and is an instant OOM
// on the tier-0 boards. Nothing here allocates after init; the scan table, the
// bond record and the characteristic table are fixed arrays.

#ifndef EOS_BLE_H
#define EOS_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include "eos_board.h"

// ------------------------------------------------------------------ limits

#define EOS_BLE_NAME_MAX    32   // NUL-terminated peripheral name, sanitised
#define EOS_BLE_SCAN_MAX     8   // peripherals kept from one scan
#define EOS_BLE_ADDR_STR    18   // "AA:BB:CC:DD:EE:FF" plus the terminator
#define EOS_BLE_BOND_BYTES  48   // the NVS bond record, exactly

// The two GATT numbers the spec names. Appearance 0x03C1 is "Keyboard";
// 0x03C0 is the generic HID parent and some keyboards advertise that instead.
#define EOS_BLE_UUID_HID        0x1812
#define EOS_BLE_APPEARANCE_HID  0x03C0
#define EOS_BLE_APPEARANCE_KBD  0x03C1

// --------------------------------------------------------------- addresses
//
// addr[] is in DISPLAY order: addr[0] is the byte printed first. NimBLE's
// ble_addr_t.val is the reverse, and the conversion happens once, at the
// NimBLE boundary inside eos_ble.c. Everything above this header — the web
// JSON, the LCD, the bond record — is display order, so an address that is
// read off the screen and typed into a form matches.

#define EOS_BLE_ADDR_PUBLIC  0
#define EOS_BLE_ADDR_RANDOM  1

// ---------------------------------------------------------- scan results

#define EOS_BLE_F_HID       0x01   // advertised service 0x1812
#define EOS_BLE_F_KEYBOARD  0x02   // appearance says keyboard specifically
#define EOS_BLE_F_NAMED     0x04   // the advertisement carried a name
#define EOS_BLE_F_BONDED    0x08   // this is the device in our bond record

typedef struct {
    uint8_t  addr[6];      // display order, see above
    uint8_t  addr_type;    // EOS_BLE_ADDR_*
    int8_t   rssi;         // dBm, 0 when unknown
    uint8_t  flags;        // EOS_BLE_F_*
    uint16_t appearance;   // 0 when the advertisement did not carry one
    char     name[EOS_BLE_NAME_MAX];
} eos_ble_dev_t;

// ------------------------------------------------------------------- bond
//
// This is penguinOS's memory of WHICH device it bonded to, not the bond itself.
// The link keys live in NimBLE's own NVS store and are never copied out of it;
// what is kept here is the address to reconnect to and the name to show, so
// that the status page can say "K809" instead of six hex bytes, and so that a
// reconnect costs a direct connect rather than a scan.

typedef struct {
    uint8_t  addr[6];
    uint8_t  addr_type;
    uint16_t appearance;
    char     name[EOS_BLE_NAME_MAX];
} eos_ble_bond_t;

// ------------------------------------------------------------------ state

typedef enum {
    EOS_BLE_OFF = 0,     // not initialised, or the board declares no keyboard
    EOS_BLE_IDLE,        // host up, radio free
    EOS_BLE_SCANNING,
    EOS_BLE_CONNECTING,  // link up, discovering or encrypting
    EOS_BLE_PAIRING,     // a passkey is on screen and the human is typing it
    EOS_BLE_READY,       // subscribed; keystrokes are flowing
} eos_ble_state_t;

static inline const char *eos_ble_state_name(uint8_t s)
{
    switch (s) {
    case EOS_BLE_OFF:        return "off";
    case EOS_BLE_IDLE:       return "idle";
    case EOS_BLE_SCANNING:   return "scanning";
    case EOS_BLE_CONNECTING: return "connecting";
    case EOS_BLE_PAIRING:    return "pairing";
    case EOS_BLE_READY:      return "ready";
    }
    return "?";
}

#define EOS_BLE_BATTERY_UNKNOWN 0xFF

typedef struct {
    uint8_t  state;          // eos_ble_state_t
    bool     bonded;         // a bond record exists
    bool     connected;      // link up and subscribed
    bool     scanning;
    bool     passkey_shown;  // passkey below is live and the peer is waiting
    uint32_t passkey;        // six digits, 0 when none pending
    uint8_t  battery;        // 0..100, or EOS_BLE_BATTERY_UNKNOWN
    int8_t   rssi;           // of the connected peer, 0 when unknown
    eos_ble_bond_t bond;     // zeroed when !bonded
} eos_ble_status_t;

// ------------------------------------------------------------------ config

typedef struct {
    const char *host_name;      // GAP name. NULL means "penguinos".
    uint16_t    scan_ms;        // default scan duration
    uint16_t    reconnect_ms;   // backoff between reconnect attempts
    bool        auto_reconnect; // chase the bonded keyboard when it wakes up
} eos_ble_cfg_t;

static inline eos_ble_cfg_t eos_ble_defaults(void)
{
    eos_ble_cfg_t c;
    c.host_name      = "penguinos";
    c.scan_ms        = 6000;
    c.reconnect_ms   = 3000;
    c.auto_reconnect = true;
    return c;
}

// --------------------------------------------------------------- callbacks
//
// One observer each, because there is exactly one screen and the web page
// polls eos_ble_status() rather than being called back. Both are optional and
// both fire from the NimBLE host task, so they must not block and must not
// draw — the LCD ones set a flag the frame loop reads.

typedef void (*eos_ble_passkey_fn)(uint32_t passkey, const eos_ble_dev_t *peer, void *user);
typedef void (*eos_ble_state_fn)(uint8_t state, void *user);

void eos_ble_on_passkey(eos_ble_passkey_fn fn, void *user);
void eos_ble_on_state(eos_ble_state_fn fn, void *user);

// The sentence to print next to the passkey. It is a function and not a macro
// so the LCD and the web page cannot drift into two different warnings.
const char *eos_ble_pair_warning(void);

// --------------------------------------------------------------- lifecycle
//
// Init MUST run before WiFi. The controller wants a large contiguous block and
// the WiFi stack fragments the heap; on the tier-0 boards the order is the
// difference between booting and an OOM. Pass NULL for eos_ble_defaults().
//
// Returns EOS_ERR_NODEV on a host build and on a board whose descriptor does
// not declare input.ble_keyboard.
eos_err_t eos_ble_init(const eos_ble_cfg_t *cfg);

// Reconnect backoff and scan-timeout safety. Call once per main-loop pass;
// eos_input_tick() already does, so nothing else needs to.
void eos_ble_tick(uint32_t now_ms);

void eos_ble_status(eos_ble_status_t *out);
bool eos_ble_connected(void);

// The passkey currently on screen, or 0. Polled by the web status endpoint.
uint32_t eos_ble_passkey(void);

// ------------------------------------------------------------------- scan
//
// Takes the radio lock for the whole scan and releases it on completion, so a
// WiFi scan started meanwhile gets EOS_ERR_BUSY instead of garbage. ms == 0
// uses the configured default. Only devices that look like HID are kept.
eos_err_t eos_ble_scan_start(uint16_t ms);
eos_err_t eos_ble_scan_stop(void);
bool      eos_ble_scanning(void);

// Copies out up to max results, strongest signal first. Returns how many were
// copied. Safe to call while a scan is running; the table is a snapshot.
int eos_ble_scan_results(eos_ble_dev_t *out, int max);

// Milliseconds since the last scan finished, or EOS_BLE_SCAN_NEVER when none
// has. The web page needs it: the provisioning spec caches the scan and makes
// rescan an explicit button, and a button that will briefly disturb the radio
// has to be able to say how old the thing it would replace is.
#define EOS_BLE_SCAN_NEVER 0xFFFFFFFFu
uint32_t eos_ble_scan_age_ms(void);

// ------------------------------------------------------------------- pair
//
// Connects, encrypts, bonds, discovers the HID service and subscribes. The
// passkey callback fires partway through. Returns immediately: watch
// eos_ble_status() for the outcome. Fails with EOS_ERR_BUSY while a scan or an
// earlier attempt is still running.
eos_err_t eos_ble_pair(const uint8_t addr[6], uint8_t addr_type);

// The same, from the string a web form posted. The address TYPE is not on the
// wire and cannot be — public and random addresses are indistinguishable as
// text — so it is recovered from the scan table, where the advertisement said
// what it was. An address that was never scanned is assumed random, which is
// what almost every BLE keyboard uses. Pair from a scan result and this is
// exact; pair from a hand-typed address and it is a good guess.
eos_err_t eos_ble_pair_addr(const char *addr);

// Drops the bond on both sides of NVS — ours and NimBLE's — and disconnects.
eos_err_t eos_ble_forget(void);

// ------------------------------------------------------- portable helpers
//
// Everything below is plain C99 with no radio in it, which is what makes the
// host test possible. It is public because the HTTP layer formats the same
// addresses and the same bond record.

// "AA:BB:CC:DD:EE:FF". Returns the length written, or EOS_ERR_TOOBIG without
// touching out when max is short. Never truncates.
int eos_ble_addr_str(char *out, int max, const uint8_t addr[6]);

// The inverse. Accepts upper or lower case and ':' or '-' separators, rejects
// everything else including a short string with trailing garbage.
bool eos_ble_addr_parse(const char *s, uint8_t out[6]);

static inline bool eos_ble_addr_eq(const uint8_t a[6], const uint8_t b[6])
{
    int i;
    for (i = 0; i < 6; i++) if (a[i] != b[i]) return false;
    return true;
}

// Copies at most srclen bytes of an advertised name into a NUL-terminated
// buffer, replacing anything outside printable ASCII with '?'. The name comes
// off the air from an untrusted peripheral and ends up on the LCD and inside
// JSON, so it is sanitised once, here, at the point it enters the system.
// Returns the length written. src need not be NUL-terminated.
int eos_ble_name_sanitize(char *dst, int dstmax, const char *src, int srclen);

// Does this advertisement look like a HID peripheral? uuid16 may be NULL when
// the advertisement carried no service list.
bool eos_ble_adv_is_hid(uint16_t appearance, const uint16_t *uuid16, int n);

// Merges one advertisement into a scan table, in place. Returns the index it
// landed at, or -1 when the table was full of better candidates. Merging is
// what makes an active scan work: the name usually arrives in the scan
// response, several milliseconds after the service list, as a second report
// from the same address.
int eos_ble_devlist_add(eos_ble_dev_t *tbl, int *n, int max, const eos_ble_dev_t *d);

// Index of the entry with this address, or -1. Address only: a scan never holds
// the same address under two types, and the caller looking one up from a web
// form is exactly the caller that does not know the type.
int eos_ble_devlist_find(const eos_ble_dev_t *tbl, int n, const uint8_t addr[6]);

// EOS_BLE_BOND_BYTES on success, or a negative eos_err_t. Fixed size, so a
// short buffer is EOS_ERR_TOOBIG rather than a partial record.
int eos_ble_bond_encode(uint8_t *out, int max, const eos_ble_bond_t *b);

// False for a record that is the wrong length, has the wrong magic or version,
// fails its checksum, or claims a name longer than the field. Never reads past
// buf[len-1] and never writes past out.
bool eos_ble_bond_decode(eos_ble_bond_t *out, const uint8_t *buf, int len);

// --------------------------------------------------------- the radio lock
//
// It used to be declared here. It is now eos_radio.h, which belongs to neither
// stack: with it inside this header eos_net could only reach the lock through
// a build flag, and a firmware built without the BLE service was silently
// unserialised. This include keeps every existing caller of eos_radio_lock()
// working unchanged.
#include "eos_radio.h"

#endif // EOS_BLE_H
