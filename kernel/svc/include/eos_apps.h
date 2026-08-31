// eos_apps — the application half of the on-board API: files, console, buddy,
// and the list of windows the shell can open.
//
// eos_httpd shipped the eight provisioning endpoints. The web app calls
// twenty-eight, and the Files tab — the one thing the owner is actually waiting
// on — is built entirely on nine of the twenty that were missing. This file is
// those nine plus the console ring, the avatar, and /api/apps. Settings,
// /api/system, /api/themes and megabrain are somebody else's routes and are
// deliberately not here.
//
// It is a separate translation unit from eos_httpd.c and it registers itself
// through eos_httpd_set_api() rather than being called directly. Two reasons.
// Three people are adding routes to one server, and a four-thousand-line file
// is how two of them lose a day to a merge. And a host suite or an image that
// links no eos_apps.c still links: the pointer stays NULL and these routes
// answer 501 instead of failing to build.
//
// The one non-obvious constraint: every handler here runs under the single
// mutex eos_httpd's worker takes across dispatch, so the scratch in this file
// is file-static and not stack. That is not an optimisation. The HTTP worker
// has a 5,376-byte stack of which 513 bytes are already the request body, and
// two 96-byte paths plus a directory entry plus a formatting buffer per frame
// is how a four-worker server starts overflowing one. The same mutex is why one
// upload handle, one log ring and one buddy is the right number of each: there
// is exactly one dispatch in flight, image-wide, at any instant.
//
// Nothing here allocates. The ring, the buddy's voxel pool, the .vox staging
// buffer and the scratch are fixed arrays sized by the tunables below; the
// numbers they add up to are in kernel/svc/README.md.

#ifndef EOS_APPS_H
#define EOS_APPS_H

#include <stdint.h>
#include <stdbool.h>

#include "eos_board.h"     // eos_err_t
#include "eos_httpd.h"
#include "eos_storage.h"
#include "eos_vox.h"
#include "eos_buddy.h"

// ---------------------------------------------------------------- tunables

// The largest upload chunk the board accepts, and the number /api/system must
// report as limits.chunk_max. It is EOS_HTTPD_BODY_MAX and not a number of its
// own on purpose: the body arrives in on_request()'s stack buffer and a chunk
// larger than that buffer is not a slow upload, it is a request the transport
// refused before any handler saw it. Raising BODY_MAX (and cfg.stack_size with
// it) raises the upload chunk for free, and nothing here has to be told.
#define EOS_APPS_CHUNK_MAX  EOS_HTTPD_BODY_MAX

// Directory entries per /api/fs/list page. A hard ceiling; the real page is
// whatever also fits EOS_HTTPD_RESP_MAX, which for 40-byte names is nearer 40.
// The client pages off `more` and `total`, so a short page costs one more
// request and never a lost entry.
#ifndef EOS_APPS_LIST_MAX
#define EOS_APPS_LIST_MAX 64
#endif

// An upload handle nobody has written to for this long is closed and the
// partial file left on the filesystem, exactly as web/README.md says. Without
// it one abandoned phone holds the board's single write handle until reboot.
#ifndef EOS_APPS_UPLOAD_IDLE_MS
#define EOS_APPS_UPLOAD_IDLE_MS 30000u
#endif

// The console ring. Lines are fixed-count and bytes are a separate circular
// pool, because the two run out at different rates: a boot log is forty short
// lines and a stack trace is four long ones.
#ifndef EOS_APPS_LOG_LINES
#define EOS_APPS_LOG_LINES 96
#endif
#ifndef EOS_APPS_LOG_BYTES
#define EOS_APPS_LOG_BYTES 3072
#endif
#ifndef EOS_APPS_LOG_TEXT_MAX
#define EOS_APPS_LOG_TEXT_MAX 144   // one line, longer is truncated with an ellipsis
#endif

// The buddy. EOS_VOX_MAX_VOXELS is 4096 and that is the file format's cap, not
// this board's: a 4096-voxel pool is 20,480 bytes of .bss and the staging
// buffer for the file that fills it is another 21 KB, on a board with 173 KB of
// heap and no renderer for it yet. 1024 voxels holds a solid 10x8x12 model
// before culling, which is a buddy. A model past the cap is refused with
// EOS_VOX_ERR_POOL and the number, so the editor can say so rather than the
// board silently drawing half a face.
#ifndef EOS_APPS_VOX_VOXELS
#define EOS_APPS_VOX_VOXELS 1024
#endif
// Enough for the above: 8 header + 12 MAIN + 24 SIZE + 12+4 XYZI header +
// 4*VOXELS + 12+1024 RGBA, rounded up.
#ifndef EOS_APPS_VOX_BYTES
#define EOS_APPS_VOX_BYTES 6144
#endif

// buddy.json, staged whole. web/README.md caps `personality` at 480 bytes and
// every other field is short, so this holds the largest document the editor
// writes with room over. A larger one is not truncated — eos_storage_load()
// refuses it and the model loads with compiled-in defaults, which is the same
// rule kernel/theme applies to a theme file it cannot hold.
#ifndef EOS_APPS_BUDDY_JSON_BYTES
#define EOS_APPS_BUDDY_JSON_BYTES 768
#endif

#ifndef EOS_APPS_BUDDY_NAME_MAX
#define EOS_APPS_BUDDY_NAME_MAX 32    // web/README.md: <= 31 bytes
#endif
// web/README.md allows 480 bytes here and says the board must truncate to
// EOS_BRAIN_SYSTEM_MAX. That is 224 including the NUL, and prepending 224 bytes
// to a system prompt that is itself capped at 224 leaves nothing, so this is
// half of it: the personality gets 111 bytes and the rest of the prompt gets
// the other half. Longer input is truncated on a UTF-8 boundary, not refused.
#ifndef EOS_APPS_BUDDY_PERSONA_MAX
#define EOS_APPS_BUDDY_PERSONA_MAX 112
#endif

// Where the avatar lives. /int and not /sd: the card is not mounted on any
// board that exists, and web/README.md's /sd/buddy/ paths are the tier-1 shape.
#ifndef EOS_APPS_BUDDY_DIR
#define EOS_APPS_BUDDY_DIR "/int/buddy"
#endif

// Windows /api/apps will report. The shell has four.
#ifndef EOS_APPS_CATALOG_MAX
#define EOS_APPS_CATALOG_MAX 12
#endif

#if EOS_APPS_LOG_TEXT_MAX * 2 > EOS_APPS_LOG_BYTES
#error "EOS_APPS_LOG_BYTES must hold at least two full lines or the ring thrashes"
#endif
#if EOS_APPS_VOX_BYTES < EOS_APPS_VOX_VOXELS * 4 + 1080
#error "EOS_APPS_VOX_BYTES cannot hold a full XYZI plus RGBA for EOS_APPS_VOX_VOXELS"
#endif

// ------------------------------------------------------------------- ports
//
// The three things this file cannot know and must not learn: how to reboot,
// what the heap is doing, and what the radios and the theme are called. Same
// shape and same reason as eos_httpd_ports_t — it is what lets the whole
// surface run on a laptop with no IDF, and it is why the console's command
// table is seven words rather than seven #ifdefs.

typedef struct {
    // Reboots the board, not before `in_ms` have passed, so the 200 gets out
    // first. NULL makes `reboot` answer that this board cannot.
    void (*reboot)(void *ctx, uint32_t in_ms);

    // One line about `topic`, into out. Bytes written, or negative when this
    // board has nothing to say about it — which is not an error, it is a
    // console line saying so. Topics asked for: "board", "heap", "wifi",
    // "theme", "brain". "fs" and "uptime" are answered here and never asked.
    int (*describe)(void *ctx, const char *topic, char *out, int cap);
} eos_apps_ports_t;

// One entry in /api/apps. Strings are borrowed and must outlive the image,
// which is what they are: they come from eos_shell_app_names() and from string
// literals in the boot glue.
typedef struct {
    const char *id;         // matches eos_shell_app_names(), and sys.autostart
    const char *name;       // what the picker shows
    const char *summary;    // one line, may be NULL
    uint8_t     tier_min;   // lowest eos_tier_t this window runs on
} eos_apps_app_t;

// Wires the ports and registers the routes with eos_httpd. Call once, at boot,
// before eos_httpd_start(). Passing NULL ports is legal and leaves the console
// commands that need them answering that the board cannot.
void eos_apps_init(const eos_apps_ports_t *ports, void *ctx);

// The window catalog /api/apps reports. `apps` is borrowed, not copied.
void eos_apps_set_apps(const eos_apps_app_t *apps, int n);

// The clock, and the only place a timed-out upload is closed. Call it from the
// OS loop next to eos_httpd_pump(). It is here and not in dispatch because
// dispatch is documented pure — no sockets, no clock — and because an upload
// that timed out did so precisely while no request was arriving.
void eos_apps_tick(uint32_t now_ms);

// Binds eos_httpd's three file ports so that a real file on storage is
// preferred and whatever was bound before this call — the copy of the web app
// linked into the image — answers when there is not one. Call it AFTER the
// fallback is bound. This is web/README.md's rule: prefer the file, and never
// leave the board with nothing to serve.
void eos_apps_bind_files(eos_httpd_t *h);

// ------------------------------------------------------------- the console

// Appends one line to the ring. `level` is one of E W I D, matching ESP-IDF and
// what the web app colours. Text is truncated at EOS_APPS_LOG_TEXT_MAX and
// split on embedded newlines. Safe from any task; not safe from an ISR.
void eos_apps_log(char level, const char *text);
void eos_apps_logf(char level, const char *fmt, ...);

// How many lines the ring has ever accepted. The `since` cursor the web app
// carries is an index into this, not a byte offset.
uint32_t eos_apps_log_seq(void);

#ifdef ESP_PLATFORM
// Installs the ESP-IDF log hook so that everything the boot path and every
// component already prints lands in the ring as well as on the UART. Without
// it the Console tab shows only what /api/console/exec produced, which is the
// least interesting half. Call it as early in app_main() as there is a ring.
void eos_apps_log_install(void);
#endif

// --------------------------------------------------------------- the buddy

// The four yaw-drift presets web/README.md names. The editor writes the name;
// the board owns what each one does.
typedef enum {
    EOS_APPS_IDLE_STILL = 0,
    EOS_APPS_IDLE_WANDER,        // the fallback for anything unrecognised
    EOS_APPS_IDLE_CURIOUS,
    EOS_APPS_IDLE_SLEEPY,
} eos_apps_idle_t;

const char *eos_apps_idle_name(int b);

// Re-reads EOS_APPS_BUDDY_DIR/buddy.json and buddy.vox and swaps both in.
// Returns EOS_OK, or the first failure: EOS_ERR_NOTFOUND when there is no
// model, EOS_ERR_TOOBIG when the file is larger than EOS_APPS_VOX_BYTES,
// EOS_ERR_ARG when eos_vox_parse() refused it.
//
// A failure does NOT leave the previous model in place, and it cannot: the
// parse writes straight into the one voxel pool this board has and can fail
// after it has already filled part of it. What a failure leaves is a model
// with no voxels — eos_apps_buddy_model() answers NULL — and a generation that
// has moved, so the renderer re-adopts and falls back to the compiled-in
// buddy. A bad upload costs the owner their model. It does not leave the panel
// drawing half of one model over half of another, which is what a promise to
// keep the previous one would actually have delivered.
eos_err_t eos_apps_buddy_reload(void);

// Why the last reload failed, as the sentence eos_vox_strerror() gave, or NULL.
const char *eos_apps_buddy_error(void);

// The live model, or NULL when nothing has ever loaded. The pointer is stable
// for the life of the image; the contents change under a reload, so a renderer
// must not hold a voxel pointer across one.
eos_vox_model_t     *eos_apps_buddy_model(void);
const eos_vox_pal_t *eos_apps_buddy_palette(void);
const eos_buddy_cfg_t *eos_apps_buddy_cfg(void);

const char *eos_apps_buddy_name(void);
const char *eos_apps_buddy_personality(void);
int         eos_apps_buddy_behaviour(void);        // eos_apps_idle_t
// 0x00rrggbb, or 0xFFFFFFFF when buddy.json named no accent.
uint32_t    eos_apps_buddy_accent(void);

// Increments on every reload that changed what the renderer should be drawing,
// which includes a FAILED one — see eos_apps_buddy_reload(). A renderer lives
// on the OS loop and a reload arrives on an HTTP worker, so the model must not
// be swapped under a half-drawn frame: the renderer compares this, and
// re-adopts the model from its OWN task on the next pass. That is the whole
// synchronisation, and it is enough because there is exactly one writer and
// the pointer never moves.
uint32_t eos_apps_buddy_generation(void);

// The state /api/buddy reports. The value is an eos_buddy_state_t; the setter
// exists so that whatever ends up driving the avatar is the single writer and
// this file only ever reports.
void eos_apps_buddy_set_state(int state);
int  eos_apps_buddy_state(void);

// --------------------------------------------- what /api/system must report
//
// These are the numbers the limits block in /api/system carries. They live here
// because this file is what enforces them, and a second copy in whoever writes
// /api/system is a second copy that will be wrong first.

int eos_apps_chunk_max(void);
int eos_apps_list_max(void);
int eos_apps_path_max(void);
int eos_apps_name_max(void);
int eos_apps_open_files(void);

// ------------------------------------------------------------------ routes

// Every route in this file, in one call. eos_httpd_dispatch() reaches it
// through the pointer eos_apps_init() registered; nothing else should call it.
int eos_apps_dispatch(eos_httpd_t *h, int route,
                      const eos_httpd_req_t *req, eos_httpd_resp_t *r);

#endif // EOS_APPS_H
