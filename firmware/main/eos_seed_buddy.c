// eos_seed_buddy — writes the shipped buddy onto /int the first time the board
// boots with an empty filesystem.
//
// Without this the panel shows a penguin that the web app cannot see: the
// avatar is compiled into the image, GET /api/buddy reports what is on /int,
// and /int is empty on a new board - so the Buddy tab 404s next to a board
// visibly wearing a buddy, and anyone wanting to change him starts from a
// blank grid rather than from the thing already on screen.
//
// The one non-obvious constraint: this runs ONCE, gated on the file not
// existing, and never overwrites. A buddy the owner edited is theirs; a seed
// that reasserts itself on every boot would silently undo their work.

#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"
#include "eos_storage.h"
#include "eos_apps.h"
#include <stdio.h>

#define SEED(sym)                                                     \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t sym##_end[]   asm("_binary_" #sym "_end");

SEED(penguin_vox)
SEED(buddy_json)

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

    // cyd-amber comes from the text embed, minus the NUL it appends.
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

void eos_seed_buddy(void)
{
    seed_themes();

    // mkdir is one level and the parent is the mount, so this is the only one.
    if (!eos_storage_exists(EOS_APPS_BUDDY_DIR))
        (void)eos_storage_mkdir(EOS_APPS_BUDDY_DIR);

    bool wrote = put(EOS_APPS_BUDDY_DIR "/buddy.vox",  penguin_vox_start, penguin_vox_end);
    wrote |= put(EOS_APPS_BUDDY_DIR "/buddy.json", buddy_json_start, buddy_json_end);

    // Only worth a reload if something landed; otherwise the owner's own
    // buddy is already there and eos_apps will have picked it up.
    if (wrote) (void)eos_apps_buddy_reload();
}
