// eos_seed_buddy — puts the shipped themes on /int, and makes sure Pip is in
// the buddy gallery every single time the board comes up.
//
// The themes half is unchanged and still writes once: a theme the owner edited
// is theirs. The buddy half is not, and the difference is the point of the
// gallery.
//
// It used to write /int/buddy/buddy.vox exactly once, gated on the file not
// existing, and never again. That was the right rule when the buddy was one
// file, and it is what turned a single import into losing Pip for an evening:
// the import overwrote him, the seeder saw a file present and kept quiet, and
// getting him back took a remove-and-reboot over curl. Now there is a gallery
// and Pip is one entry in it, so the rule becomes narrower and much stronger.
// This seeds gallery/pip.vox whenever gallery/pip.vox is not there - on every
// boot, not just the first - and touches nothing else. An import cannot
// overwrite him because an import lands under its own slug. A delete cannot
// lose him because the gallery refuses to delete the live entry and refuses to
// go empty, and because if he somehow does go, the next boot brings him back.
//
// It also carries the one-way migration. A board that has been running the old
// firmware has an owner's model at /int/buddy/buddy.vox and no gallery at all,
// so that file is MOVED into the gallery rather than left behind: renamed, not
// copied, because a copy is a second source of truth and because a rename on
// LittleFS is metadata where a copy is six kilobytes of flash. If those bytes
// are byte-for-byte the penguin this image ships, they are Pip and he goes in
// as "pip"; anything else goes in as "buddy", which is what it was called.
//
// The one non-obvious constraint: this runs before the network and before the
// panel is doing anything interesting, and every branch below can write to
// flash - which on this single-core chip turns the instruction cache off while
// it happens. That is affordable exactly here, at boot, and it is why the
// steady-state path stats first and writes nothing at all.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "eos_storage.h"
#include "eos_apps.h"
#include "eos_gallery.h"
#include <stdio.h>

#define SEED(sym)                                                     \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t sym##_end[]   asm("_binary_" #sym "_end");

SEED(penguin_vox)
SEED(buddy_json)

// The rest of the gallery. Their .vox and .json are authored under
// assets/buddy/ and every one of them was checked against this board's caps
// before it was listed here: EOS_APPS_VOX_BYTES is 7,264 and
// EOS_APPS_BUDDY_JSON_BYTES is 768, and a shipped model past either would be
// seeded onto the filesystem and then refused by the thing that loads it,
// which is the one failure a shipped buddy must never have.
SEED(cat_vox)
SEED(cat_json)
SEED(owl_vox)
SEED(owl_json)
SEED(robot_vox)
SEED(robot_json)

// The six themes embedded as binaries. cyd-amber is NOT here: it is already
// linked in as a TXTFILE for the boot-theme fallback, and embedding a file
// twice defines the same symbol twice. Its text form carries a trailing NUL
// that the binary form does not, so the seeder drops one byte when writing it.
SEED(carbon_json)
SEED(catppuccin_mocha_json)
SEED(ember_json)
SEED(goldleaf_json)
SEED(gruvbox_json)
SEED(tokyonight_json)
extern const char cyd_amber_txt_start[] asm("_binary_cyd_amber_json_start");
extern const char cyd_amber_txt_end[]   asm("_binary_cyd_amber_json_end");

#define EOS_THEME_DIR "/int/themes"

static const char *TAG = "eos";

static bool put_n(const char *path, const uint8_t *s, int n)
{
    if (n <= 0) return false;
    if (eos_storage_exists(path)) return false;
    if (eos_storage_save(path, s, n) != EOS_OK) {
        ESP_LOGW(TAG, "buddy  could not seed %s", path);
        return false;
    }
    ESP_LOGI(TAG, "seeded %s (%d B)", path, n);
    return true;
}

static bool put(const char *path, const uint8_t *s, const uint8_t *e)
{
    return put_n(path, s, (int)(e - s));
}

// Without this the theme picker offers exactly one theme - the compiled-in
// fallback - while seven ship in the image. eos_settings lists what it can see
// on the filesystem, and nothing had ever put them there.
static void seed_themes(void)
{
    static const struct { const char *name; const uint8_t *s, *e; } T[] = {
        { "carbon",           carbon_json_start,           carbon_json_end           },
        { "catppuccin-mocha", catppuccin_mocha_json_start, catppuccin_mocha_json_end },
        { "ember",            ember_json_start,            ember_json_end            },
        { "goldleaf",         goldleaf_json_start,         goldleaf_json_end         },
        { "gruvbox",          gruvbox_json_start,          gruvbox_json_end          },
        { "tokyonight",       tokyonight_json_start,       tokyonight_json_end       },
    };
    char path[64];
    int i, n = 0;

    if (!eos_storage_exists(EOS_THEME_DIR))
        (void)eos_storage_mkdir(EOS_THEME_DIR);

    for (i = 0; i < (int)(sizeof T / sizeof T[0]); i++) {
        snprintf(path, sizeof path, EOS_THEME_DIR "/%s.json", T[i].name);
        if (put(path, T[i].s, T[i].e)) n++;
    }

    // cyd-amber comes from the text embed. EMBED_TXTFILES appends a NUL that
    // EMBED_FILES does not, so one byte comes off the length - and it is done
    // on the LENGTH, because gcc will not reason about arithmetic on the end
    // of an extern array and rejects it under -Werror=array-bounds.
    {
        int len = (int)(cyd_amber_txt_end - cyd_amber_txt_start);
        if (len > 0) len--;
        snprintf(path, sizeof path, EOS_THEME_DIR "/cyd-amber.json");
        if (put_n(path, (const uint8_t *)cyd_amber_txt_start, len)) n++;
    }

    if (n) ESP_LOGI(TAG, "themes seeded %d of 7 into %s", n, EOS_THEME_DIR);
}

// ==========================================================================
// The gallery
// ==========================================================================

#define LEGACY_VOX  EOS_APPS_BUDDY_DIR "/buddy.vox"
#define LEGACY_JSON EOS_APPS_BUDDY_DIR "/buddy.json"

// The models this image ships, one row each. Adding a buddy to penguinOS is
// adding a row here plus its two EMBED_FILES lines in CMakeLists.txt, and
// nothing else: the slug becomes the filename, the .json becomes what the
// gallery lists it under, and the loop below already refuses to overwrite a
// model the owner has edited.
//
// pip is not merely the first row, he is the one the rest of this file treats
// as load-bearing - the migration recognises his bytes and the active pointer
// falls back to him. A second row is an ordinary buddy; pip is the floor.
static const struct {
    const char   *slug;
    const uint8_t *vox_s, *vox_e;
    const uint8_t *json_s, *json_e;
} SHIPPED[] = {
    { "pip",   penguin_vox_start, penguin_vox_end, buddy_json_start,  buddy_json_end  },
    { "mochi", cat_vox_start,     cat_vox_end,     cat_json_start,    cat_json_end    },
    { "hoot",  owl_vox_start,     owl_vox_end,     owl_json_start,    owl_json_end    },
    { "bolt",  robot_vox_start,   robot_vox_end,   robot_json_start,  robot_json_end  },
};
#define N_SHIPPED ((int)(sizeof SHIPPED / sizeof SHIPPED[0]))

// True when the file at `path` is byte-for-byte these `n` bytes. Read in 128
// byte bites rather than staged whole: this runs at boot on a board with about
// 117 KB of heap and the alternative is a 7 KB buffer that exists to answer one
// question once. A size mismatch answers it without opening anything at all,
// which is the case on every board that ever ran the old firmware with a model
// of its own.
static bool same_bytes(const char *path, const uint8_t *s, int n)
{
    eos_stat_t st;
    eos_file_t *f;
    uint8_t buf[128];
    int off = 0;
    bool same = true;

    if (n <= 0) return false;
    if (eos_storage_stat(path, &st) != EOS_OK || st.is_dir) return false;
    if ((int)st.size != n) return false;

    f = eos_storage_open(path, EOS_O_READ);
    if (!f) return false;
    while (off < n) {
        int want = (n - off) < (int)sizeof buf ? (n - off) : (int)sizeof buf;
        int got  = eos_storage_read(f, buf, want);
        if (got <= 0 || memcmp(buf, s + off, (size_t)got) != 0) { same = false; break; }
        off += got;
    }
    eos_storage_close(f);
    return same && off == n;
}

// Moves the pre-gallery /int/buddy/buddy.vox into the gallery, once. `to` is
// renamed onto only when nothing is there; the owner's gallery always wins over
// a file the old firmware left behind.
static bool move_in(const char *from, const char *to)
{
    if (!eos_storage_exists(from)) return false;
    if (eos_storage_exists(to)) {
        // Already migrated on an earlier boot, or the owner made an entry by
        // this name themselves. Either way the legacy copy is the redundant
        // one and leaving it would make the search order matter forever.
        (void)eos_storage_remove(from);
        return false;
    }
    if (eos_storage_rename(from, to) != EOS_OK) {
        ESP_LOGW(TAG, "buddy  could not move %s into the gallery", from);
        return false;
    }
    ESP_LOGI(TAG, "buddy  moved %s -> %s", from, to);
    return true;
}

// Fills `slug_out` with the name the legacy model was filed under, and leaves
// it alone when there was nothing to move. That slug is not a detail for the
// log: a pre-gallery buddy.vox is by definition what the board was WEARING, so
// it is the strongest evidence there is of what the owner chose, and
// ensure_active() below prefers it over the shipped default. Without that, an
// owner who imported their own model watches an update quietly put a penguin
// back on the panel - which is this component's own bug with the roles swapped.
static bool migrate_legacy(char *slug_out, int slug_cap)
{
    char vox[EOS_PATH_MAX], json[EOS_PATH_MAX];
    const char *slug;
    int shipped_len = (int)(penguin_vox_end - penguin_vox_start);

    if (!eos_storage_exists(LEGACY_VOX)) return false;

    // If those bytes are the penguin this image ships, the owner's "buddy" IS
    // Pip and filing him as a second entry would put two identical penguins in
    // a gallery whose whole job is telling penguins apart.
    slug = same_bytes(LEGACY_VOX, penguin_vox_start, shipped_len) ? "pip" : "buddy";

    if (eos_gallery_paths(slug, vox, json, EOS_PATH_MAX) != EOS_OK) return false;

    if (move_in(LEGACY_VOX, vox)) {
        // The metadata follows the model, and only the model's move earns it:
        // a buddy.json without a buddy.vox describes nothing.
        (void)move_in(LEGACY_JSON, json);
        // Only the branch that actually moved a model names a slug. The branch
        // below moved nothing, so there is no wearing to preserve and the
        // caller must fall through to the shipped default.
        if (slug_out && slug_cap > 0)
            snprintf(slug_out, (size_t)slug_cap, "%s", slug);
    } else {
        // The model did not move, so neither does its metadata - but a stray
        // legacy buddy.json left beside a gallery is a file the loader would
        // never read and the owner would keep finding. It goes with it.
        (void)eos_storage_remove(LEGACY_JSON);
    }
    // Either branch moved or removed a file, so what eos_apps last resolved is
    // no longer what is on the filesystem.
    return true;
}

// Whichever of the shipped models is not on the filesystem. This is the bug
// fix: it runs on EVERY boot, not only the first, so the shipped buddies are a
// floor the board cannot fall through rather than a one-time gift. An edited
// pip.vox is not overwritten - put_n() stats first - so a Pip the owner has
// changed is still theirs.
static int seed_shipped(void)
{
    char vox[EOS_PATH_MAX], json[EOS_PATH_MAX];
    int i, n = 0;

    for (i = 0; i < N_SHIPPED; i++) {
        if (eos_gallery_paths(SHIPPED[i].slug, vox, json, EOS_PATH_MAX) != EOS_OK) continue;
        if (put(vox,  SHIPPED[i].vox_s,  SHIPPED[i].vox_e)) n++;
        (void)put(json, SHIPPED[i].json_s, SHIPPED[i].json_e);
    }
    return n;
}

// The active pointer, which is only written when it is missing or broken. An
// owner who chose a buddy keeps that choice across a reboot and across an
// update, which is the entire point of the file.
static bool ensure_active(const char *prefer)
{
    char slug[EOS_GALLERY_SLUG_MAX + 1];
    char vox[EOS_PATH_MAX], json[EOS_PATH_MAX];

    if (eos_gallery_active(slug, (int)sizeof slug) > 0 &&
        eos_gallery_paths(slug, vox, NULL, EOS_PATH_MAX) == EOS_OK &&
        eos_storage_exists(vox))
        return false;

    // What the board was wearing before the update outranks what this image
    // ships, and it only ever gets here on the single boot that migrated it -
    // afterwards there is an active file and the check above has already
    // returned. An owner who imported a model of their own keeps it on the
    // panel; one whose legacy file was Pip's bytes migrated in as "pip" and
    // lands on the same answer either way.
    if (prefer && prefer[0] &&
        eos_gallery_paths(prefer, vox, NULL, EOS_PATH_MAX) == EOS_OK &&
        eos_storage_exists(vox)) {
        if (eos_gallery_set_active(prefer) != EOS_OK) return false;
        ESP_LOGI(TAG, "buddy  active is %s, migrated from %s", prefer, LEGACY_VOX);
        return true;
    }

    // Pip first, because a board with nothing chosen should come up wearing the
    // penguin it is named for. Failing him - an image with a different SHIPPED
    // table, or a filesystem that refused the seed - whatever the gallery
    // actually holds.
    if (eos_gallery_paths("pip", vox, NULL, EOS_PATH_MAX) == EOS_OK &&
        eos_storage_exists(vox)) {
        if (eos_gallery_set_active("pip") != EOS_OK) return false;
        ESP_LOGI(TAG, "buddy  active is pip");
        return true;
    }
    if (eos_gallery_resolve(vox, json, EOS_PATH_MAX, slug, (int)sizeof slug) == EOS_OK &&
        eos_gallery_set_active(slug) == EOS_OK) {
        ESP_LOGI(TAG, "buddy  active is %s", slug);
        return true;
    }
    return false;
}

void eos_seed_buddy(void)
{
    char migrated[EOS_GALLERY_SLUG_MAX + 1];
    bool moved, chose;
    int seeded;

    migrated[0] = '\0';

    seed_themes();

    if (eos_gallery_ensure_dir() != EOS_OK) {
        ESP_LOGW(TAG, "buddy  no %s; the gallery is unavailable this boot",
                 EOS_GALLERY_DIR);
        return;
    }

    // Order matters and is not arbitrary. Migrating first means a legacy file
    // that IS Pip becomes gallery/pip.vox, and the seed step then finds him
    // already there and writes nothing - one rename instead of a rename plus
    // six kilobytes of flash.
    moved  = migrate_legacy(migrated, (int)sizeof migrated);
    seeded = seed_shipped();
    chose  = ensure_active(migrated);

    ESP_LOGI(TAG, "buddy  gallery holds %d, %d seeded this boot",
             eos_gallery_count(), seeded);

    // Wider than the old version's "only if something landed", and it has to
    // be: the migration can move the model out from under a path eos_apps has
    // already resolved, and the active pointer can have been written for the
    // first time, and neither of those is a seed. The steady-state boot changes
    // nothing and reloads nothing - the caller's own reload does that work once
    // rather than this doing it twice.
    if (moved || seeded || chose) (void)eos_apps_buddy_reload();
}
