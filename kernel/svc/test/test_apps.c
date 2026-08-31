// Host checks for the files, console, buddy and apps endpoints.
//
// This is not a unit test of eos_apps.c. It drives eos_httpd_dispatch() with
// real request lines and real query strings, against the real eos_storage
// backend pointed at a directory in /tmp, and reads the JSON that comes back —
// which is the only way to check the thing that actually matters here. A path
// reaching eos_storage_open() came out of an HTTP query string, off whoever is
// on the WiFi, and the interesting question is never "does path_check() reject
// dot dot" but "does every one of the nine routes call it before it calls
// anything else". Only one of those two questions can be answered by looking at
// the endpoint from outside, and it is the second one.
//
// What this cannot check: the flash, the four HTTP workers actually racing, and
// esp_log's vprintf hook. A green run says the routing, the bounds, the upload
// state machine and the command table are right.

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "eos_apps.h"
#include "waveshare-c6-lcd-13.h"

const eos_board_t *eos_board_get(void) { return &EOS_BOARD; }

void eos_storage_host_reset(void);

// Host-only hook out of eos_apps.c. See the comment on it there: this layer's
// path rules cannot be observed through an endpoint, because eos_storage
// refuses the same things again underneath.
int eos_apps_host_path_check(const char *p);

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

// ==========================================================================
// The sandbox and the server
// ==========================================================================

static char ROOT[128];

static void rmtree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    char sub[512];

    if (!d) { unlink(path); return; }
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        rmtree(sub);
    }
    closedir(d);
    rmdir(path);
}

static void *fake_below_open(void *ctx, const char *path, long *size_out);
static int   fake_below_read(void *ctx, void *fh, void *buf, int n);
static void  fake_below_close(void *ctx, void *fh);

static eos_httpd_t   H;
static eos_httpd_resp_t RESP;
static char          BODY[EOS_HTTPD_RESP_MAX + 1];
static int           BODY_LEN;

// What the ports report, so the console table can be driven both ways.
static bool  fake_can_describe = true;
static int   reboot_calls;
static char  reboot_topic[64];

static void fake_reboot(void *ctx, uint32_t in_ms)
{
    (void)ctx; (void)in_ms;
    reboot_calls++;
}

static int fake_describe(void *ctx, const char *topic, char *out, int cap)
{
    (void)ctx;
    snprintf(reboot_topic, sizeof reboot_topic, "%s", topic);
    if (!fake_can_describe) return -1;
    return snprintf(out, (size_t)cap, "%s: answered by the port", topic);
}

// Drives one request all the way through and leaves the body in BODY. A FILE
// response is drained through the same three ports the ESP transport uses, so
// the streaming path is exercised rather than assumed.
static int req(const char *method, const char *uri, const char *body, int body_len,
               bool truncated)
{
    eos_httpd_req_t rq;
    int status;

    memset(&rq, 0, sizeof rq);
    rq.method = method;
    rq.uri    = uri;
    rq.body   = body;
    rq.body_len = body_len;
    rq.body_truncated = truncated;

    status = eos_httpd_dispatch(&H, &rq, &RESP);

    BODY_LEN = 0;
    BODY[0] = '\0';
    if (RESP.kind == EOS_HTTPD_BODY_FILE) {
        for (;;) {
            int n = H.ports.file_read(H.ctx, RESP.file, BODY + BODY_LEN,
                                      (int)sizeof BODY - 1 - BODY_LEN);
            if (n <= 0) break;
            BODY_LEN += n;
        }
        H.ports.file_close(H.ctx, RESP.file);
        RESP.file = NULL;
    } else if (RESP.body) {
        BODY_LEN = RESP.body_len;
        if (BODY_LEN > (int)sizeof BODY - 1) BODY_LEN = (int)sizeof BODY - 1;
        memcpy(BODY, RESP.body, (size_t)BODY_LEN);
    }
    BODY[BODY_LEN] = '\0';
    return status;
}

static int GET(const char *uri)  { return req("GET", uri, NULL, 0, false); }
static int POST(const char *uri) { return req("POST", uri, NULL, 0, false); }
static int POSTB(const char *uri, const char *body)
{
    return req("POST", uri, body, (int)strlen(body), false);
}
static int POSTN(const char *uri, const char *body, int n)
{
    return req("POST", uri, body, n, false);
}

static bool has(const char *needle) { return strstr(BODY, needle) != NULL; }

// ==========================================================================
// Building files to serve
// ==========================================================================

static void put_file(const char *path, const void *data, int n)
{
    char sys[512];
    FILE *f;
    snprintf(sys, sizeof sys, "%s%s", ROOT, path + 4);   // strip "/int"
    f = fopen(sys, "wb");
    if (!f) { printf("FAIL: could not stage %s\n", path); failed++; return; }
    if (n > 0) fwrite(data, 1, (size_t)n, f);
    fclose(f);
}

static void put_text(const char *path, const char *s) { put_file(path, s, (int)strlen(s)); }

static void put_dir(const char *path)
{
    char sys[512];
    snprintf(sys, sizeof sys, "%s%s", ROOT, path + 4);
    mkdir(sys, 0777);
}

static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// A real MagicaVoxel file of the subset eos_vox_parse() reads.
static int build_vox(uint8_t *out, int cap, int nvox, int dim)
{
    int i, p = 0;
    uint32_t xyzi = 4 + 4 * (uint32_t)nvox;
    uint32_t children = 12 + 12 + 12 + xyzi + 12 + 1024;

    if (cap < (int)(8 + 12 + children)) return -1;

    memcpy(out, "VOX ", 4);           p = 4;
    w32(out + p, 150);                p += 4;
    memcpy(out + p, "MAIN", 4);       p += 4;
    w32(out + p, 0);                  p += 4;
    w32(out + p, children);           p += 4;

    memcpy(out + p, "SIZE", 4);       p += 4;
    w32(out + p, 12);                 p += 4;
    w32(out + p, 0);                  p += 4;
    w32(out + p, (uint32_t)dim);      p += 4;
    w32(out + p, (uint32_t)dim);      p += 4;
    w32(out + p, (uint32_t)dim);      p += 4;

    memcpy(out + p, "XYZI", 4);       p += 4;
    w32(out + p, xyzi);               p += 4;
    w32(out + p, 0);                  p += 4;
    w32(out + p, (uint32_t)nvox);     p += 4;
    for (i = 0; i < nvox; i++) {
        out[p++] = (uint8_t)(i % dim);
        out[p++] = (uint8_t)((i / dim) % dim);
        out[p++] = (uint8_t)((i / (dim * dim)) % dim);
        out[p++] = (uint8_t)(1 + (i % 7));
    }

    memcpy(out + p, "RGBA", 4);       p += 4;
    w32(out + p, 1024);               p += 4;
    w32(out + p, 0);                  p += 4;
    for (i = 0; i < 256; i++) {
        out[p++] = (uint8_t)i; out[p++] = (uint8_t)(255 - i);
        out[p++] = 0x40;       out[p++] = 0xFF;
    }
    return p;
}

// ==========================================================================
// Path traversal, on every route that takes one
// ==========================================================================
//
// The corpus is every shape that has ever walked out of a web filesystem, plus
// the ones eos_storage's own suite pins. It is driven through all nine routes
// rather than through one, because the bug this catches is never "the check is
// wrong" — it is "route seven forgot to call it".

static const char *const HOSTILE[] = {
    "/int/../nvs",
    "/int/../../etc/passwd",
    "/../etc/passwd",
    "/int/a/../../b",
    "/int/./../x",
    "..",
    "/..",
    "/int/..",
    "%2e%2e%2fetc",                     // decoded once by the query parser
    "/int/%2e%2e/x",
    "/int/a%5c..%5cb",                  // a backslash, decoded
    "/int\\..\\x",
    "int/x",                            // relative
    "",
    "/int/a%00b",                       // a NUL the decoder must refuse
    "/int/a%01b",                       // a raw control byte
    "/int/a%0ab",
    "/int/a%7fb",
    "/etc/passwd",
    "/INT/x",
    // a 100-byte component, then a 119-byte path. Both are refused, and for
    // two different reasons: EOS_NAME_MAX and EOS_PATH_MAX.
    ("/int/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
    ("/int/aaaaaaaaaa/aaaaaaaaaa/aaaaaaaaaa/aaaaaaaaaa/aaaaaaaaaa"
     "/aaaaaaaaaa/aaaaaaaaaa/aaaaaaaaaa/aaaaaaaaaa/aaaaaaaaaa"),
};
#define N_HOSTILE ((int)(sizeof HOSTILE / sizeof HOSTILE[0]))

// Percent-encodes so the corpus survives being a query value.
static void qenc(char *out, int cap, const char *s)
{
    static const char HEX[] = "0123456789abcdef";
    int o = 0;
    for (; *s && o < cap - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' ||
            c == '-' || c == '%') {
            out[o++] = (char)c;                    // '%' passes through: the
        } else {                                   // corpus encodes its own
            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 15];
        }
    }
    out[o] = '\0';
}

static void t_traversal(void)
{
    static const struct { const char *m, *base, *arg; } R[] = {
        { "GET",  "/api/fs/list",  "path" },
        { "GET",  "/api/fs/stat",  "path" },
        { "GET",  "/api/fs/read",  "path" },
        { "POST", "/api/fs/write", "path" },
        { "POST", "/api/fs/upload/abort", "path" },
        { "POST", "/api/fs/mkdir", "path" },
        { "POST", "/api/fs/remove","path" },
        { "POST", "/api/fs/rename","from" },
        { "POST", "/api/fs/rename","to"   },
    };
    int nr = (int)(sizeof R / sizeof R[0]);
    int i, k, refused = 0, tried = 0;
    char enc[512], uri[700];

    for (k = 0; k < nr; k++) {
        for (i = 0; i < N_HOSTILE; i++) {
            int st;
            qenc(enc, (int)sizeof enc, HOSTILE[i]);
            if (strcmp(R[k].base, "/api/fs/rename") == 0) {
                if (strcmp(R[k].arg, "from") == 0)
                    snprintf(uri, sizeof uri, "%s?from=%s&to=/int/ok", R[k].base, enc);
                else
                    snprintf(uri, sizeof uri, "%s?from=/int/ok&to=%s", R[k].base, enc);
            } else if (strcmp(R[k].base, "/api/fs/write") == 0) {
                snprintf(uri, sizeof uri, "%s?path=%s&offset=0&final=1", R[k].base, enc);
            } else {
                snprintf(uri, sizeof uri, "%s?%s=%s", R[k].base, R[k].arg, enc);
            }

            tried++;
            st = req(R[k].m, uri, "x", 1, false);

            // Nothing hostile may ever succeed, and the answer must be an
            // error document rather than a file. /api/fs/upload/abort is the
            // one exception in shape: it answers 200 {"aborted":false} for a
            // path it does not hold, which is a refusal that says so.
            if (strcmp(R[k].base, "/api/fs/upload/abort") == 0) {
                if (st == 200 && has("\"aborted\":false")) { refused++; continue; }
            }
            if (st >= 400 && st < 500) refused++;
            else printf("FAIL: %s %s answered %d\n", R[k].m, uri, st);

            if (RESP.kind == EOS_HTTPD_BODY_FILE) {
                failed++;
                printf("FAIL: %s %s staged a FILE response\n", R[k].m, uri);
            }
        }
    }
    eq(refused, tried, "every hostile path on every fs route is refused");
    ok(tried == nr * N_HOSTILE, "the traversal corpus reached every route");

    // And the escape did not happen: nothing outside the sandbox was touched.
    ok(access("/tmp/eos-apps-escaped", F_OK) != 0, "no traversal wrote outside the sandbox");
}

// The same corpus, driven straight at this file's own check. Every one of these
// must be refused HERE, one layer above the backend that would also refuse it —
// otherwise the day somebody binds a different storage backend, or calls
// eos_storage with a path that came from somewhere else, the only rule left is
// the one that was deleted because no test noticed.
static void t_path_check_direct(void)
{
    static const char *const GOOD[] = {
        "/", "/int", "/int/", "/int/a", "/int/a/b/c", "/int/web/app.js.gz",
        "/int/...bb...", "/int/a..b", "/int/..bb", "/int/bb..", "/sd/x",
        "/int/caf\xc3\xa9.txt", "/nope/x",
    };
    int i, refused = 0;

    for (i = 0; i < N_HOSTILE; i++) {
        const char *p = HOSTILE[i];
        // Still percent-encoded: that is the query parser's input, not this
        // check's, and decoding here a second time is how %252e%252e walks
        // through a two-stage decoder.
        if (strstr(p, "%")) continue;
        // Structurally legal and merely naming no mount. That is the mount
        // table's answer to give, not this function's, and a path checker that
        // knew the mount names would be a second copy of them.
        if (strcmp(p, "/etc/passwd") == 0 || strcmp(p, "/INT/x") == 0) {
            eq(eos_apps_host_path_check(p), EOS_OK,
               "a path naming no mount is structurally legal; the mount table refuses it");
            continue;
        }
        checks++;
        if (eos_apps_host_path_check(p) != EOS_OK) refused++;
        else { failed++; printf("FAIL: path_check accepted \"%s\"\n", p); }
    }
    ok(refused >= 12, "the hostile corpus is refused by this layer on its own");

    for (i = 0; i < (int)(sizeof GOOD / sizeof GOOD[0]); i++) {
        checks++;
        if (eos_apps_host_path_check(GOOD[i]) != EOS_OK) {
            failed++;
            printf("FAIL: path_check refused the legal path \"%s\"\n", GOOD[i]);
        }
    }

    // The three rules a black-box test cannot see, one at a time.
    eq(eos_apps_host_path_check("/int/../x"), EOS_ERR_ARG, "a .. component is refused here");
    eq(eos_apps_host_path_check("/int/.."),   EOS_ERR_ARG, "a trailing .. too");
    eq(eos_apps_host_path_check("/int/a\\b"), EOS_ERR_ARG, "a backslash is refused here");
    eq(eos_apps_host_path_check("/int/a\001" "b"), EOS_ERR_ARG, "a control byte is refused here");
    eq(eos_apps_host_path_check("/int/a\177" "b"), EOS_ERR_ARG, "and DEL");
    eq(eos_apps_host_path_check("int/x"),     EOS_ERR_ARG, "a relative path is refused here");
    eq(eos_apps_host_path_check(""),          EOS_ERR_ARG, "and an empty one");
    eq(eos_apps_host_path_check(NULL),        EOS_ERR_ARG, "and a NULL one");

    // "...bb..." contains ".." and is a legal filename. A filter that gets this
    // backwards blocks real names AND still passes the real escape.
    eq(eos_apps_host_path_check("/int/...bb..."), EOS_OK,
       "a name merely containing dots is not a climb");

    {
        char p[200];
        int j;
        memcpy(p, "/int/", 5);
        for (j = 5; j < 5 + EOS_NAME_MAX; j++) p[j] = 'a';
        p[5 + EOS_NAME_MAX] = 0;
        eq(eos_apps_host_path_check(p), EOS_ERR_TOOBIG, "a component past EOS_NAME_MAX is refused");
        for (j = 5; j < 120; j++) p[j] = ((j - 4) % 20 == 0) ? '/' : 'a';
        p[120] = 0;
        eq(eos_apps_host_path_check(p), EOS_ERR_TOOBIG, "and a path past EOS_PATH_MAX");
    }
}

// A missing parameter is a malformed request on every one of them, and never a
// default that lists the root or removes something.
static void t_missing_args(void)
{
    eq(GET("/api/fs/list"),   400, "list with no path is 400");
    eq(GET("/api/fs/stat"),   400, "stat with no path is 400");
    eq(GET("/api/fs/read"),   400, "read with no path is 400");
    eq(GET("/api/fs/usage"),  400, "usage with no point is 400");
    eq(POST("/api/fs/mkdir"), 400, "mkdir with no path is 400");
    eq(POST("/api/fs/remove"),400, "remove with no path is 400");
    eq(POST("/api/fs/rename?to=/int/b"),   400, "rename with no from is 400");
    eq(POST("/api/fs/rename?from=/int/a"), 400, "rename with no to is 400");
    eq(POST("/api/fs/write"), 400, "write with no path is 400");
    ok(has("bad_argument"), "and it says bad_argument");
}

// ==========================================================================
// Bounds on every network-supplied number
// ==========================================================================

static void t_bounds(void)
{
    eq(GET("/api/fs/list?path=/int&offset=-1"),   400, "a negative offset is refused");
    eq(GET("/api/fs/list?path=/int&offset=abc"),  400, "a non-numeric offset is refused");
    eq(GET("/api/fs/list?path=/int&offset=99999999999999"), 400, "an overflowing offset is refused");
    eq(GET("/api/fs/list?path=/int&count=1000"),  400, "a count past list_max is refused");
    eq(GET("/api/fs/list?path=/int&count=0"),     200, "a count of zero is a legal empty page");
    ok(has("\"entries\":[]"), "and it really is empty");

    eq(GET("/api/console/log?since=-4"),  400, "a negative since is refused");
    eq(GET("/api/console/log?max=zz"),    400, "a non-numeric max is refused");
    eq(GET("/api/console/log?since=99999999"), 200, "a since past the end clamps");
    ok(has("\"lines\":[]"), "and returns nothing rather than wedging");

    eq(POSTB("/api/fs/write?path=/int/b.bin&offset=nope&final=0", "x"), 400,
       "a non-numeric write offset is refused");
    eq(POSTB("/api/fs/write?path=/int/b.bin&offset=0&final=7", "x"), 400,
       "a final that is not 0 or 1 is refused");

    // The transport refuses a body over EOS_HTTPD_BODY_MAX before any handler
    // sees it, and that is what limits.chunk_max reports.
    eq(req("POST", "/api/fs/write?path=/int/b.bin&offset=0&final=1", NULL, 0, true), 413,
       "a body past chunk_max is 413");
    ok(has("too_big"), "and it says too_big");
    eq(req("POST", "/api/console/exec", NULL, 0, true), 413, "an oversize exec body is 413");

    eq(eos_apps_chunk_max(), EOS_HTTPD_BODY_MAX, "chunk_max is the transport's body limit");
    ok(eos_apps_path_max() == EOS_PATH_MAX && eos_apps_name_max() == EOS_NAME_MAX,
       "the limits block reports the real numbers");

    // A path of exactly 95 bytes fits; 96 does not. A truncated path names a
    // different file, so this boundary is checked from both sides.
    {
        char p[160], uri[300];
        int i;
        // Components stay under EOS_NAME_MAX so the only rule being tested is
        // the whole-path length one.
        memcpy(p, "/int/", 5);
        for (i = 5; i < 95; i++) p[i] = ((i - 4) % 20 == 0) ? '/' : 'a';
        p[94] = 'a';
        p[95] = '\0';
        snprintf(uri, sizeof uri, "/api/fs/stat?path=%s", p);
        eq(GET(uri), 404, "a 95-byte path is accepted and merely not found");
        p[95] = 'a'; p[96] = '\0';
        snprintf(uri, sizeof uri, "/api/fs/stat?path=%s", p);
        eq(GET(uri), 413, "a 96-byte path is too_big, never truncated");
    }
}

// ==========================================================================
// Listing
// ==========================================================================

static void t_list(void)
{
    char uri[160];
    int i;

    put_dir("/int/tree");
    for (i = 0; i < 30; i++) {
        char p[64];
        snprintf(p, sizeof p, "/int/tree/f%02d.txt", i);
        put_text(p, "hello");
    }
    put_dir("/int/tree/sub");

    eq(GET("/api/fs/list?path=/int/tree"), 200, "a directory lists");
    ok(has("\"path\":\"/int/tree\""), "the listing names the path it listed");
    ok(has("\"total\":31"), "total counts every entry, not the page");

    // The per-entry fields are checked on a directory small enough that one
    // page holds it whatever EOS_APPS_LIST_MAX is. The 31-entry one above is
    // for `total` and for paging, and readdir order is the filesystem's.
    put_dir("/int/small");
    put_text("/int/small/one.txt", "hello");
    put_dir("/int/small/adir");
    eq(GET("/api/fs/list?path=/int/small"), 200, "a small directory lists whole");
    ok(has("\"one.txt\""), "the file is there");
    ok(has("\"size\":5"), "entries carry a size");
    ok(has("\"mtime\":"), "and an mtime, which eos_dirent_t does not have");
    ok(has("\"is_dir\":true"), "the subdirectory is marked");
    ok(has("\"more\":false"), "and one page held it");

    // Paging. The page is whatever also fits the response buffer, so the test
    // asks for the pages the client would ask for rather than assuming a size.
    {
        int offset = 0, seen = 0, pages = 0;
        // Derived from the tunable, not hardcoded: the suite has to keep
        // passing when somebody builds with a smaller page.
        int page = EOS_APPS_LIST_MAX < 5 ? EOS_APPS_LIST_MAX : 5;
        for (;;) {
            const char *m;
            int n = 0;
            // count=5 forces real paging. Thirty-one short entries fit one
            // response buffer, so without it the "paging" loop would take one
            // page and prove nothing about the cursor.
            snprintf(uri, sizeof uri, "/api/fs/list?path=/int/tree&offset=%d&count=%d",
                     offset, page);
            eq(GET(uri), 200, "each page is a 200");
            for (m = BODY; (m = strstr(m, "\"name\":")); m++) n++;
            seen += n;
            pages++;
            if (!has("\"more\":true")) break;
            ok(n > 0, "a page that says there is more returned something");
            offset += n;
            if (pages > 20) { failed++; printf("FAIL: paging did not terminate\n"); break; }
        }
        eq(seen, 31, "paging walks every entry exactly once");
        ok(pages >= (31 + page - 1) / page,
           "over several pages, so the cursor is really exercised");
    }

    eq(GET("/api/fs/list?path=/int/tree&offset=100"), 200, "an offset past the end is legal");
    ok(has("\"entries\":[]") && has("\"more\":false"), "and returns an empty last page");

    // Listing "/" enumerates the mounts, which is what stops the browser from
    // needing a special case for the top level.
    eq(GET("/api/fs/list?path=/"), 200, "the root lists");
    ok(has("\"int\""), "the root shows /int");
    // Only MOUNTED mounts are listed: a browser that can see /sd should be
    // able to enter it, and the fact that the slot exists belongs in
    // /api/system's fs block, which reports every declared mount.
    ok(!has("\"name\":\"sd\""), "the absent card is not listed as an enterable directory");

    // ENOTDIR out of the backend, which is bad_argument and not not_found: the
    // path IS there, it is simply not a directory.
    eq(GET("/api/fs/list?path=/int/tree/f00.txt"), 400, "listing a file is not a listing");
    eq(GET("/api/fs/list?path=/int/nope"), 404, "listing what is not there is 404");
    eq(GET("/api/fs/list?path=/sd/x"), 503, "listing the absent card is no_such_device");
    ok(has("no_such_device"), "and says so by name");
}

static void t_stat_read_usage(void)
{
    put_text("/int/read.bin", "0123456789abcdef");

    eq(GET("/api/fs/stat?path=/int/read.bin"), 200, "stat works");
    ok(has("\"size\":16"), "and reports the size");
    ok(has("\"is_dir\":false"), "and the type");
    eq(GET("/api/fs/stat?path=/int/tree"), 200, "stat of a directory works");
    ok(has("\"is_dir\":true"), "and says it is one");

    eq(GET("/api/fs/read?path=/int/read.bin"), 200, "read works");
    eq(BODY_LEN, 16, "and streams every byte");
    ok(memcmp(BODY, "0123456789abcdef", 16) == 0, "and the right ones");
    ok(RESP.content_type && strcmp(RESP.content_type, "application/octet-stream") == 0,
       "read is octet-stream, never guessed from the extension");
    eq(RESP.file_size, 16, "and carries the size it knows");

    eq(GET("/api/fs/read?path=/int/tree"), 400, "reading a directory is refused");
    eq(GET("/api/fs/read?path=/int/nope"), 404, "reading what is not there is 404");

    eq(GET("/api/fs/usage?point=/int"), 200, "usage works");
    ok(has("\"point\":\"/int\"") && has("\"free\":"), "and carries the three numbers");
    eq(GET("/api/fs/usage?point=/nope"), 404, "usage of no such mount is 404");
    eq(GET("/api/fs/usage?point=/sd"), 503, "usage of the absent card is no_such_device");
}

// ==========================================================================
// The upload state machine
// ==========================================================================

static void t_upload(void)
{
    char chunk[EOS_APPS_CHUNK_MAX];
    int i;

    for (i = 0; i < (int)sizeof chunk; i++) chunk[i] = (char)('a' + (i % 26));

    eos_apps_tick(1000);

    // A whole small file in one request.
    eq(POSTB("/api/fs/write?path=/int/u1.bin&offset=0&final=1", "hello"), 200,
       "offset 0 final 1 writes a whole file");
    ok(has("\"offset\":5") && has("\"size\":5") && has("\"final\":true"),
       "and reports the next offset, which is the end");
    eq(GET("/api/fs/read?path=/int/u1.bin"), 200, "the file is there");
    eq(BODY_LEN, 5, "with the right length");

    // A zero-byte file is offset=0&final=1 with an empty body.
    eq(POSTN("/api/fs/write?path=/int/zero.bin&offset=0&final=1", NULL, 0), 200,
       "a zero-byte file is one empty final chunk");
    eq(GET("/api/fs/stat?path=/int/zero.bin"), 200, "and it exists");
    ok(has("\"size\":0"), "and is empty");

    // Three chunks, then the final one.
    eq(POSTN("/api/fs/write?path=/int/u2.bin&offset=0&final=0", chunk, 100), 200,
       "the first chunk opens the handle");
    ok(has("\"offset\":100") && has("\"final\":false"), "and says where the next one goes");
    eq(POSTN("/api/fs/write?path=/int/u2.bin&offset=100&final=0", chunk, 100), 200,
       "the second appends");
    ok(has("\"offset\":200"), "and the offset advances");

    // Out of order, in both directions. The handle is untouched.
    eq(POSTN("/api/fs/write?path=/int/u2.bin&offset=100&final=0", chunk, 10), 409,
       "a repeated offset is a state error");
    ok(has("\"error\":\"state\""), "and says state");
    eq(POSTN("/api/fs/write?path=/int/u2.bin&offset=5000&final=0", chunk, 10), 409,
       "an offset past the handle is a state error");

    // A different path while a handle is open does not silently switch files.
    eq(POSTN("/api/fs/write?path=/int/other.bin&offset=0&final=1", chunk, 10), 409,
       "a second upload is busy");
    ok(has("\"error\":\"busy\""), "and says busy");
    ok(access("/tmp/eos-apps-test-other", F_OK) != 0, "and created nothing");

    // The handle survived all four refusals.
    eq(POSTN("/api/fs/write?path=/int/u2.bin&offset=200&final=1", chunk, 56), 200,
       "the upload finishes where it left off");
    eq(GET("/api/fs/read?path=/int/u2.bin"), 200, "the whole file reads back");
    eq(BODY_LEN, 256, "at the right length");
    ok(memcmp(BODY, chunk, 100) == 0 && memcmp(BODY + 100, chunk, 100) == 0,
       "and with the chunks in order");

    // Now the handle is closed, so the other upload is no longer refused.
    eq(POSTN("/api/fs/write?path=/int/other.bin&offset=0&final=1", chunk, 10), 200,
       "the next upload is accepted once the first has finished");

    // Restarting the same path at offset 0 truncates.
    eq(POSTN("/api/fs/write?path=/int/u3.bin&offset=0&final=0", chunk, 200), 200,
       "an upload starts");
    eq(POSTN("/api/fs/write?path=/int/u3.bin&offset=0&final=1", chunk, 4), 200,
       "and restarting it at zero truncates rather than being a state error");
    eq(GET("/api/fs/read?path=/int/u3.bin"), 200, "the restarted file reads");
    eq(BODY_LEN, 4, "and is only the second write");

    // Appending with no handle open is a state error, not a create.
    eq(POSTN("/api/fs/write?path=/int/u4.bin&offset=64&final=0", chunk, 10), 409,
       "appending with nothing open is a state error");
    eq(GET("/api/fs/stat?path=/int/u4.bin"), 404, "and created nothing");

    // A chunk of exactly chunk_max is accepted; the transport refuses larger.
    eq(POSTN("/api/fs/write?path=/int/u5.bin&offset=0&final=1",
             chunk, EOS_APPS_CHUNK_MAX), 200, "a chunk of exactly chunk_max is accepted");
}

static void t_abort(void)
{
    char chunk[64];
    memset(chunk, 'z', sizeof chunk);

    eos_apps_tick(10000);

    eq(POSTN("/api/fs/write?path=/int/ab.bin&offset=0&final=0", chunk, 64), 200,
       "an upload opens");
    eq(POST("/api/fs/upload/abort?path=/int/nope.bin"), 200,
       "aborting a path the board is not holding answers");
    ok(has("\"aborted\":false"), "and says it aborted nothing");
    eq(POSTN("/api/fs/write?path=/int/ab.bin&offset=64&final=0", chunk, 64), 200,
       "so the real upload is still open");

    eq(POST("/api/fs/upload/abort?path=/int/ab.bin"), 200, "aborting the real one works");
    ok(has("\"aborted\":true"), "and says so");
    eq(POSTN("/api/fs/write?path=/int/ab.bin&offset=128&final=0", chunk, 8), 409,
       "and the handle is gone");

    // An abort leaves NOTHING behind and, more importantly, leaves whatever was
    // already at the target alone. Uploads are written to <path>.part and only
    // renamed onto the target once the last chunk has synced, so an upload that
    // never finishes cannot destroy the file it was replacing. Writing straight
    // at the target truncated it on the FIRST chunk, and that is what cost the
    // owner their buddy: a save that failed left /int/buddy with no buddy.vox
    // at all, and the web app then reported a board that "describes a buddy" it
    // could not find.
    eq(GET("/api/fs/stat?path=/int/ab.bin"), 404, "an aborted upload leaves no file");
    eq(GET("/api/fs/stat?path=/int/ab.bin.part"), 404, "and no partial either");

    // The claim that actually matters: replacing a file cannot lose it.
    eq(POSTN("/api/fs/write?path=/int/keep.bin&offset=0&final=1", chunk, 32), 200,
       "a file exists to be replaced");
    eq(GET("/api/fs/stat?path=/int/keep.bin"), 200, "it is there");
    ok(has("\"size\":32"), "at its original size");
    eq(POSTN("/api/fs/write?path=/int/keep.bin&offset=0&final=0", chunk, 64), 200,
       "an upload starts over it");
    eq(POST("/api/fs/upload/abort?path=/int/keep.bin"), 200, "and then fails");
    eq(GET("/api/fs/stat?path=/int/keep.bin"), 200, "the original is still there");
    ok(has("\"size\":32"), "still 32 bytes, untouched by the upload that failed");

    // No path aborts whatever is open, which is a client that has lost track.
    eq(POSTN("/api/fs/write?path=/int/ab2.bin&offset=0&final=0", chunk, 8), 200, "another opens");
    eq(POST("/api/fs/upload/abort"), 200, "abort with no path");
    ok(has("\"aborted\":true"), "clears whatever was open");
    eq(POST("/api/fs/upload/abort"), 200, "and aborting nothing is not an error");
    ok(has("\"aborted\":false"), "it just says nothing was open");
}

// An upload that is never finished. This is the case that costs the board its
// one write handle until reboot if nothing expires it.
static void t_upload_timeout(void)
{
    char chunk[32];
    memset(chunk, 'q', sizeof chunk);

    eos_apps_tick(100000);
    eq(POSTN("/api/fs/write?path=/int/gone.bin&offset=0&final=0", chunk, 32), 200,
       "a phone starts an upload");

    eos_apps_tick(100000 + EOS_APPS_UPLOAD_IDLE_MS - 1);
    eq(POSTN("/api/fs/write?path=/int/other2.bin&offset=0&final=1", chunk, 4), 409,
       "one millisecond before the timeout it still holds the handle");

    eos_apps_tick(100000 + EOS_APPS_UPLOAD_IDLE_MS);
    eq(POSTN("/api/fs/write?path=/int/other2.bin&offset=0&final=1", chunk, 4), 200,
       "and after it the next upload is accepted");

    // The idle timeout ends an upload the same way an abort does, and for the
    // same reason: a phone that walked away must not have left the target
    // truncated behind it.
    eq(GET("/api/fs/stat?path=/int/gone.bin"), 404,
       "the abandoned upload leaves no file behind");
    eq(GET("/api/fs/stat?path=/int/gone.bin.part"), 404, "nor its partial");

    // The timeout is a console line, not a silent drop: an owner watching the
    // Console tab is the only person who can see this happen.
    eq(GET("/api/console/log?since=0"), 200, "the log reads");
    ok(has("idle") && has("gone.bin"), "and the expiry said which upload it was");

    // The clock never runs backwards into an early expiry.
    eos_apps_tick(200000);
    eq(POSTN("/api/fs/write?path=/int/wrap.bin&offset=0&final=0", chunk, 4), 200, "an upload opens");
    eos_apps_tick(200001);
    eq(POSTN("/api/fs/write?path=/int/wrap.bin&offset=4&final=1", chunk, 4), 200,
       "and one millisecond later it is still open");
}

// ==========================================================================
// mkdir, remove, rename
// ==========================================================================

static void t_mutations(void)
{
    eq(POST("/api/fs/mkdir?path=/int/newdir"), 200, "mkdir works");
    ok(has("\"ok\":true"), "and says ok");
    eq(POST("/api/fs/mkdir?path=/int/newdir"), 409, "mkdir of what exists is 409");
    ok(has("exists"), "and says exists");
    eq(POST("/api/fs/mkdir?path=/int/a/b/c"), 404, "mkdir does not create parents");

    put_text("/int/newdir/f.txt", "x");
    eq(POST("/api/fs/remove?path=/int/newdir"), 409, "removing a non-empty directory is 409");
    eq(POST("/api/fs/remove?path=/int/newdir/f.txt"), 200, "removing a file works");
    eq(POST("/api/fs/remove?path=/int/newdir"), 200, "and then the directory does");
    eq(POST("/api/fs/remove?path=/int/newdir"), 404, "removing it twice is 404");

    put_text("/int/ren.txt", "abc");
    eq(POST("/api/fs/rename?from=/int/ren.txt&to=/int/ren2.txt"), 200, "rename works");
    eq(GET("/api/fs/stat?path=/int/ren2.txt"), 200, "the new name is there");
    eq(GET("/api/fs/stat?path=/int/ren.txt"), 404, "and the old one is not");
    eq(POST("/api/fs/rename?from=/int/nope&to=/int/x"), 404, "renaming what is not there is 404");
    eq(POST("/api/fs/rename?from=/int/ren2.txt&to=/sd/x"), 503,
       "renaming onto the absent card is no_such_device");

    // A mutation must not pull the file out from under an open upload.
    {
        char c[8]; memset(c, '7', sizeof c);
        eq(POSTN("/api/fs/write?path=/int/busy.bin&offset=0&final=0", c, 8), 200,
           "an upload is open");
        eq(POST("/api/fs/remove?path=/int/busy.bin"), 409, "removing it is busy");
        eq(POST("/api/fs/rename?from=/int/busy.bin&to=/int/x.bin"), 409, "renaming it is busy");
        eq(POST("/api/fs/rename?from=/int/ren2.txt&to=/int/busy.bin"), 409,
           "and renaming onto it is busy");
        eq(POST("/api/fs/upload/abort?path=/int/busy.bin"), 200, "abort releases it");
        // And there is nothing left at the target to remove: the upload never
        // finalised, so it never landed there in the first place.
        eq(POST("/api/fs/remove?path=/int/busy.bin"), 404,
           "and the target was never created, so there is nothing to remove");
    }
}

// ==========================================================================
// The console
// ==========================================================================

static void t_console_log(void)
{
    char uri[80];
    long next = 0;
    int i;

    eos_apps_init(NULL, NULL);          // a fresh ring, and no ports
    eos_apps_bind_files(&H);

    eq(GET("/api/console/log?since=0"), 200, "an empty ring reads");
    ok(has("\"lines\":[]") && has("\"next\":0") && has("\"dropped\":0"),
       "and says nothing has happened");

    for (i = 0; i < 5; i++) eos_apps_logf('I', "line %d", i);
    eq(GET("/api/console/log?since=0"), 200, "five lines read");
    ok(has("line 0") && has("line 4"), "all of them");
    ok(has("\"next\":5"), "and next is the cursor for the following call");
    ok(has("\"level\":\"I\""), "with a level");

    eq(GET("/api/console/log?since=5"), 200, "reading from the cursor");
    ok(has("\"lines\":[]"), "returns nothing new");
    eos_apps_log('E', "an error");
    eq(GET("/api/console/log?since=5"), 200, "and then returns only what is new");
    ok(has("an error") && !has("line 0"), "exactly the new line");
    ok(has("\"level\":\"E\""), "at its own level");

    // Levels outside the four the web app colours fold to I rather than
    // reaching the page as something it will not render.
    eos_apps_log('Q', "odd level");
    eq(GET("/api/console/log?since=6"), 200, "an unknown level is accepted");
    ok(has("\"level\":\"I\""), "and folded to I");

    // Embedded newlines become separate lines: a multi-line log call must not
    // arrive as one line the pane renders with a literal \n in it.
    eos_apps_log('W', "first\nsecond\r\nthird");
    eq(GET("/api/console/log?since=7"), 200, "a multi-line message reads");
    ok(has("first") && has("second") && has("third"), "as three lines");
    ok(!has("\\n") && !has("\\r"), "with no embedded newline in any of them");

    // The ring drops the oldest and says how many, rather than losing them
    // silently. Two full turns so BOTH pools wrap — the line table and the byte
    // pool run out at different rates and only one of them is the interesting
    // one.
    //
    // Every line is self-describing: "n=<seq> " followed by one repeated
    // character chosen from the sequence number, at a length that varies. That
    // is what makes corruption visible. A ring that evicts a line from the
    // table but leaves its bytes to be overwritten in place still returns the
    // right NUMBER of lines and the right cursor; what it returns is somebody
    // else's text, and only content that can be checked against its own header
    // catches it.
    for (i = 0; i < EOS_APPS_LOG_LINES * 3; i++) {
        char l[EOS_APPS_LOG_TEXT_MAX];
        int o = snprintf(l, sizeof l, "n=%d ", i);
        int len = 20 + (i % 60), k;
        for (k = 0; k < len && o < (int)sizeof l - 1; k++) l[o++] = (char)('a' + i % 26);
        l[o] = '\0';
        eos_apps_log('I', l);
    }
    {
        // Read the whole ring back and check every line against its own header.
        int lines = 0, corrupt = 0;
        long cur = 0;
        for (i = 0; i < 60; i++) {
            const char *m, *p2;
            snprintf(uri, sizeof uri, "/api/console/log?since=%ld&max=100000", cur);
            eq(GET(uri), 200, "the flooded ring pages back");
            if (has("\"lines\":[]")) break;
            for (m = BODY; (m = strstr(m, "\"text\":\"n=")); m++) {
                int n2 = atoi(m + 10), k;
                const char *t = strchr(m + 10, ' ');
                if (!t) { corrupt++; continue; }
                t++;
                lines++;
                for (k = 0; t[k] && t[k] != '"'; k++)
                    if (t[k] != (char)('a' + n2 % 26)) { corrupt++; break; }
            }
            p2 = strstr(BODY, "\"next\":");
            if (!p2) break;
            cur = strtol(p2 + 7, NULL, 10);
        }
        ok(lines >= EOS_APPS_LOG_LINES / 2,
           "the flood left a ring full of readable lines");
        eq(corrupt, 0, "and not one of them carries another line's bytes");
    }
    for (i = 0; i < EOS_APPS_LOG_LINES * 3; i++)
        eos_apps_logf('I', "flood %d aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", i);
    eq(GET("/api/console/log?since=0"), 200, "the flooded ring reads");
    ok(has("\"dropped\":"), "and reports what fell out");
    ok(!has("\"dropped\":0"), "which is not zero after a flood");
    ok(has("flood " ), "and the newest lines are the ones that survived");

    // Whatever it returned, the cursor it hands back must be usable.
    {
        const char *p = strstr(BODY, "\"next\":");
        ok(p != NULL, "the flooded reply carries a cursor");
        if (p) next = strtol(p + 7, NULL, 10);
        for (i = 0; i < 40; i++) {
            snprintf(uri, sizeof uri, "/api/console/log?since=%ld", next);
            eq(GET(uri), 200, "reading from the cursor works");
            if (has("\"lines\":[]")) break;
            p = strstr(BODY, "\"next\":");
            ok(p != NULL, "every reply carries the next cursor");
            if (!p) break;
            ok(strtol(p + 7, NULL, 10) > next, "which always advances");
            next = strtol(p + 7, NULL, 10);
        }
        ok(i < 40, "so following the cursor drains the ring and stops");
    }

    // The byte budget bounds the reply and moves the cursor to where it stopped,
    // so the client picks up exactly there rather than losing a line.
    eq(GET("/api/console/log?since=0&max=40"), 200, "a small max reads");
    ok(BODY_LEN < 400, "and returns a short document");
    {
        const char *p = strstr(BODY, "\"next\":");
        ok(p && strtol(p + 7, NULL, 10) < (long)eos_apps_log_seq(),
           "with a cursor short of the end, so nothing is skipped");
    }

    // A line longer than the ring's line cap is truncated, not dropped, and
    // never overruns.
    {
        char big[EOS_APPS_LOG_TEXT_MAX * 3];
        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        char uri2[80];
        snprintf(uri2, sizeof uri2, "/api/console/log?since=%lu",
                 (unsigned long)eos_apps_log_seq());
        eos_apps_log('I', big);
        eq(GET(uri2), 200, "an over-long line reads back");
        ok(has("xxxx"), "truncated rather than lost");
        ok(BODY_LEN < EOS_APPS_LOG_TEXT_MAX + 120, "at the ring's line cap and no further");
    }
}

static void t_console_exec(void)
{
    eos_apps_ports_t p;
    static const char *const REFUSE[] = {
        "ls", "cat /etc/passwd", "help;reboot", "HELP", "help ", " help",
        "theme gruvbox", "reboot now", "peek 0x40000000", "", "hel", "helpp",
        "wifi\nreboot", "$(reboot)", "../help", "exec", "eval", "read", "write",
    };
    int i, refused = 0;

    memset(&p, 0, sizeof p);
    p.reboot   = fake_reboot;
    p.describe = fake_describe;
    eos_apps_init(&p, NULL);
    eos_apps_bind_files(&H);
    reboot_calls = 0;

    eq(POSTB("/api/console/exec", "{\"cmd\":\"help\"}"), 202, "help is accepted");
    ok(has("\"accepted\":true") && has("\"seq\":"), "and returns the cursor its output starts at");
    eq(GET("/api/console/log?since=0&max=100000"), 200, "the output is in the log");
    ok(has("help") && has("that is the whole table"),
       "and it is the table, said out loud to be the whole of it");

    eq(POSTB("/api/console/exec", "{\"cmd\":\"heap\"}"),   202, "heap runs");
    eq(POSTB("/api/console/exec", "{\"cmd\":\"theme\"}"),  202, "theme runs");
    eq(POSTB("/api/console/exec", "{\"cmd\":\"wifi\"}"),   202, "wifi runs");
    eq(POSTB("/api/console/exec", "{\"cmd\":\"brain\"}"),  202, "brain runs");
    eq(POSTB("/api/console/exec", "{\"cmd\":\"status\"}"), 202, "status runs");
    eq(GET("/api/console/log?since=0&max=100000"), 200, "and all of them said something");
    ok(has("answered by the port"), "through the describe port");
    ok(has("up 0d"), "status reports uptime from the tick clock");
    ok(has("fs /int"), "and the mounts, which this file answers itself");

    eq(reboot_calls, 0, "nothing has rebooted yet");
    eq(POSTB("/api/console/exec", "{\"cmd\":\"reboot\"}"), 202, "reboot is accepted");
    eq(reboot_calls, 1, "and reaches the port exactly once");

    // Everything else. This is the point of the endpoint being a table.
    for (i = 0; i < (int)(sizeof REFUSE / sizeof REFUSE[0]); i++) {
        char body[128];
        snprintf(body, sizeof body, "{\"cmd\":\"%s\"}", REFUSE[i]);
        if (POSTB("/api/console/exec", body) == 400) refused++;
        else printf("FAIL: exec accepted \"%s\"\n", REFUSE[i]);
    }
    eq(refused, (int)(sizeof REFUSE / sizeof REFUSE[0]),
       "every command outside the table is refused");
    eq(reboot_calls, 1, "and none of them rebooted the board");
    ok(has("help status heap reboot theme wifi brain"),
       "the refusal names the whole table, so the person can see it");

    // A refusal is visible in the pane too, not only in the status code.
    eq(GET("/api/console/log?since=0&max=100000"), 200, "the log reads");
    ok(has("refused"), "and the refusals are in it");

    // Malformed bodies.
    eq(POST("/api/console/exec"), 400, "no body is refused");
    eq(POSTB("/api/console/exec", "not json"), 400, "a body that is not JSON is refused");
    eq(POSTB("/api/console/exec", "{\"other\":\"help\"}"), 400, "a body with no cmd is refused");
    eq(POSTB("/api/console/exec", "{\"cmd\":42}"), 400, "a cmd that is not a string is refused");
    eq(POSTB("/api/console/exec",
             "{\"cmd\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}"), 400,
       "a cmd longer than any in the table is refused");
    // The nested-object case eos_json_get_str is documented to step over: a
    // cmd hidden one level down must not be found.
    eq(POSTB("/api/console/exec", "{\"a\":{\"cmd\":\"reboot\"},\"b\":1}"), 400,
       "a cmd nested inside another object is not a cmd");
    eq(reboot_calls, 1, "and still nothing else has rebooted the board");

    // With no ports at all the table still refuses the same things and simply
    // reports that the board cannot answer the ones it cannot.
    eos_apps_init(NULL, NULL);
    eos_apps_bind_files(&H);
    eq(POSTB("/api/console/exec", "{\"cmd\":\"heap\"}"), 202, "heap is still accepted");
    eq(POSTB("/api/console/exec", "{\"cmd\":\"ls\"}"), 400, "and ls is still refused");
    eq(GET("/api/console/log?since=0&max=100000"), 200, "the log reads");
    ok(has("cannot"), "and says the image cannot answer rather than crashing");
}

// ==========================================================================
// The buddy
// ==========================================================================

static void t_buddy(void)
{
    static uint8_t vox[EOS_APPS_VOX_BYTES + 4096];
    int n;

    eos_apps_init(NULL, NULL);
    eos_apps_bind_files(&H);

    // Normal on a fresh board, and the editor is written for it.
    eq(GET("/api/buddy"), 404, "a board with no buddy answers 404");
    ok(has("not_found"), "by name");
    eq(POST("/api/buddy/reload"), 404, "and reloading nothing is 404");

    put_dir("/int/buddy");
    n = build_vox(vox, (int)sizeof vox, 40, 16);
    ok(n > 0, "the test builds a real .vox");
    put_file("/int/buddy/buddy.vox", vox, n);

    eq(POST("/api/buddy/reload"), 200, "a model reloads without a reboot");
    ok(has("\"ok\":true"), "and says so");
    ok(has("\"loaded\":true"), "the model is live");
    ok(has("\"dim\":[16,16,16]"), "with the dimensions out of the file");
    ok(eos_apps_buddy_model() != NULL, "and a renderer can reach it");
    ok(eos_apps_buddy_model()->count > 0 && eos_apps_buddy_model()->count <= 40,
       "with the voxels the file declared, less whatever culling removed");
    ok(eos_apps_buddy_palette() != NULL, "and the palette came with it");
    {
        // The generation is what a renderer on the OS loop watches, and it has
        // to move on a FAILURE as well as on a success. The renderer holds a
        // pointer to the one eos_vox_model_t and re-reads it only when the
        // generation changes, while the parse writes into the one voxel pool
        // and can fail after it has already filled part of it. A generation
        // that stayed put would leave the panel drawing the old count and the
        // old dimensions over the refused file's voxels — a face made of two
        // models, until somebody power-cycled the board. Moving it makes the
        // renderer re-adopt, find no model, and fall back to the compiled-in
        // one.
        uint32_t g = eos_apps_buddy_generation();
        ok(g > 0, "a successful reload moves the generation");
        put_text("/int/buddy/buddy.vox", "rubbish");
        eq(POST("/api/buddy/reload"), 400, "a bad model is refused");
        ok(eos_apps_buddy_generation() > g,
           "and moves the generation too, so the renderer stops drawing the clobbered pool");
        ok(eos_apps_buddy_model() == NULL,
           "and the model reports itself gone, because the parse filled the one pool");
        g = eos_apps_buddy_generation();
        put_file("/int/buddy/buddy.vox", vox, n);
        eq(POST("/api/buddy/reload"), 200, "reloading a good model recovers");
        ok(eos_apps_buddy_generation() > g, "and moves the generation again");
    }

    eq(GET("/api/buddy"), 200, "and then GET works");
    ok(has("\"state\":\"idle\""), "reporting the state as a lowercase name");
    ok(has("\"voxels\":"), "and the voxel count");

    // buddy.json, including the two nested objects eos_json_get_str steps over.
    put_text("/int/buddy/buddy.json",
             "{\"schema_version\":1,\"name\":\"pip\","
             "\"personality\":\"terse, dry, helpful.\","
             "\"accent\":\"#d88e56\","
             "\"idle\":{\"behaviour\":\"curious\",\"sleep_ms\":120000,\"home_yaw\":9},"
             "\"eyes\":{\"open_index\":7,\"shut_index\":3},"
             "\"model\":{\"file\":\"buddy.vox\",\"dim\":[16,16,16],\"voxels\":13}}");
    eq(POST("/api/buddy/reload"), 200, "the config reloads with it");
    ok(has("\"name\":\"pip\""), "the name is read");
    ok(has("\"personality\":\"terse, dry, helpful.\""), "and the personality");
    ok(has("\"accent\":\"#d88e56\""), "and the accent round-trips");
    ok(has("\"behaviour\":\"curious\""), "the nested idle block is reached");
    ok(has("\"home_yaw\":9"), "including its numbers");
    ok(has("\"open_index\":7") && has("\"shut_index\":3"), "and the nested eyes block");
    eq(eos_apps_buddy_behaviour(), EOS_APPS_IDLE_CURIOUS, "the preset is stored");
    eq((long)eos_apps_buddy_accent(), 0xd88e56L, "the accent is a colour, not a string");
    eq((long)eos_apps_buddy_cfg()->home_yaw, 9, "and the cfg maps field for field");
    eq((long)eos_apps_buddy_cfg()->eye_ci, 7, "eyes.open_index -> cfg.eye_ci");
    eq((long)eos_apps_buddy_cfg()->idle_sleep_ms, 120000, "idle.sleep_ms -> cfg.idle_sleep_ms");

    // sleepy is the one preset that maps onto a number this header has.
    put_text("/int/buddy/buddy.json",
             "{\"name\":\"n\",\"idle\":{\"behaviour\":\"sleepy\",\"sleep_ms\":100000}}");
    eq(POST("/api/buddy/reload"), 200, "sleepy loads");
    eq((long)eos_apps_buddy_cfg()->idle_sleep_ms, 50000, "and halves the sleep timer");

    // An unrecognised preset falls back rather than refusing, which is the rule
    // web/README.md sets for a schema the firmware does not know.
    put_text("/int/buddy/buddy.json", "{\"idle\":{\"behaviour\":\"disco\"}}");
    eq(POST("/api/buddy/reload"), 200, "an unknown preset still loads");
    eq(eos_apps_buddy_behaviour(), EOS_APPS_IDLE_WANDER, "and falls back to wander");

    put_text("/int/buddy/buddy.json", "{\"accent\":\"not a colour\",\"name\":\"x\"}");
    eq(POST("/api/buddy/reload"), 200, "a malformed accent does not stop the load");
    eq((long)eos_apps_buddy_accent(), (long)0xFFFFFFFFu, "it is simply absent");
    ok(has("\"accent\":null"), "and is reported as null rather than as garbage");

    // A field that is not an object where an object is expected. obj_span must
    // refuse it rather than pointing the leaf reader at a string, which would
    // read fields out of whatever followed it in the document.
    put_text("/int/buddy/buddy.json",
             "{\"idle\":\"curious\",\"eyes\":[7,3],\"home_yaw\":9,\"open_index\":7}");
    eq(POST("/api/buddy/reload"), 200, "a buddy.json with the wrong shapes still loads");
    eq(eos_apps_buddy_behaviour(), EOS_APPS_IDLE_WANDER,
       "idle as a string is not an idle block");
    {
        // Compared against the compiled-in defaults rather than against zero:
        // the point is that NOTHING in that document was read, not that the
        // fields happen to be zero.
        eos_buddy_cfg_t def;
        eos_buddy_default_cfg(&def);
        eq((long)eos_apps_buddy_cfg()->home_yaw, (long)def.home_yaw,
           "and a top-level home_yaw is not read out of the nested block's place");
        eq((long)eos_apps_buddy_cfg()->eye_ci, (long)def.eye_ci,
           "nor is an eyes array read as an object");
    }

    put_text("/int/buddy/buddy.json", "{ this is not json at all ");
    eq(POST("/api/buddy/reload"), 200, "a buddy.json that is not JSON does not stop the model");
    ok(has("\"loaded\":true"), "the .vox still loaded");

    // A personality past what the megabrain's system prompt can hold is
    // truncated on a UTF-8 boundary, not refused and not cut mid-character.
    {
        char doc[900];
        int i, o;
        o = snprintf(doc, sizeof doc, "{\"personality\":\"");
        for (i = 0; i < 120; i++) o += snprintf(doc + o, sizeof doc - (size_t)o, "\xc3\xa9");
        snprintf(doc + o, sizeof doc - (size_t)o, "\"}");
        put_text("/int/buddy/buddy.json", doc);
        eq(POST("/api/buddy/reload"), 200, "a long personality loads");
        ok((int)strlen(eos_apps_buddy_personality()) < EOS_APPS_BUDDY_PERSONA_MAX,
           "and is truncated to what the prompt holds");
        ok(strlen(eos_apps_buddy_personality()) % 2 == 0,
           "on a UTF-8 boundary, never half a character");
    }

    // Bad models. eos_vox is the thing that says no; this checks the endpoint
    // reports what it said rather than pre-empting it.
    put_text("/int/buddy/buddy.vox", "not a vox file at all");
    eq(POST("/api/buddy/reload"), 400, "a file that is not a .vox is refused");
    ok(has("bad_argument"), "as a bad argument");
    ok(eos_apps_buddy_error() != NULL, "and the parser's own sentence is kept");

    n = build_vox(vox, (int)sizeof vox, 40, 16);
    vox[4] = 99;                                     // a version we refuse to guess at
    put_file("/int/buddy/buddy.vox", vox, n);
    eq(POST("/api/buddy/reload"), 400, "a version the parser refuses is refused");

    n = build_vox(vox, (int)sizeof vox, 40, 16);
    put_file("/int/buddy/buddy.vox", vox, n / 2);    // truncated mid-chunk
    eq(POST("/api/buddy/reload"), 400, "a truncated file is refused, never clamped");

    // Past this board's pool, which is smaller than the format's cap and says so.
    n = build_vox(vox, (int)sizeof vox, EOS_APPS_VOX_VOXELS + 8, 32);
    if (n > 0) {
        put_file("/int/buddy/buddy.vox", vox, n);
        ok(POST("/api/buddy/reload") >= 400, "a model past this board's pool is refused");
        eq(GET("/api/buddy"), 200, "and /api/buddy still answers");
        ok(has("\"voxels\":" ) && has("\"limits\""),
           "carrying the cap so the editor can warn before the evening is spent");
    }

    // The whole round trip the editor actually performs: draw, upload through
    // the ordinary chunked write, then reload. One upload mechanism.
    {
        int off = 0;
        n = build_vox(vox, (int)sizeof vox, 24, 8);
        while (off < n) {
            char uri[128];
            int take = n - off;
            if (take > EOS_APPS_CHUNK_MAX) take = EOS_APPS_CHUNK_MAX;
            snprintf(uri, sizeof uri, "/api/fs/write?path=/int/buddy/buddy.vox&offset=%d&final=%d",
                     off, (off + take >= n) ? 1 : 0);
            eq(POSTN(uri, (const char *)vox + off, take), 200, "each chunk of the model uploads");
            off += take;
        }
        eq(POST("/api/buddy/reload"), 200, "and the board picks it up with no reboot");
        ok(has("\"dim\":[8,8,8]"), "as the model that was just uploaded");
    }
}

// ==========================================================================
// Apps
// ==========================================================================

static void t_apps(void)
{
    static const eos_apps_app_t CAT[] = {
        { "clock", "clock", "uptime in the large face", 0 },
        { "board", "board", NULL,                       0 },
        { "heap",  "heap",  "free and largest block",   0 },
        { "keys",  "keys",  "the compiled-in keymap",   1 },
    };

    eos_apps_set_apps(NULL, 0);
    eq(GET("/api/apps"), 200, "a board with no catalog still answers");
    ok(has("\"apps\":[]"), "with an empty list rather than a 404");

    eos_apps_set_apps(CAT, 4);
    eq(GET("/api/apps"), 200, "the catalog lists");
    ok(has("\"id\":\"clock\""), "the first window is in it");
#if EOS_APPS_CATALOG_MAX >= 4
    ok(has("\"id\":\"keys\""), "and the last one");
    ok(has("\"tier_min\":1"), "with the tier the picker shows");
#else
    ok(true, "the ceiling is below this catalog, so the tail is clamped away");
    ok(true, "and the tier of a clamped-away entry is not reported");
#endif
    ok(has("uptime in the large face"), "and a summary where there is one");
    ok(!has("\"summary\":null"), "and no empty summary where there is not");

    // A catalog longer than the ceiling is clamped to it rather than
    // overflowing the response. The ceiling bounds the reply, not the caller's
    // array, and eos_apps_set_apps() says so.
    {
        static eos_apps_app_t many[EOS_APPS_CATALOG_MAX + 4];
        int i, listed = 0;
        const char *m;
        for (i = 0; i < (int)(sizeof many / sizeof many[0]); i++) {
            many[i].id = "x"; many[i].name = "x"; many[i].summary = NULL;
            many[i].tier_min = 0;
        }
        eos_apps_set_apps(many, (int)(sizeof many / sizeof many[0]));
        eq(GET("/api/apps"), 200, "an over-long catalog still answers");
        for (m = BODY; (m = strstr(m, "\"id\":")); m++) listed++;
        eq(listed, EOS_APPS_CATALOG_MAX, "clamped to the ceiling, not overrun");
    }
    eos_apps_set_apps(CAT, 4);
}

// ==========================================================================
// Routing and the 501 an image without this file answers
// ==========================================================================

static void t_routing(void)
{
    eq(eos_httpd_route("GET",  "/api/fs/list"),  EOS_ROUTE_FS_LIST,  "list routes");
    eq(eos_httpd_route("POST", "/api/fs/write"), EOS_ROUTE_FS_WRITE, "write routes");
    eq(eos_httpd_route("GET",  "/api/fs/write"), EOS_ROUTE_METHOD,   "write is POST only");
    eq(eos_httpd_route("POST", "/api/fs/list"),  EOS_ROUTE_METHOD,   "list is GET only");
    eq(eos_httpd_route("GET",  "/api/fs/upload/abort"), EOS_ROUTE_METHOD,
       "abort is POST only");
    eq(eos_httpd_route("GET",  "/api/fs"),       EOS_ROUTE_NONE,     "a prefix is not a route");
    eq(eos_httpd_route("GET",  "/api/fs/list/x"), EOS_ROUTE_NONE,    "nor is a suffix");
    eq(eos_httpd_route("GET",  "/api/buddy"),    EOS_ROUTE_BUDDY,    "buddy routes");
    eq(eos_httpd_route("POST", "/api/buddy/reload"), EOS_ROUTE_BUDDY_RELOAD, "reload routes");
    eq(eos_httpd_route("GET",  "/api/apps"),     EOS_ROUTE_APPS,     "apps routes");

    // A typo under /api/ is a 404 and never a file, which is what stops a
    // mistyped endpoint from becoming a path traversal surface.
    eq(GET("/api/fs/lst?path=/int"), 404, "a mistyped endpoint is 404");
    ok(RESP.kind != EOS_HTTPD_BODY_FILE, "and never a file");

    // With no eos_apps registered these routes answer 501 rather than failing
    // to link, which is the whole reason the registration is a pointer.
    eos_httpd_set_api(NULL);
    eq(GET("/api/fs/list?path=/int"), 501, "an image without this file answers 501");
    ok(has("unsupported"), "as unsupported");
    eos_httpd_set_api(eos_apps_dispatch);
    eq(GET("/api/fs/list?path=/int"), 200, "and works again once it is back");
}

// ==========================================================================
// Static serving still prefers the file and never serves nothing
// ==========================================================================

static const char EMBEDDED[] = "EMBEDDED-APP-JS";
static int  below_pos, below_open_n;

static void *fake_below_open(void *ctx, const char *path, long *size_out)
{
    const char *base = strrchr(path, '/');
    (void)ctx;
    base = base ? base + 1 : path;
    if (strcmp(base, "app.js") != 0) return NULL;
    below_open_n++;
    below_pos = 0;
    if (size_out) *size_out = (long)sizeof EMBEDDED - 1;
    return &below_pos;
}

static int fake_below_read(void *ctx, void *fh, void *buf, int n)
{
    int left = (int)sizeof EMBEDDED - 1 - below_pos;
    (void)ctx; (void)fh;
    if (left <= 0) return 0;
    if (n > left) n = left;
    memcpy(buf, EMBEDDED + below_pos, (size_t)n);
    below_pos += n;
    return n;
}

static void fake_below_close(void *ctx, void *fh) { (void)ctx; (void)fh; }

static void t_static_fallback(void)
{
    eos_httpd_t s;
    eos_httpd_cfg_t cfg;
    eos_httpd_ports_t ports;
    eos_httpd_req_t rq;
    eos_httpd_resp_t rs;
    char buf[256];
    int n, len = 0;

    memset(&ports, 0, sizeof ports);
    ports.file_open  = fake_below_open;
    ports.file_read  = fake_below_read;
    ports.file_close = fake_below_close;

    eos_httpd_cfg_default(&cfg);
    cfg.mode = EOS_HTTPD_MODE_RUN;
    eos_httpd_init(&s, &ports, NULL, &cfg);
    eos_apps_bind_files(&s);
    // Twice, which is what boot glue that re-binds on a mode change does. If
    // the second call captured this file's own ports as the fallback, the very
    // next request for a path that is not on storage would recurse until the
    // stack was gone — so this line is the check, and the request below is what
    // performs it.
    eos_apps_bind_files(&s);
    ok(s.ports.file_open != NULL, "binding the file ports twice leaves them usable");

    memset(&rq, 0, sizeof rq);
    rq.method = "GET";
    rq.uri    = "/app.js";
    eq(eos_httpd_dispatch(&s, &rq, &rs), 200, "with nothing deployed the embedded copy serves");
    for (;;) {
        n = s.ports.file_read(s.ctx, rs.file, buf + len, (int)sizeof buf - len);
        if (n <= 0) break;
        len += n;
    }
    s.ports.file_close(s.ctx, rs.file);
    ok(len == (int)sizeof EMBEDDED - 1 && memcmp(buf, EMBEDDED, (size_t)len) == 0,
       "and it is the embedded bytes");

    // Deploy a real one and it wins, gzipped, without the board ever being left
    // with nothing to serve in between.
    put_dir("/int/web");
    put_text("/int/web/app.js.gz", "REAL-DEPLOYED-BYTES");
    len = 0;
    eq(eos_httpd_dispatch(&s, &rq, &rs), 200, "a deployed file is served instead");
    ok(rs.content_encoding && strcmp(rs.content_encoding, "gzip") == 0,
       "with Content-Encoding gzip, because the .gz twin is what was found");
    for (;;) {
        n = s.ports.file_read(s.ctx, rs.file, buf + len, (int)sizeof buf - len);
        if (n <= 0) break;
        len += n;
    }
    s.ports.file_close(s.ctx, rs.file);
    ok(len == 19 && memcmp(buf, "REAL-DEPLOYED-BYTES", 19) == 0, "and it is the file's bytes");

    // But /api/fs/read must NOT fall back: a GET for a file that is not there
    // is 404 and never the contents of a same-named asset in the image.
    below_open_n = 0;
    eq(GET("/api/fs/read?path=/int/app.js"), 404,
       "reading a missing file does not serve the embedded one");
    eq(below_open_n, 0, "the fallback was not even consulted");
    // The same board that WILL serve /app.js from the image must still 404 a
    // read of a path that is not there. Both on one server, so the rule is not
    // an accident of which ports happened to be bound.
    {
        eos_httpd_req_t rq2;
        eos_httpd_resp_t rs2;
        memset(&rq2, 0, sizeof rq2);
        rq2.method = "GET"; rq2.uri = "/app.js";
        eq(eos_httpd_dispatch(&H, &rq2, &rs2), 200, "the static route still serves it");
        if (rs2.kind == EOS_HTTPD_BODY_FILE) H.ports.file_close(H.ctx, rs2.file);
        eq(GET("/api/fs/read?path=/app.js"), 404, "and the fs route still refuses it");
    }

    // Handle exhaustion is a reported failure, not a corruption.
    {
        void *fh[EOS_MAX_FILES + 2];
        int i, got = 0;
        put_text("/int/web/pool.txt", "x");
        for (i = 0; i < EOS_MAX_FILES + 2; i++) {
            long sz;
            fh[i] = s.ports.file_open(s.ctx, "/int/web/pool.txt", &sz);
            if (fh[i]) got++;
        }
        ok(got <= EOS_MAX_FILES, "the file slot pool has a ceiling");
        ok(got > 0, "and it is not zero");
        for (i = 0; i < EOS_MAX_FILES + 2; i++)
            if (fh[i]) s.ports.file_close(s.ctx, fh[i]);
        {
            long sz;
            void *again = s.ports.file_open(s.ctx, "/int/web/pool.txt", &sz);
            ok(again != NULL, "and every slot comes back after a close");
            if (again) s.ports.file_close(s.ctx, again);
        }
    }
}

// ==========================================================================
// The fuzz
// ==========================================================================
//
// The named cases above are the paths somebody thought of. This is the half
// that covers what nobody enumerated: query strings assembled from the tokens
// these handlers have a rule about, driven through every route.
//
// The generator is biased on purpose and the bias is ASSERTED rather than
// assumed. A fuzzer over random bytes almost never types "/int", so nearly
// every path it builds dies at "no such mount" and the half that could actually
// escape is never reached. The run prints what it reached and checks it.

static uint32_t rnd_state = 0x13579bdfu;
static uint32_t rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static void t_fuzz(void)
{
    static const char *const TOK[] = {
        "/", "//", "..", ".", "%2e%2e", "\\", "%5c", "int", "sd", "web", "buddy",
        "a", "f00.txt", "%00", "%01", "%7f", "%ff", "..%2f", "tree",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    };
    static const struct { const char *m, *fmt; } R[] = {
        { "GET",  "/api/fs/list?path=%s"                    },
        { "GET",  "/api/fs/stat?path=%s"                    },
        { "GET",  "/api/fs/read?path=%s"                    },
        { "GET",  "/api/fs/usage?point=%s"                  },
        { "POST", "/api/fs/write?path=%s&offset=0&final=1"  },
        { "POST", "/api/fs/mkdir?path=%s"                   },
        { "POST", "/api/fs/remove?path=%s"                  },
        { "POST", "/api/fs/upload/abort?path=%s"            },
        { "POST", "/api/fs/rename?from=%s&to=/int/zz"       },
        { "POST", "/api/fs/rename?from=/int/zz&to=%s"       },
    };
    int ntok = (int)(sizeof TOK / sizeof TOK[0]);
    int nr   = (int)(sizeof R / sizeof R[0]);
    int i, k, bad = 0;
    long accepted = 0, mounts = 0, climbs = 0, deep = 0, files = 0;

    for (i = 0; i < 60000; i++) {
        char p[200], uri[300];
        int o = 0, parts, j;
        bool has_climb = false, real_mount = false;
        int st;

        // Three in four start at a real mount, or the whole run dies early.
        if ((rnd() & 3u) != 0u) { o += snprintf(p + o, sizeof p - (size_t)o, "/int"); real_mount = true; }
        else                     o += snprintf(p + o, sizeof p - (size_t)o, "/");

        parts = (int)(rnd() % 5u) + 1;
        for (j = 0; j < parts && o < (int)sizeof p - 60; j++) {
            const char *t = TOK[rnd() % (uint32_t)ntok];
            if (strcmp(t, "..") == 0 || strcmp(t, "%2e%2e") == 0 ||
                strcmp(t, "..%2f") == 0) has_climb = true;
            if (p[o - 1] != '/' && t[0] != '/')
                o += snprintf(p + o, sizeof p - (size_t)o, "/");
            o += snprintf(p + o, sizeof p - (size_t)o, "%s", t);
        }
        if (parts > 2) deep++;
        if (has_climb) climbs++;
        if (real_mount) mounts++;

        k = (int)(rnd() % (uint32_t)nr);
        snprintf(uri, sizeof uri, R[k].fmt, p);
        st = req(R[k].m, uri, "abc", 3, false);

        if (st >= 200 && st < 300) {
            accepted++;
            // The one invariant that matters, and it is checked on the INPUT
            // rather than on the output: a request carrying a climb must never
            // come back as a success. Checking only the output would miss the
            // wrong fix — folding a/../b into b, whose output looks clean.
            if (has_climb && strcmp(R[k].fmt, "/api/fs/upload/abort?path=%s") != 0) {
                bad++;
                if (bad < 4) printf("FAIL: fuzz accepted a climb: %s %s\n", R[k].m, uri);
            }
        }
        if (st == 200 && RESP.kind == EOS_HTTPD_BODY_FILE) files++;

        // Nothing may ever be staged as a file for a path with a climb in it.
        if (RESP.kind == EOS_HTTPD_BODY_FILE && has_climb) {
            bad++;
            printf("FAIL: fuzz staged a file for a climb: %s\n", uri);
        }
    }

    printf("  [fuzz] accepted=%ld mounts=%ld deep=%ld climbs=%ld files=%ld\n",
           accepted, mounts, deep, climbs, files);
    eq(bad, 0, "no fuzzed request carrying a .. component ever succeeded");

    // The counts are checks and not decoration: a change that made every path
    // fail early would report zero escapes and would be worthless.
    ok(accepted > 500,  "the fuzz reached real successes");
    ok(mounts  > 30000, "and mostly aimed at a real mount");
    ok(climbs  > 5000,  "and typed a climb often enough to matter");
    ok(deep    > 20000, "and built multi-component remainders");
    ok(files   > 0,     "and reached the file-streaming path");

    // And it did not leave the sandbox.
    ok(access("/tmp/eos-apps-escaped", F_OK) != 0, "the fuzz wrote nothing outside the sandbox");
}

// ==========================================================================

int main(void)
{
    eos_httpd_cfg_t cfg;
    eos_httpd_ports_t fallback_ports;
    char env[200];

    snprintf(ROOT, sizeof ROOT, "/tmp/eos-apps-test-%d", (int)getpid());
    rmtree(ROOT);
    if (mkdir(ROOT, 0777) != 0) { printf("cannot create %s\n", ROOT); return 1; }
    snprintf(env, sizeof env, "EOS_STORAGE_HOST_ROOT=%s", ROOT);
    putenv(env);

    if (eos_storage_init() != EOS_OK) { printf("storage init failed\n"); rmtree(ROOT); return 1; }

    eos_httpd_cfg_default(&cfg);
    cfg.mode = EOS_HTTPD_MODE_RUN;
    // The main server carries a fallback, exactly as the board does: without
    // one, "/api/fs/read must not fall back" is a rule with nothing to fall
    // back TO and the check would pass with the rule deleted.
    memset(&fallback_ports, 0, sizeof fallback_ports);
    fallback_ports.file_open  = fake_below_open;
    fallback_ports.file_read  = fake_below_read;
    fallback_ports.file_close = fake_below_close;
    eos_httpd_init(&H, &fallback_ports, NULL, &cfg);
    eos_apps_init(NULL, NULL);
    eos_apps_bind_files(&H);
    eos_apps_tick(0);

    t_routing();
    t_missing_args();
    t_traversal();
    t_path_check_direct();
    t_bounds();
    t_list();
    t_stat_read_usage();
    t_upload();
    t_abort();
    t_upload_timeout();
    t_mutations();
    t_console_log();
    t_console_exec();
    t_buddy();
    t_apps();
    t_static_fallback();
    t_fuzz();

    rmtree(ROOT);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
