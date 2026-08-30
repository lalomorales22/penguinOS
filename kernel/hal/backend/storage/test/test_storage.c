// Host checks for the storage backend. Everything here except the LittleFS
// mount itself is the same code that runs on the board: the path rules, the
// two handle pools, the mount routing and every metadata call are portable
// POSIX, and on the host "/int" is a directory in /tmp instead of a partition.
//
// The half that matters most is adversarial. A path reaching eos_storage came
// out of an HTTP query string, so the traversal block below is not a
// formality — it is the check that says a phone on the same WiFi cannot read
// the NVS partition by asking for it politely.
//
// What this cannot check: the flash. A green run says the routing and the
// pools are right, not that LittleFS mounted.

#include "eos_storage.h"
#include "waveshare-c6-lcd-13.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const eos_board_t *eos_board_get(void) { return &EOS_BOARD; }

// Host-only hooks out of the backend. Declared here rather than in a header
// because they exist for this file and are not built into a firmware image.
int         eos_storage_host_split(const char *path, int *mount_out, char *rel, int rel_cap);
const char *eos_storage_host_root(void);
void        eos_storage_host_reset(void);

static int checks = 0, failed = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failed++; printf("FAIL: %s\n", what); }
}

static void eq(long got, long want, const char *what)
{
    checks++;
    if (got != want) { failed++; printf("FAIL: %s: got %ld want %ld\n", what, got, want); }
}

static void eqs(const char *got, const char *want, const char *what)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL: %s: got \"%s\" want \"%s\"\n", what, got ? got : "(null)", want);
    }
}

// ------------------------------------------------------------- the sandbox

static char ROOT[128];

static void rmtree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;
    char p[512];

    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", path, de->d_name);
        rmtree(p);
        unlink(p);
        rmdir(p);
    }
    closedir(d);
    rmdir(path);
}

static void sandbox_open(void)
{
    snprintf(ROOT, sizeof ROOT, "/tmp/eos-storage-test-%d", (int)getpid());
    rmtree(ROOT);
    setenv("EOS_STORAGE_HOST_ROOT", ROOT, 1);
    eos_storage_host_reset();
    eq(eos_storage_init(), EOS_OK, "init mounts the internal filesystem");
}

// ------------------------------------------------------------ path rules

static eos_err_t split(const char *path, int *mi, char *rel, int cap)
{
    return (eos_err_t)eos_storage_host_split(path, mi, rel, cap);
}

static void t_split_shapes(void)
{
    char rel[EOS_PATH_MAX];
    int mi;

    eq(split("/int/a/b", &mi, rel, sizeof rel), EOS_OK, "plain path splits");
    eq(mi, 0, "  and lands on mount 0");
    eqs(rel, "a/b", "  remainder is the path below the mount");

    eq(split("/int", &mi, rel, sizeof rel), EOS_OK, "a bare mount point splits");
    eqs(rel, "", "  with an empty remainder");

    eq(split("/int/", &mi, rel, sizeof rel), EOS_OK, "a trailing slash is not a component");
    eqs(rel, "", "  and leaves the remainder empty");

    eq(split("/int//a///b/", &mi, rel, sizeof rel), EOS_OK, "repeated separators collapse");
    eqs(rel, "a/b", "  to exactly one each");

    eq(split("/int/./a/./b", &mi, rel, sizeof rel), EOS_OK, "\".\" is this directory");
    eqs(rel, "a/b", "  and drops out of the remainder");

    eq(split("/", &mi, rel, sizeof rel), EOS_OK, "\"/\" is a legal path");
    eq(mi, -1, "  and names no mount");
    eqs(rel, "", "  with an empty remainder");

    eq(split("/sd/x", &mi, rel, sizeof rel), EOS_OK, "/sd routes even though nothing is there");
    eq(mi, 1, "  to mount 1");
    eqs(rel, "x", "  with the right remainder");

    // Bytes above 0x7f are filenames, not attacks. Rejecting them would be
    // this layer lying about what LittleFS can store.
    eq(split("/int/caf\xc3\xa9.txt", &mi, rel, sizeof rel), EOS_OK, "utf-8 name accepted");
    eqs(rel, "caf\xc3\xa9.txt", "  byte for byte");
    eq(split("/int/\xff\xfe", &mi, rel, sizeof rel), EOS_OK,
       "invalid utf-8 accepted: bytes are the filesystem's business");

    // Percent escapes are eos_httpd's to decode. By the time a path is here a
    // literal "%2e%2e" is a filename and must not be re-decoded into "..".
    eq(split("/int/%2e%2e", &mi, rel, sizeof rel), EOS_OK, "a literal %2e%2e is a name");
    eqs(rel, "%2e%2e", "  and is not decoded a second time");

    eq(split("/int/...", &mi, rel, sizeof rel), EOS_OK, "\"...\" is a legal name");
    eq(split("/int/..a", &mi, rel, sizeof rel), EOS_OK, "\"..a\" is a legal name");
    eq(split("/int/a..", &mi, rel, sizeof rel), EOS_OK, "\"a..\" is a legal name");
}

static void t_split_traversal(void)
{
    char rel[EOS_PATH_MAX];
    int mi;

    // The whole point of the file. Every one of these has been a CVE in
    // somebody's embedded web server.
    eq(split("..", &mi, rel, sizeof rel), EOS_ERR_ARG, "bare \"..\" refused");
    eq(split("../", &mi, rel, sizeof rel), EOS_ERR_ARG, "\"../\" refused");
    eq(split("../../etc/passwd", &mi, rel, sizeof rel), EOS_ERR_ARG, "relative climb refused");
    eq(split("/..", &mi, rel, sizeof rel), EOS_ERR_ARG, "\"/..\" refused");
    eq(split("/../..", &mi, rel, sizeof rel), EOS_ERR_ARG, "\"/../..\" refused");
    eq(split("/int/..", &mi, rel, sizeof rel), EOS_ERR_ARG, "climbing out of a mount refused");
    eq(split("/int/../int/x", &mi, rel, sizeof rel), EOS_ERR_ARG,
       "climbing out and back in refused, not folded");
    eq(split("/int/a/../../../x", &mi, rel, sizeof rel), EOS_ERR_ARG, "deep climb refused");
    eq(split("/int/./../x", &mi, rel, sizeof rel), EOS_ERR_ARG, "\".\" then \"..\" refused");
    eq(split("//../int", &mi, rel, sizeof rel), EOS_ERR_ARG, "collapsed separators still see \"..\"");
    eq(split("/int/a/..", &mi, rel, sizeof rel), EOS_ERR_ARG, "trailing \"..\" refused");

    // FatFs accepts '\' as a separator. No card is mounted today; this is the
    // rule that means finding the pins is a profile edit and not a hole.
    eq(split("/int/a\\..\\b", &mi, rel, sizeof rel), EOS_ERR_ARG, "backslash refused");
    eq(split("/int/\\", &mi, rel, sizeof rel), EOS_ERR_ARG, "a lone backslash refused");

    // Control bytes cannot be typed and are how a log line gets forged.
    eq(split("/int/a\x01""b", &mi, rel, sizeof rel), EOS_ERR_ARG, "control byte refused");
    eq(split("/int/a\nb", &mi, rel, sizeof rel), EOS_ERR_ARG, "newline refused");
    eq(split("/int/a\x7f", &mi, rel, sizeof rel), EOS_ERR_ARG, "DEL refused");

    // An absolute path that is not under a mount names nothing.
    eq(split("/etc/passwd", &mi, rel, sizeof rel), EOS_ERR_NOTFOUND, "an unknown mount is not found");
    eq(split("/INT/x", &mi, rel, sizeof rel), EOS_ERR_NOTFOUND, "mount names are case sensitive");
    eq(split("/inte/x", &mi, rel, sizeof rel), EOS_ERR_NOTFOUND, "a longer mount name is not a prefix match");
    eq(split("/in/x", &mi, rel, sizeof rel), EOS_ERR_NOTFOUND, "a shorter one is not either");

    // Relative paths have no mount and are refused outright rather than being
    // hung off whatever the last one was.
    eq(split("int/x", &mi, rel, sizeof rel), EOS_ERR_ARG, "a relative path is refused");
    eq(split("", &mi, rel, sizeof rel), EOS_ERR_ARG, "an empty path is refused");
    eq(split(NULL, &mi, rel, sizeof rel), EOS_ERR_ARG, "a NULL path is refused");
    eq(split("/int/x", &mi, NULL, sizeof rel), EOS_ERR_ARG, "a NULL output buffer is refused");
}

static void t_split_lengths(void)
{
    char rel[EOS_PATH_MAX];
    char big[EOS_PATH_MAX + 64];
    int mi, i;

    // Exactly at the limit, and one past it. EOS_PATH_MAX counts the
    // terminator, so 95 bytes is the longest path there is.
    memcpy(big, "/int/", 5);
    for (i = 5; i < EOS_PATH_MAX - 1; i++) big[i] = 'a';
    big[EOS_PATH_MAX - 1] = '\0';
    eq((long)strlen(big), EOS_PATH_MAX - 1, "the longest legal path is 95 bytes");
    eq(split(big, &mi, rel, sizeof rel), EOS_ERR_TOOBIG,
       "  ...but its single component is over EOS_NAME_MAX");

    // Same length, spread over components that each fit.
    for (i = 5; i < EOS_PATH_MAX - 1; i++) big[i] = (i % 10 == 0) ? '/' : 'a';
    big[EOS_PATH_MAX - 1] = '\0';
    eq(split(big, &mi, rel, sizeof rel), EOS_OK, "a 95-byte path in short components fits");

    big[EOS_PATH_MAX - 1] = 'a';
    big[EOS_PATH_MAX] = '\0';
    eq(split(big, &mi, rel, sizeof rel), EOS_ERR_TOOBIG, "96 bytes is too big");

    for (i = 0; i < (int)sizeof big - 1; i++) big[i] = (i == 0) ? '/' : 'a';
    big[sizeof big - 1] = '\0';
    eq(split(big, &mi, rel, sizeof rel), EOS_ERR_TOOBIG,
       "a path far past the buffer is refused, not read to its end");

    // One component longer than EOS_NAME_MAX.
    memcpy(big, "/int/", 5);
    for (i = 5; i < 5 + EOS_NAME_MAX; i++) big[i] = 'n';
    big[5 + EOS_NAME_MAX] = '\0';
    eq(split(big, &mi, rel, sizeof rel), EOS_ERR_TOOBIG, "a component over EOS_NAME_MAX is refused");

    // A remainder buffer smaller than the remainder is a refusal, never a
    // truncation: a truncated path names a different file.
    {
        char tiny[4];
        eq(split("/int/abcdef", &mi, tiny, (int)sizeof tiny), EOS_ERR_TOOBIG,
           "a remainder that does not fit is refused");
        eqs(tiny, "", "  and the buffer is left empty rather than half written");
    }
}

static void t_split_embedded_nul(void)
{
    // C strings end at the first NUL, so a path carrying one can only ever
    // name the part before it. eos_httpd_path_of() rejects a percent-decoded
    // NUL outright; this pins the layer below it, so that "a\0b" can never
    // become "b" and never reads past the terminator.
    static const char p[] = "/int/a\0b";
    char rel[EOS_PATH_MAX];
    int mi;

    eq(split(p, &mi, rel, sizeof rel), EOS_OK, "a path with an embedded NUL splits at the NUL");
    eqs(rel, "a", "  and never sees the bytes after it");
    eq((long)strlen(rel), 1, "  the remainder is one byte, not three");
}

// -------------------------------------------------------------- routing

static void t_routing(void)
{
    eos_mount_t m[EOS_MOUNT_MAX + 2];
    const eos_mount_t *p;
    int n;

    memset(m, 0, sizeof m);
    n = eos_storage_mounts(m, EOS_MOUNT_MAX + 2);
    eq(n, 2, "both mounts are declared");
    eqs(m[0].point, "/int", "mount 0 is the internal filesystem");
    ok(m[0].mounted, "  and it is mounted");
    ok(!m[0].removable, "  and it is not removable");
    eq(m[0].fs, EOS_FS_LITTLEFS, "  and it is littlefs");
    eqs(m[1].point, "/sd", "mount 1 is the card");
    ok(!m[1].mounted, "  which is not mounted");
    ok(m[1].removable, "  and is removable");
    eq(m[1].fs, EOS_FS_NONE, "  and has no filesystem: the board declares no pins");

    n = eos_storage_mounts(m, 1);
    eq(n, 2, "mounts() reports the real count even when it cannot copy them all");

    n = eos_storage_mounts(NULL, 0);
    eq(n, 2, "and answers with no buffer at all");

    p = eos_storage_mount_for("/int/settings.json");
    ok(p != NULL, "a path under /int finds a mount");
    if (p) eqs(p->point, "/int", "  and it is the right one");
    p = eos_storage_mount_for("/sd/web/app.js");
    ok(p != NULL, "a path under /sd finds a mount even unmounted");
    if (p) eqs(p->point, "/sd", "  and it is the card");
    ok(eos_storage_mount_for("/") == NULL, "\"/\" names no mount");
    ok(eos_storage_mount_for("/nope/x") == NULL, "an unknown first component names no mount");
    ok(eos_storage_mount_for("/int/../x") == NULL, "a traversal names no mount");
    ok(eos_storage_mount_for(NULL) == NULL, "NULL names no mount");
}

static void t_card_absent(void)
{
    eos_stat_t st;
    uint64_t total = 1, used = 1;

    // Every one of these has to be a clean answer. The failure this guards
    // against is a call that blocks on a bus with no card on it.
    ok(eos_storage_open("/sd/x", EOS_O_READ) == NULL, "open on the absent card fails");
    eq(eos_storage_errno(), EOS_ERR_NODEV, "  with NODEV");
    eq(eos_storage_stat("/sd/x", &st), EOS_ERR_NODEV, "stat on the absent card is NODEV");
    eq(eos_storage_mkdir("/sd/x"), EOS_ERR_NODEV, "mkdir on the absent card is NODEV");
    eq(eos_storage_remove("/sd/x"), EOS_ERR_NODEV, "remove on the absent card is NODEV");
    eq(eos_storage_rename("/sd/a", "/sd/b"), EOS_ERR_NODEV, "rename on the absent card is NODEV");
    ok(eos_storage_opendir("/sd") == NULL, "opendir on the absent card fails");
    eq(eos_storage_errno(), EOS_ERR_NODEV, "  with NODEV");
    eq(eos_storage_usage("/sd", &total, &used), EOS_ERR_NODEV, "usage on the absent card is NODEV");
    eq((long)total, 0, "  and reports nothing rather than stale numbers");
    eq((long)used, 0, "  for used as well");

    // The card-insert path. It answers, it does not hang, and it does not
    // pretend a slot with no known pins is a slot it can talk to.
    eq(eos_storage_mount("/sd"), EOS_ERR_NODEV, "mounting the absent card is NODEV");
    eq(eos_storage_unmount("/sd"), EOS_OK, "unmounting what was never mounted is fine");
    eq(eos_storage_mount("/nope"), EOS_ERR_NOTFOUND, "mounting an undeclared point is NOTFOUND");
    eq(eos_storage_unmount("/nope"), EOS_ERR_NOTFOUND, "unmounting one is too");
    eq(eos_storage_usage("/nope", NULL, NULL), EOS_ERR_NOTFOUND, "usage on one is too");
    eq(eos_storage_mount("/int/sub"), EOS_ERR_NOTFOUND,
       "a path inside a mount is not a mount point");
}

// ---------------------------------------------------------------- files

static void t_file_roundtrip(void)
{
    char buf[64];
    eos_file_t *f;
    eos_stat_t st;
    int n;

    eq(eos_storage_save("/int/hello.txt", "hello", 5), EOS_OK, "save writes a whole file");
    eq(eos_storage_stat("/int/hello.txt", &st), EOS_OK, "and it can be stat'd");
    eq(st.size, 5, "  with the right size");
    ok(!st.is_dir, "  and it is not a directory");
    ok(eos_storage_exists("/int/hello.txt"), "exists() agrees");
    ok(!eos_storage_exists("/int/nope.txt"), "and disagrees about a missing file");

    memset(buf, 0, sizeof buf);
    eq(eos_storage_load("/int/hello.txt", buf, sizeof buf), 5, "load reads it back");
    eqs(buf, "hello", "  byte for byte");

    memset(buf, 0, sizeof buf);
    eq(eos_storage_load_str("/int/hello.txt", buf, sizeof buf), 5, "load_str reads it back");
    eqs(buf, "hello", "  and terminates it");

    eq(eos_storage_load("/int/hello.txt", buf, 4), EOS_ERR_TOOBIG,
       "load refuses a buffer that cannot hold the file");
    eq(eos_storage_load("/int/nope.txt", buf, sizeof buf), EOS_ERR_NOTFOUND,
       "load of a missing file is NOTFOUND");

    // Truncating rewrite, which is what settings does on every change.
    eq(eos_storage_save("/int/hello.txt", "hi", 2), EOS_OK, "save truncates");
    eq(eos_storage_load("/int/hello.txt", buf, sizeof buf), 2, "  to the shorter length");

    f = eos_storage_open("/int/hello.txt", EOS_O_READ);
    ok(f != NULL, "open for reading");
    if (f) {
        eq((long)eos_storage_size(f), 2, "size is the file's");
        eq((long)eos_storage_tell(f), 0, "a fresh handle is at zero");
        n = eos_storage_read(f, buf, 1);
        eq(n, 1, "a short read is a read");
        eq((long)eos_storage_tell(f), 1, "and it moved the cursor");
        eq((long)eos_storage_size(f), 2, "size did not move the cursor");
        eq((long)eos_storage_tell(f), 1, "  really did not");
        eq((long)eos_storage_seek(f, 0, EOS_SEEK_END), 2, "seek to the end");
        eq(eos_storage_read(f, buf, 8), 0, "reading at the end is zero, not an error");
        eq((long)eos_storage_seek(f, 0, EOS_SEEK_SET), 0, "seek back to the start");
        eq((long)eos_storage_seek(f, 1, EOS_SEEK_CUR), 1, "relative seek");
        eq((long)eos_storage_seek(f, 0, 99), EOS_ERR_ARG, "an unknown whence is refused");
        eq(eos_storage_read(f, buf, 0), 0, "a zero-byte read moves nothing and is not an error");
        eq(eos_storage_read(f, buf, -1), EOS_ERR_ARG, "a negative count is a caller bug");
        eq(eos_storage_read(f, NULL, 1), EOS_ERR_ARG, "reading into NULL is refused");
        eq(eos_storage_write(f, "x", 1), EOS_ERR_READONLY, "writing a read handle is refused");
        eq(eos_storage_sync(f), EOS_OK, "syncing a read handle is a no-op, not an error");
        eq(eos_storage_close(f), EOS_OK, "close");
    }

    f = eos_storage_open("/int/append.txt", EOS_O_WRITE | EOS_O_CREATE);
    ok(f != NULL, "create for writing");
    if (f) {
        eq(eos_storage_write(f, "abc", 3), 3, "write");
        eq(eos_storage_write(f, "x", 0), 0, "a zero-byte write moves nothing and is not an error");
        eq(eos_storage_write(f, "x", -1), EOS_ERR_ARG, "a negative count is a caller bug");
        eq(eos_storage_write(f, NULL, 1), EOS_ERR_ARG, "writing from NULL is refused");
        eq(eos_storage_read(f, buf, 1), EOS_ERR_ARG, "reading a write handle is refused");
        eq(eos_storage_sync(f), EOS_OK, "sync");
        eq(eos_storage_close(f), EOS_OK, "close");
    }
    f = eos_storage_open("/int/append.txt", EOS_O_WRITE | EOS_O_APPEND);
    ok(f != NULL, "reopen for append");
    if (f) {
        eq(eos_storage_write(f, "de", 2), 2, "append writes at the end");
        eq(eos_storage_close(f), EOS_OK, "close");
    }
    memset(buf, 0, sizeof buf);
    eq(eos_storage_load("/int/append.txt", buf, sizeof buf), 5, "the appended file is longer");
    eqs(buf, "abcde", "  and holds both writes in order");

    ok(eos_storage_open("/int/nope.txt", EOS_O_READ) == NULL, "opening a missing file fails");
    eq(eos_storage_errno(), EOS_ERR_NOTFOUND, "  with NOTFOUND");
    ok(eos_storage_open("/int/hello.txt", 0) == NULL, "a mode with neither read nor write fails");
    eq(eos_storage_errno(), EOS_ERR_ARG, "  with ARG");
    ok(eos_storage_open("/int", EOS_O_READ) == NULL, "a mount point is not a file");
    eq(eos_storage_errno(), EOS_ERR_ARG, "  with ARG");
    ok(eos_storage_open("/", EOS_O_READ) == NULL, "\"/\" is not a file either");
    ok(eos_storage_open("/int/../x", EOS_O_WRITE | EOS_O_CREATE) == NULL,
       "a traversal cannot be opened for writing");
    eq(eos_storage_errno(), EOS_ERR_ARG, "  with ARG");

    // Writing whole files with a non-ASCII name, because the buddy and theme
    // files will eventually come off somebody's laptop.
    eq(eos_storage_save("/int/caf\xc3\xa9.txt", "x", 1), EOS_OK, "a utf-8 name round-trips");
    eq(eos_storage_load("/int/caf\xc3\xa9.txt", buf, sizeof buf), 1, "  and reads back");
}

static void t_metadata(void)
{
    eos_stat_t st;

    eq(eos_storage_mkdir("/int/dir"), EOS_OK, "mkdir");
    eq(eos_storage_stat("/int/dir", &st), EOS_OK, "the directory stats");
    ok(st.is_dir, "  as a directory");
    eq(st.size, 0, "  with no size");
    eq(eos_storage_mkdir("/int/dir"), EOS_ERR_EXISTS, "mkdir twice is EXISTS");
    eq(eos_storage_mkdir("/int/no/such/parent"), EOS_ERR_NOTFOUND,
       "mkdir is one level: a missing parent is NOTFOUND");
    eq(eos_storage_mkdir("/int"), EOS_ERR_EXISTS, "the mount point already exists");
    eq(eos_storage_mkdir("/"), EOS_ERR_EXISTS, "and so does the root");

    eq(eos_storage_stat("/", &st), EOS_OK, "the root stats");
    ok(st.is_dir, "  as a directory");
    eq(eos_storage_stat("/int", &st), EOS_OK, "a mount point stats");
    ok(st.is_dir, "  as a directory");
    eq(eos_storage_stat("/int/nope", &st), EOS_ERR_NOTFOUND, "a missing path is NOTFOUND");
    eq(eos_storage_stat("/int/x", NULL), EOS_ERR_ARG, "stat with no output is ARG");

    eq(eos_storage_save("/int/dir/a.txt", "aa", 2), EOS_OK, "a file inside the directory");
    eq(eos_storage_remove("/int/dir"), EOS_ERR_EXISTS,
       "removing a non-empty directory is EXISTS, never a recursive delete");
    ok(eos_storage_exists("/int/dir/a.txt"), "  and the file is still there");

    eq(eos_storage_rename("/int/dir/a.txt", "/int/dir/b.txt"), EOS_OK, "rename inside a mount");
    ok(!eos_storage_exists("/int/dir/a.txt"), "  the old name is gone");
    ok(eos_storage_exists("/int/dir/b.txt"), "  the new name is there");
    eq(eos_storage_rename("/int/dir/b.txt", "/sd/b.txt"), EOS_ERR_NODEV,
       "rename onto the absent card is NODEV");
    eq(eos_storage_rename("/int/nope", "/int/x"), EOS_ERR_NOTFOUND, "renaming a missing file");
    eq(eos_storage_rename("/int", "/int/x"), EOS_ERR_ARG, "renaming a mount point is ARG");
    eq(eos_storage_rename("/int/x", "/int/../x"), EOS_ERR_ARG, "renaming onto a traversal is ARG");

    eq(eos_storage_remove("/int/dir/b.txt"), EOS_OK, "remove a file");
    eq(eos_storage_remove("/int/dir"), EOS_OK, "remove the now-empty directory");
    eq(eos_storage_remove("/int/dir"), EOS_ERR_NOTFOUND, "removing it twice is NOTFOUND");
    eq(eos_storage_remove("/int"), EOS_ERR_ARG, "a mount cannot be removed");
    eq(eos_storage_remove("/"), EOS_ERR_ARG, "nor can the root");
}

// ---------------------------------------------------------------- pools

static void t_file_pool(void)
{
    eos_file_t *f[EOS_MAX_FILES + 1];
    // struct eos_file is opaque, so a handle that did not come from the pool
    // is one this test aims at it: correctly aligned, entirely foreign.
    static long long foreign_mem[16];
    eos_file_t *stackish = (eos_file_t *)foreign_mem;
    int i;

    for (i = 0; i < EOS_MAX_FILES; i++) {
        char p[32];
        snprintf(p, sizeof p, "/int/pool%d", i);
        f[i] = eos_storage_open(p, EOS_O_WRITE | EOS_O_CREATE | EOS_O_TRUNC);
        ok(f[i] != NULL, "the pool hands out a handle");
    }
    // EOS_MAX_FILES is four because esp_http_server runs four workers. One
    // more than that is a refusal with a name on it, not a crash.
    f[EOS_MAX_FILES] = eos_storage_open("/int/pool_over", EOS_O_WRITE | EOS_O_CREATE);
    ok(f[EOS_MAX_FILES] == NULL, "one past the pool is refused");
    eq(eos_storage_errno(), EOS_ERR_POOL, "  with POOL, which the web layer renders as 503");
    ok(!eos_storage_exists("/int/pool_over"),
       "  and the refused open did not create the file");

    // Every handle is distinct. Two workers sharing a slot is the bug this
    // pool exists to make impossible.
    for (i = 1; i < EOS_MAX_FILES; i++)
        ok(f[i] != f[i - 1], "handles are distinct");

    eq(eos_storage_close(f[0]), EOS_OK, "closing one returns its slot");
    f[EOS_MAX_FILES] = eos_storage_open("/int/pool_over", EOS_O_WRITE | EOS_O_CREATE);
    ok(f[EOS_MAX_FILES] != NULL, "and the next open succeeds");
    if (f[EOS_MAX_FILES]) eq(eos_storage_close(f[EOS_MAX_FILES]), EOS_OK, "close it again");

    // Double close. The slot has already been handed back, and closing it a
    // second time would close whatever the next open put there.
    eq(eos_storage_close(f[0]), EOS_ERR_STATE, "a double close is refused");
    eq(eos_storage_close(f[0]), EOS_ERR_STATE, "  and stays refused");
    ok(f[1] != NULL, "the handle a second worker holds is untouched");
    if (f[1]) eq(eos_storage_write(f[1], "ok", 2), 2, "  and still writes");

    eq(eos_storage_close(NULL), EOS_ERR_ARG, "closing NULL is ARG");
    eq(eos_storage_close(stackish), EOS_ERR_ARG, "closing a handle that is not ours is ARG");
    eq(eos_storage_close((eos_file_t *)((char *)f[1] + 1)), EOS_ERR_ARG,
       "closing a pointer into the middle of a slot is ARG");

    eq(eos_storage_read(NULL, "x", 1), EOS_ERR_ARG, "reading NULL is ARG");
    eq(eos_storage_write(stackish, "x", 1), EOS_ERR_ARG, "writing a foreign handle is ARG");
    eq((long)eos_storage_seek(NULL, 0, EOS_SEEK_SET), EOS_ERR_ARG, "seeking NULL is ARG");
    eq((long)eos_storage_tell(NULL), EOS_ERR_ARG, "telling NULL is ARG");
    eq((long)eos_storage_size(NULL), EOS_ERR_ARG, "sizing NULL is ARG");
    eq(eos_storage_sync(NULL), EOS_ERR_ARG, "syncing NULL is ARG");

    for (i = 1; i < EOS_MAX_FILES; i++) eq(eos_storage_close(f[i]), EOS_OK, "drain the pool");

    // Drained, and the whole pool is available again.
    f[0] = eos_storage_open("/int/pool0", EOS_O_READ);
    ok(f[0] != NULL, "the pool is whole again after a drain");
    if (f[0]) eq(eos_storage_close(f[0]), EOS_OK, "close");
}

static void t_dir_pool(void)
{
    eos_dirh_t *d[EOS_MAX_DIRS + 1];
    eos_dirent_t e;
    int i;

    for (i = 0; i < EOS_MAX_DIRS; i++) {
        d[i] = eos_storage_opendir("/int");
        ok(d[i] != NULL, "the directory pool hands out a handle");
    }
    d[EOS_MAX_DIRS] = eos_storage_opendir("/int");
    ok(d[EOS_MAX_DIRS] == NULL, "one past the directory pool is refused");
    eq(eos_storage_errno(), EOS_ERR_POOL, "  with POOL");

    eq(eos_storage_closedir(d[0]), EOS_OK, "closing one returns its slot");
    eq(eos_storage_closedir(d[0]), EOS_ERR_STATE, "a double closedir is refused");
    eq(eos_storage_closedir(NULL), EOS_ERR_ARG, "closing NULL is ARG");
    ok(!eos_storage_readdir(NULL, &e), "reading NULL is false");
    ok(d[EOS_MAX_DIRS - 1] != NULL, "the other handle is untouched");
    if (d[EOS_MAX_DIRS - 1])
        ok(eos_storage_readdir(d[EOS_MAX_DIRS - 1], &e), "  and still reads");

    for (i = 1; i < EOS_MAX_DIRS; i++) eq(eos_storage_closedir(d[i]), EOS_OK, "drain");
}

// ----------------------------------------------------------- directories

static int listing_has(const char *path, const char *name, uint32_t *size_out, bool *dir_out)
{
    eos_dirh_t *d = eos_storage_opendir(path);
    eos_dirent_t e;
    int found = 0;

    if (!d) return -1;
    while (eos_storage_readdir(d, &e)) {
        if (strcmp(e.name, name) == 0) {
            found = 1;
            if (size_out) *size_out = e.size;
            if (dir_out)  *dir_out  = e.is_dir;
        }
        if (strchr(e.name, '/')) found = -2;   // a component, never a path
    }
    eos_storage_closedir(d);
    return found;
}

static void t_listing(void)
{
    eos_dirh_t *d;
    eos_dirent_t e;
    uint32_t size = 999;
    bool is_dir = true;
    int n;

    eq(eos_storage_save("/int/list.bin", "12345678", 8), EOS_OK, "a file to list");
    eq(eos_storage_mkdir("/int/listdir"), EOS_OK, "a directory to list");

    eq(listing_has("/int", "list.bin", &size, &is_dir), 1, "the file is listed");
    eq(size, 8, "  with its size");
    ok(!is_dir, "  and is not a directory");
    eq(listing_has("/int", "listdir", &size, &is_dir), 1, "the directory is listed");
    ok(is_dir, "  as a directory");
    eq(size, 0, "  with no size");
    eq(listing_has("/int", ".", NULL, NULL), 0, "\".\" is not an entry");
    eq(listing_has("/int", "..", NULL, NULL), 0, "\"..\" is not an entry");

    // Listing "/" enumerates the mounts, which is what lets a file browser
    // have no special case for the top level.
    d = eos_storage_opendir("/");
    ok(d != NULL, "the root opens as a directory");
    n = 0;
    if (d) {
        while (eos_storage_readdir(d, &e)) {
            n++;
            ok(e.is_dir, "every root entry is a directory");
            eqs(e.name, "int", "and the only mounted one is the internal filesystem");
        }
        eq(eos_storage_closedir(d), EOS_OK, "close the root");
    }
    eq(n, 1, "the root lists exactly the mounts that came up");

    ok(eos_storage_opendir("/int/list.bin") == NULL, "a file does not open as a directory");
    ok(eos_storage_opendir("/int/nope") == NULL, "a missing directory does not open");
    eq(eos_storage_errno(), EOS_ERR_NOTFOUND, "  with NOTFOUND");
    ok(eos_storage_opendir("/int/../") == NULL, "a traversal does not open as a directory");

    // A name longer than eos_dirent_t can carry is skipped, not shortened: a
    // shortened name is a name for a different file and the browser would hand
    // it straight back to remove().
    {
        char p[EOS_PATH_MAX], nm[EOS_NAME_MAX + 8];
        int i;
        for (i = 0; i < EOS_NAME_MAX + 2; i++) nm[i] = 'L';
        nm[EOS_NAME_MAX + 2] = '\0';
        snprintf(p, sizeof p, "/tmp/%s", "x");          // silence unused warnings on some libcs
        (void)p;
        {
            char sys[512];
            FILE *fp;
            snprintf(sys, sizeof sys, "%s/%s", eos_storage_host_root(), nm);
            fp = fopen(sys, "wb");
            if (fp) { fputc('x', fp); fclose(fp); }
        }
        eq(listing_has("/int", nm, NULL, NULL), 0, "a name over EOS_NAME_MAX is skipped");
        nm[EOS_NAME_MAX - 1] = '\0';
        eq(listing_has("/int", nm, NULL, NULL), 0, "  and is not reported truncated either");
    }
}

// --------------------------------------------------------- mount lifetime

static void t_mount_lifetime(void)
{
    eos_file_t *f;
    uint64_t total = 1, used = 1;

    eq(eos_storage_usage("/int", &total, &used), EOS_OK, "usage on the internal filesystem");

    f = eos_storage_open("/int/hold.txt", EOS_O_WRITE | EOS_O_CREATE);
    ok(f != NULL, "hold a file open");
    eq(eos_storage_unmount("/int"), EOS_ERR_BUSY, "unmounting with a file open is BUSY");
    if (f) eq(eos_storage_close(f), EOS_OK, "close it");
    eq(eos_storage_unmount("/int"), EOS_OK, "and now it unmounts");

    ok(eos_storage_open("/int/hold.txt", EOS_O_READ) == NULL, "an unmounted mount serves nothing");
    eq(eos_storage_errno(), EOS_ERR_NODEV, "  with NODEV");
    eq(eos_storage_usage("/int", NULL, NULL), EOS_ERR_NODEV, "usage on it is NODEV");
    {
        eos_dirh_t *d = eos_storage_opendir("/");
        eos_dirent_t e;
        int n = 0;
        ok(d != NULL, "the root still opens with nothing mounted");
        if (d) {
            while (eos_storage_readdir(d, &e)) n++;
            eos_storage_closedir(d);
        }
        eq(n, 0, "  and lists nothing");
    }
    ok(eos_storage_mount_for("/int/x") != NULL, "the mount is still declared while unmounted");

    eq(eos_storage_mount("/int"), EOS_OK, "remount");
    eq(eos_storage_mount("/int"), EOS_OK, "remounting a mounted mount is idempotent");
    ok(eos_storage_exists("/int/hold.txt"), "and the file survived");

    // A directory handle pins the mount the same way a file does.
    {
        eos_dirh_t *d = eos_storage_opendir("/int");
        ok(d != NULL, "hold a directory open");
        eq(eos_storage_unmount("/int"), EOS_ERR_BUSY, "unmounting with a scan open is BUSY");
        if (d) eq(eos_storage_closedir(d), EOS_OK, "close it");
        eq(eos_storage_unmount("/int"), EOS_OK, "and now it unmounts");
        eq(eos_storage_mount("/int"), EOS_OK, "remount for the rest of the run");
    }
}

static void t_uninitialised(void)
{
    eos_stat_t st;
    eos_storage_host_reset();

    // Nothing is mounted and nothing has been declared. Every call answers.
    ok(eos_storage_open("/int/x", EOS_O_READ) == NULL, "open before init fails");
    eq(eos_storage_stat("/int/x", &st), EOS_ERR_STATE, "stat before init is STATE");
    eq(eos_storage_mount("/int"), EOS_ERR_STATE, "mount before init is STATE");
    eq(eos_storage_mounts(NULL, 0), 0, "no mounts are declared before init");
    ok(eos_storage_mount_for("/int/x") == NULL, "and nothing routes");
    ok(eos_storage_opendir("/") == NULL, "not even the root");
}

int main(void)
{
    sandbox_open();

    t_split_shapes();
    t_split_traversal();
    t_split_lengths();
    t_split_embedded_nul();
    t_routing();
    t_card_absent();
    t_file_roundtrip();
    t_metadata();
    t_file_pool();
    t_dir_pool();
    t_listing();
    t_mount_lifetime();
    t_uninitialised();

    rmtree(ROOT);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
