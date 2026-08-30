// eos_storage — one namespace over the microSD and the internal LittleFS.
//
// An app opens "/sd/buddy/buddy.vox" or "/int/settings.json" and never learns
// which device answered. The first path component selects the mount; the rest
// is handed to whatever filesystem is mounted there. Two mounts is all this
// needs to be, and pretending otherwise would cost RAM nobody has.
//
// The one non-obvious constraint: the SD card is removable and the internal
// flash is not, so "/sd" can be absent at boot, appear later, and vanish
// mid-write, while "/int" is either there or the board is broken. Every call
// that touches "/sd" can therefore fail with EOS_ERR_NODEV at any time, and
// code that treats storage as always-present will lose data the first time
// somebody pulls the card. Check the mount, or check the return.
//
// Handles come from fixed pools — EOS_MAX_FILES files and EOS_MAX_DIRS
// directories per image, no allocation anywhere. Every call in this header may
// block; none of them are safe from an ISR.

#ifndef EOS_STORAGE_H
#define EOS_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   // NULL, which the handle calls return
#include "eos_board.h"

#define EOS_PATH_MAX   96   // "/sd" + a sane tree. Sized for a 20KB heap, not for POSIX.
#define EOS_NAME_MAX   40   // one path component, FAT long names included
#define EOS_MOUNT_MAX   2   // "/sd" and "/int"
#define EOS_MAX_FILES   4   // concurrently open files across the whole OS
#define EOS_MAX_DIRS    2   // concurrently open directory scans

// ------------------------------------------------------------------ mounts

typedef enum {
    EOS_FS_NONE = 0,
    EOS_FS_LITTLEFS,   // internal flash partition
    EOS_FS_FAT,        // microSD, over SPI or SDMMC
} eos_fs_t;

typedef struct {
    const char *point;      // "/sd" or "/int", no trailing slash
    uint8_t     fs;         // eos_fs_t
    bool        mounted;
    bool        writable;
    bool        removable;  // true for the card; changes how failures are reported
    uint64_t    total;      // bytes, 0 when the filesystem will not say
    uint64_t    used;
} eos_mount_t;

// Mounts everything the board declares. A missing or unformatted SD card is
// NOT an error here: it comes back as a present-but-unmounted mount entry, and
// eos_storage_mount() can be retried later when the user inserts one. Returns
// EOS_ERR_IO only when the internal filesystem failed, which is fatal.
eos_err_t eos_storage_init(void);

// Copies out up to max mount records, returns how many exist. Always reports
// every declared mount, mounted or not.
int eos_storage_mounts(eos_mount_t *out, int max);

// The mount a path resolves to, or NULL when the first component names no
// mount. Does not touch the media, so it is cheap enough to call per open.
const eos_mount_t *eos_storage_mount_for(const char *path);

// Mount or unmount by point. This is the card-insert and card-eject path.
// Unmounting a point with files still open returns EOS_ERR_BUSY.
eos_err_t eos_storage_mount(const char *point);
eos_err_t eos_storage_unmount(const char *point);

// Refreshes total/used for a mount. Separate from eos_storage_mounts() because
// on FAT this walks the allocation table and is slow enough to notice.
eos_err_t eos_storage_usage(const char *point, uint64_t *total, uint64_t *used);

// ------------------------------------------------------------------- files

#define EOS_O_READ   0x01
#define EOS_O_WRITE  0x02
#define EOS_O_CREATE 0x04   // create when missing
#define EOS_O_TRUNC  0x08   // truncate to zero on open
#define EOS_O_APPEND 0x10   // every write goes to the end

#define EOS_SEEK_SET 0
#define EOS_SEEK_CUR 1
#define EOS_SEEK_END 2

// Opaque, from the fixed pool. Never freed by the caller in any sense other
// than eos_storage_close(), which returns the slot.
typedef struct eos_file eos_file_t;
typedef struct eos_dirh eos_dirh_t;   // eos_dir_t is the window manager's direction enum

// NULL on any failure, including a full handle pool. Call eos_storage_errno()
// for which.
eos_file_t *eos_storage_open(const char *path, uint8_t mode);
eos_err_t   eos_storage_close(eos_file_t *f);

// Bytes moved, 0 at end of file, negative eos_err_t on failure. Short reads
// and short writes are normal and are not errors.
int eos_storage_read(eos_file_t *f, void *buf, int n);
int eos_storage_write(eos_file_t *f, const void *buf, int n);

int64_t   eos_storage_seek(eos_file_t *f, int64_t off, uint8_t whence);
int64_t   eos_storage_tell(eos_file_t *f);
int64_t   eos_storage_size(eos_file_t *f);
eos_err_t eos_storage_sync(eos_file_t *f);   // pushes to the media; may block a while

// Why the last NULL-returning or negative call failed, on this task.
eos_err_t eos_storage_errno(void);

// --------------------------------------------------------------- metadata

typedef struct {
    uint32_t size;
    uint32_t mtime;    // unix seconds, 0 when the filesystem does not keep one
    bool     is_dir;
} eos_stat_t;

eos_err_t eos_storage_stat(const char *path, eos_stat_t *out);
eos_err_t eos_storage_remove(const char *path);      // files and empty directories
eos_err_t eos_storage_rename(const char *from, const char *to);  // same mount only
eos_err_t eos_storage_mkdir(const char *path);       // one level; parents must exist

static inline bool eos_storage_exists(const char *path)
{
    eos_stat_t st;
    return eos_storage_stat(path, &st) == EOS_OK;
}

// ------------------------------------------------------------- directories

typedef struct {
    char     name[EOS_NAME_MAX];   // component only, never a full path
    uint32_t size;
    bool     is_dir;
} eos_dirent_t;

eos_dirh_t *eos_storage_opendir(const char *path);
bool        eos_storage_readdir(eos_dirh_t *d, eos_dirent_t *out);  // false at the end
eos_err_t   eos_storage_closedir(eos_dirh_t *d);

// Listing the root, "/", enumerates the mounts themselves as directories, so a
// file browser needs no special case for the top level.

// ---------------------------------------------------- whole-file shortcuts
//
// Settings, save files and small assets are read and written whole. These are
// written in terms of the calls above so a backend implements the primitives
// only, and they take a caller-owned buffer because nothing here allocates.

// Bytes read, or a negative eos_err_t. EOS_ERR_TOOBIG when the file does not
// fit in max — nothing is copied in that case, so a caller can size a buffer
// from eos_storage_stat() and retry.
static inline int eos_storage_load(const char *path, void *buf, int max)
{
    eos_stat_t st;
    eos_err_t e = eos_storage_stat(path, &st);
    if (e != EOS_OK) return (int)e;
    if (st.is_dir) return (int)EOS_ERR_ARG;
    if ((int64_t)st.size > (int64_t)max) return (int)EOS_ERR_TOOBIG;

    eos_file_t *f = eos_storage_open(path, EOS_O_READ);
    if (!f) return (int)eos_storage_errno();

    int got = 0;
    while (got < (int)st.size) {
        int n = eos_storage_read(f, (char *)buf + got, (int)st.size - got);
        if (n < 0)  { eos_storage_close(f); return n; }
        if (n == 0) break;
        got += n;
    }
    eos_storage_close(f);
    return got;
}

// Truncating write of a whole file. Syncs before closing, because the common
// caller is settings and the common failure is a power cut.
static inline eos_err_t eos_storage_save(const char *path, const void *buf, int n)
{
    if (n < 0) return EOS_ERR_ARG;
    eos_file_t *f = eos_storage_open(path, EOS_O_WRITE | EOS_O_CREATE | EOS_O_TRUNC);
    if (!f) return eos_storage_errno();

    int put = 0;
    while (put < n) {
        int w = eos_storage_write(f, (const char *)buf + put, n - put);
        if (w <= 0) { eos_storage_close(f); return w < 0 ? (eos_err_t)w : EOS_ERR_IO; }
        put += w;
    }
    eos_err_t e = eos_storage_sync(f);
    eos_err_t c = eos_storage_close(f);
    return e != EOS_OK ? e : c;
}

// Reads a file and NUL-terminates it, for the JSON and config paths. Returns
// the length written, not counting the terminator.
static inline int eos_storage_load_str(const char *path, char *buf, int max)
{
    if (max < 1) return (int)EOS_ERR_ARG;
    int n = eos_storage_load(path, buf, max - 1);
    if (n < 0) return n;
    buf[n] = '\0';
    return n;
}

// ------------------------------------------------------------ path helper
//
// Joins into a caller-owned buffer with exactly one separator. Returns the
// length, or EOS_ERR_TOOBIG when it would not fit — it never truncates
// silently, because a truncated path names a different file.
static inline int eos_path_join(char *out, int max, const char *dir, const char *leaf)
{
    if (!out || max <= 0 || !dir || !leaf) return (int)EOS_ERR_ARG;
    int i = 0;
    while (dir[i] && i < max - 1) { out[i] = dir[i]; i++; }
    if (dir[i]) return (int)EOS_ERR_TOOBIG;
    while (i > 0 && out[i - 1] == '/') i--;          // drop trailing separators
    while (*leaf == '/') leaf++;                     // and leading ones
    if (i >= max - 1) return (int)EOS_ERR_TOOBIG;
    out[i++] = '/';
    int j = 0;
    while (leaf[j] && i < max - 1) out[i++] = leaf[j++];
    if (leaf[j]) return (int)EOS_ERR_TOOBIG;
    out[i] = '\0';
    return i;
}

// Points at the last component of a path, or at the whole thing when there is
// no separator. Never copies.
static inline const char *eos_path_leaf(const char *path)
{
    const char *p = path, *last = path;
    for (; *p; p++) if (*p == '/') last = p + 1;
    return last;
}

// Points at the extension including the dot, or at the terminating NUL when
// the leaf has none. Never copies.
static inline const char *eos_path_ext(const char *path)
{
    const char *leaf = eos_path_leaf(path), *p = leaf, *dot = NULL;
    for (; *p; p++) if (*p == '.') dot = p;
    return dot ? dot : p;
}

#endif // EOS_STORAGE_H
