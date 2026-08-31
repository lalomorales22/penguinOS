// Host checks for the buddy gallery.
//
// Four things are worth testing here and the rest is bookkeeping.
//
// The slug rules, because a slug becomes a filename and arrives off the
// network. They are driven twice: directly through eos_gallery_slug_ok(), which
// is where a rule can be checked against bytes an HTTP request cannot easily
// carry - an embedded NUL, a lone continuation byte, a 300-byte name - and then
// through the endpoints, because the interesting question is never "is the rule
// right" but "does every route call it before it calls anything else".
//
// The select state machine, because its whole value is an ordering claim: the
// pointer on the filesystem moves only after eos_vox_parse() has accepted the
// model. A test that only checks the happy path would pass with the order
// reversed, so the tests that matter are the ones that select a corrupt model
// and then assert that the previous buddy is still on the panel AND still named
// by the file on disk.
//
// The two refusals, because a gallery you can empty and a gallery that lets you
// delete the model being drawn are both ways of arriving back at the bug this
// component exists to close.
//
// And a listing that survives a bad entry. This is the one that would otherwise
// be found in the worst possible place: an upload that died halfway leaves a
// truncated .json or a truncated .vox behind, and if THAT makes the listing
// 500, the gallery has broken exactly when the owner needs it to recover.
//
// What this cannot check: the flash, and the panel re-adopting the model on the
// next frame. A green run says the naming, the ordering and the refusals hold.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "eos_gallery.h"
#include "waveshare-c6-lcd-13.h"

const eos_board_t *eos_board_get(void) { return &EOS_BOARD; }

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

static void streq(const char *got, const char *want, const char *what)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL: %s: got \"%s\" want \"%s\"\n", what, got ? got : "(null)", want);
    }
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

static eos_httpd_t      H;
static eos_httpd_resp_t RESP;
static char             BODY[EOS_HTTPD_RESP_MAX + 1];
static int              BODY_LEN;

static int req(const char *method, const char *uri, const char *body, int body_len)
{
    eos_httpd_req_t rq;
    int status;

    memset(&rq, 0, sizeof rq);
    rq.method   = method;
    rq.uri      = uri;
    rq.body     = body;
    rq.body_len = body_len;

    status = eos_httpd_dispatch(&H, &rq, &RESP);

    BODY_LEN = 0;
    BODY[0]  = '\0';
    if (RESP.kind == EOS_HTTPD_BODY_FILE) {
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

static int GET(const char *uri) { return req("GET", uri, NULL, 0); }
static int POSTB(const char *uri, const char *body)
{
    return req("POST", uri, body, (int)strlen(body));
}
static int POSTN(const char *uri, const char *body, int n)
{
    return req("POST", uri, body, n);
}

static bool has(const char *needle) { return strstr(BODY, needle) != NULL; }

// The live model's x dimension, or -1 when nothing is loaded. Every check on
// the panel goes through this rather than through eos_apps_buddy_model()->sx
// directly, and that is not fussiness: the interesting failures in this file
// are exactly the ones where a broken gallery leaves NO model loaded, and a
// suite that segfaults there reports nothing at all instead of naming the four
// checks that would have told you which promise broke.
static long live_dim_x(void)
{
    const eos_vox_model_t *m = eos_apps_buddy_model();
    return m ? (long)m->sx : -1;
}


// select/remove with a slug spliced into a JSON body, which is how every
// hostile name below reaches the board.
static int select_slug(const char *slug)
{
    char body[512];
    snprintf(body, sizeof body, "{\"slug\":\"%s\"}", slug);
    return POSTB("/api/buddy/gallery/select", body);
}

static int remove_slug(const char *slug)
{
    char body[512];
    snprintf(body, sizeof body, "{\"slug\":\"%s\"}", slug);
    return POSTB("/api/buddy/gallery/remove", body);
}

// ==========================================================================
// Building files
// ==========================================================================

static void host_path(const char *path, char *out, int cap)
{
    snprintf(out, (size_t)cap, "%s%s", ROOT, path + 4);   // strip "/int"
}

static void put_file(const char *path, const void *data, int n)
{
    char sys[512];
    FILE *f;

    host_path(path, sys, (int)sizeof sys);
    f = fopen(sys, "wb");
    if (!f) { printf("FAIL: could not stage %s\n", path); failed++; return; }
    if (n > 0) fwrite(data, 1, (size_t)n, f);
    fclose(f);
}

static void put_text(const char *path, const char *s) { put_file(path, s, (int)strlen(s)); }

static void put_dir(const char *path)
{
    char sys[512];
    host_path(path, sys, (int)sizeof sys);
    mkdir(sys, 0777);
}

static void drop(const char *path)
{
    char sys[512];
    host_path(path, sys, (int)sizeof sys);
    unlink(sys);
}

static bool there(const char *path)
{
    char sys[512];
    host_path(path, sys, (int)sizeof sys);
    return access(sys, F_OK) == 0;
}

static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// A real MagicaVoxel file of the subset eos_vox_parse() reads, same shape as
// the one kernel/svc/test/test_apps.c builds.
static int build_vox_xyz(uint8_t *out, int cap, int nvox, int dx, int dy, int dz)
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
    w32(out + p, (uint32_t)dx);       p += 4;
    w32(out + p, (uint32_t)dy);       p += 4;
    w32(out + p, (uint32_t)dz);       p += 4;

    memcpy(out + p, "XYZI", 4);       p += 4;
    w32(out + p, xyzi);               p += 4;
    w32(out + p, 0);                  p += 4;
    w32(out + p, (uint32_t)nvox);     p += 4;
    for (i = 0; i < nvox; i++) {
        out[p++] = (uint8_t)(i % dx);
        out[p++] = (uint8_t)((i / dx) % dy);
        out[p++] = (uint8_t)((i / (dx * dy)) % dz);
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

static int build_vox(uint8_t *out, int cap, int nvox, int dim)
{
    return build_vox_xyz(out, cap, nvox, dim, dim, dim);
}

static uint8_t VOXBUF[EOS_APPS_VOX_BYTES + 4096];
static int     VOXLEN;

// Stages one gallery entry with a real model and a real metadata document.
static void put_entry(const char *slug, const char *name, int nvox, int dim)
{
    char path[128];
    char json[512];
    int n = build_vox(VOXBUF, (int)sizeof VOXBUF, nvox, dim);

    ok(n > 0, "the test builds a real .vox");
    snprintf(path, sizeof path, EOS_GALLERY_DIR "/%s.vox", slug);
    put_file(path, VOXBUF, n);
    snprintf(path, sizeof path, EOS_GALLERY_DIR "/%s.json", slug);
    snprintf(json, sizeof json, "{\"schema_version\":1,\"name\":\"%s\"}", name);
    put_text(path, json);
}

// Back to a known board: a gallery holding pip and pig, pip active and loaded.
static void reset_board(void)
{
    char sys[512];

    host_path(EOS_APPS_BUDDY_DIR, sys, (int)sizeof sys);
    rmtree(sys);
    put_dir(EOS_APPS_BUDDY_DIR);
    put_dir(EOS_GALLERY_DIR);
    put_entry("pip", "Pip", 40, 16);
    put_entry("pig", "Pig", 24, 12);
    put_text(EOS_GALLERY_ACTIVE, "pip\n");

    eos_apps_init(NULL, NULL);
    eos_apps_bind_files(&H);
    eq(eos_apps_buddy_reload(), EOS_OK, "the board comes up wearing the active buddy");
}

// ==========================================================================
// Slugs, directly
// ==========================================================================
//
// Driven through eos_gallery_slug_ok() and not only through the endpoints,
// because the bytes that matter most cannot be spelled in a JSON string this
// test writes by hand. A NUL inside a name, a name of three hundred bytes and a
// bare 0x80 all have to be handed over as (pointer, length).

static void t_slug_rules(void)
{
    static const char *const GOOD[] = {
        "pip", "a", "pig", "buddy-2", "0", "z9", "a-b-c",
        "penguin-in-a-hat", "0123456789", "------",
        "abcdefghijklmnopqrstuvwx",            // exactly EOS_GALLERY_SLUG_MAX
    };
    // Everything that has ever walked out of a filename taken off a network,
    // plus the shapes eos_storage's own suite pins.
    static const char *const BAD[] = {
        "", ".", "..", "...", "./x", "../x", "x/..", "a/b", "/pip", "pip/",
        "..\\x", "a\\b", "\\", "/", "//", "./.", "../../etc/passwd",
        "%2e%2e", "%2E%2E%2Fetc", "..%2f..", "%00", "..;/", "....//",
        "Pip", "PIP", "pIp",                    // upper case is not in the set
        "pip.vox", "pip.json", "pip.vox.part", "a.b",
        "pip ", " pip", "pi p", "pip\t", "pip\n", "pip\r",
        "pip;rm", "pip|x", "pip&x", "pip$x", "pip`x", "pip'x", "pip\"x",
        "pip*", "pip?", "pip[", "pip:x", "~pip", "#pip", "pip%", "pip+x",
        "pip_x", "pip=x", "pip,x", "pip<x", "pip>x", "pip@x", "pip!x",
        "\xc3\xa9", "p\xc3\xafp", "\xd0\xb0",   // e-acute, i-diaeresis, Cyrillic a
        "\x80", "\xff", "\x7f",
        "abcdefghijklmnopqrstuvwxy",            // one past EOS_GALLERY_SLUG_MAX
    };
    char big[300];
    int i;

    for (i = 0; i < (int)(sizeof GOOD / sizeof GOOD[0]); i++) {
        checks++;
        if (!eos_gallery_slug_ok(GOOD[i], -1)) {
            failed++;
            printf("FAIL: a legal slug was refused: \"%s\"\n", GOOD[i]);
        }
    }
    for (i = 0; i < (int)(sizeof BAD / sizeof BAD[0]); i++) {
        checks++;
        if (eos_gallery_slug_ok(BAD[i], -1)) {
            failed++;
            printf("FAIL: an illegal slug was accepted: \"%s\"\n", BAD[i]);
        }
    }

    // The length-delimited cases, which is the reason the function takes a
    // length at all. A checker that took a C string would see "pip" here and
    // wave through a name whose remaining bytes go into a filename.
    ok(!eos_gallery_slug_ok("pip\0evil", 8), "a NUL inside a slug is refused");
    ok(!eos_gallery_slug_ok("\0", 1), "a slug that is only a NUL is refused");
    ok(!eos_gallery_slug_ok("pip", 0), "a zero length is refused whatever the bytes say");
    ok(!eos_gallery_slug_ok("..", 2), "dot dot is refused as bytes, not just as a string");
    ok( eos_gallery_slug_ok("pip.vox", 3), "and a length shorter than the buffer is honoured");
    ok(!eos_gallery_slug_ok(NULL, -1), "a NULL slug is refused rather than dereferenced");

    memset(big, 'a', sizeof big);
    ok(!eos_gallery_slug_ok(big, (int)sizeof big), "a 300 byte slug is refused");
    ok(!eos_gallery_slug_ok(big, EOS_GALLERY_SLUG_MAX + 1),
       "and so is one byte past the cap");
    ok( eos_gallery_slug_ok(big, EOS_GALLERY_SLUG_MAX),
       "while the cap itself is legal, so the number in the message is the real one");

    // A path is only ever built from a slug that passed, and the two must agree
    // or the check is decoration.
    {
        char vox[EOS_PATH_MAX], json[EOS_PATH_MAX];
        eq(eos_gallery_paths("..", vox, json, EOS_PATH_MAX), EOS_ERR_ARG,
           "eos_gallery_paths refuses to build a path out of dot dot");
        eq(eos_gallery_paths("a/b", vox, json, EOS_PATH_MAX), EOS_ERR_ARG,
           "or out of anything with a separator in it");
        eq(eos_gallery_paths("pip", vox, json, EOS_PATH_MAX), EOS_OK,
           "and builds one out of a real slug");
        streq(vox,  EOS_GALLERY_DIR "/pip.vox",  "the model path");
        streq(json, EOS_GALLERY_DIR "/pip.json", "and the metadata beside it");
        eq(eos_gallery_paths("pip", vox, json, 8), EOS_ERR_ARG,
           "a buffer smaller than EOS_PATH_MAX is refused rather than filled short");
    }
}

// The same corpus again, through the endpoints. The rule being checked is not
// "is the slug rule right" - the block above did that - it is "does the route
// consult it before it consults the filesystem".
static void t_slug_over_http(void)
{
    static const char *const HOSTILE[] = {
        "..", ".", "../pip", "..%2f..", "%2e%2e", "/int/buddy/gallery/pip",
        "pip/../../../etc/passwd", "a\\\\b", "PIP", "pip.vox", "",
        "abcdefghijklmnopqrstuvwxyz0123456789",
    };
    int i;

    reset_board();

    for (i = 0; i < (int)(sizeof HOSTILE / sizeof HOSTILE[0]); i++) {
        eq(select_slug(HOSTILE[i]), 400, "select refuses a hostile slug");
        ok(has("bad_argument"), "by name");
        eq(remove_slug(HOSTILE[i]), 400, "and so does remove");
    }

    // And after all of that the board is exactly where it started.
    streq(eos_apps_buddy_slug(), "pip", "no hostile slug changed the live buddy");
    ok(there(EOS_GALLERY_DIR "/pip.vox"), "and nothing was deleted");
    ok(there(EOS_GALLERY_DIR "/pig.vox"), "including the entry that was not named");
    ok(!there("/int/buddy/gallery/passwd"), "and nothing was created outside the gallery");

    // The shapes a body can be wrong in, which are not slug problems but arrive
    // at the same door.
    eq(POSTB("/api/buddy/gallery/select", ""), 400, "an empty body is refused");
    eq(POSTB("/api/buddy/gallery/select", "{}"), 400, "so is a body with no slug");
    eq(POSTB("/api/buddy/gallery/select", "{\"slug\":7}"), 400, "a slug that is a number");
    eq(POSTB("/api/buddy/gallery/select", "{\"slug\":null}"), 400, "a slug that is null");
    eq(POSTB("/api/buddy/gallery/select", "{\"slug\":[\"pip\"]}"), 400,
       "a slug that is an array");
    eq(POSTB("/api/buddy/gallery/select", "{ not json"), 400, "a body that is not JSON");
    eq(POSTB("/api/buddy/gallery/select", "{\"slug\":\"pi\\u0000p\"}"), 400,
       "and an escaped NUL, which is the one that reaches a filename as two names");
    {
        // The decoder turns escapes into UTF-8, so a slug that was ASCII on the
        // wire is not necessarily ASCII by the time it is a filename. This is
        // the check that the rule is applied AFTER decoding.
        eq(POSTB("/api/buddy/gallery/select", "{\"slug\":\"p\\u00efp\"}"), 400,
           "a slug that only becomes non-ASCII after \\u decoding");
        eq(POSTB("/api/buddy/gallery/select", "{\"slug\":\"\\u002e\\u002e\"}"), 400,
           "and a dot dot spelled in escapes");
    }
    {
        // A NUL in the raw body bytes rather than in an escape.
        static const char body[] = "{\"slug\":\"pip\"}";
        char b2[64];
        memcpy(b2, body, sizeof body);
        b2[10] = '\0';
        eq(POSTN("/api/buddy/gallery/select", b2, (int)sizeof body - 1), 400,
           "a raw NUL inside the body's slug is refused");
    }
    streq(eos_apps_buddy_slug(), "pip", "and none of those moved the buddy either");
}

// ==========================================================================
// Routing
// ==========================================================================

static void t_routing(void)
{
    reset_board();

    eq(GET("/api/buddy/gallery"), 200, "GET /api/buddy/gallery lists");
    eq(req("POST", "/api/buddy/gallery", NULL, 0), 405,
       "and POSTing to it is a method error, not a 404");
    eq(GET("/api/buddy/gallery/select"), 405, "select is a POST");
    eq(GET("/api/buddy/gallery/remove"), 405, "and so is remove");
    eq(GET("/api/buddy/galler"), 404, "a typo under /api/ is a 404 and never a file");
    eq(GET("/api/buddy/gallery/"), 404, "a trailing slash is not the same route");
    eq(GET("/api/buddy/gallery/select/x"), 404, "nor is a longer path");

    // The query string is not where the slug lives, and a select that read one
    // would be a mutation reachable from a link.
    eq(GET("/api/buddy/gallery?offset=0&count=1"), 200,
       "the listing takes the same paging parameters /api/fs/list does");
    eq(GET("/api/buddy/gallery?offset=x"), 400, "a non-numeric offset is refused");
    eq(GET("/api/buddy/gallery?count=9999"), 400, "and a count past the ceiling");
}

// ==========================================================================
// Listing
// ==========================================================================

static void t_list(void)
{
    reset_board();

    eq(GET("/api/buddy/gallery"), 200, "the gallery lists");
    ok(has("\"dir\":\"" EOS_GALLERY_DIR "\""), "and says where it lives");
    ok(has("\"slug\":\"pip\""), "pip is in it");
    ok(has("\"slug\":\"pig\""), "and so is pig");
    ok(has("\"name\":\"Pip\""), "the name comes out of the entry's own json");
    ok(has("\"name\":\"Pig\""), "one per model, not one for the board");
    ok(has("\"active\":\"pip\""), "the live slug is reported at the top level");
    ok(has("\"total\":2"), "and the total counts what is there");
    ok(has("\"more\":false"), "with nothing left over");
    ok(has("\"dim\":[16,16,16]"), "the dimensions come out of the file's own SIZE chunk");
    ok(has("\"voxels\":40"), "and the count out of its XYZI, before culling");
    ok(has("\"ok\":true"), "a readable header says so");

    // /api/buddy reports the same model by path, which is what a web app has to
    // read now that the filename is no longer a constant.
    eq(GET("/api/buddy"), 200, "and /api/buddy answers");
    ok(has("\"slug\":\"pip\""), "naming the gallery entry it is wearing");
    ok(has("\"path\":\"" EOS_GALLERY_DIR "/pip.vox\""), "and the path it read");
    ok(has("\"file\":\"pip.vox\""), "with the basename kept for the old client");

    // The peek and the parser have to agree, or one of the two numbers on the
    // owner's screen is a lie. This is the check that keeps a header reader
    // that never touches a voxel pool honest against the thing that does.
    //
    // The model deliberately has THREE DIFFERENT dimensions. A cube would let
    // the peek report the axes in any order it liked and every one of these
    // checks would still pass, which is the sort of test that reads as thorough
    // and pins nothing - and getting the axes wrong is a real failure mode,
    // because .vox is z-up and half the tools that write it are not.
    {
        eos_gallery_peek_t pk;
        int n = build_vox_xyz(VOXBUF, (int)sizeof VOXBUF, 40, 11, 9, 15);
        ok(n > 0, "the test builds a model that is not a cube");
        put_file(EOS_GALLERY_DIR "/odd.vox", VOXBUF, n);

        eos_gallery_peek(EOS_GALLERY_DIR "/odd.vox", &pk);
        ok(pk.ok, "the peek read the header");
        eq(pk.sx, 11, "and agrees with eos_vox_parse about x");
        eq(pk.sy,  9, "and y, which a cube could never have told us");
        eq(pk.sz, 15, "and z");
        eq(pk.voxels, 40, "and about how many voxels the file declares");

        eq(select_slug("odd"), 200, "and the parser takes the same file");
        {
            const eos_vox_model_t *m = eos_apps_buddy_model();
            ok(m != NULL, "holding the model");
            if (m) {
                eq(m->sx, pk.sx, "at the x the peek reported");
                eq(m->sy, pk.sy, "the same y");
                eq(m->sz, pk.sz, "and the same z, so the two readers agree axis for axis");
                ok(m->count > 0 && m->count <= 40,
                   "with a count at or below the file's, because finish() drops the buried");
            }
        }
        eq(GET("/api/buddy/gallery"), 200, "and the listing reports those dimensions");
        ok(has("\"dim\":[11,9,15]"), "in the file's own axis order");
        eq(select_slug("pip"), 200, "put pip back");
        drop(EOS_GALLERY_DIR "/odd.vox");
    }

    // Paging, exactly as /api/fs/list does it.
    eq(GET("/api/buddy/gallery?count=1"), 200, "a short page is legal");
    ok(has("\"total\":2"), "the total still counts everything");
    ok(has("\"more\":true"), "and says there is another page");
    eq(GET("/api/buddy/gallery?offset=2"), 200, "an offset past the end is not an error");
    ok(has("\"entries\":[]"), "it is simply empty");
    ok(has("\"more\":false"), "with nothing more to fetch");

    // The two files that WILL turn up in this directory and must not become
    // entries: the metadata beside each model, and a dead upload's partial.
    put_text(EOS_GALLERY_DIR "/pip.vox.part", "half an upload");
    put_text(EOS_GALLERY_DIR "/notes.txt", "hello");
    put_dir(EOS_GALLERY_DIR "/subdir");
    eq(GET("/api/buddy/gallery"), 200, "the listing still answers");
    ok(has("\"total\":2"), "an abandoned .part, a stray file and a directory are not buddies");
    ok(!has("pip.vox.part"), "and the partial is not named as one");
    drop(EOS_GALLERY_DIR "/pip.vox.part");
    drop(EOS_GALLERY_DIR "/notes.txt");

    // A gallery directory that is not there yet is EMPTY, not broken. That is
    // the state of a board whose seeder has not run, and a 404 would make the
    // Buddy tab look broken on a board that is merely new.
    {
        char sys[512];
        host_path(EOS_GALLERY_DIR, sys, (int)sizeof sys);
        rmtree(sys);
        eq(GET("/api/buddy/gallery"), 200, "a board with no gallery directory lists");
        ok(has("\"entries\":[]"), "as an empty gallery");
        ok(has("\"total\":0"), "with nothing in it");
    }
}

// The requirement with the sharpest edge. The thing that produces a truncated
// .json or a truncated .vox is an upload that died, and an upload that died is
// exactly when the owner opens the gallery to pick something else. If one bad
// entry takes the whole listing down, the recovery path is gone at the moment
// it is needed.
static void t_list_survives_damage(void)
{
    char big[EOS_APPS_BUDDY_JSON_BYTES + 256];

    reset_board();
    put_entry("gus", "Gus", 30, 8);

    // A .json truncated mid-string, which is what a dropped connection leaves.
    put_text(EOS_GALLERY_DIR "/pig.json", "{\"name\":\"Pi");
    eq(GET("/api/buddy/gallery"), 200, "a truncated entry json does not fail the list");
    ok(has("\"total\":3"), "every entry is still counted");
    ok(has("\"slug\":\"pig\""), "including the damaged one");
    ok(has("\"name\":\"pig\""), "which falls back to its slug for a name");
    ok(has("\"name\":\"Gus\""), "while its neighbours keep theirs");

    // Every other way a metadata document can be wrong.
    put_text(EOS_GALLERY_DIR "/pig.json", "");
    eq(GET("/api/buddy/gallery"), 200, "an empty entry json is survivable");
    ok(has("\"name\":\"pig\""), "and falls back");
    put_text(EOS_GALLERY_DIR "/pig.json", "not json at all");
    eq(GET("/api/buddy/gallery"), 200, "so is one that is not JSON");
    put_text(EOS_GALLERY_DIR "/pig.json", "[1,2,3]");
    eq(GET("/api/buddy/gallery"), 200, "so is one that is an array");
    put_text(EOS_GALLERY_DIR "/pig.json", "{\"name\":{\"first\":\"Pig\"}}");
    eq(GET("/api/buddy/gallery"), 200, "so is a name that is an object");
    ok(has("\"name\":\"pig\""), "and it falls back rather than emitting the object");
    put_text(EOS_GALLERY_DIR "/pig.json", "{\"name\":\"\"}");
    eq(GET("/api/buddy/gallery"), 200, "so is an empty name");
    ok(has("\"name\":\"pig\""), "which also falls back, because a nameless row is useless");

    // A document larger than the board stages. It must fall back rather than
    // being read half-way, which would show a name the loader will never use.
    memset(big, 'x', sizeof big);
    big[0] = '{'; memcpy(big + 1, "\"name\":\"", 8);
    big[sizeof big - 3] = '"'; big[sizeof big - 2] = '}'; big[sizeof big - 1] = '\0';
    put_text(EOS_GALLERY_DIR "/pig.json", big);
    eq(GET("/api/buddy/gallery"), 200, "an oversized entry json is survivable");
    ok(has("\"name\":\"pig\""), "and falls back to the slug");

    // And a .vox that is damaged rather than its metadata. The row must still
    // be there - it is the only way the owner can see it to delete it.
    put_text(EOS_GALLERY_DIR "/pig.vox", "rubbish");
    eq(GET("/api/buddy/gallery"), 200, "a corrupt model does not fail the list");
    ok(has("\"slug\":\"pig\""), "the entry is still listed");
    ok(has("\"ok\":false"), "flagged as one this board could not read the header of");
    ok(has("\"total\":3"), "and still counted");

    put_file(EOS_GALLERY_DIR "/pig.vox", "", 0);
    eq(GET("/api/buddy/gallery"), 200, "and a zero byte model does not either");
    ok(has("\"ok\":false"), "also flagged");

    // A .vox truncated in the middle of its own header, which is the shape a
    // chunked upload leaves when it dies between chunks.
    {
        int n = build_vox(VOXBUF, (int)sizeof VOXBUF, 30, 8);
        ok(n > 40, "the test built something long enough to cut");
        put_file(EOS_GALLERY_DIR "/pig.vox", VOXBUF, 30);
        eq(GET("/api/buddy/gallery"), 200, "a model cut inside its header lists");
        ok(has("\"ok\":false"), "as unreadable");
        put_file(EOS_GALLERY_DIR "/pig.vox", VOXBUF, n - 100);
        eq(GET("/api/buddy/gallery"), 200, "and one cut inside its voxels lists too");
        // The header IS intact here, so the peek reports numbers the parser will
        // refuse. That is the honest answer: the peek reads a header and the
        // parser reads a file, and select is where the file gets the last word.
        ok(has("\"slug\":\"pig\""), "with its numbers, which select is free to overrule");
        eq(select_slug("pig"), 400, "and select does overrule them");
    }
}

// ==========================================================================
// Selecting
// ==========================================================================

static void t_select(void)
{
    char act[64];

    reset_board();
    streq(eos_apps_buddy_slug(), "pip", "the board starts on pip");

    eq(select_slug("pig"), 200, "selecting another buddy works");
    ok(has("\"ok\":true"), "and says so");
    ok(has("\"active\":\"pig\""), "naming the new one");
    ok(has("\"loaded\":true"), "with a model actually loaded");
    ok(has("\"dim\":[12,12,12]"), "at the new model's dimensions, so the panel changed");
    streq(eos_apps_buddy_slug(), "pig", "the live slug moved");
    eq(eos_gallery_active(act, (int)sizeof act), 3, "and the pointer on the filesystem");
    streq(act, "pig", "names the same one, so it survives a reboot");

    // Live, with no reboot: the generation is what the renderer watches.
    {
        uint32_t g = eos_apps_buddy_generation();
        eq(select_slug("pip"), 200, "and back again");
        ok(eos_apps_buddy_generation() > g,
           "the generation moved, which is how the panel re-adopts without a reboot");
        eq(live_dim_x(), 16, "and the model on the panel is the new one");
    }

    eq(select_slug("nobody"), 404, "selecting a buddy that is not there is a 404");
    ok(has("not_found"), "by name");
    streq(eos_apps_buddy_slug(), "pip", "and changes nothing");

    // Selecting what is already live is a no-op that succeeds. The alternative
    // is a UI that has to know what it already chose.
    eq(select_slug("pip"), 200, "selecting the live buddy again is fine");
    streq(eos_apps_buddy_slug(), "pip", "and leaves it live");
}

// The ordering claim, which is the whole reason this component has a state
// machine rather than two writes. A test that only selected good models would
// pass with the order reversed and the owner would find out on the next reboot.
static void t_select_refuses_a_bad_model(void)
{
    char act[64];
    uint32_t gen;

    reset_board();
    eq(select_slug("pig"), 200, "start somewhere other than the default");
    streq(eos_apps_buddy_slug(), "pig", "which is pig");

    put_text(EOS_GALLERY_DIR "/gus.vox", "this is not a vox file");
    put_text(EOS_GALLERY_DIR "/gus.json", "{\"name\":\"Gus\"}");

    gen = eos_apps_buddy_generation();
    eq(select_slug("gus"), 400, "a model eos_vox_parse refuses is refused");
    ok(has("bad_argument"), "by name");

    // The three things that must be true afterwards, and they are three
    // separate claims. What the renderer draws, what the API reports, and what
    // the filesystem will still say after a power cut.
    streq(eos_apps_buddy_slug(), "pig", "the previous buddy is still the live one");
    ok(eos_apps_buddy_model() != NULL, "and is still loaded, not left as nothing");
    eq(live_dim_x(), 12, "at pig's dimensions, so the panel never moved");
    eq(eos_gallery_active(act, (int)sizeof act), 3, "the pointer was never written");
    streq(act, "pig", "and still names the buddy that works");
    ok(eos_apps_buddy_generation() > gen,
       "the generation still moved, because the refused parse used the one voxel pool");

    // The same for a model that is well-formed but past what this board holds:
    // a different refusal, the same promise.
    {
        int n = build_vox(VOXBUF, (int)sizeof VOXBUF, EOS_APPS_VOX_VOXELS + 200, 32);
        ok(n > 0, "the test built a model larger than this board's pool");
        put_file(EOS_GALLERY_DIR "/huge.vox", VOXBUF, n);
        eq(select_slug("huge"), 413, "a model past what the board stages is refused");

        // The SENTENCE and not only the code, because this is the one refusal
        // whose generic wording actively misdirects. The catch-all below says
        // the model was loadable and the board could not record the choice;
        // for an oversized model both halves are false, and an owner reading it
        // goes looking at a full filesystem instead of at the model they just
        // built. A .vox arrives here through /api/fs/write, which caps a chunk
        // and not a file, so this is an ordinary mistake and not a corner.
        ok(!has("could not record the choice"),
           "an oversized model is not blamed on the pointer");
        ok(has("larger than"), "the refusal says the model is too large");
        ok(has("7264"), "and names the number the board actually stages");

        streq(eos_apps_buddy_slug(), "pig", "and still costs nothing");
        eq(eos_gallery_active(act, (int)sizeof act), 3, "with the pointer untouched");
        streq(act, "pig", "still naming pig");
    }

    // And a zero-byte model, which is what an upload that died on its first
    // chunk used to leave behind.
    put_file(EOS_GALLERY_DIR "/gus.vox", "", 0);
    eq(select_slug("gus"), 400, "an empty model is refused");
    streq(eos_apps_buddy_slug(), "pig", "and pig is still on the panel");

    // Recovery: the buddy that does parse still selects afterwards, so a bad
    // import does not leave the gallery in a state you have to reboot out of.
    eq(select_slug("pip"), 200, "a good model still selects after all of that");
    streq(eos_apps_buddy_slug(), "pip", "and becomes live");
}

// ==========================================================================
// Removing
// ==========================================================================

static void t_remove(void)
{
    reset_board();
    put_entry("gus", "Gus", 30, 8);

    eq(remove_slug("gus"), 200, "an entry that is neither live nor last is removed");
    ok(has("\"ok\":true"), "and says so");
    ok(has("\"removed\":\"gus\""), "naming what went");
    ok(!there(EOS_GALLERY_DIR "/gus.vox"), "the model is gone");
    ok(!there(EOS_GALLERY_DIR "/gus.json"), "and its metadata went with it");
    eq(GET("/api/buddy/gallery"), 200, "and the listing agrees");
    ok(has("\"total\":2"), "two left");

    eq(remove_slug("gus"), 404, "removing it twice is a 404, not a silent success");
    eq(remove_slug("nobody"), 404, "and so is one that never existed");

    // Deleting out from under an upload. /api/fs/remove refuses this and the
    // gallery has to refuse it the same way, or the two delete paths disagree:
    // the upload writes to <path>.part and its closing rename would put the
    // model back with its metadata already deleted, leaving a buddy that has
    // lost the sentence the owner wrote about it.
    {
        int n = build_vox(VOXBUF, (int)sizeof VOXBUF, 30, 8);
        ok(n > 400, "the test built a model big enough to upload in pieces");

        put_entry("half", "Half", 30, 8);
        eq(req("POST",
               "/api/fs/write?path=%2Fint%2Fbuddy%2Fgallery%2Fhalf.vox&offset=0&final=0",
               (const char *)VOXBUF, 400), 200, "an upload to half.vox is open");

        eq(remove_slug("half"), 409, "and the entry it is landing on will not delete");
        ok(has("\"busy\""), "as a busy, the same code /api/fs/remove uses");
        ok(there(EOS_GALLERY_DIR "/half.vox"), "the model is still there");
        ok(there(EOS_GALLERY_DIR "/half.json"), "and so is its metadata");

        eq(POSTB("/api/fs/upload/abort?path=%2Fint%2Fbuddy%2Fgallery%2Fhalf.vox", ""), 200,
           "abort the upload");
        eq(remove_slug("half"), 200, "and now the same delete goes through");
        ok(!there(EOS_GALLERY_DIR "/half.vox"), "taking the model");
        ok(!there(EOS_GALLERY_DIR "/half.json"), "and the metadata with it");
    }
}

static void t_remove_refuses_the_active(void)
{
    reset_board();
    streq(eos_apps_buddy_slug(), "pip", "pip is live");

    eq(remove_slug("pip"), 409, "the live buddy cannot be deleted");
    ok(has("\"busy\""), "which is a busy, not a bad argument");
    ok(!has("\"state\""), "and not the last-one refusal wearing the wrong sentence");
    ok(there(EOS_GALLERY_DIR "/pip.vox"), "and he is still there");
    ok(eos_apps_buddy_model() != NULL, "and still on the panel");

    // Select away and the same delete is fine, which is the whole shape of the
    // refusal: it is an ordering rule, not a prohibition.
    eq(select_slug("pig"), 200, "select something else");
    eq(remove_slug("pip"), 200, "and now the old one can go");
    ok(!there(EOS_GALLERY_DIR "/pip.vox"), "and it went");

    // The other half of the two questions eos_gallery_remove asks. Here the
    // pointer file has gone missing while a model is loaded, so only the LIVE
    // slug can protect the model on the panel.
    reset_board();
    drop(EOS_GALLERY_ACTIVE);
    eq(remove_slug("pip"), 409,
       "the buddy on the panel is protected even with no pointer on the filesystem");
    ok(there(EOS_GALLERY_DIR "/pip.vox"), "and survives");
}

static void t_remove_refuses_the_last(void)
{
    char sys[512];

    reset_board();

    eq(remove_slug("pig"), 200, "one of the two goes");
    eq(GET("/api/buddy/gallery"), 200, "leaving one");
    ok(has("\"total\":1"), "just the one");
    eq(remove_slug("pip"), 409, "and the last one will not go");

    // The last entry is normally ALSO the live one, so a board in that state
    // proves nothing: the live rule answers first and the last rule could be
    // deleted without a single check noticing. Isolating it needs a gallery
    // holding exactly one model that is NOT what the board is wearing, and the
    // way to get one is a board still on the pre-gallery buddy.vox with a
    // single entry beside it. It is not a contrived state - it is what the
    // boot before eos_seed_buddy() migrates looks like.
    host_path(EOS_GALLERY_DIR, sys, (int)sizeof sys);
    rmtree(sys);
    drop(EOS_GALLERY_ACTIVE);
    VOXLEN = build_vox(VOXBUF, (int)sizeof VOXBUF, 20, 10);
    put_file("/int/buddy/buddy.vox", VOXBUF, VOXLEN);
    eq(eos_apps_buddy_reload(), EOS_OK, "the board is on the legacy model");
    streq(eos_apps_buddy_slug(), "", "so no gallery entry is live");
    put_dir(EOS_GALLERY_DIR);
    put_entry("solo", "Solo", 24, 12);
    eq(eos_gallery_count(), 1, "and the gallery holds exactly one");

    eq(remove_slug("solo"), 409, "the only entry is refused even though nothing is wearing it");
    ok(has("\"state\""), "as a state error and not as a busy one");
    ok(!has("\"busy\""), "which is what proves the last rule ran and not the live rule");
    ok(there(EOS_GALLERY_DIR "/solo.vox"), "a gallery cannot be emptied");
    ok(there(EOS_GALLERY_DIR "/solo.json"), "and its metadata stays with it");

    // Two entries and neither of them live: now the rule stops applying, which
    // is what says it is a rule about the LAST one rather than a rule about
    // deleting at all.
    put_entry("duo", "Duo", 24, 12);
    eq(remove_slug("solo"), 200, "with a second one there, the same delete goes through");
    ok(!there(EOS_GALLERY_DIR "/solo.vox"), "and it goes");
    eq(remove_slug("duo"), 409, "leaving the new last one refused in its turn");

    reset_board();
    eq(select_slug("pig"), 200, "make pig live");
    eq(remove_slug("pip"), 200, "remove the other one");
    ok(has("\"total\":1"), "one left");
    eq(remove_slug("pig"), 409, "and the last one is refused as well");
    ok(has("\"busy\""), "here as a busy, because it is also the live one");
    ok(there(EOS_GALLERY_DIR "/pig.vox"), "and it stays");
    ok(eos_apps_buddy_model() != NULL, "so the board is never left with no buddy at all");
}

// ==========================================================================
// The active pointer, and what a board does when it is wrong
// ==========================================================================

// A reload that FAILS must not rename the buddy that is still drawing.
//
// Every refusal in eos_apps_buddy_reload_from() before the parse returns with
// the voxel pool untouched, so the previous model is still on the panel. If the
// slug and the path were published on the way in, the board would then be
// drawing one buddy and reporting another, from a path with nothing loadable at
// it. Two things downstream believe that report: the web app reads model.path
// to fetch the live model, and eos_gallery_remove() asks eos_apps_buddy_slug()
// as its SECOND guard against deleting the entry being drawn.
static void t_failed_reload_keeps_the_identity(void)
{
    int n;

    reset_board();
    streq(eos_apps_buddy_slug(), "pip", "the board is wearing pip out of the gallery");

    // A model too large for the loader's stage, pointed at by the active file.
    // The reload resolves it, refuses it on size, and never reaches the pool.
    n = build_vox_xyz(VOXBUF, (int)sizeof VOXBUF, EOS_APPS_VOX_VOXELS + 400, 32, 32, 32);
    ok(n > (int)EOS_APPS_VOX_BYTES, "the test built a model past what the board stages");
    put_file(EOS_GALLERY_DIR "/whale.vox", VOXBUF, n);
    put_text(EOS_GALLERY_ACTIVE, "whale\n");

    eq(eos_apps_buddy_reload(), EOS_ERR_TOOBIG, "the reload refuses the oversized model");
    ok(eos_apps_buddy_model() != NULL, "and the model that was loaded is still loaded");
    eq(live_dim_x(), 16, "at pip's dimensions, so the panel never moved");
    streq(eos_apps_buddy_slug(), "pip", "the board still says it is wearing pip");
    streq(eos_apps_buddy_file(), EOS_GALLERY_DIR "/pip.vox",
          "from the path it actually read him from, not the one that just failed");
}

// The second delete guard, on its own. eos_gallery_remove() asks two questions
// - what the `active` file says and what the panel is drawing - so that either
// one covers the other when they disagree. This is that disagreement: the
// pointer names pig, the panel is still drawing pip, and pip must survive a
// delete on the strength of the second question alone.
static void t_remove_guard_when_the_two_disagree(void)
{
    reset_board();
    streq(eos_apps_buddy_slug(), "pip", "the panel is drawing pip");

    // Written straight to the file, which is a thing the owner can do over
    // curl, and which no reload has been run against.
    put_text(EOS_GALLERY_ACTIVE, "pig\n");
    streq(eos_apps_buddy_slug(), "pip", "while the pointer now names pig");

    eq(remove_slug("pip"), 409, "deleting the buddy on the panel is still refused");
    ok(there(EOS_GALLERY_DIR "/pip.vox"), "and his model is still on the board");
}

static void t_active_recovery(void)
{
    char act[64];
    char vox[EOS_PATH_MAX], json[EOS_PATH_MAX], slug[64];

    reset_board();

    // The pointer names a model that is not there. Falling back to the
    // compiled-in penguin when the gallery is full of models would be the
    // wrong answer, so the first entry stands in and nothing is written.
    put_text(EOS_GALLERY_ACTIVE, "gone\n");
    eq(eos_gallery_resolve(vox, json, EOS_PATH_MAX, slug, (int)sizeof slug), EOS_OK,
       "a pointer at a missing model still resolves");
    ok(strcmp(slug, "pip") == 0 || strcmp(slug, "pig") == 0,
       "to something the gallery actually holds");
    eq(eos_apps_buddy_reload(), EOS_OK, "and the board comes up wearing it");
    ok(eos_apps_buddy_model() != NULL, "with a real model");
    eq(eos_gallery_active(act, (int)sizeof act), 4,
       "while the pointer is left alone, so a read-only filesystem still boots");

    // The shapes the pointer file can be wrong in.
    put_text(EOS_GALLERY_ACTIVE, "");
    ok(eos_gallery_active(act, (int)sizeof act) < 0, "an empty pointer is not a slug");
    put_text(EOS_GALLERY_ACTIVE, "../../etc/passwd\n");
    ok(eos_gallery_active(act, (int)sizeof act) < 0, "nor is a traversal written into it");
    eq(eos_apps_buddy_reload(), EOS_OK, "and the board still comes up");
    put_text(EOS_GALLERY_ACTIVE,
             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
    ok(eos_gallery_active(act, (int)sizeof act) < 0, "nor is one longer than a slug");
    drop(EOS_GALLERY_ACTIVE);
    eq(eos_gallery_active(act, (int)sizeof act), (long)EOS_ERR_NOTFOUND,
       "an absent pointer says so rather than guessing");

    // A trailing newline is normal and so is no newline at all: the owner
    // writing this file by hand over curl is a supported way to fix a board.
    put_text(EOS_GALLERY_ACTIVE, "pig");
    eq(eos_gallery_active(act, (int)sizeof act), 3, "a pointer with no newline reads");
    streq(act, "pig", "as the slug it names");
    put_text(EOS_GALLERY_ACTIVE, "pig\r\n");
    eq(eos_gallery_active(act, (int)sizeof act), 3, "and so does one with CRLF");
    streq(act, "pig", "trimmed to the same slug");

    // An empty gallery falls all the way through to the legacy file, which is
    // the last step of the search order and what a pre-gallery board loads.
    {
        char sys[512];
        host_path(EOS_GALLERY_DIR, sys, (int)sizeof sys);
        rmtree(sys);
        drop(EOS_GALLERY_ACTIVE);
        VOXLEN = build_vox(VOXBUF, (int)sizeof VOXBUF, 20, 10);
        put_file("/int/buddy/buddy.vox", VOXBUF, VOXLEN);
        put_text("/int/buddy/buddy.json", "{\"name\":\"legacy\"}");
        eq(eos_apps_buddy_reload(), EOS_OK, "a board with no gallery loads the legacy model");
        streq(eos_apps_buddy_name(), "legacy", "with the legacy metadata");
        streq(eos_apps_buddy_slug(), "", "and no slug, because it is not a gallery entry");
        eq(GET("/api/buddy"), 200, "and /api/buddy reports it");
        ok(has("\"slug\":null"), "with a null slug rather than a made-up one");
        ok(has("\"file\":\"buddy.vox\""), "under the name it actually has");

        // The gallery wins the moment there is one, which is what makes the
        // search order total rather than two sources of truth.
        put_dir(EOS_GALLERY_DIR);
        put_entry("pip", "Pip", 40, 16);
        eq(eos_apps_buddy_reload(), EOS_OK, "and once a gallery exists it wins");
        streq(eos_apps_buddy_slug(), "pip", "over the legacy file");
        streq(eos_apps_buddy_name(), "Pip", "metadata and all");
    }
}

// ==========================================================================
// Uploads land in the gallery through the ordinary /api/fs/write
// ==========================================================================
//
// There is deliberately no second upload mechanism. The point of checking it
// here is the sentence in the header of eos_seed_buddy.c: an import must land
// BESIDE what is on the board rather than on top of it, and after one the
// buddy that was live is still live.

static void t_import_is_not_destructive(void)
{
    char uri[256];
    int n, off = 0;

    reset_board();
    streq(eos_apps_buddy_slug(), "pip", "pip is live before the import");

    n = build_vox(VOXBUF, (int)sizeof VOXBUF, 50, 14);
    ok(n > 0, "the test built a model to import");

    while (off < n) {
        int chunk = n - off < 400 ? n - off : 400;
        int final = (off + chunk >= n);
        snprintf(uri, sizeof uri,
                 "/api/fs/write?path=%%2Fint%%2Fbuddy%%2Fgallery%%2Fnewbie.vox"
                 "&offset=%d&final=%d", off, final);
        eq(req("POST", uri, (const char *)VOXBUF + off, chunk), 200, "a chunk lands");
        off += chunk;
    }
    ok(there(EOS_GALLERY_DIR "/newbie.vox"), "the import arrived");
    ok(!there(EOS_GALLERY_DIR "/newbie.vox.part"), "with its partial cleaned up");

    // The two claims that matter, and they are the reason the gallery exists.
    streq(eos_apps_buddy_slug(), "pip", "the import did not become the buddy");
    ok(there(EOS_GALLERY_DIR "/pip.vox"), "and did not overwrite the one that was");
    eq(live_dim_x(), 16, "the panel is still drawing pip");

    eq(GET("/api/buddy/gallery"), 200, "the newcomer is in the gallery");
    ok(has("\"slug\":\"newbie\""), "under its own slug");
    ok(has("\"name\":\"newbie\""), "named for it until it has metadata");
    ok(has("\"total\":3"), "beside the two that were there");

    eq(select_slug("newbie"), 200, "and can be chosen when the owner wants it");
    eq(live_dim_x(), 14, "at which point the panel changes");
    eq(select_slug("pip"), 200, "and Pip is one click away, which was the whole point");
    eq(live_dim_x(), 16, "and comes straight back");
}

// ==========================================================================

int main(void)
{
    eos_httpd_cfg_t cfg;
    char env[200];

    snprintf(ROOT, sizeof ROOT, "/tmp/eos-gallery-test-%d", (int)getpid());
    rmtree(ROOT);
    if (mkdir(ROOT, 0777) != 0) { printf("cannot create %s\n", ROOT); return 1; }
    snprintf(env, sizeof env, "EOS_STORAGE_HOST_ROOT=%s", ROOT);
    putenv(env);

    if (eos_storage_init() != EOS_OK) { printf("storage init failed\n"); rmtree(ROOT); return 1; }

    eos_httpd_cfg_default(&cfg);
    cfg.mode = EOS_HTTPD_MODE_RUN;
    eos_httpd_init(&H, NULL, NULL, &cfg);
    eos_apps_init(NULL, NULL);
    eos_apps_bind_files(&H);
    eos_apps_tick(0);

    t_slug_rules();
    t_slug_over_http();
    t_routing();
    t_list();
    t_list_survives_damage();
    t_select();
    t_select_refuses_a_bad_model();
    t_remove();
    t_remove_refuses_the_active();
    t_remove_refuses_the_last();
    t_active_recovery();
    t_failed_reload_keeps_the_identity();
    t_remove_guard_when_the_two_disagree();
    t_import_is_not_destructive();

    rmtree(ROOT);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
