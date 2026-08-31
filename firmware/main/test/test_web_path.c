// test_web_path — the asset path arithmetic from eos_web_embed.
//
// This suite exists because of a specific bug. eos_web_embed.c matched request
// paths on their LAST COMPONENT only, so a SETUP-mode request that eos_httpd
// had resolved to "/int/setup/index.html" matched the RUN application's
// "index.html" asset. Setup mode therefore served the 20 KB app in place of the
// 2,756-byte built-in panel, and BUILTIN_SETUP became unreachable code.
//
// It was not merely the wrong page. It was the only response the server could
// produce larger than CONFIG_LWIP_TCP_SND_BUF_DEFAULT (5,760 bytes), and send()
// returns EAGAIN exactly when it cannot place one byte for the whole send
// timeout - so it was also the only way setup mode could hang. It did, on the
// board with the least internal DRAM in the fleet: a captive portal that
// associated, took a DHCP lease, and then never rendered.
//
// eos_web_embed.c itself cannot be compiled here - its inputs are
// _binary_*_start linker symbols whose mangling differs Mach-O vs ELF, which is
// exactly why the file had no coverage. The path arithmetic needs none of them,
// so it lives in eos_web_path.h and is tested below.

#include <stdio.h>
#include <string.h>

#include "eos_web_path.h"

static int checks, fails;
#define CK(c, msg) do { checks++; if (!(c)) { fails++; printf("  FAIL %s\n", msg); } } while (0)

// Convenience: compare the returned basename, tolerating NULL on both sides.
static int is(const char *root, const char *path, const char *want)
{
    const char *got = eos_web_asset_base(root, path);
    if (!want) return got == NULL;
    return got && strcmp(got, want) == 0;
}

#define RUN   "/int/web"
#define SETUP "/int/setup"

int main(void)
{
    // ---- the regression this file exists for -----------------------------
    CK(is(RUN, "/int/setup/index.html", NULL),
       "a SETUP path does NOT resolve to a run asset (the portal hang)");
    CK(is(RUN, "/int/setup/", NULL),          "nor does the setup root itself");
    CK(is(RUN, "/int/setup/app.js", NULL),    "nor any other name under setup");
    CK(is(RUN, "/int/setup/index.html.gz", NULL),
       "nor the .gz twin eos_httpd probes first");

    // ---- the run app still resolves, which is the whole point ------------
    CK(is(RUN, "/int/web/index.html", "index.html"), "the run index resolves");
    CK(is(RUN, "/int/web/app.js",     "app.js"),     "run app.js resolves");
    CK(is(RUN, "/int/web/style.css",  "style.css"),  "run style.css resolves");
    CK(is(RUN, "/int/web/setup.js",   "setup.js"),
       "setup.js is a RUN asset - web/index.html loads it - and still resolves");
    CK(is(RUN, "/int/web/voxel-editor.js", "voxel-editor.js"), "the editor resolves");
    CK(is(RUN, "/int/web/index.html.gz", "index.html.gz"),
       "the .gz twin is returned as-is; the caller decides if it has one");

    // ---- the root itself means the app -----------------------------------
    CK(is(RUN, "/int/web/", "index.html"), "a trailing slash means the index");
    CK(is(RUN, "/int/web",  "index.html"), "the bare root means the index too");

    // ---- boundaries: the root must end at a separator --------------------
    CK(is(RUN, "/int/webbing/app.js", NULL), "\"/int/webbing\" is not \"/int/web\"");
    CK(is(RUN, "/int/web2/app.js",    NULL), "nor is \"/int/web2\"");
    CK(is(RUN, "/int/we/app.js",      NULL), "a shorter prefix does not match");
    CK(is(RUN, "/app.js",             NULL), "an unrooted path does not match");
    CK(is(RUN, "app.js",              NULL), "a relative path does not match");

    // ---- nesting ---------------------------------------------------------
    CK(is(RUN, "/int/web/sub/deep/app.js", "app.js"),
       "a nested path still yields its basename; the table is flat by design");

    // ---- degenerate input ------------------------------------------------
    CK(is(RUN, NULL, NULL),                  "a NULL path is not a match");
    CK(is(NULL, "/anything/app.js", "app.js"),
       "a NULL root disables anchoring, matching the old behaviour exactly");
    CK(is("",   "/anything/app.js", "app.js"), "an empty root does the same");
    CK(is(RUN, "", NULL),                    "an empty path is not a match");

    // ---- the returned pointer aliases the input, never a copy ------------
    {
        const char *p = "/int/web/app.js";
        const char *b = eos_web_asset_base(RUN, p);
        CK(b >= p && b <= p + strlen(p), "the basename points into the caller's path");
    }
    // ...except for the synthesised index, which is a literal.
    CK(strcmp(eos_web_asset_base(RUN, "/int/web/"), "index.html") == 0,
       "the synthesised index is a literal and is safe to return");

    // ---- a different root is honoured, not hardcoded ---------------------
    CK(is("/w", "/w/app.js", "app.js"),  "a short custom root works");
    CK(is("/w", "/int/web/app.js", NULL), "and excludes the old default");
    CK(is(SETUP, "/int/setup/index.html", "index.html"),
       "anchoring to the setup root would resolve setup paths - it is the ROOT "
       "that decides, not the function");

    printf("\n=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
