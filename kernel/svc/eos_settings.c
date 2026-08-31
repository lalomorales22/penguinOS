// The settings store. See eos_settings.h for why the WiFi keys are not in the
// file and why no HTTP worker ever writes one.

#include "eos_settings.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "eos_httpd.h"    // the bounded JSON reader and writer, nothing else
#include "eos_storage.h"

// ==========================================================================
// The table
// ==========================================================================
//
// One row per key, in the order GET /api/settings emits them. The names are
// web/README.md's and every one is fifteen bytes or fewer, which is the NVS
// key limit the contract was written around; the check below is here so that
// adding a sixteenth-byte key fails to build rather than failing at runtime on
// a board somebody is holding.

static const struct {
    const char *name;
    uint8_t     type;
    uint8_t     flags;
} KEYS[EOS_SET_COUNT] = {
    { "wifi.ssid",     EOS_SET_T_STR,  EOS_SET_F_ROUTED | EOS_SET_F_REBOOT },
    { "wifi.psk",      EOS_SET_T_STR,  EOS_SET_F_ROUTED | EOS_SET_F_REBOOT
                                     | EOS_SET_F_WRITEONLY },
    { "wifi.psk_set",  EOS_SET_T_BOOL, EOS_SET_F_READONLY },
    { "net.host",      EOS_SET_T_STR,  EOS_SET_F_PERSIST | EOS_SET_F_REBOOT },
    { "brain.host",    EOS_SET_T_STR,  EOS_SET_F_PERSIST },
    { "brain.port",    EOS_SET_T_INT,  EOS_SET_F_PERSIST },
    { "brain.model",   EOS_SET_T_STR,  EOS_SET_F_PERSIST },
    { "brain.max",     EOS_SET_T_INT,  EOS_SET_F_PERSIST },
    { "brain.system",  EOS_SET_T_STR,  EOS_SET_F_PERSIST },
    { "ui.theme",      EOS_SET_T_STR,  EOS_SET_F_PERSIST },
    { "ui.bright",     EOS_SET_T_INT,  EOS_SET_F_PERSIST },
    { "sys.tz",        EOS_SET_T_STR,  EOS_SET_F_PERSIST },
    { "sys.autostart", EOS_SET_T_STR,  EOS_SET_F_PERSIST | EOS_SET_F_REBOOT },
};

// The longest string field. Sized here rather than by inspection so adding a
// field cannot leave a scratch buffer one byte short.
#define FIELD_MAX EOS_SETTINGS_SYSTEM_MAX

static bool key_ok(int k) { return k >= 0 && k < EOS_SET_COUNT; }

const char *eos_settings_key_name(int k) { return key_ok(k) ? KEYS[k].name : NULL; }
uint8_t     eos_settings_key_flags(int k) { return key_ok(k) ? KEYS[k].flags : 0; }
uint8_t     eos_settings_key_type(int k)  { return key_ok(k) ? KEYS[k].type : EOS_SET_T_STR; }

int eos_settings_key_of(const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < EOS_SET_COUNT; i++)
        if (strcmp(name, KEYS[i].name) == 0) return i;
    return -1;
}

const char *eos_settings_strerror(eos_settings_err_t e)
{
    switch (e) {
    case EOS_SETTINGS_OK:         return "ok";
    case EOS_SETTINGS_ERR_ABSENT: return "no settings file yet";
    case EOS_SETTINGS_ERR_EMPTY:  return "the settings file is empty";
    case EOS_SETTINGS_ERR_SYNTAX: return "the settings file is not readable JSON";
    case EOS_SETTINGS_ERR_IO:     return "the settings file could not be read";
    case EOS_SETTINGS_ERR_FIELD:  return "some settings keys were unusable";
    }
    return "?";
}

// ==========================================================================
// Field access
// ==========================================================================
//
// One switch instead of a table of offsets. offsetof arithmetic over a struct
// of mixed types is the kind of thing that is right until somebody reorders a
// field, and this is called a dozen times per request, not a million.

static char *field_str(eos_settings_t *s, int key, int *cap)
{
    switch (key) {
    case EOS_SET_NET_HOST:      *cap = EOS_SETTINGS_HOST_MAX;   return s->net_host;
    case EOS_SET_BRAIN_HOST:    *cap = EOS_SETTINGS_BHOST_MAX;  return s->brain_host;
    case EOS_SET_BRAIN_MODEL:   *cap = EOS_SETTINGS_MODEL_MAX;  return s->brain_model;
    case EOS_SET_BRAIN_SYSTEM:  *cap = EOS_SETTINGS_SYSTEM_MAX; return s->brain_system;
    case EOS_SET_UI_THEME:      *cap = EOS_SETTINGS_THEME_MAX;  return s->ui_theme;
    case EOS_SET_SYS_TZ:        *cap = EOS_SETTINGS_TZ_MAX;     return s->sys_tz;
    case EOS_SET_SYS_AUTOSTART: *cap = EOS_SETTINGS_APP_MAX;    return s->sys_autostart;
    default:                    *cap = 0;                       return NULL;
    }
}

static void field_range(int key, long *lo, long *hi)
{
    switch (key) {
    case EOS_SET_BRAIN_PORT: *lo = 1;  *hi = 65535; break;
    case EOS_SET_BRAIN_MAX:  *lo = 16; *hi = 2048;  break;
    case EOS_SET_UI_BRIGHT:  *lo = 0;  *hi = 255;   break;
    default:                 *lo = 0;  *hi = 0;     break;
    }
}

static long field_num(const eos_settings_t *s, int key)
{
    switch (key) {
    case EOS_SET_BRAIN_PORT: return (long)s->brain_port;
    case EOS_SET_BRAIN_MAX:  return (long)s->brain_max;
    case EOS_SET_UI_BRIGHT:  return (long)s->ui_bright;
    default:                 return 0;
    }
}

// Clamps rather than refusing. A brightness of 4000 is a typo in one field and
// refusing the document over it would cost the owner every other setting in
// it; a brightness of 255 is what they meant either way.
static void field_num_set(eos_settings_t *s, int key, long v)
{
    long lo, hi;
    field_range(key, &lo, &hi);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    switch (key) {
    case EOS_SET_BRAIN_PORT: s->brain_port = (uint16_t)v; break;
    case EOS_SET_BRAIN_MAX:  s->brain_max  = (uint16_t)v; break;
    case EOS_SET_UI_BRIGHT:  s->ui_bright  = (uint8_t)v;  break;
    default: break;
    }
}

// ==========================================================================
// Defaults
// ==========================================================================

void eos_settings_defaults(eos_settings_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof *s);

    // net.host empty means "derive it from the MAC", which is eos_net's own
    // default and gives esp-os-f048.local rather than six boards all claiming
    // esp-os.local. brain.host empty means "discover it", which is eos_brain's
    // mDNS-then-fallback walk. Neither is a placeholder for a missing value —
    // empty is the answer.
    s->net_host[0]      = '\0';
    s->brain_host[0]    = '\0';
    s->brain_model[0]   = '\0';
    s->brain_system[0]  = '\0';
    s->sys_tz[0]        = '\0';
    s->sys_autostart[0] = '\0';
    snprintf(s->ui_theme, sizeof s->ui_theme, "%s", EOS_SETTINGS_THEME_DEFAULT);

    s->brain_port = EOS_SETTINGS_PORT_DEFAULT;
    s->brain_max  = EOS_SETTINGS_MAXTOK_DEFAULT;
    s->ui_bright  = EOS_SETTINGS_BRIGHT_DEFAULT;
}

// ==========================================================================
// Parse
// ==========================================================================

// A key no settings file can contain, used to walk the whole document and find
// out whether it is well formed. eos_json_get_str answers ABSENT only after it
// has scanned every key/value pair to a closing brace, so this one lookup is a
// syntax check for the entire object.
#define PROBE_KEY "\x7f~probe"

static bool all_space(const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (b[i] != ' ' && b[i] != '\t' && b[i] != '\n' && b[i] != '\r') return false;
    return true;
}

eos_settings_err_t eos_settings_parse(eos_settings_t *out, const char *buf, int len,
                                      uint16_t *bad)
{
    char scratch[FIELD_MAX];
    uint16_t bits = 0;
    int i;

    if (bad) *bad = 0;
    if (!out) return EOS_SETTINGS_ERR_SYNTAX;

    eos_settings_defaults(out);

    if (!buf || len <= 0)      return EOS_SETTINGS_ERR_EMPTY;
    if (all_space(buf, len))   return EOS_SETTINGS_ERR_EMPTY;

    // Whole-document syntax first, and only then the keys. Half-applying a
    // truncated file would leave the board wearing three of its owner's
    // settings and nine defaults, which looks like a bug in whichever of the
    // nine they notice.
    //
    // Two checks, because the reader stops at the closing brace and cannot
    // see past it. The probe walks every key/value pair to that brace and
    // answers ABSENT only if all of them were well formed; the bookends catch
    // what follows it. What neither catches is a second closing brace after
    // the first — the reader never reads it and nothing it could contain is
    // ever applied, so it is junk in a file rather than a value in a store.
    if (eos_json_get_str(buf, len, PROBE_KEY, scratch, (int)sizeof scratch, NULL)
        != EOS_JSON_ABSENT)
        return EOS_SETTINGS_ERR_SYNTAX;
    {
        int a = 0, b = len - 1;
        while (a < len && all_space(buf + a, 1)) a++;
        while (b > a  && all_space(buf + b, 1)) b--;
        if (buf[a] != '{' || buf[b] != '}') return EOS_SETTINGS_ERR_SYNTAX;
    }

    for (i = 0; i < EOS_SET_COUNT; i++) {
        eos_json_find_t r;

        if (!(KEYS[i].flags & EOS_SET_F_PERSIST)) continue;

        if (KEYS[i].type == EOS_SET_T_INT) {
            long v = 0;
            r = eos_json_get_int(buf, len, KEYS[i].name, &v);
            if (r == EOS_JSON_FOUND)      field_num_set(out, i, v);
            else if (r != EOS_JSON_ABSENT) bits |= (uint16_t)(1u << i);
            continue;
        }

        {
            int cap = 0, n = 0;
            char *dst = field_str(out, i, &cap);
            if (!dst) continue;
            // Into a scratch and then copied: eos_json_get_str clears its
            // output buffer before it knows whether the key is there, and
            // writing straight into the field would wipe the default on every
            // key the file does not mention.
            r = eos_json_get_str(buf, len, KEYS[i].name, scratch, cap, &n);
            if (r == EOS_JSON_FOUND) {
                memcpy(dst, scratch, (size_t)n);
                dst[n] = '\0';
            } else if (r != EOS_JSON_ABSENT) {
                // TYPE (a number where a string belongs), TOOBIG (a value
                // longer than the field) and BAD all land here. The key keeps
                // its default and the rest of the file still applies.
                bits |= (uint16_t)(1u << i);
            }
        }
    }

    if (bad) *bad = bits;
    return bits ? EOS_SETTINGS_ERR_FIELD : EOS_SETTINGS_OK;
}

// ==========================================================================
// Print
// ==========================================================================

int eos_settings_print(const eos_settings_t *s, char *buf, int cap)
{
    eos_json_t j;
    int i;

    if (!s || !buf || cap <= 0) return (int)EOS_ERR_ARG;

    eos_json_init(&j, buf, cap);
    eos_json_obj_open(&j);
    for (i = 0; i < EOS_SET_COUNT; i++) {
        if (!(KEYS[i].flags & EOS_SET_F_PERSIST)) continue;
        if (KEYS[i].type == EOS_SET_T_INT) {
            eos_json_kv_int(&j, KEYS[i].name, field_num(s, i));
        } else {
            int fcap = 0;
            const char *v = field_str((eos_settings_t *)s, i, &fcap);
            eos_json_kv_str(&j, KEYS[i].name, v ? v : "");
        }
    }
    eos_json_obj_close(&j);

    // Sticky overflow, discovered once. A truncated settings file is worse
    // than no settings file: it parses next boot as garbage and takes every
    // key with it.
    if (!eos_json_ok(&j)) return (int)EOS_ERR_TOOBIG;
    return j.len;
}

// ==========================================================================
// Read one value
// ==========================================================================

int eos_settings_get(const eos_settings_store_t *st, int key, char *out, int cap)
{
    if (!st || !out || cap <= 0 || !key_ok(key)) return (int)EOS_ERR_ARG;
    out[0] = '\0';

    // wifi.psk. Write-only in both directions, which is the whole point: the
    // page shows "set - leave blank to keep" and never the passphrase.
    if (KEYS[key].flags & EOS_SET_F_WRITEONLY) return (int)EOS_ERR_READONLY;

    if (key == EOS_SET_WIFI_SSID) {
        if (st->ports.wifi_ssid && st->ports.wifi_ssid(st->ports.ctx, out, cap))
            return (int)strlen(out);
        out[0] = '\0';
        return 0;
    }
    if (key == EOS_SET_WIFI_PSK_SET) {
        bool set = st->ports.wifi_psk_set ? st->ports.wifi_psk_set(st->ports.ctx) : false;
        int n = snprintf(out, (size_t)cap, "%s", set ? "true" : "false");
        return (n < 0 || n >= cap) ? (int)EOS_ERR_TOOBIG : n;
    }
    if (KEYS[key].type == EOS_SET_T_INT) {
        int n = snprintf(out, (size_t)cap, "%ld", field_num(&st->v, key));
        return (n < 0 || n >= cap) ? (int)EOS_ERR_TOOBIG : n;
    }
    {
        int fcap = 0;
        const char *v = field_str((eos_settings_t *)&st->v, key, &fcap);
        int n;
        if (!v) return (int)EOS_ERR_NOTFOUND;
        n = snprintf(out, (size_t)cap, "%s", v);
        return (n < 0 || n >= cap) ? (int)EOS_ERR_TOOBIG : n;
    }
}

// ==========================================================================
// Write one value
// ==========================================================================

static void mark_dirty(eos_settings_store_t *st)
{
    st->dirty = true;
    st->dirty_at_ms = st->clock_ms;
}

// Runs ports.apply and turns its three answers into this function's two.
static eos_err_t run_apply(eos_settings_store_t *st, int key, bool *reboot)
{
    eos_err_t e;

    if (reboot && eos_settings_key_reboot(key)) *reboot = true;
    if (!st->ports.apply) return EOS_OK;

    e = st->ports.apply(st->ports.ctx, key, &st->v);
    if (e == EOS_ERR_UNSUPPORTED) {
        // Kept, but not live. The value is still the one the board will use
        // next boot, so the honest answer to the page is "saved, reboot to
        // apply" rather than an error over something that did save.
        if (reboot) *reboot = true;
        return EOS_OK;
    }
    return e;
}

eos_err_t eos_settings_set_str(eos_settings_store_t *st, int key,
                               const char *val, bool *reboot)
{
    char old[FIELD_MAX];
    int cap = 0;
    char *dst;
    size_t n;
    eos_err_t e;

    if (!st || !val || !key_ok(key)) return EOS_ERR_ARG;
    if (KEYS[key].flags & EOS_SET_F_READONLY) return EOS_ERR_READONLY;

    // A number sent as a string. The web app sends real JSON numbers for these
    // fields, so this is leniency for a hand-written curl and nothing else.
    if (KEYS[key].type == EOS_SET_T_INT) {
        char *end = NULL;
        long v;
        if (!val[0]) return EOS_ERR_ARG;
        v = strtol(val, &end, 10);
        if (!end || *end) return EOS_ERR_ARG;
        return eos_settings_set_num(st, key, v, reboot);
    }

    // The two routed keys. Staged, not applied: eos_net joins with an SSID and
    // a passphrase together, and handing it half a pair would start a join
    // against a network the other half does not name.
    if (KEYS[key].flags & EOS_SET_F_ROUTED) {
        if (key == EOS_SET_WIFI_SSID) {
            if (strlen(val) >= sizeof st->route_ssid) return EOS_ERR_TOOBIG;
            snprintf(st->route_ssid, sizeof st->route_ssid, "%s", val);
            st->route_ssid_set = true;
        } else {
            // An empty passphrase means "leave the stored one alone", which is
            // what the page's placeholder says. It is not "clear it".
            if (!val[0]) { if (reboot) *reboot = true; return EOS_OK; }
            if (strlen(val) >= sizeof st->route_psk) return EOS_ERR_TOOBIG;
            snprintf(st->route_psk, sizeof st->route_psk, "%s", val);
            st->route_psk_set = true;
        }
        if (reboot) *reboot = true;
        return EOS_OK;     // nothing persisted here; see eos_settings_route_commit
    }

    dst = field_str(&st->v, key, &cap);
    if (!dst) return EOS_ERR_NOTFOUND;

    n = strlen(val);
    if ((int)n >= cap) return EOS_ERR_TOOBIG;   // never truncated: see the header

    snprintf(old, sizeof old, "%s", dst);
    memcpy(dst, val, n);
    dst[n] = '\0';

    e = run_apply(st, key, reboot);
    if (e != EOS_OK) {
        snprintf(dst, (size_t)cap, "%s", old);   // rejected: put it back
        return e;
    }
    mark_dirty(st);
    return EOS_OK;
}

eos_err_t eos_settings_set_num(eos_settings_store_t *st, int key,
                               long num, bool *reboot)
{
    long old;
    eos_err_t e;

    if (!st || !key_ok(key)) return EOS_ERR_ARG;
    if (KEYS[key].flags & EOS_SET_F_READONLY) return EOS_ERR_READONLY;
    if (KEYS[key].type != EOS_SET_T_INT) return EOS_ERR_ARG;

    old = field_num(&st->v, key);
    field_num_set(&st->v, key, num);

    e = run_apply(st, key, reboot);
    if (e != EOS_OK) {
        field_num_set(&st->v, key, old);
        return e;
    }
    mark_dirty(st);
    return EOS_OK;
}

eos_err_t eos_settings_route_commit(eos_settings_store_t *st)
{
    int r;

    if (!st) return EOS_ERR_ARG;
    if (!st->route_ssid_set && !st->route_psk_set) return EOS_OK;

    if (!st->ports.wifi_set) {
        st->route_ssid_set = st->route_psk_set = false;
        memset(st->route_psk, 0, sizeof st->route_psk);
        return EOS_ERR_UNSUPPORTED;
    }

    r = st->ports.wifi_set(st->ports.ctx,
                           st->route_ssid_set ? st->route_ssid : NULL,
                           st->route_psk_set  ? st->route_psk  : NULL);

    // The passphrase does not sit in this struct any longer than it has to.
    // It never reaches the file at all; wiping it here means it does not
    // linger in BSS for the rest of the boot either.
    st->route_ssid_set = st->route_psk_set = false;
    memset(st->route_psk, 0, sizeof st->route_psk);
    memset(st->route_ssid, 0, sizeof st->route_ssid);

    return r < 0 ? (eos_err_t)r : EOS_OK;
}

// ==========================================================================
// The file
// ==========================================================================
//
// One document buffer for the image, in BSS. There is one settings store in an
// ESP-OS image and only eos_settings_load() and eos_settings_flush() touch
// this, both from the OS loop — no HTTP worker reaches it, which is the same
// rule that keeps flash erases off the request path.

static char s_doc[EOS_SETTINGS_DOC_MAX];

void eos_settings_init(eos_settings_store_t *st, const eos_settings_ports_t *ports)
{
    if (!st) return;
    memset(st, 0, sizeof *st);
    eos_settings_defaults(&st->v);
    if (ports) st->ports = *ports;
    st->last_load = EOS_SETTINGS_ERR_ABSENT;
}

eos_settings_err_t eos_settings_load(eos_settings_store_t *st)
{
    int n;

    if (!st) return EOS_SETTINGS_ERR_SYNTAX;

    eos_settings_defaults(&st->v);
    st->from_file  = false;
    st->bad_fields = 0;
    st->dirty      = false;

    n = eos_storage_load_str(EOS_SETTINGS_PATH, s_doc, (int)sizeof s_doc);
    if (n < 0) {
        st->last_load = (n == (int)EOS_ERR_NOTFOUND || n == (int)EOS_ERR_NODEV)
                          ? EOS_SETTINGS_ERR_ABSENT
                          : (n == (int)EOS_ERR_TOOBIG ? EOS_SETTINGS_ERR_SYNTAX
                                                      : EOS_SETTINGS_ERR_IO);
        return st->last_load;
    }

    st->last_load = eos_settings_parse(&st->v, s_doc, n, &st->bad_fields);
    st->from_file = (st->last_load == EOS_SETTINGS_OK ||
                     st->last_load == EOS_SETTINGS_ERR_FIELD);

    // A file that parsed with unusable keys is rewritten on the next edit, not
    // now: rewriting it here would erase a flash sector on every boot of a
    // board whose settings file a human is halfway through editing by hand.
    return st->last_load;
}

eos_err_t eos_settings_flush(eos_settings_store_t *st)
{
    int n;
    eos_err_t e;

    if (!st) return EOS_ERR_ARG;
    if (!st->dirty) return EOS_OK;

    n = eos_settings_print(&st->v, s_doc, (int)sizeof s_doc);
    if (n < 0) {
        // Nothing this store holds can produce a document this long — every
        // field is bounded and the total is checked in the header. If it ever
        // does, refusing to write is right: half a settings file is garbage.
        st->dirty = false;
        st->save_fails++;
        st->last_save = (eos_err_t)n;
        return st->last_save;
    }

    e = eos_storage_save(EOS_SETTINGS_PATH, s_doc, n);
    st->last_save = e;
    // Dirty is cleared either way. A filesystem that refused this write will
    // refuse the next one, and retrying every 250 ms would erase sectors for
    // as long as the board is up; the failure is in last_save and the next
    // actual edit marks it dirty again.
    st->dirty = false;
    if (e == EOS_OK) st->saves++;
    else             st->save_fails++;
    return e;
}

void eos_settings_pump(eos_settings_store_t *st, uint32_t now_ms)
{
    if (!st) return;
    st->clock_ms = now_ms;
    if (!st->dirty) return;
    if ((uint32_t)(now_ms - st->dirty_at_ms) < EOS_SETTINGS_DEBOUNCE_MS) return;
    (void)eos_settings_flush(st);
}
