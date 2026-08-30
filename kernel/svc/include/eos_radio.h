// eos_radio — one radio, two stacks, one lock.
//
// The ESP32-C6 has a single 2.4 GHz antenna and WiFi, BLE and 802.15.4 all
// want it. docs/provisioning.md says a BLE scan and a WiFi scan must never run
// at the same time, and that is not a style preference: both stacks reprogram
// the same PHY, and the observable result of overlapping them is a scan that
// returns nothing, a join that times out, or a link that drops — none of which
// look like a coexistence bug when you are staring at them.
//
// This file exists so that neither stack owns the lock. It used to live at the
// tail of eos_ble.h, which meant eos_net could only reach it through a build
// flag, and a board built without the BLE service was silently unserialised.
// Here, both bind to it unconditionally: eos_net.c picks this header up with
// __has_include and eos_ble.h includes it, so there is no build in which one
// side takes the lock and the other does not.
//
// The one non-obvious constraint: this is a flag guarded by a spinlock, not a
// FreeRTOS mutex, and deliberately so. A mutex needs creating, and whichever
// stack comes up first would have to be the one to create it — which is a race
// at exactly the moment the lock exists to protect. A file-scope flag is valid
// from the first instruction of app_main.
//
// Never hold this across anything that waits for a human. A pairing passkey
// takes as long as someone takes to type it, and a WiFi scan blocked behind
// that is a setup page that never loads.

#ifndef EOS_RADIO_H
#define EOS_RADIO_H

#include <stdint.h>
#include <stdbool.h>

// Who is asking. Kept as an enum rather than a bare string so that the two
// callers cannot disagree about spelling, which is how "wifi" and "wifi-scan"
// end up looking like two different owners to eos_radio_unlock().
typedef enum {
    EOS_RADIO_WIFI = 0,
    EOS_RADIO_BLE,
    EOS_RADIO_USER_COUNT
} eos_radio_user_t;

// "wifi" / "ble", or "?" for anything else. Stable: it is what
// eos_radio_owner() returns and what the logs print.
const char *eos_radio_user_name(eos_radio_user_t u);

// The lock. wait_ms 0 means try once and fail; on the host there is nothing to
// wait for, so any wait fails immediately rather than spinning forever.
//
// `owner` is stored, not copied, so it must outlive the lock — pass a string
// literal or eos_radio_user_name().
bool        eos_radio_lock(const char *owner, uint32_t wait_ms);

// Releasing a lock somebody else holds does nothing. That turns a
// release-by-the-wrong-stack bug into a stall, which shows up in a log, rather
// than into two concurrent scans, which shows up as flaky hardware. Pass NULL
// to release unconditionally — recovery only.
void        eos_radio_unlock(const char *owner);

bool        eos_radio_busy(void);
const char *eos_radio_owner(void);   // "" when free

// The typed spelling. Identical behaviour; this is what eos_net.c calls.
bool eos_radio_acquire(eos_radio_user_t u, uint32_t wait_ms);
void eos_radio_release(eos_radio_user_t u);

#endif // EOS_RADIO_H
