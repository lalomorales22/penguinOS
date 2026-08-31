// eos_gallery — several buddies kept on the board, one of them live.
//
// There used to be exactly one buddy, at /int/buddy/buddy.vox, and importing a
// new one wrote over it. That is not a rough edge, it is how the owner lost Pip
// one evening: the import succeeded, the penguin was gone, and getting him back
// took a remove-and-reboot over curl because the seeder only writes when there
// is nothing there. Keeping several models and choosing between them is the
// actual fix. An import lands beside what is already on the board instead of on
// top of it, and Pip is a click away rather than a reflash away.
//
// The layout, under EOS_APPS_BUDDY_DIR:
//
//   gallery/<slug>.vox    the model
//   gallery/<slug>.json   its name, personality and accent - one per model
//   active                one line naming the slug that is live
//
// `active` is its own file rather than a key in a buddy.json, and that is a
// decision worth writing down. buddy.json stopped being a board-wide document
// the moment every model got its own; there is no longer a single one to put
// the key in. And the pointer is rewritten on every select, while the metadata
// beside it holds the sentence the owner wrote about their penguin. Twenty-odd
// bytes is one LittleFS block; re-emitting a 500-byte document to change one
// key would put that sentence through a flash write every time somebody clicked
// a different buddy, and a flash write is the operation that can be interrupted.
//
// The one non-obvious constraint: a slug is a filename, and it arrives off the
// network from whoever is on the WiFi. Everything below treats it as hostile.
// [a-z0-9-], one to EOS_GALLERY_SLUG_MAX bytes, and nothing else ever - which
// is a whitelist and not a blacklist on purpose, because "." and ".." and NUL
// and a percent escape and a UTF-8 homoglyph are all simply not in the set, and
// a rule that has to enumerate the attacks is a rule that will miss the next
// one. It is checked over an explicit length, never over a C string, so an
// embedded NUL is refused rather than silently ending the name early.

#ifndef EOS_GALLERY_H
#define EOS_GALLERY_H

#include <stdbool.h>
#include <stdint.h>

#include "eos_board.h"     // eos_err_t
#include "eos_httpd.h"
#include "eos_storage.h"
#include "eos_apps.h"      // EOS_APPS_BUDDY_DIR, and the buddy this loads

// ---------------------------------------------------------------- tunables

#define EOS_GALLERY_DIR        EOS_APPS_BUDDY_DIR "/gallery"
#define EOS_GALLERY_ACTIVE     EOS_APPS_BUDDY_DIR "/active"

// The longest slug, in bytes, NOT counting the NUL. It is bounded from above by
// EOS_NAME_MAX rather than by taste: an upload writes "<slug>.vox.part" and
// eos_storage refuses a component of 40 bytes or more, so 30 is the true
// ceiling. 24 leaves room under it and is already more name than a penguin
// needs. A longer one is refused with this number in the sentence, not
// truncated - a truncated slug names a different file.
#ifndef EOS_GALLERY_SLUG_MAX
#define EOS_GALLERY_SLUG_MAX 24
#endif

#if EOS_GALLERY_SLUG_MAX + 9 >= EOS_NAME_MAX
#error "EOS_GALLERY_SLUG_MAX leaves no room for <slug>.vox.part under EOS_NAME_MAX"
#endif
#if EOS_GALLERY_SLUG_MAX + 1 > EOS_APPS_BUDDY_SLUG_MAX
#error "EOS_APPS_BUDDY_SLUG_MAX cannot hold an EOS_GALLERY_SLUG_MAX slug and its NUL"
#endif

// Entries per /api/buddy/gallery page. A hard ceiling; the real page is
// whatever also fits EOS_HTTPD_RESP_MAX, which for short names is nearer 40.
// The client pages off `more` and `total` exactly as it does for /api/fs/list.
#ifndef EOS_GALLERY_LIST_MAX
#define EOS_GALLERY_LIST_MAX 64
#endif

// How much of a .vox this reads to report its dimensions and voxel count in a
// listing. Header only: "VOX " + MAIN + SIZE + XYZI's count is 60 bytes, and a
// PACK chunk in front of them costs 16 more. 256 holds every file MagicaVoxel
// or the editor produces with room to spare, and a file whose header does not
// fit is listed with ok:false rather than dropped - see eos_gallery_peek().
#ifndef EOS_GALLERY_PEEK_BYTES
#define EOS_GALLERY_PEEK_BYTES 256
#endif

// ------------------------------------------------------------------- slugs

// True when these bytes are a slug this board will put on the filesystem.
// `n` is the length; pass -1 for a NUL-terminated string. Empty is false,
// longer than EOS_GALLERY_SLUG_MAX is false, and anything outside [a-z0-9-] is
// false - which is what refuses "..", ".", a NUL, a slash, a backslash, a
// percent escape and every non-ASCII byte, without naming any of them.
bool eos_gallery_slug_ok(const char *s, int n);

// Builds gallery/<slug>.vox and gallery/<slug>.json. Either output may be NULL
// when the caller wants only the other. Returns EOS_ERR_ARG for a slug that is
// not one, EOS_ERR_TOOBIG when the paths do not fit `cap` (which must be at
// least EOS_PATH_MAX).
eos_err_t eos_gallery_paths(const char *slug, char *vox, char *json, int cap);

// ---------------------------------------------------------- the active slug

// Creates EOS_GALLERY_DIR if it is not there. The parent is created too, since
// eos_storage_mkdir() is one level. Safe to call repeatedly; it stats first, so
// the common path costs no flash write.
eos_err_t eos_gallery_ensure_dir(void);

// Reads EOS_GALLERY_ACTIVE into `out`. Returns the length, or a negative
// eos_err_t: EOS_ERR_NOTFOUND when the file is absent, EOS_ERR_ARG when what is
// in it is not a slug. Trailing whitespace and a trailing newline are trimmed,
// so a file written by `echo pip >` works. Nothing is cached: the file IS the
// state, and a cache is the copy that goes stale under a /api/fs/write nobody
// told this component about.
int eos_gallery_active(char *out, int cap);

// Writes EOS_GALLERY_ACTIVE. One whole-file write of about twenty bytes, which
// on this chip means the instruction cache is off for one flash page while the
// panel and the radio wait. Does NOT load the model - eos_gallery_select() is
// what does both, in the order that cannot leave a pointer at a model that will
// not parse.
eos_err_t eos_gallery_set_active(const char *slug);

// Resolves what the board should be wearing into paths. Fills `vox` and `json`
// (each at least EOS_PATH_MAX) and, if `slug_out` is not NULL, the slug that
// won. Returns EOS_OK, or EOS_ERR_NOTFOUND when the gallery cannot answer - no
// active file, an active slug with no .vox behind it, or an empty gallery - at
// which point eos_apps falls back to the legacy /int/buddy/buddy.vox. When the
// active file names nothing usable but the gallery holds entries, the first
// entry in directory order wins rather than nothing: a board that has models
// should wear one.
eos_err_t eos_gallery_resolve(char *vox, char *json, int cap,
                              char *slug_out, int slug_cap);

// How many <slug>.vox files the gallery holds. Negative on a directory that
// will not open.
int eos_gallery_count(void);

// ------------------------------------------------------------- the machine

// Makes `slug` the live buddy, with no reboot. The order is the whole point:
// the model is PARSED first, and the pointer on the filesystem moves only after
// it has. A model that does not parse is refused, the previous one is reloaded,
// and nothing was written - so a bad import cannot cost the owner the buddy
// they had, which is the entire reason this component exists. Returns EOS_OK,
// EOS_ERR_ARG for a bad slug or a model eos_vox_parse() refused,
// EOS_ERR_NOTFOUND when there is no such entry, or the filesystem's error when
// the pointer could not be persisted - in which case the previous buddy is put
// back rather than the board being left half-moved.
eos_err_t eos_gallery_select(const char *slug);

// Deletes one entry, both files. Refuses the live one and refuses the last one,
// both with EOS_ERR_STATE: a gallery you can empty is a gallery that can lose
// you your last penguin, and deleting what the panel is drawing is the bug this
// component was written to close. Select something else first.
eos_err_t eos_gallery_remove(const char *slug);

// --------------------------------------------------------------- the peek

// What a listing says about one model, read from the first
// EOS_GALLERY_PEEK_BYTES of its file. `ok` is false when this board could not
// read the header, and that is NOT the same as the model being broken: the
// entry still lists, still carries its size, and is still selectable, because
// eos_vox_parse() is the authority and it gets the whole file. A number here is
// advisory; a refusal at select time is not.
//
// `voxels` is the count XYZI declares, which is the number BEFORE
// eos_vox_finish() drops the buried voxels. /api/buddy reports the number after
// for the live model. Pip is 1,280 here and 572 there, and web/README.md
// already tells the editor to show both.
typedef struct {
    uint16_t voxels;
    uint8_t  sx, sy, sz;
    bool     ok;
} eos_gallery_peek_t;

// Reads the header of the .vox at `path`. Never fails destructively: an
// unreadable or nonsense file comes back as { 0, 0,0,0, false }.
void eos_gallery_peek(const char *path, eos_gallery_peek_t *out);

// ------------------------------------------------------------------ routes

// The three gallery routes. eos_apps_dispatch() forwards to this rather than
// eos_httpd reaching it directly, because eos_httpd holds exactly one API
// pointer and a second one is a second thing to leave NULL.
int eos_gallery_dispatch(eos_httpd_t *h, int route,
                         const eos_httpd_req_t *req, eos_httpd_resp_t *r);

#endif // EOS_GALLERY_H
