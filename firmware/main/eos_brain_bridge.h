// eos_brain_bridge — the one task that owns eos_brain, and the seam that lets
// four HTTP workers, the status bar and the avatar all read it without any of
// them touching it.
//
// eos_brain is a single-request state machine with no lock of its own, and its
// pump blocks: mDNS discovery costs two seconds and a connect to a mini that is
// switched off costs three. So it gets a task, that task is its only caller,
// and everybody else posts intent through the functions below. Nothing here is
// reentrant with respect to eos_brain and nothing here calls it — see the
// comment at the top of the .c for why that rule is absolute rather than
// merely tidy.
//
// The non-obvious constraint: the reply is drained through a ring, and the
// task STOPS PUMPING when the ring is nearly full rather than dropping text.
// That turns a slow HTTP client into TCP back-pressure on the megabrain socket,
// which is the only place in the chain that can absorb it for free. Growing the
// ring instead would only move the moment it overflows.

#ifndef EOS_BRAIN_BRIDGE_H
#define EOS_BRAIN_BRIDGE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "eos_brain.h"
#include "eos_buddy.h"   // the event vocabulary the request lifecycle speaks
#include "eos_httpd.h"

// Everything a brain.* settings patch can change. All of it is live: the task
// applies a pending change the next time it is between requests, so a host
// edited mid-reply takes effect on the following ask and never mid-stream.
typedef struct {
    char     host[EOS_BRAIN_HOST_MAX];    // "" keeps EOS_BRAIN_FALLBACK_HOST
    uint16_t port;                        // 0 keeps 80
    char     model[EOS_BRAIN_MODEL_MAX];  // "" keeps EOS_BRAIN_DEFAULT_MODEL
    char     system[EOS_BRAIN_SYSTEM_MAX];// "" keeps EOS_BRAIN_SYSTEM_TINY
    int      max_tokens;                  // 0 keeps 256
} eos_brain_bridge_cfg_t;

// Fills cfg with the compiled-in defaults. Call this, edit what the settings
// say, then hand it to configure() — that way a settings store that only knows
// two of the five keys cannot blank the other three.
void eos_brain_bridge_defaults(eos_brain_bridge_cfg_t *cfg);

// Creates the task. Safe to call once; a second call is a no-op that returns 0.
// Returns 0, or a negative eos_err_t.
int  eos_brain_bridge_start(void);

// Live. Takes effect on the next request, never in the middle of one.
void eos_brain_bridge_configure(const eos_brain_bridge_cfg_t *cfg);

// The same thing spelled in eos_settings' vocabulary, compiled only when that
// header is present. It exists so that the five brain.* keys web/README.md
// marks "live" actually are: the settings store calls this from its apply hook
// and from wherever it loads the file at boot, and nothing else has to know
// that the two structs are the same five fields.
#if defined(__has_include)
#  if __has_include("eos_settings.h")
#    define EOS_BRAIN_BRIDGE_HAS_SETTINGS 1
#    include "eos_settings.h"
void eos_brain_bridge_from_settings(const eos_settings_t *s);
#  endif
#endif

// Whether the station link is up. The task runs no health probe while this is
// false, because in SETUP mode every candidate would time out in turn and the
// only thing that would achieve is eleven seconds of a task walking a list.
void eos_brain_bridge_set_online(bool online);

// Wires ports.brain_* on an already-bound server. Call it after
// eos_httpd_idf_bind(), which zeroes the whole port table.
void eos_brain_bridge_bind(eos_httpd_t *h);

// ------------------------------------------------------------ what the shell reads

// True when a health probe answered inside eos_brain's link ttl. This is what
// turns "no brain" in the status bar into the model name.
bool eos_brain_bridge_reachable(void);

// The model an ask with no model of its own would use, copied out under the
// lock rather than lent: the bar borrows the pointer it is given for the length
// of a build, and the task can rewrite the live config between two frames.
void eos_brain_bridge_model(char *out, size_t cap);

// The next avatar event, as an eos_buddy_event_t, or -1 when there is none.
// Drain it in a loop from the frame loop: the queue coalesces runs of stream
// chunks but keeps every state change, so a caller that reads one per frame
// would fall behind on a fast reply.
int  eos_brain_bridge_next_event(void);

#endif
