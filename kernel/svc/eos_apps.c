// eos_apps — files, console, buddy and apps, over HTTP.
//
// The Files tab said "no such endpoint on this board" because the web app calls
// twenty-eight endpoints and the firmware served eight. This is fourteen of the
// twenty that were missing. Everything here is portable C99 driven by
// eos_httpd's dispatch and eos_storage's namespace, and the whole of it runs on
// a laptop against a directory in /tmp — which is where the path-traversal and
// upload-state-machine checks run, because a filesystem endpoint that has only
// ever been exercised by hand is a filesystem endpoint nobody has exercised.
//
// The one non-obvious constraint: eos_httpd serialises dispatch behind one
// mutex, so there is exactly one request in flight image-wide and every buffer
// below can be a file static. That is not a saving, it is a requirement. An
// HTTP worker has a 5,376-byte stack with 513 bytes of request body already on
// it, and two 96-byte paths plus a directory entry plus a line of formatting per
// frame is how four workers turn a busy Files tab into a stack overflow. The
// same mutex is why one upload handle is the right number: a second concurrent
// upload cannot exist to want one.
//
// The second constraint is that a flash write stops this chip. Erasing turns
// the instruction cache off on the single core that is also driving the panel,
// the radio and this server, so every write here is exactly what the client
// sent and never a loop, and the sync that costs tens of milliseconds happens
// once per upload — on the final chunk — and not once per chunk.
//
// Nothing here allocates. Handles come from eos_storage's fixed pools, the log
// ring and the buddy's voxel pool are fixed arrays, and the response document
// is eos_httpd's own buffer.

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "eos_apps.h"
#include "eos_gallery.h"

// ==========================================================================
// State
// ==========================================================================

static eos_apps_ports_t s_ports;
static void            *s_ctx;

static const eos_apps_app_t *s_apps;
static int                   s_apps_n;

// The clock eos_apps_tick() feeds. Handlers read it and never advance it, which
// is what keeps eos_httpd_dispatch() as clockless as its header claims.
static uint32_t s_now_ms;

// Scratch, shared under eos_httpd's dispatch mutex. See the file header for why
// this is not on the worker's stack.
static struct {
    char path[EOS_PATH_MAX];    // the request's path, decoded and checked
    char path2[EOS_PATH_MAX];   // rename's second one
    char full[EOS_PATH_MAX];    // a listing entry joined onto its directory
} s_scr;

// ==========================================================================
// Paths
// ==========================================================================

// Defence in depth, and the word "depth" is load bearing: eos_storage's
// path_split() checks every rule below again and is the authority. This copy
// exists because the string arrives off the network — off whoever is on the
// WiFi — and the cheapest place to refuse it is before it has been handed to
// anything at all. The two disagreeing would be a bug, so the host suite drives
// the same hostile corpus through both and asserts they agree.
//
// eos_httpd_query_get() has already percent-decoded exactly once and refused a
// decoded NUL, so a `%2e%2e` in the URI arrives here as `..` and is caught by
// the component rule below. Nothing decodes a second time: that is how
// `%252e%252e` walks through a two-stage decoder.
static eos_err_t path_check(const char *p)
{
    int i, comp = 0;

    if (!p || p[0] != '/') return EOS_ERR_ARG;

    for (i = 0; p[i]; i++) {
        unsigned char c = (unsigned char)p[i];

        if (i >= EOS_PATH_MAX - 1) return EOS_ERR_TOOBIG;
        if (c < 0x20 || c == 0x7F) return EOS_ERR_ARG;   // control bytes
        if (c == '\\')             return EOS_ERR_ARG;   // a FAT separator

        if (c == '/') {
            comp = 0;
        } else if (++comp >= EOS_NAME_MAX) {
            return EOS_ERR_TOOBIG;                       // a component too long
        }

        // ".." as a whole component. Per component and never strstr: "...bb..."
        // contains ".." and is a legal filename, and a filter that gets that
        // backwards blocks real names while still passing the real escape.
        if (c == '.' && p[i + 1] == '.' &&
            (i == 0 || p[i - 1] == '/') &&
            (p[i + 2] == '/' || p[i + 2] == '\0'))
            return EOS_ERR_ARG;
    }
    if (i == 0) return EOS_ERR_ARG;
    return EOS_OK;
}

// Reads one path-valued query parameter into `out`, which must be EOS_PATH_MAX.
// Returns EOS_OK, or the error the response should carry.
static eos_err_t q_path(const char *uri, const char *name, char *out)
{
    int n = eos_httpd_query_get(uri, name, out, EOS_PATH_MAX);

    if (n == (int)EOS_ERR_NOTFOUND) return EOS_ERR_ARG;   // missing is malformed
    if (n < 0) return (eos_err_t)n;                       // TOOBIG, or a bad escape
    return path_check(out);
}

// A non-negative decimal query parameter. Absent is `def`; anything that is not
// a run of digits, or that is past `max`, is EOS_ERR_ARG — a silently clamped
// offset would page a directory listing wrong and look like missing files.
static eos_err_t q_uint(const char *uri, const char *name, long def, long max, long *out)
{
    char v[20];
    int n = eos_httpd_query_get(uri, name, v, (int)sizeof v);
    long acc = 0;
    int i;

    *out = def;
    if (n == (int)EOS_ERR_NOTFOUND) return EOS_OK;
    if (n < 0) return EOS_ERR_ARG;
    if (n == 0) return EOS_OK;                            // bare `?count` means default

    for (i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9') return EOS_ERR_ARG;
        acc = acc * 10 + (v[i] - '0');
        if (acc > max) return EOS_ERR_ARG;
    }
    *out = acc;
    return EOS_OK;
}

// A 64-bit filesystem size, as the JSON writer's long. `long` is 32 bits on
// riscv32, so a card past 2 GB would wrap; it is clamped rather than wrapped
// because a negative free-space figure in the Files tab is worse than a
// truncated one. /int is 960 KB and never reaches this.
static long clamp_size(uint64_t v)
{
    return v > 0x7FFFFFFFu ? 0x7FFFFFFFL : (long)v;
}

#ifndef ESP_PLATFORM
// Host-only hook, and the reason it exists is the whole point of the rules
// above. path_check() is INVISIBLE from outside: eos_storage refuses the same
// paths again one layer down, so deleting every rule here changes no endpoint's
// answer and no black-box test can tell. That is what defence in depth means,
// and it is exactly why this layer has to be driven directly — otherwise the
// suite passes with the defence gone, which is the state the next person
// inherits without knowing it.
int eos_apps_host_path_check(const char *p);
int eos_apps_host_path_check(const char *p) { return (int)path_check(p); }
#endif

// ==========================================================================
// The file ports
// ==========================================================================
//
// eos_httpd asks for static files through three ports and does not care where
// the bytes come from. The boot glue binds the copy of the web app linked into
// the image; this wraps that so a REAL file on storage is preferred and the
// embedded copy answers only when there is not one. That is web/README.md's
// rule — prefer the file, never leave the board with nothing to serve — and it
// is also what makes deploying the app onto /int a copy rather than a rebuild.
//
// /api/fs/read borrows the same read and close through a handle opened with the
// fallback switched OFF, because falling back there would answer a GET for a
// file that does not exist with the contents of a different one.

typedef struct {
    bool        used;
    eos_file_t *f;        // a storage handle, when inner is NULL
    void       *inner;    // the previously-bound port's handle
} fslot_t;

static fslot_t s_fslot[EOS_MAX_FILES];

// Only the three file ports, and not a whole eos_httpd_ports_t: the table is
// 128 bytes of function pointers to radios this file will never call, and
// holding it would read as though it might.
static struct {
    void *(*file_open)(void *ctx, const char *path, long *size_out);
    int   (*file_read)(void *ctx, void *fh, void *buf, int n);
    void  (*file_close)(void *ctx, void *fh);
} s_below;

static fslot_t *slot_take(void)
{
    int i;
    for (i = 0; i < EOS_MAX_FILES; i++) {
        if (s_fslot[i].used) continue;
        s_fslot[i].used  = true;
        s_fslot[i].f     = NULL;
        s_fslot[i].inner = NULL;
        return &s_fslot[i];
    }
    return NULL;
}

static void *file_open_at(void *ctx, const char *path, long *size_out, bool fallback)
{
    eos_stat_t st;
    fslot_t   *s;

    if (path && path_check(path) == EOS_OK &&
        eos_storage_stat(path, &st) == EOS_OK && !st.is_dir) {
        eos_file_t *f = eos_storage_open(path, EOS_O_READ);
        if (f) {
            s = slot_take();
            if (!s) { eos_storage_close(f); return NULL; }
            s->f = f;
            if (size_out) *size_out = (long)st.size;
            return s;
        }
    }

    if (!fallback || !s_below.file_open) return NULL;
    {
        void *inner = s_below.file_open(ctx, path, size_out);
        if (!inner) return NULL;
        s = slot_take();
        if (!s) {
            if (s_below.file_close) s_below.file_close(ctx, inner);
            return NULL;
        }
        s->inner = inner;
        return s;
    }
}

static void *apps_file_open(void *ctx, const char *path, long *size_out)
{
    return file_open_at(ctx, path, size_out, true);
}

static int apps_file_read(void *ctx, void *fh, void *buf, int n)
{
    fslot_t *s = (fslot_t *)fh;

    if (!s || !s->used || !buf || n <= 0) return 0;
    if (s->inner) return s_below.file_read ? s_below.file_read(ctx, s->inner, buf, n) : 0;
    return eos_storage_read(s->f, buf, n);
}

static void apps_file_close(void *ctx, void *fh)
{
    fslot_t *s = (fslot_t *)fh;

    if (!s || !s->used) return;
    if (s->inner) { if (s_below.file_close) s_below.file_close(ctx, s->inner); }
    else if (s->f) { eos_storage_close(s->f); }
    s->used = false; s->f = NULL; s->inner = NULL;
}

void eos_apps_bind_files(eos_httpd_t *h)
{
    if (!h) return;
    // Idempotent, and it has to be. Called twice, the naive version captures
    // THIS file's own ports as the fallback, and the first path that is not on
    // storage recurses until the stack is gone — on the HTTP worker, from a
    // GET, on a board with a 5,376-byte stack. Boot glue that calls it once per
    // mode change is a reasonable thing to write.
    if (h->ports.file_open == apps_file_open) return;

    s_below.file_open  = h->ports.file_open;
    s_below.file_read  = h->ports.file_read;
    s_below.file_close = h->ports.file_close;

    h->ports.file_open  = apps_file_open;
    h->ports.file_read  = apps_file_read;
    h->ports.file_close = apps_file_close;
}

// ==========================================================================
// The console ring
// ==========================================================================
//
// Two pools, because they run out at different rates: a boot log is forty short
// lines and a stack trace is four long ones. The byte pool is circular and a
// line NEVER wraps across its end — a write that would not fit before the end
// restarts at zero and drops whatever it lands on — so every line is one
// contiguous run and the JSON writer takes it in one call. Reassembling a
// wrapped line would need a copy buffer this file does not have room for.

typedef struct {
    uint32_t seq;
    uint16_t off;
    uint16_t len;
    char     lvl;
} logline_t;

static char      s_lbuf[EOS_APPS_LOG_BYTES];
static logline_t s_lline[EOS_APPS_LOG_LINES];
static int       s_lhead, s_lcount, s_lwpos;
static uint32_t  s_lseq;

// The ring is written from whatever task logged and read from an HTTP worker,
// which is the one place in this file two tasks meet. Everything else is
// serialised by eos_httpd's dispatch mutex.
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static StaticSemaphore_t s_lmem;
static SemaphoreHandle_t s_lmutex;
static void log_lock(void)   { if (s_lmutex) xSemaphoreTake(s_lmutex, portMAX_DELAY); }
static void log_unlock(void) { if (s_lmutex) xSemaphoreGive(s_lmutex); }
#else
static void log_lock(void)   { }
static void log_unlock(void) { }
#endif

static void log_push(char lvl, const char *t, int n)
{
    int i;

    if (n <= 0) return;
    if (n > EOS_APPS_LOG_TEXT_MAX) n = EOS_APPS_LOG_TEXT_MAX;
    if (s_lwpos + n > EOS_APPS_LOG_BYTES) s_lwpos = 0;

    // Drop the oldest line while the table is full, or while its bytes are
    // where the new ones are about to land. Both conditions retire oldest
    // first, so one loop covers them.
    while (s_lcount > 0) {
        const logline_t *o = &s_lline[s_lhead];
        bool overlap = (int)o->off < s_lwpos + n && s_lwpos < (int)o->off + (int)o->len;
        if (s_lcount < EOS_APPS_LOG_LINES && !overlap) break;
        s_lhead = (s_lhead + 1) % EOS_APPS_LOG_LINES;
        s_lcount--;
    }

    i = (s_lhead + s_lcount) % EOS_APPS_LOG_LINES;
    memcpy(s_lbuf + s_lwpos, t, (size_t)n);
    s_lline[i].seq = s_lseq++;
    s_lline[i].off = (uint16_t)s_lwpos;
    s_lline[i].len = (uint16_t)n;
    s_lline[i].lvl = lvl;
    s_lwpos += n;
    s_lcount++;
}

void eos_apps_log(char level, const char *text)
{
    const char *p;

    if (!text) return;
    if (level != 'E' && level != 'W' && level != 'I' && level != 'D') level = 'I';

    log_lock();
    for (p = text; *p; ) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        while (n > 0 && p[n - 1] == '\r') n--;          // CRLF from anywhere
        if (n > 0) log_push(level, p, n);
        if (!nl) break;
        p = nl + 1;
    }
    log_unlock();
}

void eos_apps_logf(char level, const char *fmt, ...)
{
    char buf[EOS_APPS_LOG_TEXT_MAX + 1];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    eos_apps_log(level, buf);
}

uint32_t eos_apps_log_seq(void)
{
    uint32_t v;
    log_lock();
    v = s_lseq;
    log_unlock();
    return v;
}

// ==========================================================================
// The buddy
// ==========================================================================

static struct {
    eos_voxel_t     pool[EOS_APPS_VOX_VOXELS];
    eos_vox_pal_t   pal;
    eos_vox_model_t model;
    eos_buddy_cfg_t cfg;
    char            name[EOS_APPS_BUDDY_NAME_MAX];
    char            persona[EOS_APPS_BUDDY_PERSONA_MAX];
    char            slug[EOS_APPS_BUDDY_SLUG_MAX];  // "" for the legacy buddy.vox
    char            file[EOS_PATH_MAX];             // what actually loaded
    uint32_t        accent;        // 0xFFFFFFFF when buddy.json named none
    long            schema;
    uint8_t         behaviour;     // eos_apps_idle_t
    uint8_t         state;         // eos_buddy_state_t
    bool            loaded;        // a .vox has parsed
    bool            have_json;
    uint32_t        gen;
    const char     *err;
} s_bud;

// The whole file, staged. eos_vox_parse() wants a contiguous buffer and will
// not seek, which is the right shape for a format arriving off a card.
static uint8_t s_voxbuf[EOS_APPS_VOX_BYTES];
static char    s_budjson[EOS_APPS_BUDDY_JSON_BYTES];

static const char *const IDLE_NAMES[EOS_APPS_IDLE_COUNT] = {
    "still", "wander", "curious", "sleepy", "roam", "play"
};

// web/README.md wants the state lowercased. eos_buddy_state_name() spells it in
// capitals because that is what the panel draws, and the panel's spelling is
// not this file's to change — so the two live side by side rather than one of
// them being bent to the other's audience.
static const char *state_name(int s)
{
    static const char *const N[EOS_BUDDY_STATE_COUNT] = {
        "idle", "thinking", "talking", "listening", "sleeping", "happy", "confused"
    };
    return ((unsigned)s < EOS_BUDDY_STATE_COUNT) ? N[s] : "idle";
}

const char *eos_apps_idle_name(int b)
{
    if (b < 0 || b >= (int)EOS_APPS_IDLE_COUNT) b = EOS_APPS_IDLE_WANDER;
    return IDLE_NAMES[b];
}

// ------------------------------------------------------- reading buddy.json
//
// eos_json_get_*() reads the top level of an object and steps OVER nested ones,
// which is exactly right for a request body and useless for buddy.json: every
// field that matters — idle.behaviour, eyes.open_index — is one level down. So
// this finds a named object's extent and points the same hardened reader at it,
// rather than writing a second JSON parser, which would be the one with the
// bug. Key matching is raw bytes: buddy.json's keys are ASCII and an escaped
// key is a document the editor does not produce.

static int skip_ws(const char *b, int len, int i)
{
    while (i < len && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r')) i++;
    return i;
}

// The index just past the value that starts at `i`, or -1 if it is malformed.
static int skip_value(const char *b, int len, int i)
{
    i = skip_ws(b, len, i);
    if (i >= len) return -1;

    if (b[i] == '"') {
        for (i++; i < len; i++) {
            if (b[i] == '\\') { i++; continue; }
            if (b[i] == '"')  return i + 1;
        }
        return -1;
    }
    if (b[i] == '{' || b[i] == '[') {
        int depth = 0;
        bool instr = false;
        for (; i < len; i++) {
            char c = b[i];
            if (instr) {
                if (c == '\\') i++;
                else if (c == '"') instr = false;
                continue;
            }
            if (c == '"') instr = true;
            else if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') {
                depth--;
                if (depth == 0) return i + 1;
                if (depth < 0)  return -1;
            }
        }
        return -1;
    }
    while (i < len && b[i] != ',' && b[i] != '}' && b[i] != ']') i++;
    return i;
}

static bool obj_span(const char *b, int len, const char *key,
                     const char **out, int *out_len)
{
    size_t klen = strlen(key);
    int i = skip_ws(b, len, 0);

    if (i >= len || b[i] != '{') return false;
    i++;

    for (;;) {
        int ks, ke, vs, ve;

        i = skip_ws(b, len, i);
        if (i >= len || b[i] != '"') return false;
        ks = i + 1;
        ke = skip_value(b, len, i);
        if (ke < 0) return false;

        i = skip_ws(b, len, ke);
        if (i >= len || b[i] != ':') return false;
        vs = skip_ws(b, len, i + 1);
        ve = skip_value(b, len, vs);
        if (ve < 0 || vs >= len) return false;

        if ((size_t)(ke - 1 - ks) == klen && memcmp(b + ks, key, klen) == 0) {
            if (b[vs] != '{') return false;
            *out     = b + vs;
            *out_len = ve - vs;
            return true;
        }

        i = skip_ws(b, len, ve);
        if (i < len && b[i] == ',') { i++; continue; }
        return false;
    }
}

// Copies at most cap-1 bytes and backs off a truncation to a UTF-8 boundary, so
// a personality cut at EOS_APPS_BUDDY_PERSONA_MAX never ends in half a
// character. A lone continuation byte at the end would reach the megabrain's
// prompt as a replacement character and read as a bug in the model.
static void copy_utf8(char *dst, int cap, const char *src, int n)
{
    int m = n < cap - 1 ? n : cap - 1;

    while (m > 0 && ((unsigned char)src[m] & 0xC0) == 0x80) m--;
    memcpy(dst, src, (size_t)m);
    dst[m] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "#rrggbb" -> 0x00rrggbb, or 0xFFFFFFFF for anything else. Anything else is
// not an error: web/README.md's rule for a schema the firmware does not know is
// to fall back, not to refuse.
static uint32_t parse_accent(const char *s)
{
    uint32_t v = 0;
    int i;

    if (!s || s[0] != '#') return 0xFFFFFFFFu;
    for (i = 1; i <= 6; i++) {
        int h = hexval(s[i]);
        if (h < 0) return 0xFFFFFFFFu;
        v = (v << 4) | (uint32_t)h;
    }
    return s[7] == '\0' ? v : 0xFFFFFFFFu;
}

static void buddy_defaults(void)
{
    eos_buddy_default_cfg(&s_bud.cfg);
    s_bud.name[0]    = '\0';
    s_bud.persona[0] = '\0';
    s_bud.accent     = 0xFFFFFFFFu;
    s_bud.schema     = 1;
    s_bud.behaviour  = EOS_APPS_IDLE_WANDER;
    s_bud.have_json  = false;
}

static void load_buddy_json(const char *path)
{
    const char *sub;
    int sublen, got, n;
    long v;
    char sbuf[EOS_APPS_BUDDY_PERSONA_MAX > 64 ? 512 : 64];

    buddy_defaults();

    if (!path || !path[0]) return;
    got = eos_storage_load(path, s_budjson, (int)sizeof s_budjson);
    if (got < 0) return;                       // absent, or larger than we stage
    s_bud.have_json = true;

    if (eos_json_get_int(s_budjson, got, "schema_version", &v) == EOS_JSON_FOUND)
        s_bud.schema = v;

    if (eos_json_get_str(s_budjson, got, "name", sbuf, (int)sizeof sbuf, &n) == EOS_JSON_FOUND)
        copy_utf8(s_bud.name, (int)sizeof s_bud.name, sbuf, n);

    if (eos_json_get_str(s_budjson, got, "personality", sbuf, (int)sizeof sbuf, &n) == EOS_JSON_FOUND)
        copy_utf8(s_bud.persona, (int)sizeof s_bud.persona, sbuf, n);

    if (eos_json_get_str(s_budjson, got, "accent", sbuf, (int)sizeof sbuf, &n) == EOS_JSON_FOUND)
        s_bud.accent = parse_accent(sbuf);

    if (obj_span(s_budjson, got, "idle", &sub, &sublen)) {
        if (eos_json_get_str(sub, sublen, "behaviour", sbuf, (int)sizeof sbuf, &n) == EOS_JSON_FOUND) {
            int k;
            s_bud.behaviour = EOS_APPS_IDLE_WANDER;     // web/README.md: unknown -> wander
            for (k = 0; k < (int)EOS_APPS_IDLE_COUNT; k++)
                if (strcmp(sbuf, IDLE_NAMES[k]) == 0) s_bud.behaviour = (uint8_t)k;
        }
        if (eos_json_get_int(sub, sublen, "sleep_ms", &v) == EOS_JSON_FOUND && v >= 0)
            s_bud.cfg.idle_sleep_ms = (uint32_t)v;
        if (eos_json_get_int(sub, sublen, "home_yaw", &v) == EOS_JSON_FOUND && v >= 0)
            s_bud.cfg.home_yaw = (uint8_t)(v % EOS_BUDDY_YAW_STEPS);
    }
    if (obj_span(s_budjson, got, "eyes", &sub, &sublen)) {
        if (eos_json_get_int(sub, sublen, "open_index", &v) == EOS_JSON_FOUND && v >= 0 && v < 256)
            s_bud.cfg.eye_ci = (uint8_t)v;
        if (eos_json_get_int(sub, sublen, "shut_index", &v) == EOS_JSON_FOUND && v >= 0 && v < 256)
            s_bud.cfg.eye_shut_ci = (uint8_t)v;
    }

    // The one preset that maps onto a number this header already has. The rest
    // are motion, which this file has no business knowing about: the value is
    // stored and reported, and kernel/avatar/eos_stroll.c is what consumes it,
    // by name, wherever the avatar is actually being ticked.
    if (s_bud.behaviour == EOS_APPS_IDLE_SLEEPY) s_bud.cfg.idle_sleep_ms /= 2u;
}

eos_err_t eos_apps_buddy_reload_from(const char *slug, const char *vox_path,
                                     const char *json_path)
{
    eos_stat_t st;
    eos_vox_err_t ve;
    eos_vox_model_t m;
    int got;

    s_bud.err = NULL;

    // Which buddy this IS - the slug and the path - is published at the bottom
    // and not here, and the four refusals below are the reason. Every one of
    // them returns before the voxel pool has been touched, so the model that
    // was already loaded is still the model on the panel; overwriting the slug
    // and the path on the way past would leave the board DRAWING one buddy and
    // REPORTING another, from a path with nothing at it.
    //
    // That is not only untidy. eos_gallery_remove() refuses to delete the live
    // buddy by asking two questions - what the `active` file says, and what
    // eos_apps_buddy_slug() says - precisely so that one answer covers the
    // other when they come apart. A failed reload is exactly when they come
    // apart, and clearing the slug here is what would quietly retire the second
    // question at the moment it is needed. The web app has the same problem
    // more visibly: it reads model.path to fetch the live buddy, and a path
    // that 404s makes a perfectly healthy penguin look broken in the tab.
    //
    // The parse failure further down is the one case that DOES publish, and it
    // should: there the pool really has been overwritten, the previous model is
    // genuinely gone, and the attempted name beside s_bud.err is the honest
    // report.
    load_buddy_json(json_path);

    if (!vox_path || !vox_path[0]) {
        s_bud.err = "there is no model to load";
        return EOS_ERR_ARG;
    }
    if (eos_storage_stat(vox_path, &st) != EOS_OK || st.is_dir) {
        s_bud.err = "there is no .vox at that path on this board";
        return EOS_ERR_NOTFOUND;
    }
    if (st.size > (uint32_t)sizeof s_voxbuf) {
        s_bud.err = "the model is larger than this board stages a .vox in";
        return EOS_ERR_TOOBIG;
    }

    got = eos_storage_load(vox_path, s_voxbuf, (int)sizeof s_voxbuf);
    if (got < 0) {
        s_bud.err = "the model could not be read";
        return (eos_err_t)got;
    }

    // Handed to the parser as it came off the filesystem. eos_vox.c was fuzzed
    // over eight thousand mutated files and every offset in it is checked
    // against what actually remains in the buffer, so pre-validating here would
    // add a second opinion that can only be wrong.
    ve = eos_vox_parse(s_voxbuf, (uint32_t)got, s_bud.pool, EOS_APPS_VOX_VOXELS,
                       &s_bud.pal, &m);
    if (ve != EOS_VOX_OK) {
        // The parse filled the one voxel pool this board has, so the previous
        // model did not survive it. Saying so is better than reporting a model
        // that is now half of two. A second pool to make a bad upload
        // survivable is 5,120 bytes for a case the editor already prevents.
        //
        // Which is exactly why the generation has to move on the failure path
        // too. The renderer holds a pointer to the eos_vox_model_t below and
        // re-reads it only when the generation changes; leaving the generation
        // still would leave it drawing the OLD count and the OLD dimensions
        // over voxels the refused file has already written, which is a face
        // made of two models. Moving it makes the renderer re-adopt, find
        // eos_apps_buddy_model() NULL, and fall back to the compiled-in buddy
        // - a rejected upload costs the owner their model, not the panel.
        //
        // The gallery is what makes that survivable rather than merely honest:
        // eos_gallery_select() calls this BEFORE it moves the pointer on the
        // filesystem, so a refusal here is followed by a plain reload of the
        // buddy that was already active, and the owner keeps their penguin.
        s_bud.loaded      = false;
        s_bud.model.count = 0;
        s_bud.err         = eos_vox_strerror(ve);
        s_bud.gen++;
        snprintf(s_bud.slug, sizeof s_bud.slug, "%s", slug ? slug : "");
        snprintf(s_bud.file, sizeof s_bud.file, "%s", vox_path);
        return EOS_ERR_ARG;
    }

    s_bud.model     = m;
    s_bud.model.pal = &s_bud.pal;
    s_bud.loaded    = true;
    s_bud.gen++;
    snprintf(s_bud.slug, sizeof s_bud.slug, "%s", slug ? slug : "");
    snprintf(s_bud.file, sizeof s_bud.file, "%s", vox_path);
    return EOS_OK;
}

// Loads whatever this board should be wearing, in one fixed order:
//
//   1. the gallery entry EOS_GALLERY_ACTIVE names
//   2. failing that, the first entry the gallery holds
//   3. failing that, the legacy /int/buddy/buddy.vox
//
// Three steps but one authority, and the order is total, so there is exactly
// one answer at any moment. Step 3 is not a second source of truth, it is the
// end of the search: it is what a board carrying a pre-gallery buddy.vox loads
// on the boot before eos_seed_buddy() has migrated that file into the gallery,
// and what an image built with no seeder at all loads forever. Once the
// migration has run there is no buddy.vox left for it to find.
eos_err_t eos_apps_buddy_reload(void)
{
    static char vox[EOS_PATH_MAX], json[EOS_PATH_MAX];
    char slug[EOS_APPS_BUDDY_SLUG_MAX];

    // Not on the stack: this runs on an HTTP worker with 5,376 bytes of it and
    // 513 already spent on the request body, and it is the same rule the rest
    // of this file follows. There is one dispatch in flight image-wide.
    if (eos_gallery_resolve(vox, json, EOS_PATH_MAX, slug, (int)sizeof slug) == EOS_OK)
        return eos_apps_buddy_reload_from(slug, vox, json);

    if (eos_path_join(vox, EOS_PATH_MAX, EOS_APPS_BUDDY_DIR, "buddy.vox") < 0 ||
        eos_path_join(json, EOS_PATH_MAX, EOS_APPS_BUDDY_DIR, "buddy.json") < 0) {
        s_bud.err = "the buddy directory does not fit a path";
        return EOS_ERR_TOOBIG;
    }
    return eos_apps_buddy_reload_from("", vox, json);
}

const char *eos_apps_buddy_error(void)        { return s_bud.err; }
eos_vox_model_t *eos_apps_buddy_model(void)   { return s_bud.loaded ? &s_bud.model : NULL; }
const eos_vox_pal_t *eos_apps_buddy_palette(void) { return s_bud.loaded ? &s_bud.pal : NULL; }
const eos_buddy_cfg_t *eos_apps_buddy_cfg(void)   { return &s_bud.cfg; }
const char *eos_apps_buddy_slug(void)         { return s_bud.slug; }
const char *eos_apps_buddy_file(void)         { return s_bud.file; }
const char *eos_apps_buddy_name(void)         { return s_bud.name; }
const char *eos_apps_buddy_personality(void)  { return s_bud.persona; }
int         eos_apps_buddy_behaviour(void)    { return (int)s_bud.behaviour; }
uint32_t    eos_apps_buddy_accent(void)       { return s_bud.accent; }
int         eos_apps_buddy_state(void)        { return (int)s_bud.state; }
uint32_t    eos_apps_buddy_generation(void)   { return s_bud.gen; }

void eos_apps_buddy_set_state(int state)
{
    if (state >= 0 && state < (int)EOS_BUDDY_STATE_COUNT) s_bud.state = (uint8_t)state;
}

// ==========================================================================
// The upload state machine
// ==========================================================================
//
// One handle, board-wide, keyed by path. web/README.md's table, verbatim:
//
//   offset 0,  final 0   create/truncate, keep the handle open
//   offset 0,  final 1   create/truncate, write, sync, close
//   offset==pos, final 0 append, keep open
//   offset==pos, final 1 append, sync, close
//   anything else        409 state, handle untouched
//
// A write to a DIFFERENT path while a handle is open is 409 busy, at any
// offset — including zero, which is the case that would otherwise silently
// switch files. The escape from a stuck handle is /api/fs/upload/abort, which
// is exactly what the client already does after three failed retries, plus the
// idle timeout below for the client that never comes back at all.

// An upload writes to `tmp` and is renamed onto `path` only when the last
// chunk has been synced. Writing straight at the target truncates it on the
// FIRST chunk, so an upload that then failed - a dropped connection, a client
// that gave up, the idle timeout - destroyed the file it was replacing. That
// cost the owner their buddy: a save that never completed left /int/buddy with
// no buddy.vox at all, and the panel fell back to the compiled-in penguin
// while the web app reported a board that "describes a buddy" it could not
// find. Replacing a file must never be able to lose it.
static struct {
    eos_file_t *f;
    char        path[EOS_PATH_MAX];   // where it lands
    char        tmp[EOS_PATH_MAX];    // where it is written
    uint32_t    off;
    uint32_t    last_ms;
    bool        open;
} s_up;

// Closes the handle. `keep_tmp` is false everywhere an upload ends WITHOUT
// completing, so the partial is removed and whatever was already at the target
// is untouched; it is true only on the success path, where the rename that
// follows consumes it.
static void upload_close_ex(bool keep_tmp)
{
    if (s_up.open && s_up.f) eos_storage_close(s_up.f);
    if (!keep_tmp && s_up.tmp[0]) (void)eos_storage_remove(s_up.tmp);
    s_up.f = NULL;
    s_up.open = false;
    s_up.off = 0;
    s_up.path[0] = '\0';
    s_up.tmp[0]  = '\0';
}

static void upload_close(void) { upload_close_ex(false); }

bool eos_apps_upload_targets(const char *path)
{
    return path && s_up.open && strcmp(s_up.path, path) == 0;
}

void eos_apps_tick(uint32_t now_ms)
{
    s_now_ms = now_ms;

    if (s_up.open && (uint32_t)(now_ms - s_up.last_ms) >= EOS_APPS_UPLOAD_IDLE_MS) {
        eos_apps_logf('W', "fs: upload of %s idle for %us, handle closed, "
                           "%u bytes left on the filesystem",
                      s_up.path, (unsigned)(EOS_APPS_UPLOAD_IDLE_MS / 1000u),
                      (unsigned)s_up.off);
        upload_close();
    }
}

// ==========================================================================
// Handlers — files
// ==========================================================================

#define J_TAIL 72   // bytes kept back for the object's closing fields

static int h_fs_list(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    eos_dirh_t *d;
    eos_dirent_t de;
    eos_err_t e;
    long offset, count, total = 0;
    int shown = 0;

    e = q_path(req->uri, "path", s_scr.path);
    if (e != EOS_OK) return eos_httpd_fail_err(h, r, (int)e, "the path parameter is missing or not a path this board accepts");
    if (q_uint(req->uri, "offset", 0, 0x7FFFFFFFL, &offset) != EOS_OK)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG, "offset must be a non-negative number");
    if (q_uint(req->uri, "count", EOS_APPS_LIST_MAX, EOS_APPS_LIST_MAX, &count) != EOS_OK)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG,
                                  "count must be a number no larger than limits.list_max");

    d = eos_storage_opendir(s_scr.path);
    if (!d) return eos_httpd_fail_err(h, r, (int)eos_storage_errno(),
                                      "that directory could not be opened");

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_str(&j, "path", s_scr.path);
    eos_json_key(&j, "entries");
    eos_json_arr_open(&j);

    while (eos_storage_readdir(d, &de)) {
        eos_stat_t st;
        int nlen;

        total++;
        if (total - 1 < offset) continue;
        if (shown >= count) continue;          // keep counting for `total`

        // Stop emitting when one more entry would not leave room to close the
        // document. The page is short, `more` is true, and the client asks for
        // the next one; a truncated JSON document would be a 500 instead.
        nlen = (int)strlen(de.name);
        if (j.len + eos_json_escaped_len(de.name, nlen) + 64 + J_TAIL > j.cap) {
            count = shown;
            continue;
        }

        st.mtime = 0;
        if (eos_path_join(s_scr.full, EOS_PATH_MAX, s_scr.path, de.name) >= 0)
            (void)eos_storage_stat(s_scr.full, &st);

        eos_json_obj_open(&j);
        eos_json_kv_strn(&j, "name", de.name, nlen);
        eos_json_kv_int (&j, "size", (long)de.size);
        eos_json_kv_bool(&j, "is_dir", de.is_dir);
        eos_json_kv_int (&j, "mtime", (long)st.mtime);
        eos_json_obj_close(&j);
        shown++;
    }
    eos_storage_closedir(d);

    eos_json_arr_close(&j);
    eos_json_kv_int (&j, "offset", offset);
    eos_json_kv_int (&j, "total",  total);
    eos_json_kv_bool(&j, "more",   offset + shown < total);
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 200, &j);
}

static int h_fs_stat(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    eos_stat_t st;
    eos_err_t e;

    e = q_path(req->uri, "path", s_scr.path);
    if (e != EOS_OK) return eos_httpd_fail_err(h, r, (int)e, "the path parameter is missing or not a path this board accepts");

    e = eos_storage_stat(s_scr.path, &st);
    if (e != EOS_OK) return eos_httpd_fail_err(h, r, (int)e, "no such path on this board");

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_int (&j, "size",   (long)st.size);
    eos_json_kv_int (&j, "mtime",  (long)st.mtime);
    eos_json_kv_bool(&j, "is_dir", st.is_dir);
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 200, &j);
}

static int h_fs_read(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_stat_t st;
    eos_err_t e;
    long size = -1;
    void *fh;

    e = q_path(req->uri, "path", s_scr.path);
    if (e != EOS_OK) return eos_httpd_fail_err(h, r, (int)e, "the path parameter is missing or not a path this board accepts");

    if (!h->ports.file_read || !h->ports.file_close)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_UNSUPPORTED,
                                  "this image cannot stream files");

    e = eos_storage_stat(s_scr.path, &st);
    if (e != EOS_OK) return eos_httpd_fail_err(h, r, (int)e, "no such file on this board");
    if (st.is_dir)  return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG,
                                              "that is a directory; list it instead");

    // Fallback off: a GET for a file that is not there must be 404 and not the
    // contents of a same-named asset linked into the image.
    fh = file_open_at(h->ctx, s_scr.path, &size, false);
    if (!fh) return eos_httpd_fail_err(h, r, (int)eos_storage_errno(),
                                       "that file could not be opened");

    r->status           = 200;
    r->kind             = EOS_HTTPD_BODY_FILE;
    r->path             = s_scr.path;
    r->file             = fh;
    r->file_size        = size;
    r->content_type     = "application/octet-stream";
    r->content_encoding = NULL;
    r->cache_control    = "no-store";
    return 200;
}

static int h_fs_usage(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    uint64_t total = 0, used = 0;
    eos_err_t e;
    int n;

    n = eos_httpd_query_get(req->uri, "point", s_scr.path, EOS_PATH_MAX);
    if (n < 0) return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG,
                                         "the point parameter names the mount, for example /int");

    e = eos_storage_usage(s_scr.path, &total, &used);
    if (e != EOS_OK) return eos_httpd_fail_err(h, r, (int)e, "that mount is not there");

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_str(&j, "point", s_scr.path);
    eos_json_kv_int(&j, "total", clamp_size(total));
    eos_json_kv_int(&j, "used",  clamp_size(used));
    eos_json_kv_int(&j, "free",  clamp_size(total > used ? total - used : 0));
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 200, &j);
}

static int h_fs_write(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    eos_err_t e;
    long offset, fin;
    bool final;
    int wrote = 0;

    // The transport refused a body larger than the buffer it reads into, so it
    // was never on this board at all. That is what limits.chunk_max means and
    // why a 5 MB photo cannot cost the heap anything: the bound is a stack
    // buffer, not a policy.
    if (req->body_truncated)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_TOOBIG,
                                  "that chunk is larger than limits.chunk_max");

    e = q_path(req->uri, "path", s_scr.path);
    if (e != EOS_OK) return eos_httpd_fail_err(h, r, (int)e, "the path parameter is missing or not a path this board accepts");
    if (q_uint(req->uri, "offset", 0, 0x7FFFFFFFL, &offset) != EOS_OK)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG, "offset must be a non-negative number");
    if (q_uint(req->uri, "final", 0, 1, &fin) != EOS_OK)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG, "final must be 0 or 1");
    final = fin != 0;

    if (req->body_len > EOS_APPS_CHUNK_MAX)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_TOOBIG,
                                  "that chunk is larger than limits.chunk_max");

    if (s_up.open && strcmp(s_up.path, s_scr.path) != 0)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_BUSY,
                                  "another upload holds this board's write handle; "
                                  "finish it or POST /api/fs/upload/abort");

    if (offset == 0) {
        char tmp[EOS_PATH_MAX];
        int  need = snprintf(tmp, sizeof tmp, "%s.part", s_scr.path);
        if (need < 0 || need >= (int)sizeof tmp)
            return eos_httpd_fail_err(h, r, (int)EOS_ERR_TOOBIG,
                                      "that path is too long to upload to safely");

        upload_close();                        // a restart of the same upload
        s_up.f = eos_storage_open(tmp, EOS_O_WRITE | EOS_O_CREATE | EOS_O_TRUNC);
        if (!s_up.f) return eos_httpd_fail_err(h, r, (int)eos_storage_errno(),
                                               "that file could not be created");
        s_up.open = true;
        s_up.off  = 0;
        snprintf(s_up.path, sizeof s_up.path, "%s", s_scr.path);
        snprintf(s_up.tmp,  sizeof s_up.tmp,  "%s", tmp);
    } else {
        if (!s_up.open)
            return eos_httpd_fail_err(h, r, (int)EOS_ERR_STATE,
                                      "no upload is open for that path; start again at offset 0");
        if ((uint32_t)offset != s_up.off)
            return eos_httpd_fail_err(h, r, (int)EOS_ERR_STATE,
                                      "that offset is not where this upload is");
    }

    if (req->body_len > 0) {
        // Exactly one write of exactly what arrived, never a loop: a loop would
        // hold the frame loop for as long as the request was big, on a chip
        // whose instruction cache is off for the duration of a flash program.
        wrote = eos_storage_write(s_up.f, req->body, req->body_len);
        if (wrote < 0) {
            upload_close();
            return eos_httpd_fail_err(h, r, wrote, "the write failed; the upload was dropped");
        }
        if (wrote < req->body_len) {
            upload_close();
            return eos_httpd_fail_err(h, r, (int)EOS_ERR_IO,
                                      "the filesystem took only part of that chunk; "
                                      "it is probably full");
        }
        s_up.off += (uint32_t)wrote;
    }
    s_up.last_ms = s_now_ms;

    if (final) {
        eos_err_t se = eos_storage_sync(s_up.f);
        uint32_t size = s_up.off;
        char     from[EOS_PATH_MAX], to[EOS_PATH_MAX];

        if (se != EOS_OK) {
            upload_close();            // removes the partial; the target stands
            return eos_httpd_fail_err(h, r, (int)se, "the file was written but not committed");
        }

        // Synced, so the bytes are on the media. Only now does the target move.
        snprintf(from, sizeof from, "%s", s_up.tmp);
        snprintf(to,   sizeof to,   "%s", s_up.path);
        upload_close_ex(true);
        (void)eos_storage_remove(to);   // rename onto an existing name is not portable
        se = eos_storage_rename(from, to);
        if (se != EOS_OK) {
            (void)eos_storage_remove(from);
            return eos_httpd_fail_err(h, r, (int)se,
                                      "the upload completed but could not replace the file");
        }

        eos_json_init(&j, h->resp, (int)sizeof h->resp);
        eos_json_obj_open(&j);
        eos_json_kv_str (&j, "path", s_scr.path);
        eos_json_kv_int (&j, "offset", (long)size);
        eos_json_kv_int (&j, "size",   (long)size);
        eos_json_kv_bool(&j, "final",  true);
        eos_json_obj_close(&j);
        return eos_httpd_reply_json(h, r, 200, &j);
    }

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_str (&j, "path", s_scr.path);
    eos_json_kv_int (&j, "offset", (long)s_up.off);
    eos_json_kv_int (&j, "size",   (long)s_up.off);
    eos_json_kv_bool(&j, "final",  false);
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 200, &j);
}

static int h_fs_abort(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    bool aborted = false;
    int n = eos_httpd_query_get(req->uri, "path", s_scr.path, EOS_PATH_MAX);

    // No path aborts whatever is open, which is the only way a client that has
    // lost track of what it was uploading can clear the handle. A path that
    // does not match leaves the handle alone and says so, so one tab cannot
    // cancel another's upload by guessing.
    if (s_up.open && (n < 0 || strcmp(s_up.path, s_scr.path) == 0)) {
        // The partial file is left where it is, exactly as the idle timeout
        // leaves it: the client knows what it was uploading and can remove it.
        upload_close();
        aborted = true;
    }

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "aborted", aborted);
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 200, &j);
}

static int h_fs_simple(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r,
                       int route)
{
    eos_json_t j;
    eos_err_t e;

    if (route == EOS_ROUTE_FS_RENAME) {
        e = q_path(req->uri, "from", s_scr.path);
        if (e != EOS_OK) return eos_httpd_fail_err(h, r, (int)e, "the from parameter is missing or not a path this board accepts");
        e = q_path(req->uri, "to", s_scr.path2);
        if (e != EOS_OK) return eos_httpd_fail_err(h, r, (int)e, "the to parameter is missing or not a path this board accepts");
        if (s_up.open && (strcmp(s_up.path, s_scr.path) == 0 || strcmp(s_up.path, s_scr.path2) == 0))
            return eos_httpd_fail_err(h, r, (int)EOS_ERR_BUSY,
                                      "an upload has that file open");
        e = eos_storage_rename(s_scr.path, s_scr.path2);
        if (e != EOS_OK) return eos_httpd_fail_err(h, r, (int)e, "the rename failed");
    } else {
        e = q_path(req->uri, "path", s_scr.path);
        if (e != EOS_OK) return eos_httpd_fail_err(h, r, (int)e, "the path parameter is missing or not a path this board accepts");
        if (route == EOS_ROUTE_FS_MKDIR) {
            e = eos_storage_mkdir(s_scr.path);
            if (e != EOS_OK) return eos_httpd_fail_err(h, r, (int)e,
                                                       "that directory could not be created; "
                                                       "parents are not created for you");
        } else {
            // Removing the file an upload has open would leave the handle
            // pointing at nothing on some filesystems and at a ghost on others.
            if (s_up.open && strcmp(s_up.path, s_scr.path) == 0)
                return eos_httpd_fail_err(h, r, (int)EOS_ERR_BUSY,
                                          "an upload has that file open");
            e = eos_storage_remove(s_scr.path);
            if (e != EOS_OK) return eos_httpd_fail_err(h, r, (int)e,
                                                       "that could not be removed; a directory "
                                                       "has to be empty first");
        }
    }

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "ok", true);
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 200, &j);
}

// ==========================================================================
// Handlers — the console
// ==========================================================================

static int h_console_log(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    long since, maxb;
    uint32_t first_seq, next;
    long dropped = 0;
    int i, spent = 0;

    if (q_uint(req->uri, "since", 0, 0x7FFFFFFFL, &since) != EOS_OK)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG, "since must be a non-negative number");
    if (q_uint(req->uri, "max", 4096, 0x7FFFFFFFL, &maxb) != EOS_OK)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG, "max must be a non-negative number");

    log_lock();

    // A `since` past the end answers an empty page with the real cursor rather
    // than erroring, and it does so by falling out of the loop below: no line
    // has a sequence that high. That is the case where the board rebooted and
    // the counter restarted under a client still holding the old cursor, and a
    // page that wedged on it would need a reload every time the board came
    // back. There is deliberately no clamp here — the loop is the answer, and a
    // clamp would be a second one that could disagree with it.
    first_seq = s_lcount ? s_lline[s_lhead].seq : s_lseq;
    if ((uint32_t)since < first_seq) dropped = (long)(first_seq - (uint32_t)since);
    next = s_lseq;

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_key(&j, "lines");
    eos_json_arr_open(&j);

    for (i = 0; i < s_lcount; i++) {
        const logline_t *l = &s_lline[(s_lhead + i) % EOS_APPS_LOG_LINES];
        int need;

        if (l->seq < (uint32_t)since) continue;
        if (spent + (int)l->len > maxb) { next = l->seq; break; }

        need = eos_json_escaped_len(s_lbuf + l->off, (int)l->len) + 32;
        if (j.len + need + J_TAIL > j.cap) { next = l->seq; break; }

        eos_json_obj_open(&j);
        eos_json_kv_strn(&j, "text", s_lbuf + l->off, (int)l->len);
        eos_json_kv_strn(&j, "level", &l->lvl, 1);
        eos_json_obj_close(&j);
        spent += (int)l->len;
    }

    log_unlock();

    eos_json_arr_close(&j);
    eos_json_kv_int(&j, "next",    (long)next);
    eos_json_kv_int(&j, "dropped", dropped);
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 200, &j);
}

// The command table, and it is CLOSED.
//
// This endpoint is reachable, unauthenticated, from any page open on any phone
// on the same WiFi as a board that is holding that WiFi's password in NVS.
// There is no login, there cannot usefully be one on a device provisioned by
// pointing a camera at a QR code, and the whole surface is one POST away from
// anything it can reach. So the table is seven words: every one of them is
// read-only except `reboot`, which does no more than pulling the USB cable
// does, and none of them takes an argument.
//
// What is deliberately absent and must stay absent: any form of eval, any
// read or write of an address, any file operation (the /api/fs routes above
// already do that, and they are the ones the path checks are written against),
// and anything that changes a setting — settings go through POST /api/settings
// where they are validated once, in one place, by the component that owns them.
// An "exec" that can be extended by a caller is a shell, and a shell on a board
// that stores WiFi credentials is a credential dump waiting for one guest on
// the network.
//
// Output is fire and forget: it goes to the log ring and comes back through
// /api/console/log like everything else. A synchronous exec would have to
// buffer its whole output on a 173 KB heap.

typedef enum {
    CMD_HELP = 0, CMD_STATUS, CMD_HEAP, CMD_REBOOT, CMD_THEME, CMD_WIFI, CMD_BRAIN
} cmd_t;

static const struct { const char *word; const char *help; } CMDS[] = {
    { "help",   "this list"                                  },
    { "status", "board, uptime, heap, network and filesystems" },
    { "heap",   "free and largest block"                     },
    { "reboot", "restart the board in half a second"         },
    { "theme",  "the theme the OS is wearing"                },
    { "wifi",   "the network, address and signal"            },
    { "brain",  "the megabrain host and model"               },
};
#define N_CMDS ((int)(sizeof CMDS / sizeof CMDS[0]))

static void say(const char *topic)
{
    char buf[EOS_APPS_LOG_TEXT_MAX];
    int n;

    if (!s_ports.describe) {
        eos_apps_logf('W', "%s: this image cannot answer that", topic);
        return;
    }
    n = s_ports.describe(s_ctx, topic, buf, (int)sizeof buf);
    if (n < 0) eos_apps_logf('W', "%s: this board has nothing to say", topic);
    else       eos_apps_log('I', buf);
}

static void say_fs(void)
{
    eos_mount_t mnt[EOS_MOUNT_MAX];
    int i, n = eos_storage_mounts(mnt, EOS_MOUNT_MAX);

    for (i = 0; i < n && i < EOS_MOUNT_MAX; i++) {
        uint64_t total = 0, used = 0;
        if (mnt[i].mounted) eos_storage_usage(mnt[i].point, &total, &used);
        eos_apps_logf('I', "fs %s %s %lu of %lu bytes used", mnt[i].point,
                      mnt[i].mounted ? "mounted" : "not present",
                      (unsigned long)clamp_size(used), (unsigned long)clamp_size(total));
    }
}

static void say_uptime(void)
{
    uint32_t s = s_now_ms / 1000u;
    eos_apps_logf('I', "up %ud %02u:%02u:%02u", (unsigned)(s / 86400u),
                  (unsigned)((s / 3600u) % 24u), (unsigned)((s / 60u) % 60u),
                  (unsigned)(s % 60u));
}

static void run_cmd(int c)
{
    int i;

    switch (c) {
    case CMD_HELP:
        for (i = 0; i < N_CMDS; i++)
            eos_apps_logf('I', "%-7s %s", CMDS[i].word, CMDS[i].help);
        eos_apps_log('I', "that is the whole table; nothing else is accepted");
        break;
    case CMD_STATUS:
        say("board"); say_uptime(); say("heap"); say("wifi"); say_fs();
        break;
    case CMD_HEAP:   say("heap");  break;
    case CMD_THEME:  say("theme"); break;
    case CMD_WIFI:   say("wifi");  break;
    case CMD_BRAIN:  say("brain"); break;
    case CMD_REBOOT:
        if (!s_ports.reboot) { eos_apps_log('W', "reboot: this image cannot"); break; }
        eos_apps_log('W', "reboot: going down in half a second");
        s_ports.reboot(s_ctx, 500);
        break;
    default: break;
    }
}

static int h_console_exec(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    char cmd[32];
    int n = 0, i, found = -1;
    uint32_t seq;

    if (req->body_truncated)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_TOOBIG, "that body is too large");
    if (!req->body)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG, "the body must carry a cmd");

    switch (eos_json_get_str(req->body, req->body_len, "cmd", cmd, (int)sizeof cmd, &n)) {
    case EOS_JSON_FOUND: break;
    case EOS_JSON_TOOBIG:
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG,
                                  "no command in the table is that long");
    default:
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG,
                                  "the body must be an object carrying a cmd string");
    }

    // Matched exactly, with no trimming and no argument splitting. "help " is
    // refused. A table that quietly forgives whitespace is a table that has
    // started parsing, and the next forgiving step is the one that lets
    // something through.
    (void)n;
    for (i = 0; i < N_CMDS; i++)
        if (strcmp(cmd, CMDS[i].word) == 0) { found = i; break; }

    if (found < 0) {
        // Refused loudly and in both directions: a 400 the client can render,
        // and a console line so the person watching the pane sees why.
        eos_apps_logf('W', "refused: \"%.20s\" is not in the command table", cmd);
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG,
                                  "this board runs seven commands and no arguments: "
                                  "help status heap reboot theme wifi brain");
    }

    seq = eos_apps_log_seq();
    eos_apps_logf('I', "$ %s", CMDS[found].word);
    run_cmd(found);

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_int (&j, "seq", (long)seq);
    eos_json_kv_bool(&j, "accepted", true);
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 202, &j);
}

// ==========================================================================
// Handlers — buddy and apps
// ==========================================================================

void eos_apps_buddy_write_json(eos_json_t *j)
{
    char accent[8];

    eos_json_obj_open(j);
    eos_json_kv_int(j, "schema_version", s_bud.schema);
    eos_json_kv_str(j, "name", s_bud.name);
    eos_json_kv_str(j, "personality", s_bud.persona);
    if (s_bud.accent == 0xFFFFFFFFu) {
        eos_json_kv_null(j, "accent");
    } else {
        snprintf(accent, sizeof accent, "#%06lx",
                 (unsigned long)(s_bud.accent & 0xFFFFFFu));   // 24 bits, provably
        eos_json_kv_str(j, "accent", accent);
    }

    eos_json_key(j, "idle");
    eos_json_obj_open(j);
    eos_json_kv_str(j, "behaviour", eos_apps_idle_name(s_bud.behaviour));
    eos_json_kv_int(j, "sleep_ms", (long)s_bud.cfg.idle_sleep_ms);
    eos_json_kv_int(j, "home_yaw", s_bud.cfg.home_yaw);
    eos_json_obj_close(j);

    eos_json_key(j, "eyes");
    eos_json_obj_open(j);
    eos_json_kv_int(j, "open_index", s_bud.cfg.eye_ci);
    eos_json_kv_int(j, "shut_index", s_bud.cfg.eye_shut_ci);
    eos_json_obj_close(j);

    // Reported from the model that actually parsed, not echoed from the file.
    // web/README.md already calls buddy.json's model block advisory and the
    // .vox authoritative; this is the board saying which one it is holding.
    eos_json_key(j, "model");
    eos_json_obj_open(j);
    // `file` is the basename and stays for the client that only ever knew one
    // filename; `path` is where the model was actually read from and `slug` is
    // which gallery entry that was. A web app should read `path`: with a
    // gallery the name is no longer a constant, and guessing "buddy.vox" is
    // how the Buddy tab ends up loading a model the board is not wearing.
    {
        const char *f = s_bud.file;
        const char *base = strrchr(f, '/');
        eos_json_kv_str(j, "file", base ? base + 1 : (f[0] ? f : "buddy.vox"));
    }
    eos_json_kv_str (j, "path", s_bud.file);
    if (s_bud.slug[0]) eos_json_kv_str (j, "slug", s_bud.slug);
    else               eos_json_kv_null(j, "slug");
    eos_json_kv_bool(j, "loaded", s_bud.loaded);
    eos_json_key(j, "dim");
    eos_json_arr_open(j);
    eos_json_int(j, s_bud.loaded ? s_bud.model.sx : 0);
    eos_json_int(j, s_bud.loaded ? s_bud.model.sy : 0);
    eos_json_int(j, s_bud.loaded ? s_bud.model.sz : 0);
    eos_json_arr_close(j);
    eos_json_kv_int(j, "voxels", s_bud.loaded ? s_bud.model.count : 0);
    eos_json_obj_close(j);
    eos_json_obj_close(j);
}

static int h_buddy(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_json_t j;

    // Normal on a fresh board, and the editor is written for it: it says there
    // is no model yet and lets you build one.
    if (!s_bud.have_json && !s_bud.loaded)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_NOTFOUND,
                                  "there is no buddy on this board yet");

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_key(&j, "buddy");
    eos_apps_buddy_write_json(&j);
    eos_json_kv_str(&j, "state", state_name((int)s_bud.state));
    if (s_bud.err) eos_json_kv_str(&j, "error", s_bud.err);
    else           eos_json_kv_null(&j, "error");
    // The file format's caps are 4096 voxels and 32 on a side; this board
    // stages less than that and says so, so the editor can warn before somebody
    // spends an evening on a model it will refuse.
    eos_json_key(&j, "limits");
    eos_json_obj_open(&j);
    eos_json_kv_int(&j, "voxels", EOS_APPS_VOX_VOXELS);
    eos_json_kv_int(&j, "bytes",  EOS_APPS_VOX_BYTES);
    eos_json_kv_int(&j, "dim",    EOS_VOX_MAX_DIM);
    eos_json_obj_close(&j);
    eos_json_kv_str(&j, "dir", EOS_APPS_BUDDY_DIR);
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 200, &j);
}

static int h_buddy_reload(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_json_t j;
    eos_err_t e = eos_apps_buddy_reload();

    if (e != EOS_OK)
        return eos_httpd_fail_err(h, r, (int)e,
                                  s_bud.err ? s_bud.err : "the buddy could not be reloaded");

    eos_apps_logf('I', "buddy: %u voxels, %ux%ux%u, %s",
                  (unsigned)s_bud.model.count, (unsigned)s_bud.model.sx,
                  (unsigned)s_bud.model.sy, (unsigned)s_bud.model.sz,
                  s_bud.name[0] ? s_bud.name : "unnamed");

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "ok", true);
    eos_json_kv_str (&j, "state", state_name((int)s_bud.state));
    eos_json_key(&j, "buddy");
    eos_apps_buddy_write_json(&j);
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 200, &j);
}

static int h_apps(eos_httpd_t *h, eos_httpd_resp_t *r)
{
    eos_json_t j;
    int i;

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_key(&j, "apps");
    eos_json_arr_open(&j);
    for (i = 0; i < s_apps_n; i++) {
        eos_json_obj_open(&j);
        eos_json_kv_str(&j, "id",   s_apps[i].id   ? s_apps[i].id   : "");
        eos_json_kv_str(&j, "name", s_apps[i].name ? s_apps[i].name : "");
        if (s_apps[i].summary) eos_json_kv_str(&j, "summary", s_apps[i].summary);
        eos_json_kv_int(&j, "tier_min", s_apps[i].tier_min);
        eos_json_obj_close(&j);
    }
    eos_json_arr_close(&j);
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 200, &j);
}

// ==========================================================================
// Dispatch
// ==========================================================================

int eos_apps_dispatch(eos_httpd_t *h, int route,
                      const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    if (!h || !req || !r) return 500;

    switch (route) {
    case EOS_ROUTE_FS_LIST:      return h_fs_list(h, req, r);
    case EOS_ROUTE_FS_STAT:      return h_fs_stat(h, req, r);
    case EOS_ROUTE_FS_READ:      return h_fs_read(h, req, r);
    case EOS_ROUTE_FS_USAGE:     return h_fs_usage(h, req, r);
    case EOS_ROUTE_FS_WRITE:     return h_fs_write(h, req, r);
    case EOS_ROUTE_FS_ABORT:     return h_fs_abort(h, req, r);
    case EOS_ROUTE_FS_MKDIR:
    case EOS_ROUTE_FS_REMOVE:
    case EOS_ROUTE_FS_RENAME:    return h_fs_simple(h, req, r, route);
    case EOS_ROUTE_CONSOLE_LOG:  return h_console_log(h, req, r);
    case EOS_ROUTE_CONSOLE_EXEC: return h_console_exec(h, req, r);
    case EOS_ROUTE_BUDDY:        return h_buddy(h, r);
    case EOS_ROUTE_BUDDY_RELOAD: return h_buddy_reload(h, r);

    // The gallery lives in its own translation unit and is reached through
    // this one call rather than through a second eos_httpd_set_api() pointer.
    // eos_httpd holds exactly one, and a second is a second thing an image can
    // leave NULL while its routes are still in the table.
    case EOS_ROUTE_BUDDY_GALLERY:
    case EOS_ROUTE_BUDDY_GALLERY_SELECT:
    case EOS_ROUTE_BUDDY_GALLERY_REMOVE:
        return eos_gallery_dispatch(h, route, req, r);
    case EOS_ROUTE_APPS:         return h_apps(h, r);
    default: break;
    }
    return eos_httpd_fail_err(h, r, (int)EOS_ERR_NOTFOUND, "no such endpoint on this board");
}

// ==========================================================================
// Wiring
// ==========================================================================

int eos_apps_chunk_max(void)  { return EOS_APPS_CHUNK_MAX; }
int eos_apps_list_max(void)   { return EOS_APPS_LIST_MAX; }
int eos_apps_path_max(void)   { return EOS_PATH_MAX; }
int eos_apps_name_max(void)   { return EOS_NAME_MAX; }
int eos_apps_open_files(void) { return EOS_MAX_FILES; }

void eos_apps_set_apps(const eos_apps_app_t *apps, int n)
{
    // The ceiling bounds the RESPONSE, not the caller's array: nothing here can
    // know how long that array really is, and a clamp that reads as protection
    // while providing none is worse than no clamp. `n` is the boot glue's
    // promise about its own compiled-in table, and it is the only bound there
    // is. What this does is stop a wrong n from also overflowing the JSON.
    if (n < 0) n = 0;
    if (n > EOS_APPS_CATALOG_MAX) n = EOS_APPS_CATALOG_MAX;
    s_apps   = apps;
    s_apps_n = apps ? n : 0;
}

void eos_apps_init(const eos_apps_ports_t *ports, void *ctx)
{
    memset(&s_ports, 0, sizeof s_ports);
    if (ports) s_ports = *ports;
    s_ctx = ctx;

    memset(&s_up, 0, sizeof s_up);
    memset(&s_fslot, 0, sizeof s_fslot);
    s_lhead = s_lcount = s_lwpos = 0;
    s_lseq = 0;

    memset(&s_bud, 0, sizeof s_bud);
    buddy_defaults();
    s_bud.state = EOS_BUDDY_IDLE;

#ifdef ESP_PLATFORM
    if (!s_lmutex) s_lmutex = xSemaphoreCreateMutexStatic(&s_lmem);
#endif

    eos_httpd_set_api(eos_apps_dispatch);
}

// ==========================================================================
// ESP-IDF bindings
// ==========================================================================
//
// Everything above this line is portable C99 and is what the host suite runs.

#ifdef ESP_PLATFORM

#include "esp_log.h"

static vprintf_like_t s_prev_vprintf;

// ESP-IDF hands this a fully formatted line, colour escapes and all, of the
// shape "\033[0;32mI (1234) tag: message\033[0m\n". The ring wants the level
// letter and the text; the UART wants exactly what it was going to get. So this
// forwards first and then strips: escape sequences, the level letter, and the
// timestamp in parentheses, leaving "tag: message" — which is what reads as a
// console line rather than as a log file quoted into one.
//
// The formatted line goes on the CALLING task's stack, not into a shared
// buffer, because this is called from every task in the image and a shared one
// would need a lock held across a vsnprintf.
static int log_hook(const char *fmt, va_list ap)
{
    char buf[EOS_APPS_LOG_TEXT_MAX + 40];
    char lvl = 'I';
    const char *p = buf;
    va_list copy;
    int n;

    va_copy(copy, ap);
    n = vsnprintf(buf, sizeof buf, fmt, copy);
    va_end(copy);

    if (n > 0) {
        while (*p) {
            if (*p == '\033') { while (*p && *p != 'm') p++; if (*p) p++; continue; }
            break;
        }
        if (*p && strchr("EWIDV", *p) && p[1] == ' ' && p[2] == '(') {
            lvl = (*p == 'V') ? 'D' : *p;
            p += 3;
            while (*p && *p != ')') p++;
            if (*p == ')') p++;
            while (*p == ' ') p++;
        }
        eos_apps_log(lvl, p);
    }

    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
}

void eos_apps_log_install(void)
{
    if (s_prev_vprintf) return;
    s_prev_vprintf = esp_log_set_vprintf(log_hook);
}

#endif // ESP_PLATFORM
