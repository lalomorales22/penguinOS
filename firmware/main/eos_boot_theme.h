// eos_boot_theme — get a theme into the display's palette, from wherever one
// happens to be, without ever letting a bad one stop the boot.
//
// eos_theme.h promises that a truncated, garbled or missing theme file leaves
// the caller holding the compiled-in default. This file is what honours that
// promise on a real board: it tries the removable card, then the internal
// filesystem, then the theme built into the image, and only then the neutral
// slate default, and every step that fails is a log line rather than a stop.
//
// The one non-obvious constraint: eos_display_init() has already seeded its
// colour LUT from eos_theme_default(), so a board that never reaches this file
// still draws in real colours rather than in whatever was in BSS. That is why
// the palette upload here is an update and not an initialisation, and why
// getting it wrong shows up as the wrong theme rather than as a black screen.

#ifndef EOS_BOOT_THEME_H
#define EOS_BOOT_THEME_H

#include "eos_board.h"
#include "eos_theme.h"

typedef enum {
    EOS_THEME_SRC_SD = 0,      // <board.storage.sd_point>/theme.json
    EOS_THEME_SRC_INT,         // <board.storage.int_point>/theme.json
    EOS_THEME_SRC_EMBEDDED,    // the copy linked into the image
    EOS_THEME_SRC_DEFAULT      // eos_theme_default()
} eos_theme_src_t;

// Names the theme the settings store asked for, before eos_boot_theme_load()
// runs. With one set, the search starts at <mount>/themes/<name>.json on both
// mounts and only then falls back to the <mount>/theme.json convention, the
// embedded copy and the default. Passing NULL or "" clears it.
//
// It is a separate call rather than an argument because the settings store is
// loaded between storage and theme in app_main and only one line of the boot
// path should have to know that.
void eos_boot_theme_prefer(const char *name);

// Fills `out` with the best theme it can find and returns where it came from.
// Never fails: EOS_THEME_SRC_DEFAULT is always reachable.
eos_theme_src_t eos_boot_theme_load(const eos_board_t *b, eos_theme_t *out);

// Reads and parses ONE theme file. `out` is written only on success; on any
// failure — no such file, too large, bad JSON, a missing role — it is left
// exactly as it was, which is what lets a live theme switch fail without
// taking the theme the panel is already wearing with it.
//
// It uses this file's own 4 KB scratch and a parked eos_theme_t, so there is
// one of each in the image rather than one per caller. Not reentrant: call it
// from the HTTP dispatch, which eos_httpd serialises, or from the OS loop.
eos_theme_err_t eos_boot_theme_read(const char *path, eos_theme_t *out);

const char *eos_boot_theme_src_name(eos_theme_src_t s);

// Uploads a theme's 256-entry CLUT into the display backend. The theme carries
// RGB565 and eos_display_palette() takes RGB888, so the conversion happens
// here, in chunks, rather than by putting a kilobyte of scratch on the stack.
void eos_boot_theme_upload(const eos_theme_t *t);

#endif // EOS_BOOT_THEME_H
