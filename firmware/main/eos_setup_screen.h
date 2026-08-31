// eos_setup_screen — the panel as the instruction sheet.
//
// docs/provisioning.md turns on one observation: every board in this fleet has
// a screen, and a screen is an out-of-band channel. That is what makes the
// SoftAP safe to close. The board invents a WPA2 password nobody has ever seen,
// keeps it in NVS, and prints it here, so joining costs the owner one glance
// instead of exposing a page that accepts WiFi passwords to everyone in range.
// The same argument covers the BLE passkey: a keyboard has no screen, so the
// six digits it wants typed on it have to come off this one.
//
// Three full-screen scenes, none of which involve the window manager. They are
// what is on the glass before there is a desktop to show, so they own the whole
// panel and they declare their own damage — the caller does not.
//
// The one non-obvious constraint: the QR is drawn BLACK ON WHITE and ignores
// the theme completely. Every other pixel in penguinOS is a theme role; this one
// cannot be, because a phone camera has to decode it. An amber-on-black QR in
// a dark theme is a decoration, not a link, and the failure is silent — it
// simply never scans, on someone else's phone, in a room you are not in.

#ifndef EOS_SETUP_SCREEN_H
#define EOS_SETUP_SCREEN_H

#include <stdint.h>
#include <stdbool.h>

#include "eos_theme.h"

// What SETUP puts on the panel. Everything is borrowed for the call.
typedef struct {
    const eos_theme_t *theme;
    const char *ap_ssid;      // penguinos-f048
    const char *ap_psk;       // the generated WPA2 password
    const char *url;          // http://192.168.4.1
    const char *qr;           // WIFI:S:...;T:WPA;P:...;; or NULL for text only
    const char *status;       // one line at the foot, or NULL
    bool        status_warn;  // draw that line in the warning colour
} eos_setup_view_t;

// The setup screen: QR, AP name, AP password, URL. Falls back to a text-only
// layout when the payload will not encode or the panel cannot give the symbol
// at least two pixels per module — a 33-module symbol at 1x is not a QR, it is
// a smudge, and printing one that does not scan is worse than not printing one.
void eos_setup_screen_draw(const eos_setup_view_t *v);

// True when the last eos_setup_screen_draw() actually drew a symbol. The boot
// glue logs it, because "the QR is missing" is otherwise something you find out
// by standing in front of the board with a phone.
bool eos_setup_screen_had_qr(void);

// The pairing screen. The passkey is drawn with integer-scaled glyphs — as
// large as the panel will take — because it is read across a desk and typed on
// a keyboard that cannot show it. `peer` and `warning` may be NULL.
void eos_setup_screen_passkey(const eos_theme_t *t, uint32_t passkey,
                              const char *peer, const char *warning);

// A centred title and one line. This is what is on screen during the join at
// boot, which blocks for up to fifteen seconds: without it the panel is black
// for that whole time and the board looks dead.
void eos_setup_screen_message(const eos_theme_t *t, const char *title,
                              const char *line);

#endif // EOS_SETUP_SCREEN_H
