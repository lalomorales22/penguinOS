// Binds eos_httpd's settings, system, reboot and theme ports to the things on
// this board that actually know the answers: eos_settings, eos_board,
// eos_display, eos_storage, eos_net and the heap.
//
// It is here and not in eos_httpd.c's own adapter for the same reason
// eos_web_embed is: eos_httpd's adapter knows about radios, and /api/system is
// a description of a BOARD — its panel, its partitions, its build. Putting it
// down there would make every image that starts the server drag in the board
// registry and the display HAL.
//
// Call it AFTER eos_httpd_idf_bind(), which overwrites the whole port table.
//
// The one non-obvious constraint: nothing here writes flash on an HTTP worker
// and nothing here restarts the chip inside a handler. A settings save marks
// the store dirty and eos_settings_bind_pump() flushes it from the OS loop once
// the edits go quiet; POST /api/system/reboot arms a deadline that the same
// pump acts on, half a second later, by which time the response has gone out.

#ifndef EOS_SETTINGS_BIND_H
#define EOS_SETTINGS_BIND_H

#include <stdbool.h>
#include <stdint.h>

#include "eos_board.h"
#include "eos_httpd.h"
#include "eos_settings.h"
#include "eos_theme.h"

// `theme` is the live theme the shell draws from — a live theme switch writes
// into it and reprograms the panel's CLUT, so it must be the same object
// eos_shell_view_t points at. `net` may be NULL on a board with no radio, in
// which case the WiFi settings keys report "" and refuse a write.
void eos_settings_bind(eos_httpd_t *h, eos_settings_store_t *st,
                       const eos_board_t *b, eos_theme_t *theme, void *net);

// Fills the store's port table only. Call it before eos_settings_load(), which
// happens between storage and theme in app_main and therefore before there is
// an eos_httpd_t to bind to.
void eos_settings_bind_ports(eos_settings_store_t *st, const eos_board_t *b,
                             eos_theme_t *theme);

// The debounced settings flush and the armed reboot. Same loop, same clock as
// eos_net_pump() and eos_httpd_pump(). Never returns from a reboot.
void eos_settings_bind_pump(uint32_t now_ms);

// True once, when a live theme switch has changed the palette and the whole
// screen needs redrawing. Reading it clears it.
bool eos_settings_bind_take_redraw(void);

#endif // EOS_SETTINGS_BIND_H
