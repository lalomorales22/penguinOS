// eos_httpd — the on-board HTTP server: the provisioning API, and the web app.
//
// This is the thing that makes a board carried into a strange room recoverable.
// It runs in SETUP mode behind the SoftAP, answers the captive-portal probes so
// the phone pops the page by itself, and offers the eight endpoints in
// docs/provisioning.md; in RUN mode it serves the same app from LittleFS over
// the joined network. Nothing here decides WiFi or Bluetooth policy — it only
// exposes what eos_net and eos_ble already do.
//
// The non-obvious constraint is that a WiFi scan takes about three seconds, a
// join takes up to fifteen, and a BLE scan takes five — while an esp_http_server
// has four workers and a phone gives up in about ten. So no handler here ever
// waits for a radio. Every slow operation is a job: the POST or the rescan
// starts it and returns immediately, and the client polls a status endpoint
// until the state changes. That is also the only reason a browser reloading the
// setup page mid-scan cannot pile four blocked workers onto one radio.
//
// The second constraint is that an SSID is 32 arbitrary bytes off the air. It
// will contain quotes, backslashes, control bytes and invalid UTF-8, and a JSON
// writer that assumes otherwise emits a document the phone cannot parse — on
// exactly the screen the owner needs to get the board onto the network. The
// writer below escapes to RFC 8259 and substitutes U+FFFD for every byte that
// is not part of a well-formed UTF-8 sequence, so its output is always valid
// UTF-8; the raw bytes ride alongside as hex in `ssid_hex` for the round trip.
//
// No allocation. The response document and the staged path live in eos_httpd_t,
// which is meant for BSS, and dispatch is serialised so there is exactly one of
// each. A phone that reloads the page ten times costs the heap nothing.
//
// The request body is the exception and deliberately so: it is a bounded buffer
// on the worker's own stack, received BEFORE the lock is taken. A client that
// declares a content length and then goes quiet would otherwise hold the whole
// server for as long as it cared to, which is a wedge one open socket can cause.
//
// Everything above the port table is portable C99 and is what the host test
// exercises. The esp_http_server, LittleFS and DNS bindings are the tail of
// eos_httpd.c behind #ifdef ESP_PLATFORM.

#ifndef EOS_HTTPD_H
#define EOS_HTTPD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---------------------------------------------------------------- tunables
//
// These size fixed buffers inside eos_httpd_t. Defaults are the tier-1 numbers;
// a tier-0 board that runs provisioning as a mode can cut EOS_HTTPD_RESP_MAX
// and EOS_HTTPD_SCAN_MAX and lose nothing but the length of the network list.

#ifndef EOS_HTTPD_URI_MAX
#define EOS_HTTPD_URI_MAX 160    // request target, query string included
#endif
#ifndef EOS_HTTPD_BODY_MAX
#define EOS_HTTPD_BODY_MAX 512   // a POST body larger than this is 413, never read
#endif
// That number is spent out of the HTTP worker's task stack, byte for byte: the
// body buffer lives in on_request()'s frame. Measured on riscv32 at -Os, the
// deepest request path costs 1,248 bytes of ESP-OS frames of which 513 are this
// buffer, against the 5,376-byte stack eos_httpd_start() asks for and the 4,096
// esp_http_server assumes for a bare handler. Raising BODY_MAX without raising
// cfg.stack_size in eos_httpd_start() by the same amount spends the margin.
#ifndef EOS_HTTPD_RESP_MAX
#define EOS_HTTPD_RESP_MAX 4096  // the whole JSON document, built in one pass
#endif
#ifndef EOS_HTTPD_SCAN_MAX
#define EOS_HTTPD_SCAN_MAX 16    // networks or peripherals reported per response
#endif
#ifndef EOS_HTTPD_PATH_MAX
#define EOS_HTTPD_PATH_MAX 96    // matches EOS_PATH_MAX; a longer path is 404
#endif
#ifndef EOS_HTTPD_HDR_MAX
#define EOS_HTTPD_HDR_MAX 12     // request headers accepted before the request is
#endif                           // refused; esp_http_server enforces this one
#ifndef EOS_HTTPD_JSON_DEPTH
#define EOS_HTTPD_JSON_DEPTH 6   // nesting the writer tracks; ours never exceeds 3
#endif
#ifndef EOS_HTTPD_SCAN_POOL
#define EOS_HTTPD_SCAN_POOL 48   // cached results ranked before the top SCAN_MAX
#endif                           // are reported; costs 2 bytes of stack each
#ifndef EOS_HTTPD_CHUNK
#define EOS_HTTPD_CHUNK 1024     // bytes moved per read when streaming a file
#endif

#if EOS_HTTPD_SCAN_POOL < EOS_HTTPD_SCAN_MAX
#error "EOS_HTTPD_SCAN_POOL below EOS_HTTPD_SCAN_MAX would rank fewer results than it reports"
#endif
#if EOS_HTTPD_SCAN_MAX > 64
#error "EOS_HTTPD_SCAN_MAX past 64 will not fit EOS_HTTPD_RESP_MAX; raise both or neither"
#endif
#if EOS_HTTPD_JSON_DEPTH > 32
#error "EOS_HTTPD_JSON_DEPTH past 32 will not fit eos_json_t.isobj"
#endif
#if EOS_HTTPD_BODY_MAX < 160
#error "EOS_HTTPD_BODY_MAX below 160 cannot hold a 32-byte SSID and a 63-byte PSK as JSON"
#endif

// ============================================================ JSON writing
//
// A bounded writer over a caller-provided buffer. Overflow is sticky and
// silent at the call site — every write after the first that did not fit is a
// no-op — and eos_json_ok() is the single place it is discovered. That is
// deliberate: a handler that checks after every field is a handler that will
// forget one, and a truncated JSON document is worse than a 500.
//
// The buffer is NUL-terminated after every call, including the ones that
// overflowed, so it is always safe to print.

typedef struct {
    char    *buf;
    int      cap;                            // usable bytes, NUL not included
    int      len;
    bool     ovf;
    uint8_t  depth;
    uint32_t isobj;                          // bit d set: level d is an object
    bool     first[EOS_HTTPD_JSON_DEPTH];    // does this level still need no comma
} eos_json_t;

void eos_json_init(eos_json_t *j, char *buf, int cap);
bool eos_json_ok(const eos_json_t *j);       // false once anything overflowed

void eos_json_obj_open(eos_json_t *j);
void eos_json_obj_close(eos_json_t *j);
void eos_json_arr_open(eos_json_t *j);
void eos_json_arr_close(eos_json_t *j);
void eos_json_key(eos_json_t *j, const char *key);

// Values. eos_json_strn takes arbitrary bytes; eos_json_str is the NUL-terminated
// convenience. Both escape and both repair invalid UTF-8.
void eos_json_strn(eos_json_t *j, const char *s, int n);
void eos_json_str(eos_json_t *j, const char *s);
void eos_json_hexn(eos_json_t *j, const void *bytes, int n);   // as a lowercase hex string
void eos_json_int(eos_json_t *j, long v);
void eos_json_bool(eos_json_t *j, bool v);
void eos_json_null(eos_json_t *j);

// key/value in one call, which is how the handlers spell everything.
void eos_json_kv_strn(eos_json_t *j, const char *key, const char *s, int n);
void eos_json_kv_str(eos_json_t *j, const char *key, const char *s);
void eos_json_kv_int(eos_json_t *j, const char *key, long v);
void eos_json_kv_bool(eos_json_t *j, const char *key, bool v);
void eos_json_kv_null(eos_json_t *j, const char *key);

// The number of bytes eos_json_strn would emit for these n bytes, escaping and
// UTF-8 repair included, quotes excluded. Handlers use it to decide whether one
// more array element still fits, so the list is short rather than truncated.
int eos_json_escaped_len(const char *s, int n);

// ============================================================ JSON reading
//
// Just enough to read a flat request body: find a key at the top level of an
// object and hand back its value. Nested objects and arrays are skipped, not
// descended into, so {"a":{"ssid":"x"},"ssid":"y"} yields "y" and never "x".

typedef enum {
    EOS_JSON_ABSENT  =  0,   // well-formed document, no such key at depth 1
    EOS_JSON_FOUND   =  1,
    EOS_JSON_BAD     = -1,   // not a JSON object, or malformed before the key
    EOS_JSON_TOOBIG  = -2,   // the value does not fit the caller's buffer
    EOS_JSON_TYPE    = -3,   // the key is there and is not the type asked for
} eos_json_find_t;

// Decodes escapes, \uXXXX and surrogate pairs included, into UTF-8. out is
// always NUL-terminated on FOUND; *out_len is the byte length, which may be
// shorter than the source. A value carrying an escaped NUL is rejected as
// EOS_JSON_BAD rather than silently shortened: a truncated SSID names a
// different network.
eos_json_find_t eos_json_get_str(const char *body, int len, const char *key,
                                 char *out, int out_cap, int *out_len);
eos_json_find_t eos_json_get_int(const char *body, int len, const char *key, long *out);
eos_json_find_t eos_json_get_bool(const char *body, int len, const char *key, bool *out);

// ============================================================== URI parsing

// Copies the path part of a request target (everything before '?') into out,
// percent-decoding it. Returns the length, or a negative eos_err_t: TOOBIG if
// it does not fit, ARG if a percent escape is malformed or the decoded path
// contains a NUL. Never truncates — a truncated path names a different file.
int eos_httpd_path_of(const char *uri, char *out, int out_cap);

// Finds `name` in the query string of `uri` and percent-decodes its value.
// Returns the decoded length, EOS_ERR_NOTFOUND when absent, EOS_ERR_TOOBIG when
// it does not fit. A bare `?rescan` with no '=' is a present, empty value.
int eos_httpd_query_get(const char *uri, const char *name, char *out, int out_cap);

// True for a query value a human means as yes: "1", "true", "yes", "on", "".
bool eos_httpd_flag(const char *v);

// ================================================================== routing

typedef enum {
    EOS_ROUTE_NONE = 0,        // 404
    EOS_ROUTE_METHOD,          // the path exists, the method does not: 405
    EOS_ROUTE_WIFI_SCAN,
    EOS_ROUTE_WIFI_CONNECT,
    EOS_ROUTE_WIFI_FORGET,
    EOS_ROUTE_NET_STATUS,
    EOS_ROUTE_BLE_SCAN,
    EOS_ROUTE_BLE_PAIR,
    EOS_ROUTE_BLE_STATUS,
    EOS_ROUTE_BLE_FORGET,
    EOS_ROUTE_CAPTIVE,         // an OS connectivity probe: redirect to the portal
    EOS_ROUTE_STATIC,          // a file under the active document root
} eos_route_t;

// method is "GET" or "POST", uppercase, exactly as it came off the wire. uri is
// the raw target. Any path under /api/ that is not in the table is NONE, never
// STATIC, so a typo cannot fall through to the filesystem.
eos_route_t eos_httpd_route(const char *method, const char *uri);

// The Content-Type for a static path, by extension. Unknown extensions get
// application/octet-stream, which is right: a browser must not be told to
// execute something the board cannot name.
const char *eos_httpd_mime(const char *path);

// True when this path is one of the connectivity-check URLs iOS, Android,
// Windows, Firefox or GNOME fetch to decide whether a network is walled.
bool eos_httpd_is_captive_probe(const char *path);

// ================================================================ the ports
//
// Everything the handlers need from the radios, as function pointers. This is
// the same shape as eos_brain's transport and for the same reason: it is what
// lets the whole request layer run on the host with no WiFi, no Bluetooth and
// no IDF, and it means the exact spelling of eos_net.h and eos_ble.h is
// absorbed in one adapter at the bottom of eos_httpd.c instead of spread
// through nine handlers.

typedef enum {
    EOS_HTTPD_SCAN_IDLE = 0,
    EOS_HTTPD_SCAN_RUNNING,
    EOS_HTTPD_SCAN_DONE,      // results are cached and readable
    EOS_HTTPD_SCAN_FAILED,
} eos_httpd_scan_state_t;

typedef enum {
    EOS_HTTPD_NET_DOWN = 0,
    EOS_HTTPD_NET_SETUP,      // SoftAP is up, no station link
    EOS_HTTPD_NET_JOINING,
    EOS_HTTPD_NET_UP,         // station has an IP
} eos_httpd_net_state_t;

// The outcome of the last join. It reaches the web app as two fields, a coarse
// `state` the page branches on and a `reason` it turns into a sentence, because
// "failed" on its own is useless: a refused password, a misspelt name and a
// board that is out of range need three different things from the person.
typedef enum {
    EOS_HTTPD_JOIN_NONE = 0,  // nothing has been tried since boot
    EOS_HTTPD_JOIN_OK,
    EOS_HTTPD_JOIN_RUNNING,
    EOS_HTTPD_JOIN_AUTH,      // wrong password         -> failed / bad_auth
    EOS_HTTPD_JOIN_NOTFOUND,  // SSID was not on the air -> failed / no_ap
    EOS_HTTPD_JOIN_TIMEOUT,   // associated, no address  -> failed / ip_fail
    EOS_HTTPD_JOIN_FAILED,    // anything else           -> failed / failed
} eos_httpd_join_t;

// "none" / "ok" / "trying" / "failed", and the reason behind a failure, or NULL
// when there is nothing to explain. The vocabulary is the web app's, not this
// file's — web/README.md's "Why a join failed" table is the other half of it.
const char *eos_httpd_join_state(int j);
const char *eos_httpd_join_reason(int j);

// Auth modes, reported as a lowercase string by /api/wifi/scan. The numbers
// match nothing in IDF on purpose — the adapter maps wifi_auth_mode_t onto
// these, so an IDF release that renumbers its enum cannot change the API.
typedef enum {
    EOS_HTTPD_AUTH_OPEN = 0,
    EOS_HTTPD_AUTH_WEP,
    EOS_HTTPD_AUTH_WPA,
    EOS_HTTPD_AUTH_WPA2,
    EOS_HTTPD_AUTH_WPA_WPA2,
    EOS_HTTPD_AUTH_WPA3,
    EOS_HTTPD_AUTH_WPA2_WPA3,
    EOS_HTTPD_AUTH_ENTERPRISE,
    EOS_HTTPD_AUTH_OTHER,
} eos_httpd_auth_t;

const char *eos_httpd_auth_name(int auth);

typedef struct {
    uint8_t ssid[32];      // raw bytes off the air. NOT NUL-terminated.
    uint8_t ssid_len;      // 0..32. A hidden network is length 0.
    uint8_t bssid[6];
    int8_t  rssi;          // dBm
    uint8_t channel;
    uint8_t auth;          // eos_httpd_auth_t
    bool    saved;         // matches the credentials in NVS
} eos_httpd_ap_t;

typedef struct {
    uint8_t  state;        // eos_httpd_net_state_t
    uint8_t  join;         // eos_httpd_join_t, the outcome of the last attempt
    uint8_t  ssid[32];     // the station's network, raw bytes
    uint8_t  ssid_len;
    bool     ssid_stored;  // credentials are in NVS
    int8_t   rssi;
    char     ip[16];       // "192.168.0.51", or "" when there is none
    bool     ap_up;
    uint8_t  ap_ssid[32];
    uint8_t  ap_ssid_len;
    char     ap_ip[16];
    uint8_t  ap_clients;
    char     host[25];     // mDNS label without ".local"
} eos_httpd_net_t;

typedef struct {
    uint8_t name[32];      // raw bytes out of the advertisement
    uint8_t name_len;
    char    addr[18];      // "aa:bb:cc:dd:ee:ff", lowercase
    int8_t  rssi;
    bool    is_hid;        // advertises service 0x1812 or appearance 0x03C1
    bool    bonded;        // this is the device already in the bond record
} eos_httpd_ble_dev_t;

// Mirrors eos_ble_state_t numerically. It is restated rather than included so
// the handlers still compile with no BLE service present; the adapter asserts
// the two agree.
typedef enum {
    EOS_HTTPD_BLE_OFF = 0,
    EOS_HTTPD_BLE_IDLE,
    EOS_HTTPD_BLE_SCANNING,
    EOS_HTTPD_BLE_CONNECTING,
    EOS_HTTPD_BLE_PAIRING,      // a passkey is on the panel and being typed
    EOS_HTTPD_BLE_READY,        // subscribed; keystrokes are flowing
} eos_httpd_ble_state_t;

const char *eos_httpd_ble_state_name(int s);

typedef struct {
    bool     bonded;
    bool     connected;
    bool     pairing;          // a pair attempt is in flight
    uint8_t  name[32];
    uint8_t  name_len;
    char     addr[18];         // "" when nothing is bonded
    int16_t  battery;          // percent, or -1 when the device does not say
    bool     passkey_shown;    // the host has picked a passkey and wants it typed
    uint32_t passkey;          // six digits, valid only while passkey_shown
    uint8_t  state;            // eos_httpd_ble_state_t
    const char *reason;        // why the last attempt failed, or NULL. The web
                               // app's vocabulary: not_found, connect_fail,
                               // bond_fail, no_hid, no_reports, timeout.
} eos_httpd_ble_status_t;

typedef struct {
    // WiFi. scan_start returns 0, or a negative eos_err_t; BUSY means the radio
    // is doing something else and the caller must not have asked.
    int      (*wifi_scan_state)(void *ctx);
    int      (*wifi_scan_count)(void *ctx);
    bool     (*wifi_scan_get)(void *ctx, int i, eos_httpd_ap_t *out);
    uint32_t (*wifi_scan_age_ms)(void *ctx);
    int      (*wifi_scan_start)(void *ctx);

    // Starts a join and returns immediately. MUST NOT write the credentials to
    // NVS here — it stores them only once the join has succeeded. See the note
    // on eos_httpd_start().
    int      (*wifi_join)(void *ctx, const uint8_t *ssid, int ssid_len,
                          const char *psk, int psk_len);
    // Same job shape as the join, and for the same reason: dropping the
    // credentials tears the station link down — which in RUN mode is the socket
    // this request arrived on — and brings the SoftAP back up. It queues and
    // returns; eos_httpd_pump() does the work.
    int      (*wifi_forget)(void *ctx);
    bool     (*net_status)(void *ctx, eos_httpd_net_t *out);

    // BLE. Same job shape.
    int      (*ble_scan_state)(void *ctx);
    int      (*ble_scan_count)(void *ctx);
    bool     (*ble_scan_get)(void *ctx, int i, eos_httpd_ble_dev_t *out);
    uint32_t (*ble_scan_age_ms)(void *ctx);
    int      (*ble_scan_start)(void *ctx);
    int      (*ble_pair)(void *ctx, const char *addr);
    int      (*ble_forget)(void *ctx);
    bool     (*ble_status)(void *ctx, eos_httpd_ble_status_t *out);

    // The sentence shown next to the passkey. Optional: when it is NULL the
    // pair handler uses its own wording. It is a port rather than a constant
    // because eos_ble owns that sentence and the panel prints the same one —
    // two spellings of the same warning is how one of them goes stale.
    const char *(*ble_pair_warning)(void *ctx);

    // Optional. Bytes read, 0 at end of file, negative on error; open/close
    // bracket it. NULL for all three disables static serving and every
    // EOS_ROUTE_STATIC becomes a 404 — which is what a board with no
    // filesystem wants, since the built-in setup page is served without them.
    void    *(*file_open)(void *ctx, const char *path, long *size_out);
    int      (*file_read)(void *ctx, void *fh, void *buf, int n);
    void     (*file_close)(void *ctx, void *fh);
} eos_httpd_ports_t;

// ============================================================== the server

typedef enum {
    EOS_HTTPD_MODE_SETUP = 0,   // SoftAP, captive portal, setup document root
    EOS_HTTPD_MODE_RUN,         // joined network, full app, no portal redirects
} eos_httpd_mode_t;

typedef struct {
    uint8_t     mode;           // eos_httpd_mode_t
    const char *root_setup;     // default "/int/setup"
    const char *root_run;       // default "/int/web"
    const char *portal_ip;      // default "192.168.4.1", where probes redirect
    uint16_t    port;           // default 80
    uint8_t     workers;        // default 4
} eos_httpd_cfg_t;

void eos_httpd_cfg_default(eos_httpd_cfg_t *cfg);

// One response, staged. Exactly one of body/file/redirect is meaningful, said
// by `kind`. body points into the server's own buffer and is valid until the
// next dispatch on the same server.
typedef enum {
    EOS_HTTPD_BODY_BUF = 0,     // body[0..body_len) is the whole response
    EOS_HTTPD_BODY_FILE,        // stream `path` instead, already resolved
    EOS_HTTPD_BODY_REDIRECT,    // 302 to `location`
} eos_httpd_body_kind_t;

typedef struct {
    int         status;             // 200, 202, 302, 400, 404, 405, 409, 413, 500, 503
    uint8_t     kind;               // eos_httpd_body_kind_t
    const char *content_type;
    const char *content_encoding;   // NULL, or "gzip"
    const char *cache_control;
    const char *location;           // EOS_HTTPD_BODY_REDIRECT only
    const char *path;               // EOS_HTTPD_BODY_FILE only
    long        file_size;          // EOS_HTTPD_BODY_FILE only, -1 if unknown
    const char *body;
    int         body_len;
    // EOS_HTTPD_BODY_FILE only: the open handle, positioned at byte 0. Dispatch
    // opened it to find out whether the .gz twin existed, so the caller streams
    // from it and closes it — it must not open the path a second time.
    void       *file;
} eos_httpd_resp_t;

typedef struct {
    const char *method;         // "GET" / "POST", uppercase
    const char *uri;            // raw request target
    const char *body;           // NULL when there is none
    int         body_len;
    bool        body_truncated; // the transport refused a body over BODY_MAX
} eos_httpd_req_t;

typedef struct {
    eos_httpd_ports_t ports;
    void             *ctx;
    eos_httpd_cfg_t   cfg;

    char   resp[EOS_HTTPD_RESP_MAX];
    char   path[EOS_HTTPD_PATH_MAX];      // staged static path, ".gz" included
    char   arg[80];                       // one decoded body field at a time
    char   uripath[EOS_HTTPD_URI_MAX];
    char   loc[64];                       // staged redirect target

    // Counters. Cheap, and the only way to see a captive portal misbehave from
    // the other side of a SoftAP.
    uint32_t req_total, req_api, req_static, req_portal, req_rejected;

    bool   running;
    void  *impl;                          // httpd_handle_t under ESP_PLATFORM
    void  *lock;                          // the mutex that makes the buffers above
                                          // safe to share across four workers
    void  *dns;                           // the captive DNS task handle
} eos_httpd_t;

// Wires the ports and the config. Does not touch a radio and does not open a
// socket; eos_httpd_start() does that.
//
// The rule the whole flow rests on lives on the other side of ports.wifi_join:
// credentials are written to NVS ONLY after the join has succeeded. This server
// never asks for them to be stored, and POST /api/wifi/connect returns 202 with
// the attempt still running, so there is no moment at which it could. Save then
// try means one typo leaves the board retrying a network it cannot reach,
// forever, recoverable only with a serial cable.
void eos_httpd_init(eos_httpd_t *h, const eos_httpd_ports_t *ports, void *ctx,
                    const eos_httpd_cfg_t *cfg);

// Builds the response for one request. Pure: no sockets, no clock, no radio
// beyond the port calls. This is the whole server, and it is what the host test
// drives. Returns resp->status for convenience.
int eos_httpd_dispatch(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *resp);

// The built-in setup page. Served when SETUP mode has no document root on
// flash, which is every board that has never been provisioned. It is not the
// web app — it is the smallest thing that can list networks and join one, so
// that an empty filesystem is still a recoverable board.
const char *eos_httpd_builtin_setup(int *len_out);

#ifdef ESP_PLATFORM
// Starts esp_http_server on cfg.port. It does NOT start a DNS responder: the
// captive-portal DNS belongs to eos_net, which raises it with the SoftAP, and
// a second listener on port 53 would simply fail to bind.
int eos_httpd_start(eos_httpd_t *h);
int eos_httpd_stop(eos_httpd_t *h);

// Wires the ports to eos_net and eos_ble, and sets h->ctx. `net`
// is the eos_net_t the caller started, as a void * so this header does not drag
// eos_net.h into every host suite that includes it; pass NULL for a board with
// no network and the WiFi endpoints answer 501. eos_ble is a singleton and
// needs no handle.
//
// It does NOT wire the three file ports. kernel/hal has no storage backend yet
// and binding one here would make every image that starts this server require
// one; the port table exists so that binding happens at the edge. They are left
// NULL, which this header already defines as "no filesystem" — static routes
// 404 and SETUP serves the built-in page. A caller with a filesystem assigns
// h->ports.file_open/read/close itself after this returns.
void eos_httpd_idf_bind(eos_httpd_t *h, void *net);

// Runs the deferred radio work the handlers queued, from the OS loop, with the
// same millisecond clock eos_net_pump() gets. This is where a queued join
// actually happens, and it is the one place credentials are written: it calls
// eos_net_try(), and eos_net_commit() only if that returned OK. The queued
// rescan and the queued forget are drained here too, which is what leaves
// eos_net_t with exactly one writer — this task — and esp_wifi's mode with one
// caller. A forget outranks both of the others: it invalidates them.
//
// It must not run on the HTTP task. eos_net_try() blocks for up to fifteen
// seconds and the connection that asked for the join comes through the SoftAP
// the radio is about to re-mode; that is the whole reason POST
// /api/wifi/connect answers 202 and the client polls /api/net/status.
void eos_httpd_pump(eos_httpd_t *h, uint32_t now_ms);
#endif

#endif // EOS_HTTPD_H
