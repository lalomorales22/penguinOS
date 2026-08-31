// eos_gallery — the buddy gallery: several models on the board, one of them
// live, and an import that can no longer be the thing that loses you Pip.
//
// The whole component is three questions. Which models are here, which one is
// live, and can this name safely become a filename. The third is the one with
// teeth: a slug arrives in a POST body from whoever is on the WiFi and comes
// out the far end as a path handed to eos_storage_open(), so the rule is a
// whitelist - [a-z0-9-], one to EOS_GALLERY_SLUG_MAX bytes - checked over an
// explicit length rather than over a C string. A whitelist is not a stylistic
// preference here. "..", ".", a NUL, "%2e%2e", a backslash and a Cyrillic "а"
// are all refused by the same four lines without any of them being named, and
// the rule that has to name its attacks is the rule that misses the next one.
//
// The second thing worth reading before changing anything is the ORDER inside
// eos_gallery_select(). The model is parsed first and the pointer on the
// filesystem moves only after the parse has succeeded. Do it the other way and
// a model that will not parse leaves the board pointing at it across a reboot -
// which is exactly the failure this component was written to end, with the
// filename changed. A refused select writes nothing at all.
//
// The one non-obvious constraint: every buffer here is file-static and not on
// the stack, for the same reason eos_apps.c gives. eos_httpd serialises
// dispatch behind one mutex, so there is one request in flight image-wide and
// one of each buffer is the right number; and an HTTP worker has a 5,376-byte
// stack with the request body already on it, which two EOS_PATH_MAX paths plus
// a 768-byte metadata stage would eat a fifth of. Nothing here allocates.
//
// Flash: listing writes nothing. Selecting writes about twenty bytes, once.
// Removing erases two files. Each of those stops this chip while it happens -
// the instruction cache is off on the single core that is also driving the
// panel and the radio - which is why a select is one small write and never a
// copy of the model.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "eos_gallery.h"
#include "eos_vox.h"

// ==========================================================================
// State
// ==========================================================================
//
// Scratch, shared under eos_httpd's dispatch mutex. See the file header.

static struct {
    char vox[EOS_PATH_MAX];
    char json[EOS_PATH_MAX];
    char slug[EOS_GALLERY_SLUG_MAX + 1];
} s_g;

// The .vox header window and the <slug>.json stage. The json stage is exactly
// what eos_apps stages a buddy.json in, deliberately: a document too large for
// the loader must also be too large for the listing, or the Buddy tab shows a
// name the board will never actually use.
static uint8_t s_peek[EOS_GALLERY_PEEK_BYTES];
static char    s_meta[EOS_APPS_BUDDY_JSON_BYTES];

// ==========================================================================
// Slugs
// ==========================================================================

bool eos_gallery_slug_ok(const char *s, int n)
{
    int i;

    if (!s) return false;
    if (n < 0) n = (int)strlen(s);
    if (n <= 0 || n > EOS_GALLERY_SLUG_MAX) return false;

    for (i = 0; i < n; i++) {
        char c = s[i];
        bool good = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!good) return false;
    }
    return true;
}

eos_err_t eos_gallery_paths(const char *slug, char *vox, char *json, int cap)
{
    int need;

    if (cap < EOS_PATH_MAX) return EOS_ERR_ARG;
    if (!eos_gallery_slug_ok(slug, -1)) return EOS_ERR_ARG;

    if (vox) {
        need = snprintf(vox, (size_t)cap, EOS_GALLERY_DIR "/%s.vox", slug);
        if (need < 0 || need >= cap) return EOS_ERR_TOOBIG;
    }
    if (json) {
        need = snprintf(json, (size_t)cap, EOS_GALLERY_DIR "/%s.json", slug);
        if (need < 0 || need >= cap) return EOS_ERR_TOOBIG;
    }
    return EOS_OK;
}

// The slug behind a directory entry, or false for anything that is not one of
// this gallery's models. It is what quietly does the right thing with the two
// files that will otherwise turn up in here: "pip.json" has the wrong suffix,
// and an abandoned upload's "pip.vox.part" has a stem of "pip.vox", which
// contains a dot and is therefore not a slug. Neither becomes an entry, and
// neither had to be named as a special case.
static bool slug_of_entry(const char *name, char *out, int cap)
{
    int n = (int)strlen(name);

    if (n <= 4 || strcmp(name + n - 4, ".vox") != 0) return false;
    n -= 4;
    if (!eos_gallery_slug_ok(name, n)) return false;
    if (n >= cap) return false;
    memcpy(out, name, (size_t)n);
    out[n] = '\0';
    return true;
}

// One scan of the gallery directory. Returns how many models are in it, or a
// negative eos_err_t; fills `first` with the first slug in directory order when
// it is not NULL. Two callers wanted a count and a first entry and a scan is
// the expensive part, so they share one.
static int scan(char *first, int first_cap)
{
    eos_dirh_t *d;
    eos_dirent_t de;
    char slug[EOS_GALLERY_SLUG_MAX + 1];
    int n = 0;

    if (first && first_cap > 0) first[0] = '\0';

    d = eos_storage_opendir(EOS_GALLERY_DIR);
    if (!d) return (int)EOS_ERR_NOTFOUND;

    while (eos_storage_readdir(d, &de)) {
        if (de.is_dir) continue;
        if (!slug_of_entry(de.name, slug, (int)sizeof slug)) continue;
        if (n == 0 && first && first_cap > 0)
            snprintf(first, (size_t)first_cap, "%s", slug);
        n++;
    }
    eos_storage_closedir(d);
    return n;
}

int eos_gallery_count(void) { return scan(NULL, 0); }

// ==========================================================================
// The active slug
// ==========================================================================

eos_err_t eos_gallery_ensure_dir(void)
{
    eos_err_t e;

    // eos_storage_mkdir() is one level, so the parent goes first. Both are
    // stat-gated: the common case is that they exist and this costs no write.
    if (!eos_storage_exists(EOS_APPS_BUDDY_DIR)) {
        e = eos_storage_mkdir(EOS_APPS_BUDDY_DIR);
        if (e != EOS_OK) return e;
    }
    if (!eos_storage_exists(EOS_GALLERY_DIR)) {
        e = eos_storage_mkdir(EOS_GALLERY_DIR);
        if (e != EOS_OK) return e;
    }
    return EOS_OK;
}

int eos_gallery_active(char *out, int cap)
{
    char buf[EOS_GALLERY_SLUG_MAX + 8];
    int n;

    if (!out || cap <= 0) return (int)EOS_ERR_ARG;
    out[0] = '\0';

    // A file larger than a slug plus a line ending comes back TOOBIG and is
    // treated as no pointer at all, which is right: whatever is in there, it is
    // not something this board wrote.
    n = eos_storage_load(EOS_GALLERY_ACTIVE, buf, (int)sizeof buf - 1);
    if (n < 0) return n;

    // A trailing newline, and anything else below a space. `echo pip > active`
    // over curl is a reasonable thing for the owner to do and it should work.
    while (n > 0 && (unsigned char)buf[n - 1] <= ' ') n--;

    if (!eos_gallery_slug_ok(buf, n)) return (int)EOS_ERR_ARG;
    if (n >= cap) return (int)EOS_ERR_TOOBIG;

    memcpy(out, buf, (size_t)n);
    out[n] = '\0';
    return n;
}

eos_err_t eos_gallery_set_active(const char *slug)
{
    char line[EOS_GALLERY_SLUG_MAX + 2];
    int n;

    if (!eos_gallery_slug_ok(slug, -1)) return EOS_ERR_ARG;
    n = snprintf(line, sizeof line, "%s\n", slug);
    if (n < 0 || n >= (int)sizeof line) return EOS_ERR_TOOBIG;
    return eos_storage_save(EOS_GALLERY_ACTIVE, line, n);
}

eos_err_t eos_gallery_resolve(char *vox, char *json, int cap,
                              char *slug_out, int slug_cap)
{
    char slug[EOS_GALLERY_SLUG_MAX + 1];
    eos_stat_t st;
    bool found = false;

    if (!vox || !json || cap < EOS_PATH_MAX) return EOS_ERR_ARG;

    if (eos_gallery_active(slug, (int)sizeof slug) > 0 &&
        eos_gallery_paths(slug, vox, json, cap) == EOS_OK &&
        eos_storage_stat(vox, &st) == EOS_OK && !st.is_dir) {
        found = true;
    } else if (scan(slug, (int)sizeof slug) > 0 && slug[0] &&
               eos_gallery_paths(slug, vox, json, cap) == EOS_OK) {
        // The pointer is missing, unreadable or names a model that is not
        // there, and the gallery is not empty. Wearing the first entry beats
        // falling all the way back to the compiled-in penguin, and NOTHING is
        // written here on purpose: a read-only or full filesystem still boots
        // wearing a buddy, and the next select is what makes the choice stick.
        found = true;
    }

    if (!found) return EOS_ERR_NOTFOUND;
    if (slug_out && slug_cap > 0) snprintf(slug_out, (size_t)slug_cap, "%s", slug);
    return EOS_OK;
}

// ==========================================================================
// The header peek
// ==========================================================================
//
// A second reader of the .vox format, which the rest of this OS is right to be
// suspicious of - eos_apps.c refuses to pre-validate a model precisely because
// a second opinion can only be wrong. This one earns its place by being unable
// to matter. It reads the first EOS_GALLERY_PEEK_BYTES of a file, writes two
// numbers into a struct, and touches no voxel pool, no palette and no model. A
// bug in it mis-labels a row in a list. eos_vox_parse() still gets the whole
// file at select time and is still the only thing that decides whether a model
// loads, and the host suite drives the same files through both and asserts they
// agree about dimensions and count.
//
// The alternative was parsing every entry to list the directory: one 7 KB stage
// and one sort of up to 1,536 voxels per row, on the task that is also holding
// the dispatch mutex, at 10 Hz. A ten-model gallery would have been a visible
// hitch on the panel every time the tab was opened.

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void eos_gallery_peek(const char *path, eos_gallery_peek_t *out)
{
    eos_file_t *f;
    uint32_t lim, pos, end;
    int got;
    bool have_size = false;

    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!path) return;

    f = eos_storage_open(path, EOS_O_READ);
    if (!f) return;
    got = eos_storage_read(f, s_peek, (int)sizeof s_peek);
    eos_storage_close(f);

    if (got < 8 + 12) return;
    lim = (uint32_t)got;

    if (memcmp(s_peek, "VOX ", 4) != 0) return;
    {
        uint32_t ver = rd32(s_peek + 4);
        if (ver < 100 || ver > 250) return;         // the same window eos_vox opens
    }
    if (memcmp(s_peek + 8, "MAIN", 4) != 0) return;

    // MAIN's own content is empty in every file anyone writes; if it is not, it
    // is in front of everything this wants and the answer is simply "cannot
    // tell", which is what ok:false means.
    {
        uint32_t main_content = rd32(s_peek + 12);
        if (main_content > lim - 20) return;
        pos = 20 + main_content;
    }
    end = lim;                                       // walk only what was read

    while (pos + 12 <= end) {
        const uint8_t *id = s_peek + pos;
        uint32_t clen  = rd32(s_peek + pos + 4);
        uint32_t klen  = rd32(s_peek + pos + 8);
        uint32_t cbody = pos + 12;

        if (!have_size && memcmp(id, "SIZE", 4) == 0) {
            uint32_t sx, sy, sz;
            if (clen < 12 || cbody + 12 > end) return;
            sx = rd32(s_peek + cbody);
            sy = rd32(s_peek + cbody + 4);
            sz = rd32(s_peek + cbody + 8);
            if (sx == 0 || sy == 0 || sz == 0) return;
            if (sx > EOS_VOX_MAX_DIM || sy > EOS_VOX_MAX_DIM || sz > EOS_VOX_MAX_DIM)
                return;
            out->sx = (uint8_t)sx;
            out->sy = (uint8_t)sy;
            out->sz = (uint8_t)sz;
            have_size = true;

        } else if (memcmp(id, "XYZI", 4) == 0) {
            uint32_t n;
            if (clen < 4 || cbody + 4 > end) return;
            n = rd32(s_peek + cbody);
            if (n > EOS_VOX_MAX_VOXELS) return;
            out->voxels = (uint16_t)n;
            out->ok     = have_size;
            return;                                  // the first pair wins, as in eos_vox
        }

        // Stepping over a chunk needs its whole length to be inside the window.
        // A file that buries SIZE behind something bigger than the window is
        // not broken, it is unreadable FROM HERE, and it lists as ok:false and
        // stays perfectly selectable.
        if (clen > end - cbody) return;
        if (klen > end - cbody - clen) return;
        pos = cbody + clen + klen;
    }
}

// ==========================================================================
// The state machine
// ==========================================================================

eos_err_t eos_gallery_select(const char *slug)
{
    eos_stat_t st;
    eos_err_t e;

    if (!eos_gallery_slug_ok(slug, -1)) return EOS_ERR_ARG;

    e = eos_gallery_paths(slug, s_g.vox, s_g.json, EOS_PATH_MAX);
    if (e != EOS_OK) return e;
    if (eos_storage_stat(s_g.vox, &st) != EOS_OK || st.is_dir) return EOS_ERR_NOTFOUND;

    // Parse before the pointer moves. A model that eos_vox_parse() refuses has
    // already overwritten the one voxel pool this board has - see the long
    // comment in eos_apps_buddy_reload() - so the recovery is to reload
    // whatever the filesystem still says is active, which is untouched because
    // nothing has been written yet.
    e = eos_apps_buddy_reload_from(slug, s_g.vox, s_g.json);
    if (e != EOS_OK) {
        (void)eos_apps_buddy_reload();
        return e;
    }

    e = eos_gallery_set_active(slug);
    if (e != EOS_OK) {
        // The model is live but the choice would not survive a reboot, which is
        // a board in two minds. Put the previous buddy back so that what is on
        // the panel and what is on the filesystem agree, and let the client say
        // the filesystem is full.
        (void)eos_apps_buddy_reload();
        return e;
    }
    return EOS_OK;
}

eos_err_t eos_gallery_remove(const char *slug)
{
    char act[EOS_GALLERY_SLUG_MAX + 1];
    eos_err_t e;
    int n;

    if (!eos_gallery_slug_ok(slug, -1)) return EOS_ERR_ARG;

    e = eos_gallery_paths(slug, s_g.vox, s_g.json, EOS_PATH_MAX);
    if (e != EOS_OK) return e;
    if (!eos_storage_exists(s_g.vox)) return EOS_ERR_NOTFOUND;

    // An upload on its way to either of these files. /api/fs/remove already
    // refuses this and the gallery must refuse it identically, or the two
    // delete paths disagree about one rule. The concrete loss is small and
    // ugly: delete "x" mid-upload of x.vox and the upload's closing rename
    // puts the model back with its metadata gone, leaving a buddy that lists
    // under its slug and has lost the sentence the owner wrote about it.
    if (eos_apps_upload_targets(s_g.vox) || eos_apps_upload_targets(s_g.json))
        return EOS_ERR_BUSY;

    // Two questions and not one. The file says which buddy survives a reboot;
    // the loaded slug says which one the panel is drawing this second. They
    // agree on a healthy board, and asking both is what stops a delete from
    // taking the model on screen when they have come apart.
    if (eos_gallery_active(act, (int)sizeof act) > 0 && strcmp(act, slug) == 0)
        return EOS_ERR_BUSY;
    if (strcmp(eos_apps_buddy_slug(), slug) == 0)
        return EOS_ERR_BUSY;

    n = eos_gallery_count();
    if (n <= 1) return EOS_ERR_STATE;

    e = eos_storage_remove(s_g.vox);
    if (e != EOS_OK) return e;
    // The metadata is optional and a model without one lists under its slug, so
    // its absence is not a failure and must not turn a completed delete into
    // one - the .vox is already gone.
    (void)eos_storage_remove(s_g.json);
    return EOS_OK;
}

// ==========================================================================
// Handlers
// ==========================================================================

// The same fifteen lines as eos_apps.c's q_uint(). Their real home is
// eos_httpd, next to eos_httpd_query_get() and eos_httpd_flag() - and moving
// them there is an edit to the file every route in this OS is currently being
// appended to. A copy here that is deleted the day someone consolidates them is
// cheaper than that merge, and both copies are covered by their own suites.
static eos_err_t q_uint(const char *uri, const char *name, long def, long max, long *out)
{
    char v[20];
    int n = eos_httpd_query_get(uri, name, v, (int)sizeof v);
    long acc = 0;
    int i;

    *out = def;
    if (n == (int)EOS_ERR_NOTFOUND) return EOS_OK;
    if (n < 0) return EOS_ERR_ARG;
    if (n == 0) return EOS_OK;

    for (i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9') return EOS_ERR_ARG;
        acc = acc * 10 + (v[i] - '0');
        if (acc > max) return EOS_ERR_ARG;
    }
    *out = acc;
    return EOS_OK;
}

// The name for one entry, out of <slug>.json. Falls back to the slug for a
// document that is missing, too large to stage, not JSON, or simply carries no
// name - which is the requirement that matters most in this whole file. One
// half-written metadata file must cost that ONE row its name and nothing else.
// A listing that 500s because a single .json was truncated mid-upload is a
// gallery the owner cannot use to recover from the very thing that truncated
// it.
static void entry_name(const char *json_path, const char *slug, char *out, int cap)
{
    int got, n;

    snprintf(out, (size_t)cap, "%s", slug);

    got = eos_storage_load(json_path, s_meta, (int)sizeof s_meta);
    if (got <= 0) return;
    if (eos_json_get_str(s_meta, got, "name", out, cap, &n) != EOS_JSON_FOUND || n <= 0)
        snprintf(out, (size_t)cap, "%s", slug);
}

#define J_TAIL 96   // bytes kept back for the closing fields, as in h_fs_list

static int h_gallery_list(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    eos_dirh_t *d;
    eos_dirent_t de;
    char slug[EOS_GALLERY_SLUG_MAX + 1];
    char name[EOS_APPS_BUDDY_NAME_MAX];
    const char *live = eos_apps_buddy_slug();
    long offset, count, total = 0;
    int shown = 0;

    if (q_uint(req->uri, "offset", 0, 0x7FFFFFFFL, &offset) != EOS_OK)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG,
                                  "offset must be a non-negative number");
    if (q_uint(req->uri, "count", EOS_GALLERY_LIST_MAX, EOS_GALLERY_LIST_MAX, &count) != EOS_OK)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG,
                                  "count must be a number no larger than limits.list_max");

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_str(&j, "dir", EOS_GALLERY_DIR);
    if (live[0]) eos_json_kv_str (&j, "active", live);
    else         eos_json_kv_null(&j, "active");
    eos_json_key(&j, "entries");
    eos_json_arr_open(&j);

    // A gallery directory that is not there is an EMPTY gallery, not a broken
    // endpoint. That is the state of a board whose seeder has not run yet, and
    // answering 404 would make the Buddy tab look broken on a board that is
    // merely new.
    d = eos_storage_opendir(EOS_GALLERY_DIR);
    while (d && eos_storage_readdir(d, &de)) {
        eos_gallery_peek_t pk;

        if (de.is_dir) continue;
        if (!slug_of_entry(de.name, slug, (int)sizeof slug)) continue;

        total++;
        if (total - 1 < offset) continue;
        if (shown >= count) continue;              // keep counting for `total`

        if (eos_gallery_paths(slug, s_g.vox, s_g.json, EOS_PATH_MAX) != EOS_OK) continue;

        // Stop emitting when one more entry would not leave room to close the
        // document. The page is short, `more` is true, and the client asks for
        // the next one - the same rule /api/fs/list follows.
        entry_name(s_g.json, slug, name, (int)sizeof name);
        if (j.len + eos_json_escaped_len(name, (int)strlen(name))
                  + eos_json_escaped_len(slug, (int)strlen(slug))
                  + 96 + J_TAIL > j.cap) {
            count = shown;
            continue;                              // `total` still counts it; `more` is true
        }

        eos_gallery_peek(s_g.vox, &pk);

        eos_json_obj_open(&j);
        eos_json_kv_str (&j, "slug", slug);
        eos_json_kv_str (&j, "name", name);
        eos_json_kv_int (&j, "voxels", pk.voxels);
        eos_json_key(&j, "dim");
        eos_json_arr_open(&j);
        eos_json_int(&j, pk.sx);
        eos_json_int(&j, pk.sy);
        eos_json_int(&j, pk.sz);
        eos_json_arr_close(&j);
        eos_json_kv_int (&j, "bytes",  (long)de.size);
        eos_json_kv_bool(&j, "active", live[0] && strcmp(live, slug) == 0);
        eos_json_kv_bool(&j, "ok",     pk.ok);
        eos_json_obj_close(&j);
        shown++;
    }
    if (d) eos_storage_closedir(d);

    eos_json_arr_close(&j);
    eos_json_kv_int (&j, "offset", offset);
    eos_json_kv_int (&j, "total",  total);
    eos_json_kv_bool(&j, "more",   offset + shown < total);
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 200, &j);
}

// The slug out of a POST body. One sentence covers every way it can be wrong,
// because from the client's side they are one mistake: the name it sent is not
// a name this board will put on its filesystem.
#define EOS_G_STR2(x) #x
#define EOS_G_STR(x)  EOS_G_STR2(x)
static const char *SLUG_RULE =
    "slug must be 1 to " EOS_G_STR(EOS_GALLERY_SLUG_MAX)
    " bytes of a-z, 0-9 and dash, and nothing else";

static eos_err_t body_slug(const eos_httpd_req_t *req, char *out, int cap)
{
    int n = 0;

    out[0] = '\0';
    if (!req->body || req->body_len <= 0) return EOS_ERR_ARG;
    if (eos_json_get_str(req->body, req->body_len, "slug", out, cap, &n) != EOS_JSON_FOUND)
        return EOS_ERR_ARG;
    // Over the length, and every byte of it - eos_json_get_str() decodes \uXXXX
    // escapes, so this is where a name that was ASCII on the wire and something
    // else by the time it is a filename gets refused.
    if (!eos_gallery_slug_ok(out, n)) return EOS_ERR_ARG;
    return EOS_OK;
}

static int h_gallery_select(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    eos_err_t e;

    if (body_slug(req, s_g.slug, (int)sizeof s_g.slug) != EOS_OK)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG, SLUG_RULE);

    e = eos_gallery_select(s_g.slug);
    if (e == EOS_ERR_NOTFOUND)
        return eos_httpd_fail_err(h, r, (int)e, "there is no buddy by that name in the gallery");
    if (e == EOS_ERR_ARG)
        // The model was refused by eos_vox_parse(), which said why in a
        // sentence. The previous buddy is already back on the panel.
        return eos_httpd_fail_err(h, r, (int)e,
                                  eos_apps_buddy_error() ? eos_apps_buddy_error()
                                                         : "that model is not one this board reads");
    if (e == EOS_ERR_TOOBIG)
        // The model never got as far as being loaded, so the sentence below -
        // which blames the filesystem for not recording a choice - would be
        // false twice over and would send the owner to look at their disk when
        // the thing to look at is their model. Nothing was written either way.
        // A .vox reaches the gallery through /api/fs/write, which caps a CHUNK
        // and not a file, so a model past the loader's stage is a perfectly
        // ordinary thing to find here and it deserves its own sentence with the
        // real number in it.
        return eos_httpd_fail_err(h, r, (int)e,
                                  "that model is larger than the "
                                  EOS_G_STR(EOS_APPS_VOX_BYTES)
                                  " bytes this board stages a .vox in; "
                                  "the previous buddy is still live");
    if (e != EOS_OK)
        return eos_httpd_fail_err(h, r, (int)e,
                                  "that buddy is loadable but the board could not record "
                                  "the choice; the previous one is still live");

    eos_apps_logf('I', "buddy: %s is live", s_g.slug);

    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "ok", true);
    eos_json_kv_str (&j, "active", s_g.slug);
    eos_json_key(&j, "buddy");
    eos_apps_buddy_write_json(&j);
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 200, &j);
}

static int h_gallery_remove(eos_httpd_t *h, const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    eos_json_t j;
    const char *live;
    eos_err_t e;

    if (body_slug(req, s_g.slug, (int)sizeof s_g.slug) != EOS_OK)
        return eos_httpd_fail_err(h, r, (int)EOS_ERR_ARG, SLUG_RULE);

    e = eos_gallery_remove(s_g.slug);
    if (e == EOS_ERR_BUSY)
        return eos_httpd_fail_err(h, r, (int)e,
                                  "that is the buddy this board is wearing; "
                                  "select another one first");
    if (e == EOS_ERR_STATE)
        return eos_httpd_fail_err(h, r, (int)e,
                                  "that is the only buddy on this board; "
                                  "add another one before removing it");
    if (e == EOS_ERR_NOTFOUND)
        return eos_httpd_fail_err(h, r, (int)e, "there is no buddy by that name in the gallery");
    if (e != EOS_OK)
        return eos_httpd_fail_err(h, r, (int)e, "that buddy could not be removed");

    eos_apps_logf('I', "buddy: %s removed from the gallery", s_g.slug);

    live = eos_apps_buddy_slug();
    eos_json_init(&j, h->resp, (int)sizeof h->resp);
    eos_json_obj_open(&j);
    eos_json_kv_bool(&j, "ok", true);
    eos_json_kv_str (&j, "removed", s_g.slug);
    if (live[0]) eos_json_kv_str (&j, "active", live);
    else         eos_json_kv_null(&j, "active");
    eos_json_kv_int (&j, "total", eos_gallery_count());
    eos_json_obj_close(&j);
    return eos_httpd_reply_json(h, r, 200, &j);
}

int eos_gallery_dispatch(eos_httpd_t *h, int route,
                         const eos_httpd_req_t *req, eos_httpd_resp_t *r)
{
    if (!h || !req || !r) return 500;

    switch (route) {
    case EOS_ROUTE_BUDDY_GALLERY:        return h_gallery_list(h, req, r);
    case EOS_ROUTE_BUDDY_GALLERY_SELECT: return h_gallery_select(h, req, r);
    case EOS_ROUTE_BUDDY_GALLERY_REMOVE: return h_gallery_remove(h, req, r);
    default: break;
    }
    return eos_httpd_fail_err(h, r, (int)EOS_ERR_NOTFOUND, "no such endpoint on this board");
}
