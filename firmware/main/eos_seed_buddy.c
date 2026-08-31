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

#define SEED(sym)                                                     \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t sym##_end[]   asm("_binary_" #sym "_end");

SEED(penguin_vox)
SEED(buddy_json)

static const char *TAG = "eos";

static bool put(const char *path, const uint8_t *s, const uint8_t *e)
{
    int n = (int)(e - s);
    if (eos_storage_exists(path)) return false;
    if (eos_storage_save(path, s, n) != EOS_OK) {
        ESP_LOGW(TAG, "buddy  could not seed %s", path);
        return false;
    }
    ESP_LOGI(TAG, "buddy  seeded %s (%d B)", path, n);
    return true;
}

void eos_seed_buddy(void)
{
    // mkdir is one level and the parent is the mount, so this is the only one.
    if (!eos_storage_exists(EOS_APPS_BUDDY_DIR))
        (void)eos_storage_mkdir(EOS_APPS_BUDDY_DIR);

    bool wrote = put(EOS_APPS_BUDDY_DIR "/buddy.vox",  penguin_vox_start, penguin_vox_end);
    wrote |= put(EOS_APPS_BUDDY_DIR "/buddy.json", buddy_json_start, buddy_json_end);

    // Only worth a reload if something landed; otherwise the owner's own
    // buddy is already there and eos_apps will have picked it up.
    if (wrote) (void)eos_apps_buddy_reload();
}
