// eos_web_path — the path half of eos_web_embed, split out so it can be tested.
//
// eos_web_embed.c cannot be compiled by a host suite: its inputs are
// _binary_*_start linker symbols whose mangling differs between Mach-O and ELF,
// so a suite that linked on one machine would fail on another. That is why the
// file had no coverage, and it is why a route bug lived in it undetected long
// enough to hang a captive portal.
//
// The bug was entirely in the PATH arithmetic, which needs no linker symbols at
// all. Splitting it out costs one header and makes the part that was actually
// wrong testable on any machine.

#ifndef EOS_WEB_PATH_H
#define EOS_WEB_PATH_H

#include <stddef.h>
#include <string.h>

// Given the document root the embedded assets belong to and a full request
// path, return the basename to look up in the asset table, or NULL when the
// path does not belong to that root at all.
//
// The anchoring is the point. eos_httpd builds a full path from whichever root
// its MODE selects - "/int/web/app.js" in RUN, "/int/setup/index.html" in
// SETUP - and every embedded asset belongs to the run app. Matching the
// basename alone made a SETUP request resolve to the RUN app's index.html,
// which served the 20 KB application in place of the 2,756-byte built-in setup
// panel and left that panel unreachable.
//
// Returns a pointer INTO path; never allocates.
static inline const char *eos_web_asset_base(const char *root, const char *path)
{
    size_t rl;
    const char *base;

    if (!path) return NULL;
    if (root && root[0]) {
        rl = strlen(root);
        if (strncmp(path, root, rl) != 0) return NULL;
        // The root must end at a boundary: "/int/webbing" is not "/int/web".
        if (path[rl] != '/' && path[rl] != '\0') return NULL;
        path += rl;
    }

    base = strrchr(path, '/');
    base = base ? base + 1 : path;

    // "/" and "/int/web/" both mean the app itself.
    if (!base[0]) return "index.html";
    return base;
}

#endif // EOS_WEB_PATH_H
