// eos_settings — the twelve keys the Settings page edits, and the one file on
// /int that carries them across a reboot.
//
// A flat table of dotted keys, no nesting, every name at most fifteen bytes.
// That shape is not aesthetic: web/README.md fixed it so a settings key can
// also be an NVS key, and so a patch is a handful of key/value pairs rather
// than a document that has to be parsed and re-serialised whole on a board
// with 170 KB of heap and a single core.
//
// The rule that outranks everything else here is kernel/theme's rule, for the
// same reason: a settings file that is truncated, garbled, empty or absent
// must leave the caller holding usable defaults and must never stop the board
// booting. There is no partial load. A file that will not parse is a log line;
// a key with the wrong type keeps its default and the rest of the file still
// applies. The board that cannot be reconfigured because its config file is
// bad is the board that has to be recovered with a serial cable.
//
// WiFi credentials are the deliberate exception and they are NOT in this file.
// `wifi.ssid` and `wifi.psk` belong to eos_net and its NVS store, which
// enforces the one rule the whole provisioning flow rests on: credentials are
// written only after a join has actually succeeded. Persisting them here as
// well would be a second copy with no try-then-commit in front of it, and one
// typo in the Settings page would leave a board retrying a network it cannot
// reach. So those two keys ROUTE: eos_settings hands them to ports.wifi_join
// and stores nothing. `wifi.psk_set` is read back from eos_net and the
// passphrase itself is never readable in either direction.
//
// The second non-obvious constraint is wear. Writing this file is a LittleFS
// truncate-write-sync-close, and a sync is one or more 4 KB sector erases with
// the instruction cache off — tens of milliseconds during which the panel, the
// radio and the HTTP server are all stopped, on a chip with one core. So no
// HTTP worker ever writes flash here. eos_settings_set() mutates RAM and marks
// the store dirty; eos_settings_pump(), called from the OS loop, writes it out
// once the edits have been quiet for EOS_SETTINGS_DEBOUNCE_MS. Four saves in
// one second become one erase, which the filesystem underneath cannot work out
// for itself — see kernel/hal/backend/storage/README.md, "Wear".
//
// The file format is the wire format: exactly the object POST /api/settings
// takes, written back out. One shape, one parser — eos_httpd's bounded JSON
// reader and writer, which is why this file includes eos_httpd.h for a
// general-purpose reader rather than carrying a second JSON parser into an
// image that already has two.

#ifndef EOS_SETTINGS_H
#define EOS_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

#include "eos_board.h"    // eos_err_t

// Where the file lives. /int, never /sd: the card is removable and settings
// that vanish when somebody pulls it are worse than settings that never
// persisted at all.
#ifndef EOS_SETTINGS_PATH
#define EOS_SETTINGS_PATH "/int/settings.json"
#endif

// Field sizes, NUL included. Each is one larger than the maximum web/README.md
// states for that key, so a value at the documented limit round-trips and one
// byte past it is EOS_ERR_TOOBIG rather than a silent truncation.
#define EOS_SETTINGS_HOST_MAX    25    // net.host,   <= 24, also the mDNS label
#define EOS_SETTINGS_BHOST_MAX   48    // brain.host, <= 47, matches EOS_BRAIN_HOST_MAX
#define EOS_SETTINGS_MODEL_MAX   32    // brain.model,<= 31, matches EOS_BRAIN_MODEL_MAX
#define EOS_SETTINGS_SYSTEM_MAX 224    // brain.system, matches EOS_BRAIN_SYSTEM_MAX
#define EOS_SETTINGS_THEME_MAX   33    // ui.theme,   <= 32, matches EOS_THEME_NAME_MAX
#define EOS_SETTINGS_TZ_MAX      48    // sys.tz,     <= 47, a POSIX TZ string
#define EOS_SETTINGS_APP_MAX     17    // sys.autostart, an app id

// The serialised document. Every field at its maximum, escaped, plus the keys
// and the punctuation, comes to 560 bytes; the slack is for escapes in the
// system prompt. eos_settings_print() refuses rather than truncating.
#ifndef EOS_SETTINGS_DOC_MAX
#define EOS_SETTINGS_DOC_MAX 768
#endif

// How long the edits have to be quiet before the store reaches flash. Long
// enough that dragging the brightness slider is one erase and not sixty;
// short enough that a power cut a couple of seconds after a save is the only
// window in which an edit is lost.
#ifndef EOS_SETTINGS_DEBOUNCE_MS
#define EOS_SETTINGS_DEBOUNCE_MS 2000u
#endif

// The default theme name. It matches the theme firmware/main links into the
// image with EMBED_TXTFILES, so a board with an empty filesystem asks for a
// theme it definitely has.
#ifndef EOS_SETTINGS_THEME_DEFAULT
#define EOS_SETTINGS_THEME_DEFAULT "cyd-amber"
#endif

#define EOS_SETTINGS_PORT_DEFAULT   80
#define EOS_SETTINGS_MAXTOK_DEFAULT 256
#define EOS_SETTINGS_BRIGHT_DEFAULT 255

// ------------------------------------------------------------------- keys
//
// The order is the order GET /api/settings emits them in, which is the order
// the Settings page draws its four panels. `wifi.psk` is never emitted.

typedef enum {
    EOS_SET_WIFI_SSID = 0,   // routed to eos_net, not stored here
    EOS_SET_WIFI_PSK,        // routed, write-only, never read back
    EOS_SET_WIFI_PSK_SET,    // read-only, derived: does eos_net hold one
    EOS_SET_NET_HOST,
    EOS_SET_BRAIN_HOST,
    EOS_SET_BRAIN_PORT,
    EOS_SET_BRAIN_MODEL,
    EOS_SET_BRAIN_MAX,
    EOS_SET_BRAIN_SYSTEM,
    EOS_SET_UI_THEME,
    EOS_SET_UI_BRIGHT,
    EOS_SET_SYS_TZ,
    EOS_SET_SYS_AUTOSTART,
    EOS_SET_COUNT
} eos_settings_key_t;

typedef enum {
    EOS_SET_T_STR = 0,
    EOS_SET_T_INT,
    EOS_SET_T_BOOL,
} eos_settings_type_t;

// What a key is, without the caller having to know which one it is.
#define EOS_SET_F_PERSIST  0x01   // lives in the file
#define EOS_SET_F_REBOOT   0x02   // will not take effect until the board restarts
#define EOS_SET_F_ROUTED   0x04   // handed to eos_net; this store keeps nothing
#define EOS_SET_F_WRITEONLY 0x08  // accepted, never emitted
#define EOS_SET_F_READONLY 0x10   // emitted, refused as EOS_ERR_READONLY

// ------------------------------------------------------------- the values

// About 440 bytes. Meant for BSS alongside the store below; nothing here
// allocates and nothing here is safe to touch from an ISR.
typedef struct {
    char     net_host[EOS_SETTINGS_HOST_MAX];
    char     brain_host[EOS_SETTINGS_BHOST_MAX];
    char     brain_model[EOS_SETTINGS_MODEL_MAX];
    char     brain_system[EOS_SETTINGS_SYSTEM_MAX];
    char     ui_theme[EOS_SETTINGS_THEME_MAX];
    char     sys_tz[EOS_SETTINGS_TZ_MAX];
    char     sys_autostart[EOS_SETTINGS_APP_MAX];
    uint16_t brain_port;      // 1..65535
    uint16_t brain_max;       // 16..2048
    uint8_t  ui_bright;       // 0..255
} eos_settings_t;

// --------------------------------------------------------------- the ports
//
// Two things this store cannot do for itself: hand the WiFi keys to eos_net,
// and make a value take effect on hardware it does not know about. Both are
// function pointers for the same reason eos_httpd's radios are — it is what
// lets the whole store, the parser, the clamps and the routing run in a host
// suite with no radio, no panel and no flash.

typedef struct {
    void *ctx;

    // ssid or psk may be NULL, meaning "leave that half alone". The web
    // contract says an empty psk means keep the stored one, so an empty string
    // never reaches this. Returns 0 or a negative eos_err_t.
    int  (*wifi_set)(void *ctx, const char *ssid, const char *psk);

    // What GET /api/settings reports for wifi.ssid and wifi.psk_set. Both may
    // be NULL, which reports "" and false — a board with no network service.
    bool (*wifi_ssid)(void *ctx, char *out, int cap);
    bool (*wifi_psk_set)(void *ctx);

    // Make `key` take effect now. Called after the value has been written into
    // the store, so the whole settings object is available. Return EOS_OK when
    // it applied, EOS_ERR_UNSUPPORTED when it cannot be applied until reboot
    // (the caller adds the key to reboot_required and the value is still
    // kept), or another negative eos_err_t to REJECT the value — in which case
    // the store rolls the key back to what it was.
    eos_err_t (*apply)(void *ctx, int key, const eos_settings_t *s);
} eos_settings_ports_t;

// --------------------------------------------------------------- the store

typedef enum {
    EOS_SETTINGS_OK = 0,
    EOS_SETTINGS_ERR_ABSENT,   // no such file. The first boot, and not an error
    EOS_SETTINGS_ERR_EMPTY,    // the file exists and holds nothing
    EOS_SETTINGS_ERR_SYNTAX,   // not a JSON object, truncated, or garbage bytes
    EOS_SETTINGS_ERR_IO,       // the filesystem refused the read
    EOS_SETTINGS_ERR_FIELD,    // the document parsed; at least one key did not
} eos_settings_err_t;

const char *eos_settings_strerror(eos_settings_err_t e);

typedef struct {
    eos_settings_t       v;
    eos_settings_ports_t ports;

    // The WiFi pair, staged. eos_net joins with an SSID and a passphrase
    // together, so a patch that carries either one is held here until the
    // whole patch has been applied and eos_settings_route_commit() hands both
    // over at once. The passphrase is wiped the moment that returns; it never
    // reaches the file and does not linger in BSS.
    char     route_ssid[33];
    char     route_psk[64];
    bool     route_ssid_set;
    bool     route_psk_set;

    bool     dirty;          // RAM differs from flash
    bool     from_file;      // the load found a file that parsed
    // The OS loop's clock, as of the last eos_settings_pump(). The setters do
    // not take a time of their own: an edit is stamped with the last tick,
    // which on a 250 ms loop makes the debounce accurate to one tick and
    // saves every caller having to carry a clock into a settings write.
    uint32_t clock_ms;
    uint32_t dirty_at_ms;    // when the last edit landed
    uint32_t saves;          // how many times this store has reached flash
    uint32_t save_fails;
    uint16_t bad_fields;     // bit per key the last load could not use
    eos_settings_err_t last_load;
    eos_err_t          last_save;
} eos_settings_store_t;

// -------------------------------------------------------------- key facts

const char *eos_settings_key_name(int key);     // "wifi.ssid", NULL out of range
int         eos_settings_key_of(const char *name);   // the key, or -1
uint8_t     eos_settings_key_flags(int key);
uint8_t     eos_settings_key_type(int key);     // eos_settings_type_t

// True when this key cannot take effect until the board restarts. Static
// metadata, straight out of web/README.md's table; ports.apply may add a key
// to the same list at runtime by answering EOS_ERR_UNSUPPORTED.
static inline bool eos_settings_key_reboot(int key)
{
    return (eos_settings_key_flags(key) & EOS_SET_F_REBOOT) != 0;
}

// ----------------------------------------------------------- values, pure
//
// These touch neither the ports nor the filesystem, which is what makes the
// round trip and the corrupt-input cases testable without either.

void eos_settings_defaults(eos_settings_t *s);

// Parses `len` bytes at `buf`. Reads nothing outside [buf, buf+len). On ANY
// document-level failure `out` holds exactly what eos_settings_defaults()
// would have written and the return says why. On EOS_SETTINGS_ERR_FIELD the
// document was fine and at least one key was not: every good key applied and
// each bad one kept its default, with its bit set in *bad_fields when that is
// not NULL. Numbers out of range are CLAMPED rather than rejected, because a
// brightness of 4000 is a typo and refusing the whole file over it would cost
// the owner every other setting.
eos_settings_err_t eos_settings_parse(eos_settings_t *out, const char *buf, int len,
                                      uint16_t *bad_fields);

// Writes the persisted keys as the same flat object POST /api/settings takes.
// Returns the byte count, or EOS_ERR_TOOBIG — never a truncated document.
int eos_settings_print(const eos_settings_t *s, char *buf, int cap);

// The current value of `key` as text. Numeric keys come back as digits; ask
// eos_settings_key_type() before deciding how to emit them. A write-only key
// is EOS_ERR_READONLY and writes nothing. wifi.ssid and wifi.psk_set read
// through the ports, so they take the store rather than the values.
int eos_settings_get(const eos_settings_store_t *st, int key, char *out, int cap);

// ------------------------------------------------------- the live store

void eos_settings_init(eos_settings_store_t *st, const eos_settings_ports_t *ports);

// Reads EOS_SETTINGS_PATH through eos_storage. Never fails in a way the caller
// has to handle: whatever happens, the store afterwards holds a usable set of
// values. The return is for the log line.
eos_settings_err_t eos_settings_load(eos_settings_store_t *st);

// Applies one key. `val` is the decoded string form for EOS_SET_T_STR keys and
// is ignored for numbers; `num` is the reverse. Strings longer than the field
// are EOS_ERR_TOOBIG and change nothing. Numbers clamp. Routed keys go to the
// ports and are never stored. On success the key is applied through
// ports.apply and the store is marked dirty — no flash is touched here.
//
// *reboot, when not NULL, is set true if the value will not take effect until
// the board restarts, whether that is because the key says so or because
// ports.apply answered EOS_ERR_UNSUPPORTED.
eos_err_t eos_settings_set_str(eos_settings_store_t *st, int key,
                               const char *val, bool *reboot);
eos_err_t eos_settings_set_num(eos_settings_store_t *st, int key,
                               long num, bool *reboot);

// Hands the staged WiFi pair to eos_net. Call it once after the whole patch
// has been applied, never per key: a join needs the SSID and the passphrase
// together and starting one with half a pair joins a network the other half
// does not name. A no-op when the patch carried neither key, and the reason
// this is not folded into the setters. Wipes the staged passphrase either way.
// EOS_ERR_UNSUPPORTED when the board has no network service bound.
eos_err_t eos_settings_route_commit(eos_settings_store_t *st);

// Writes the store out now, if it is dirty. This is the call that erases flash
// sectors and stops the chip for tens of milliseconds; it belongs on the OS
// loop and nowhere else. Returns EOS_OK when there was nothing to do.
eos_err_t eos_settings_flush(eos_settings_store_t *st);

// The debounce. Call it from the same loop that pumps eos_net and eos_httpd,
// with the same millisecond clock. It flushes once the store has been dirty
// and untouched for EOS_SETTINGS_DEBOUNCE_MS.
void eos_settings_pump(eos_settings_store_t *st, uint32_t now_ms);

#endif // EOS_SETTINGS_H
