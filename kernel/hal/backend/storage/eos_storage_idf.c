// eos_storage over LittleFS — the internal flash filesystem, and the card slot
// that nobody has wired yet.
//
// Everything above this file opens "/int/settings.json" and never learns what
// answered. This is what answers. The internal partition is a LittleFS image
// on the 960 KB "int" partition, reached through the POSIX layer ESP-IDF's VFS
// already puts in front of it, and every call below is the same C on the host
// as on the board — on the host the mount is a directory instead of a
// partition, which is what lets the path rules and the handle pools be
// attacked off-target rather than trusted.
//
// The one non-obvious constraint is that a flash write on this chip stops the
// chip. Erasing or programming the SPI flash means the instruction cache is
// turned off for the duration, and the ESP32-C6 has one core which is also
// running the frame loop, the WiFi stack and the HTTP server. A write here is
// not slow like a disk is slow — it is a hole in the whole machine, roughly a
// millisecond per 4 KB page programmed and tens of milliseconds per 4 KB
// sector erased. That is why nothing in this file loops over the media, why
// eos_storage_write() moves exactly what it was handed and returns, and why
// the caller — settings, a file upload — is the thing that has to be bounded.
// Reads do not pay this: LittleFS reads go through the flash cache and cost
// microseconds.
//
// The second thing worth stating twice: a path arriving here came off the
// network. /api/fs/read takes it out of a query string. So path_split()
// REJECTS "..", it does not resolve it, it rejects backslashes because FatFs
// accepts those as separators the day the card is wired, it rejects control
// bytes, and it refuses anything over EOS_PATH_MAX rather than truncating —
// a truncated path names a different file. Nothing below path_split() ever
// sees a string it did not build itself out of the mount's own base and one
// checked remainder.
//
// The card is declared and routed and does not exist. boards/*.json says
// sdcard.present false on the board this runs on because a JTAG scan cannot
// see MISO, so "/sd" is in the mount table, reports itself unmounted, and
// answers every call with EOS_ERR_NODEV instead of hanging on a bus that has
// no card on it. When the pins are found this file needs a FAT mount in
// mount_one() and nothing else.

#include "eos_storage.h"
#include "eos_board.h"

#include <stdio.h>    /* rename() lives here and nowhere else */
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#else
#include <stdlib.h>   /* getenv, for the host root */
#endif

// The pool has to cover one open file per HTTP worker or a static file request
// can lose its slot to three other tabs and 404 for no reason the owner can
// see. esp_http_server runs four; eos_httpd_cfg_default() says so too.
#if EOS_MAX_FILES < 4
#error "EOS_MAX_FILES below four can starve an esp_http_server worker mid-response"
#endif
#if EOS_MAX_DIRS < 1
#error "EOS_MAX_DIRS below one cannot list a directory"
#endif

// A system path is a mount base plus one checked remainder plus, inside
// readdir, one entry name. Sized with room to spare because the host root is a
// real directory and can be long; every builder bound-checks anyway, so the
// safety does not rest on this number being right.
#define SYS_MAX (EOS_PATH_MAX + EOS_NAME_MAX + 64)

#ifndef EOS_STORAGE_HOST_ROOT_DEFAULT
#define EOS_STORAGE_HOST_ROOT_DEFAULT "/tmp/eos-int"
#endif

#ifdef ESP_PLATFORM
static const char *TAG = "eos_storage";
#endif

// ---------------------------------------------------------------- errno
//
// Per task, because two HTTP workers fail at the same time for different
// reasons and a shared word would hand one of them the other's answer. It is
// four bytes of thread-local storage per task and it follows C errno's rule:
// set on failure, never cleared on success, meaningful only immediately after
// a call that said it failed.

#if defined(__GNUC__) || defined(__clang__)
#define EOS_TLS __thread
#else
#define EOS_TLS
#endif

static EOS_TLS eos_err_t s_err;

eos_err_t eos_storage_errno(void) { return s_err; }

static eos_err_t fail(eos_err_t e) { s_err = e; return e; }

// errno is what the VFS and the host both speak. This is the only place it is
// translated, so the web layer's error table maps one to one off eos_err_t and
// never sees a POSIX number.
static eos_err_t from_errno(int e)
{
    switch (e) {
    case ENOENT:  return EOS_ERR_NOTFOUND;
    case EEXIST:  return EOS_ERR_EXISTS;
#ifdef ENOTEMPTY
    case ENOTEMPTY: return EOS_ERR_EXISTS;   // a non-empty directory is not removed
#endif
    case EACCES:
    case EPERM:
    case EROFS:   return EOS_ERR_READONLY;
    case EINVAL:
    case EISDIR:
    case ENOTDIR:
    case ENAMETOOLONG: return EOS_ERR_ARG;
    case EBUSY:   return EOS_ERR_BUSY;
    case EMFILE:
    case ENFILE:  return EOS_ERR_POOL;
    case ENODEV:
    case ENXIO:   return EOS_ERR_NODEV;
    case EXDEV:   return EOS_ERR_UNSUPPORTED;   // rename across mounts
    default:      return EOS_ERR_IO;            // ENOSPC lands here: the media said no
    }
}

// ---------------------------------------------------------------- state

typedef struct {
    eos_mount_t pub;
    const char *base;     // POSIX prefix: "/int" on target, a real directory on the host
    const char *label;    // partition label, LittleFS only
    bool        present;  // the board says the hardware is there
    uint8_t     held;     // open files + open dirs; unmount is BUSY while nonzero
} mnt_t;

static mnt_t s_mnt[EOS_MOUNT_MAX];
static int   s_mnt_n;
static bool  s_inited;

struct eos_file {
    bool    used;
    int8_t  mnt;
    uint8_t mode;
    int     fd;
};

struct eos_dirh {
    bool    used;
    int8_t  mnt;              // -1 is the root pseudo-directory: it lists the mounts
    int     idx;              // root only: the next mount to report
    DIR    *d;
    char    base[SYS_MAX];    // this directory's system path, for the per-entry stat
};

static struct eos_file s_files[EOS_MAX_FILES];
static struct eos_dirh s_dirs[EOS_MAX_DIRS];

#ifndef ESP_PLATFORM
static char s_host_root[SYS_MAX];
#endif

// The pool claim is the only thing four HTTP workers can race for. It is a
// static mutex — FreeRTOS builds it in the storage this file already owns, so
// there is still no allocation anywhere — and no media call is ever made while
// it is held.
#ifdef ESP_PLATFORM
static StaticSemaphore_t s_lock_mem;
static SemaphoreHandle_t s_lock;
#define LOCK()   do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_lock) xSemaphoreGive(s_lock); } while (0)
#else
#define LOCK()   do { } while (0)
#define UNLOCK() do { } while (0)
#endif

// ------------------------------------------------------------ path rules

static int mount_index(const char *name, int len)
{
    int i;
    for (i = 0; i < s_mnt_n; i++) {
        const char *p = s_mnt[i].pub.point + 1;   // past the leading '/'
        int j = 0;
        while (j < len && p[j] && p[j] == name[j]) j++;
        if (j == len && p[j] == '\0') return i;
    }
    return -1;
}

// Splits an penguinOS path into a mount and the remainder below it, and is the
// whole of this file's defence against a hostile path.
//
//   "/int/a/b"     -> mount int, rel "a/b"
//   "/int//a/./b/" -> mount int, rel "a/b"      separators collapse, "." drops
//   "/"            -> mount -1,  rel ""         the pseudo-directory of mounts
//   "/int/../x"    -> EOS_ERR_ARG               ".." is refused, never resolved
//   "a/b"          -> EOS_ERR_ARG               a relative path names no mount
//   "/nope/x"      -> EOS_ERR_NOTFOUND
//
// ".." is rejected rather than folded away on purpose. Folding is correct and
// it is also one off-by-one away from being an escape; refusing costs a caller
// nothing, because nothing in penguinOS builds a path that needs a parent.
static eos_err_t path_split(const char *path, int *mount_out, char *rel, int rel_cap)
{
    int i = 0, n = 0, m = -1;

    if (mount_out) *mount_out = -1;
    if (rel && rel_cap > 0) rel[0] = '\0';
    if (!path || !rel || rel_cap < 1) return EOS_ERR_ARG;
    if (path[0] != '/') return EOS_ERR_ARG;

    // Length before anything else, so nothing below walks off the end of a
    // string that has no terminator inside the budget.
    while (i < EOS_PATH_MAX && path[i]) i++;
    if (i >= EOS_PATH_MAX) return EOS_ERR_TOOBIG;

    i = 0;
    while (path[i]) {
        int c0, len, k;

        while (path[i] == '/') i++;
        if (!path[i]) break;

        c0 = i;
        while (path[i] && path[i] != '/') i++;
        len = i - c0;
        if (len >= EOS_NAME_MAX) return EOS_ERR_TOOBIG;

        for (k = 0; k < len; k++) {
            unsigned char ch = (unsigned char)path[c0 + k];
            // Control bytes cannot be typed, cannot be shown, and are how a
            // log line or a JSON string gets broken from the other side.
            if (ch < 0x20 || ch == 0x7f) return EOS_ERR_ARG;
            // FatFs treats '\' as a separator. No card is mounted today; the
            // day one is, this rule is already here.
            if (ch == '\\') return EOS_ERR_ARG;
        }

        if (len == 1 && path[c0] == '.') continue;
        if (len == 2 && path[c0] == '.' && path[c0 + 1] == '.') return EOS_ERR_ARG;

        if (m < 0) {
            m = mount_index(path + c0, len);
            if (m < 0) return EOS_ERR_NOTFOUND;
            continue;
        }

        if (n) {
            if (n + 1 >= rel_cap) return EOS_ERR_TOOBIG;
            rel[n++] = '/';
        }
        if (n + len >= rel_cap) return EOS_ERR_TOOBIG;
        memcpy(rel + n, path + c0, (size_t)len);
        n += len;
    }

    rel[n] = '\0';
    if (mount_out) *mount_out = m;
    return EOS_OK;
}

// base + "/" + rel. An empty remainder becomes "<base>/" and not "<base>",
// because esp_vfs hands the filesystem everything past the base path and
// LittleFS wants "/" for its root, not "".
static eos_err_t sys_join(const char *base, const char *rel, char *out, int cap)
{
    int n = 0, i;

    if (!base || !rel || !out || cap < 2) return EOS_ERR_ARG;
    for (i = 0; base[i]; i++) {
        if (n >= cap - 1) return EOS_ERR_TOOBIG;
        out[n++] = base[i];
    }
    if (n >= cap - 1) return EOS_ERR_TOOBIG;
    out[n++] = '/';
    for (i = 0; rel[i]; i++) {
        if (n >= cap - 1) return EOS_ERR_TOOBIG;
        out[n++] = rel[i];
    }
    out[n] = '\0';
    return EOS_OK;
}

// The one call every public entry point starts with: check the path, find the
// mount, insist it is mounted, and build the system path. rel_out is optional
// and is how a caller finds out it was handed a mount root rather than a file.
static eos_err_t resolve(const char *path, int *mi_out, char *sys, int cap, char *rel_out)
{
    char rel[EOS_PATH_MAX];
    eos_err_t e;
    int mi;

    if (!s_inited) return EOS_ERR_STATE;
    e = path_split(path, &mi, rel, (int)sizeof rel);
    if (e != EOS_OK) return e;
    if (rel_out) memcpy(rel_out, rel, strlen(rel) + 1);
    if (mi_out) *mi_out = mi;
    if (mi < 0) return EOS_OK;                       // "/" — no media to reach
    if (!s_mnt[mi].pub.mounted) return EOS_ERR_NODEV; // the card is not there
    return sys_join(s_mnt[mi].base, rel, sys, cap);
}

// ------------------------------------------------------------ the mounts

static eos_err_t mount_one(mnt_t *m)
{
    if (m->pub.mounted) return EOS_OK;
    if (!m->present) return EOS_ERR_NODEV;

#ifdef ESP_PLATFORM
    if (m->pub.fs == EOS_FS_LITTLEFS) {
        esp_vfs_littlefs_conf_t conf = {
            .base_path              = m->pub.point,
            .partition_label        = m->label,
            .format_if_mount_failed = true,   // a blank partition is a first boot,
            .dont_mount             = false,  // not a fault
        };
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = esp_vfs_littlefs_register(&conf);
        int64_t ms = (esp_timer_get_time() - t0) / 1000;

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: littlefs '%s' failed: %s",
                     m->pub.point, m->label, esp_err_to_name(err));
            return err == ESP_ERR_NOT_FOUND ? EOS_ERR_NODEV : EOS_ERR_IO;
        }
        // The number this print carries is the one nobody can guess: a mount
        // that had to format is tens of times slower than one that did not,
        // and it happens exactly once in a board's life.
        ESP_LOGI(TAG, "%s: littlefs '%s' mounted in %lld ms", m->pub.point, m->label,
                 (long long)ms);
        m->pub.mounted  = true;
        m->pub.writable = true;
        return EOS_OK;
    }
    // EOS_FS_FAT. No board in the registry declares working card pins, so
    // there is no SDSPI mount here to be wrong about. present is false on all
    // of them and this line is unreachable today; it is the honest answer for
    // a profile that turns the slot on before this file grows a FAT mount.
    return EOS_ERR_UNSUPPORTED;
#else
    if (m->pub.fs != EOS_FS_LITTLEFS) return EOS_ERR_UNSUPPORTED;
    if (mkdir(m->base, 0777) != 0 && errno != EEXIST) return from_errno(errno);
    m->pub.mounted  = true;
    m->pub.writable = true;
    return EOS_OK;
#endif
}

static eos_err_t unmount_one(mnt_t *m)
{
    if (!m->pub.mounted) return EOS_OK;
    if (m->held) return EOS_ERR_BUSY;

#ifdef ESP_PLATFORM
    if (m->pub.fs == EOS_FS_LITTLEFS) {
        esp_err_t err = esp_vfs_littlefs_unregister(m->label);
        if (err != ESP_OK) return EOS_ERR_IO;
    }
#endif
    m->pub.mounted  = false;
    m->pub.writable = false;
    m->pub.total    = 0;
    m->pub.used     = 0;
    return EOS_OK;
}

static void table_build(const eos_board_t *b)
{
    mnt_t *m;

    memset(s_mnt, 0, sizeof s_mnt);
    s_mnt_n = 0;

    // "/int" is first, and it is first in the root listing for the same
    // reason: it is the mount that is always there.
    if (s_mnt_n < EOS_MOUNT_MAX && b->storage.int_label && b->storage.int_label[0]) {
        m = &s_mnt[s_mnt_n++];
        m->pub.point     = b->storage.int_point ? b->storage.int_point : "/int";
        m->pub.fs        = EOS_FS_LITTLEFS;
        m->pub.removable = false;
        m->label         = b->storage.int_label;
        m->present       = true;
#ifdef ESP_PLATFORM
        m->base = m->pub.point;          // esp_vfs registers this as the base path
#else
        m->base = s_host_root;
#endif
    }

    // "/sd" is declared whether or not the slot is wired, so that
    // eos_storage_mounts() — and /api/system through it — can say the card is
    // not there, rather than leaving a file browser to infer it from a 404.
    if (s_mnt_n < EOS_MOUNT_MAX) {
        m = &s_mnt[s_mnt_n++];
        m->pub.point     = b->storage.sd_point ? b->storage.sd_point : "/sd";
        m->pub.fs        = b->storage.sd ? (uint8_t)EOS_FS_FAT : (uint8_t)EOS_FS_NONE;
        m->pub.removable = true;
        m->present       = b->storage.sd;
        m->base          = m->pub.point;
    }
}

eos_err_t eos_storage_init(void)
{
    const eos_board_t *b = eos_board_get();
    eos_err_t e_int = EOS_ERR_NODEV;
    int i;

    if (s_inited) return EOS_OK;
    if (!b) return fail(EOS_ERR_STATE);

#ifdef ESP_PLATFORM
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_mem);
#else
    {
        const char *r = getenv("EOS_STORAGE_HOST_ROOT");
        if (!r || !r[0]) r = EOS_STORAGE_HOST_ROOT_DEFAULT;
        if (strlen(r) >= sizeof s_host_root) return fail(EOS_ERR_TOOBIG);
        strcpy(s_host_root, r);
    }
#endif

    memset(s_files, 0, sizeof s_files);
    memset(s_dirs, 0, sizeof s_dirs);
    table_build(b);

    for (i = 0; i < s_mnt_n; i++) {
        eos_err_t e = mount_one(&s_mnt[i]);
        if (s_mnt[i].pub.fs == EOS_FS_LITTLEFS) e_int = e;
        // A missing card is not a failure here and never has been. It comes
        // back as a declared, unmounted mount and eos_storage_mount("/sd")
        // can be retried the moment somebody pushes one in.
    }

    // Initialised either way, and that is deliberate. A board whose internal
    // filesystem did not come up still has a mount table worth reporting: it
    // is what makes /api/system say "/int, littlefs, not mounted" instead of
    // saying nothing, what turns every file call into a clean NODEV instead of
    // a STATE nobody can act on, and what lets eos_storage_mount("/int") be
    // retried. The IO return is still the truth: the partition is wrong or the
    // flash is failing, and the caller should say so.
    s_inited = true;
    if (e_int != EOS_OK) return fail(EOS_ERR_IO);
    return EOS_OK;
}

int eos_storage_mounts(eos_mount_t *out, int max)
{
    int i;
    if (out)
        for (i = 0; i < s_mnt_n && i < max; i++) out[i] = s_mnt[i].pub;
    return s_mnt_n;
}

const eos_mount_t *eos_storage_mount_for(const char *path)
{
    char rel[EOS_PATH_MAX];
    int mi;
    if (path_split(path, &mi, rel, (int)sizeof rel) != EOS_OK) return NULL;
    if (mi < 0) return NULL;
    return &s_mnt[mi].pub;
}

// Mount and unmount name a point exactly: "/sd", not a path inside it. That is
// the card-insert path and it should not be reachable by accident from a file
// operation that happened to be handed a directory name.
static int point_index(const char *point)
{
    int i;
    if (!point) return -1;
    for (i = 0; i < s_mnt_n; i++)
        if (strcmp(point, s_mnt[i].pub.point) == 0) return i;
    return -1;
}

eos_err_t eos_storage_mount(const char *point)
{
    int i = point_index(point);
    if (!s_inited) return fail(EOS_ERR_STATE);
    if (i < 0) return fail(EOS_ERR_NOTFOUND);
    {
        eos_err_t e = mount_one(&s_mnt[i]);
        return e == EOS_OK ? EOS_OK : fail(e);
    }
}

eos_err_t eos_storage_unmount(const char *point)
{
    int i = point_index(point);
    if (!s_inited) return fail(EOS_ERR_STATE);
    if (i < 0) return fail(EOS_ERR_NOTFOUND);
    {
        eos_err_t e = unmount_one(&s_mnt[i]);
        return e == EOS_OK ? EOS_OK : fail(e);
    }
}

eos_err_t eos_storage_usage(const char *point, uint64_t *total, uint64_t *used)
{
    int i = point_index(point);

    if (total) *total = 0;
    if (used)  *used  = 0;
    if (!s_inited) return fail(EOS_ERR_STATE);
    if (i < 0) return fail(EOS_ERR_NOTFOUND);
    if (!s_mnt[i].pub.mounted) return fail(EOS_ERR_NODEV);

#ifdef ESP_PLATFORM
    if (s_mnt[i].pub.fs == EOS_FS_LITTLEFS) {
        size_t t = 0, u = 0;
        // Walks LittleFS's block allocation, so it is not free — this is the
        // reason it is a separate call from eos_storage_mounts().
        if (esp_littlefs_info(s_mnt[i].label, &t, &u) != ESP_OK) return fail(EOS_ERR_IO);
        s_mnt[i].pub.total = (uint64_t)t;
        s_mnt[i].pub.used  = (uint64_t)u;
    }
#else
    // The host mount is a directory on somebody else's filesystem. Its size is
    // not this filesystem's size, and the header already says 0 means "the
    // filesystem will not say" rather than "empty".
    s_mnt[i].pub.total = 0;
    s_mnt[i].pub.used  = 0;
#endif

    if (total) *total = s_mnt[i].pub.total;
    if (used)  *used  = s_mnt[i].pub.used;
    return EOS_OK;
}

// ------------------------------------------------------------- the pools

static bool file_valid(const eos_file_t *f)
{
    return f && f >= &s_files[0] && f < &s_files[EOS_MAX_FILES] &&
           ((size_t)((const char *)f - (const char *)s_files) % sizeof s_files[0]) == 0;
}

static bool dir_valid(const eos_dirh_t *d)
{
    return d && d >= &s_dirs[0] && d < &s_dirs[EOS_MAX_DIRS] &&
           ((size_t)((const char *)d - (const char *)s_dirs) % sizeof s_dirs[0]) == 0;
}

eos_file_t *eos_storage_open(const char *path, uint8_t mode)
{
    char sys[SYS_MAX], rel[EOS_PATH_MAX];
    eos_err_t e;
    int mi, flags, fd, i;
    eos_file_t *f = NULL;

    if ((mode & (EOS_O_READ | EOS_O_WRITE)) == 0) { fail(EOS_ERR_ARG); return NULL; }
    e = resolve(path, &mi, sys, (int)sizeof sys, rel);
    if (e != EOS_OK) { fail(e); return NULL; }
    if (mi < 0 || rel[0] == '\0') { fail(EOS_ERR_ARG); return NULL; }  // "/" and "/int"
    if ((mode & EOS_O_WRITE) && !s_mnt[mi].pub.writable) { fail(EOS_ERR_READONLY); return NULL; }

    if ((mode & EOS_O_WRITE) && (mode & EOS_O_READ)) flags = O_RDWR;
    else if (mode & EOS_O_WRITE)                     flags = O_WRONLY;
    else                                             flags = O_RDONLY;
    if (mode & EOS_O_CREATE) flags |= O_CREAT;
    if (mode & EOS_O_TRUNC)  flags |= O_TRUNC;
    if (mode & EOS_O_APPEND) flags |= O_APPEND;

    LOCK();
    for (i = 0; i < EOS_MAX_FILES; i++) {
        if (s_files[i].used) continue;
        s_files[i].used = true;          // claimed before the media call, so two
        s_files[i].fd   = -1;            // workers cannot land on the same slot
        f = &s_files[i];
        break;
    }
    UNLOCK();
    if (!f) { fail(EOS_ERR_POOL); return NULL; }

    fd = open(sys, flags, 0666);
    if (fd < 0) {
        eos_err_t why = from_errno(errno);
        LOCK(); f->used = false; UNLOCK();
        fail(why);
        return NULL;
    }

    f->fd   = fd;
    f->mnt  = (int8_t)mi;
    f->mode = mode;
    LOCK(); s_mnt[mi].held++; UNLOCK();
    return f;
}

eos_err_t eos_storage_close(eos_file_t *f)
{
    int fd;

    if (!file_valid(f)) return fail(EOS_ERR_ARG);
    // A second close on a slot that has already been returned is the bug that
    // hands one worker another worker's file. It is refused, loudly.
    if (!f->used) return fail(EOS_ERR_STATE);

    fd = f->fd;
    LOCK();
    if (f->mnt >= 0 && f->mnt < s_mnt_n && s_mnt[f->mnt].held) s_mnt[f->mnt].held--;
    f->used = false;
    f->fd   = -1;
    f->mnt  = -1;
    UNLOCK();

    if (fd >= 0 && close(fd) != 0) return fail(from_errno(errno));
    return EOS_OK;
}

int eos_storage_read(eos_file_t *f, void *buf, int n)
{
    ssize_t got;

    if (!file_valid(f) || !f->used || !buf) return (int)fail(EOS_ERR_ARG);
    if (!(f->mode & EOS_O_READ)) return (int)fail(EOS_ERR_ARG);
    if (n < 0) return (int)fail(EOS_ERR_ARG);
    if (n == 0) return 0;

    got = read(f->fd, buf, (size_t)n);
    if (got < 0) return (int)fail(from_errno(errno));
    return (int)got;
}

int eos_storage_write(eos_file_t *f, const void *buf, int n)
{
    ssize_t put;

    if (!file_valid(f) || !f->used || !buf) return (int)fail(EOS_ERR_ARG);
    if (!(f->mode & EOS_O_WRITE)) return (int)fail(EOS_ERR_READONLY);
    if (n < 0) return (int)fail(EOS_ERR_ARG);
    if (n == 0) return 0;

    // One write, exactly what was asked for, and back to the caller. This is
    // where the chip stalls; looping here to satisfy a large request would
    // hold the frame loop for as long as the request was big.
    put = write(f->fd, buf, (size_t)n);
    if (put < 0) return (int)fail(from_errno(errno));
    return (int)put;
}

int64_t eos_storage_seek(eos_file_t *f, int64_t off, uint8_t whence)
{
    int w;
    off_t r;

    if (!file_valid(f) || !f->used) return fail(EOS_ERR_ARG);
    switch (whence) {
    case EOS_SEEK_SET: w = SEEK_SET; break;
    case EOS_SEEK_CUR: w = SEEK_CUR; break;
    case EOS_SEEK_END: w = SEEK_END; break;
    default: return fail(EOS_ERR_ARG);
    }
    r = lseek(f->fd, (off_t)off, w);
    if (r < 0) return fail(from_errno(errno));
    return (int64_t)r;
}

int64_t eos_storage_tell(eos_file_t *f)
{
    off_t r;
    if (!file_valid(f) || !f->used) return fail(EOS_ERR_ARG);
    r = lseek(f->fd, 0, SEEK_CUR);
    if (r < 0) return fail(from_errno(errno));
    return (int64_t)r;
}

int64_t eos_storage_size(eos_file_t *f)
{
    struct stat st;
    // fstat and not a pair of seeks: the cursor is the caller's and a size
    // query that moves it is a bug that only shows up under a partial read.
    if (!file_valid(f) || !f->used) return fail(EOS_ERR_ARG);
    if (fstat(f->fd, &st) != 0) return fail(from_errno(errno));
    return (int64_t)st.st_size;
}

eos_err_t eos_storage_sync(eos_file_t *f)
{
    if (!file_valid(f) || !f->used) return fail(EOS_ERR_ARG);
    if (!(f->mode & EOS_O_WRITE)) return EOS_OK;
    // The expensive call in this file. It is what turns a cached write into
    // erased and programmed sectors, and it is why eos_storage_save() is the
    // shape it is: one open, one write, one sync, rather than a sync per line.
    if (fsync(f->fd) != 0) return fail(from_errno(errno));
    return EOS_OK;
}

// ------------------------------------------------------------- metadata

eos_err_t eos_storage_stat(const char *path, eos_stat_t *out)
{
    char sys[SYS_MAX], rel[EOS_PATH_MAX];
    struct stat st;
    eos_err_t e;
    int mi;

    if (!out) return fail(EOS_ERR_ARG);
    out->size = 0; out->mtime = 0; out->is_dir = false;

    e = resolve(path, &mi, sys, (int)sizeof sys, rel);
    if (e != EOS_OK) return fail(e);

    // "/" and a mount point itself are directories this file knows about
    // without asking the media, which also keeps the VFS out of the "" path
    // it does not handle.
    if (mi < 0 || rel[0] == '\0') { out->is_dir = true; return EOS_OK; }

    if (stat(sys, &st) != 0) return fail(from_errno(errno));
    out->is_dir = (st.st_mode & S_IFMT) == S_IFDIR;
    out->size   = out->is_dir ? 0u : (uint32_t)st.st_size;
    out->mtime  = (uint32_t)st.st_mtime;   // 0 where the filesystem keeps none
    return EOS_OK;
}

eos_err_t eos_storage_remove(const char *path)
{
    char sys[SYS_MAX], rel[EOS_PATH_MAX];
    struct stat st;
    eos_err_t e;
    int mi;

    e = resolve(path, &mi, sys, (int)sizeof sys, rel);
    if (e != EOS_OK) return fail(e);
    if (mi < 0 || rel[0] == '\0') return fail(EOS_ERR_ARG);   // a mount is not a file
    if (!s_mnt[mi].pub.writable) return fail(EOS_ERR_READONLY);

    if (stat(sys, &st) != 0) return fail(from_errno(errno));
    // Directories go through rmdir, which refuses a non-empty one. Nothing
    // here walks a tree: the web contract says a non-empty directory is a 409
    // and the board never recursively deletes on somebody's behalf.
    if ((st.st_mode & S_IFMT) == S_IFDIR) {
        if (rmdir(sys) != 0) return fail(from_errno(errno));
        return EOS_OK;
    }
    if (unlink(sys) != 0) return fail(from_errno(errno));
    return EOS_OK;
}

eos_err_t eos_storage_rename(const char *from, const char *to)
{
    char a[SYS_MAX], b[SYS_MAX], ra[EOS_PATH_MAX], rb[EOS_PATH_MAX];
    eos_err_t e;
    int ma, mb;

    e = resolve(from, &ma, a, (int)sizeof a, ra);
    if (e != EOS_OK) return fail(e);
    e = resolve(to, &mb, b, (int)sizeof b, rb);
    if (e != EOS_OK) return fail(e);
    if (ma < 0 || mb < 0 || ra[0] == '\0' || rb[0] == '\0') return fail(EOS_ERR_ARG);
    // Same mount only. A cross-mount move is a copy and a delete, it is not
    // atomic, and it is the caller's to decide — not something this hides.
    if (ma != mb) return fail(EOS_ERR_UNSUPPORTED);
    if (!s_mnt[ma].pub.writable) return fail(EOS_ERR_READONLY);

    if (rename(a, b) != 0) return fail(from_errno(errno));
    return EOS_OK;
}

eos_err_t eos_storage_mkdir(const char *path)
{
    char sys[SYS_MAX], rel[EOS_PATH_MAX];
    eos_err_t e;
    int mi;

    e = resolve(path, &mi, sys, (int)sizeof sys, rel);
    if (e != EOS_OK) return fail(e);
    if (mi < 0 || rel[0] == '\0') return fail(EOS_ERR_EXISTS);   // the mount is already there
    if (!s_mnt[mi].pub.writable) return fail(EOS_ERR_READONLY);

    if (mkdir(sys, 0777) != 0) return fail(from_errno(errno));
    return EOS_OK;
}

// ----------------------------------------------------------- directories

eos_dirh_t *eos_storage_opendir(const char *path)
{
    char sys[SYS_MAX], rel[EOS_PATH_MAX];
    eos_err_t e;
    int mi, i;
    eos_dirh_t *h = NULL;
    DIR *d = NULL;

    e = resolve(path, &mi, sys, (int)sizeof sys, rel);
    if (e != EOS_OK) { fail(e); return NULL; }

    if (mi >= 0) {
        d = opendir(sys);
        if (!d) { fail(from_errno(errno)); return NULL; }
    }

    LOCK();
    for (i = 0; i < EOS_MAX_DIRS; i++) {
        if (s_dirs[i].used) continue;
        s_dirs[i].used = true;
        h = &s_dirs[i];
        break;
    }
    UNLOCK();
    if (!h) {
        if (d) closedir(d);
        fail(EOS_ERR_POOL);
        return NULL;
    }

    h->mnt = (int8_t)mi;
    h->idx = 0;
    h->d   = d;
    h->base[0] = '\0';
    if (mi >= 0) {
        memcpy(h->base, sys, strlen(sys) + 1);
        LOCK(); s_mnt[mi].held++; UNLOCK();
    }
    return h;
}

bool eos_storage_readdir(eos_dirh_t *h, eos_dirent_t *out)
{
    if (!dir_valid(h) || !h->used || !out) { fail(EOS_ERR_ARG); return false; }

    memset(out, 0, sizeof *out);

    // The root pseudo-directory. Only mounted mounts are listed: a file
    // browser that can see "/sd" should be able to enter it, and the fact that
    // the slot exists at all belongs in eos_storage_mounts(), which reports
    // every declared mount whether it came up or not.
    if (h->mnt < 0) {
        while (h->idx < s_mnt_n) {
            const mnt_t *m = &s_mnt[h->idx++];
            const char *name = m->pub.point + 1;
            if (!m->pub.mounted) continue;
            if (strlen(name) >= EOS_NAME_MAX) continue;
            memcpy(out->name, name, strlen(name) + 1);
            out->is_dir = true;
            return true;
        }
        return false;
    }

    for (;;) {
        struct dirent *de = readdir(h->d);
        struct stat st;
        char p[SYS_MAX];
        size_t len;

        if (!de) return false;
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;

        len = strlen(de->d_name);
        // A name this cannot carry is skipped rather than shortened. A
        // shortened name is a name for a different file, and the browser would
        // hand it straight back to remove().
        if (len >= EOS_NAME_MAX) continue;
        memcpy(out->name, de->d_name, len + 1);

#ifdef DT_DIR
        if (de->d_type == DT_DIR) { out->is_dir = true; return true; }
#endif
        // Size costs one stat per entry, which is the whole reason a listing
        // is paged. A stat that fails reports zero rather than losing the
        // entry: the file is there, its length is what could not be read.
        if (sys_join(h->base, de->d_name, p, (int)sizeof p) == EOS_OK &&
            stat(p, &st) == 0) {
            out->is_dir = (st.st_mode & S_IFMT) == S_IFDIR;
            out->size   = out->is_dir ? 0u : (uint32_t)st.st_size;
        }
        return true;
    }
}

eos_err_t eos_storage_closedir(eos_dirh_t *h)
{
    DIR *d;

    if (!dir_valid(h)) return fail(EOS_ERR_ARG);
    if (!h->used) return fail(EOS_ERR_STATE);

    d = h->d;
    LOCK();
    if (h->mnt >= 0 && h->mnt < s_mnt_n && s_mnt[h->mnt].held) s_mnt[h->mnt].held--;
    h->used = false;
    h->d    = NULL;
    h->mnt  = -1;
    UNLOCK();

    if (d && closedir(d) != 0) return fail(from_errno(errno));
    return EOS_OK;
}

// ------------------------------------------------------------ host hooks
//
// Not compiled into a firmware image. path_split() is the security boundary of
// this whole file and it answers questions no public call can be asked
// directly — what a path normalised to, which mount it chose — so the host
// suite gets at it here rather than inferring it from an open() that failed.

#ifndef ESP_PLATFORM
int eos_storage_host_split(const char *path, int *mount_out, char *rel, int rel_cap)
{
    return (int)path_split(path, mount_out, rel, rel_cap);
}

const char *eos_storage_host_root(void) { return s_host_root; }

void eos_storage_host_reset(void)
{
    int i;
    for (i = 0; i < EOS_MAX_FILES; i++) if (s_files[i].used && s_files[i].fd >= 0) close(s_files[i].fd);
    for (i = 0; i < EOS_MAX_DIRS; i++)  if (s_dirs[i].used && s_dirs[i].d) closedir(s_dirs[i].d);
    memset(s_files, 0, sizeof s_files);
    memset(s_dirs, 0, sizeof s_dirs);
    memset(s_mnt, 0, sizeof s_mnt);
    s_mnt_n  = 0;
    s_inited = false;
    s_err    = EOS_OK;
}
#endif
