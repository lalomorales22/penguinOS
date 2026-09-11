// eos_ota — writing a new penguinOS into the slot that is not running.
//
// The three ports eos_httpd declares, implemented against esp_ota. The whole
// feature is "update from the web app instead of a cable", and the reason it
// can exist at all is partitions.csv carrying the app twice.
//
// WHAT MAKES THIS SAFE IS THE BOOTLOADER, NOT THIS FILE.
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE means a freshly written image boots in
// PENDING_VERIFY, and if it does not call
// esp_ota_mark_app_valid_cancel_rollback() before the next restart the
// bootloader goes back to the slot that worked. So a bad update costs a reboot,
// not a USB cable - which matters most on exactly the boards you cannot easily
// reach, which are the ones people want this feature for.
//
// eos_ota_mark_running_good() below is the other half of that bargain and
// main.c calls it once the display, the filesystem and the web server are all
// up. That is the honest definition of "this image works" for something whose
// job is to be reachable: an image that boots and then cannot serve its own
// update page is not one you want to keep.

#include "eos_ota.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "eos_ota";

static esp_ota_handle_t s_h;
static const esp_partition_t *s_part;
static bool     s_open;
static uint32_t s_written;     // bytes accepted so far, and the only cursor
static uint32_t s_expect;      // total the client promised, or 0

int eos_ota_begin(void *ctx, uint32_t total)
{
    const esp_partition_t *next;
    esp_err_t err;

    (void)ctx;

    // An upload already in flight is abandoned rather than joined. Two browsers
    // pushing different images into one slot produces an image that is neither.
    if (s_open) {
        esp_ota_abort(s_h);
        s_open = false;
    }

    next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        // The single-slot table. Worth naming, because the alternative is a
        // board that accepts a whole upload and fails at the end.
        ESP_LOGE(TAG, "no second app slot: this image was built with a "
                      "single-slot partition table");
        return EOS_ERR_NODEV;
    }

    // Refuse an image that cannot fit BEFORE spending a minute receiving it.
    // total is 0 when the client did not say, and then the write path finds out
    // the hard way - which is still safe, just later.
    if (total && total > next->size) {
        ESP_LOGE(TAG, "image is %u bytes, slot '%s' holds %u",
                 (unsigned)total, next->label, (unsigned)next->size);
        return EOS_ERR_TOOBIG;
    }

    // OTA_SIZE_UNKNOWN erases the whole slot, which on a 1.6 MB partition is
    // seconds of blocking flash work. Passing the real size erases only what is
    // needed, so a client that says how big the image is gets a faster start as
    // well as an earlier refusal.
    err = esp_ota_begin(next, total ? total : OTA_SIZE_UNKNOWN, &s_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return EOS_ERR_IO;
    }

    s_part    = next;
    s_open    = true;
    s_written = 0;
    s_expect  = total;
    ESP_LOGI(TAG, "begin  slot '%s', %u bytes, expecting %u",
             next->label, (unsigned)next->size, (unsigned)total);
    return EOS_OK;
}

int eos_ota_write(void *ctx, uint32_t offset, const void *data, int len)
{
    esp_err_t err;

    (void)ctx;
    if (!s_open) return EOS_ERR_STATE;
    if (!data || len <= 0) return EOS_ERR_ARG;

    // IN ORDER, STRICTLY. esp_ota_write appends to its own cursor and cannot
    // seek, so an offset that is not exactly where we are would write the
    // client's bytes to the wrong place and produce an image that passes every
    // length check and is garbage. Refusing is the only correct answer, and the
    // client is told what the board expected so it can resume rather than guess.
    if (offset != s_written) {
        ESP_LOGW(TAG, "offset %u arrived out of order; expected %u",
                 (unsigned)offset, (unsigned)s_written);
        return EOS_ERR_STATE;
    }
    if (s_part && s_written + (uint32_t)len > s_part->size) return EOS_ERR_TOOBIG;

    err = esp_ota_write(s_h, data, (size_t)len);
    if (err != ESP_OK) {
        // ESP_ERR_OTA_VALIDATE_FAILED here is not an I/O problem and must not
        // be reported as one. esp_ota_write checks the image header on the
        // FIRST chunk, so this is what "you uploaded the wrong file" looks
        // like - a .565 picture, a .vox buddy, a zip - and it is caught after
        // one kilobyte instead of after a minute and a half. Telling someone
        // their flash failed when they picked the wrong file sends them to
        // debug the board.
        bool bad_image = (err == ESP_ERR_OTA_VALIDATE_FAILED);
        ESP_LOGE(TAG, "esp_ota_write at %u: %s%s", (unsigned)offset,
                 esp_err_to_name(err),
                 bad_image ? " - this is not a penguinOS image" : "");
        esp_ota_abort(s_h);
        s_open = false;
        return bad_image ? EOS_ERR_ARG : EOS_ERR_IO;
    }
    s_written += (uint32_t)len;
    return EOS_OK;
}

int eos_ota_end(void *ctx, bool commit)
{
    esp_err_t err;

    (void)ctx;
    if (!s_open) return EOS_ERR_STATE;

    if (!commit) {
        esp_ota_abort(s_h);
        s_open = false;
        ESP_LOGI(TAG, "abandoned after %u bytes; the running image is untouched",
                 (unsigned)s_written);
        return EOS_OK;
    }

    if (s_expect && s_written != s_expect) {
        ESP_LOGE(TAG, "short image: %u of %u bytes",
                 (unsigned)s_written, (unsigned)s_expect);
        esp_ota_abort(s_h);
        s_open = false;
        return EOS_ERR_ARG;
    }

    // esp_ota_end validates the image header and checksum. A truncated or
    // corrupt upload is refused HERE, while the running image is still the one
    // the bootloader points at, which is the whole reason this is a separate
    // call from set_boot_partition below.
    err = esp_ota_end(s_h);
    s_open = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s - the image was not accepted",
                 esp_err_to_name(err));
        return (err == ESP_ERR_OTA_VALIDATE_FAILED) ? EOS_ERR_ARG : EOS_ERR_IO;
    }

    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return EOS_ERR_IO;
    }

    ESP_LOGI(TAG, "committed %u bytes to '%s'; next boot runs it",
             (unsigned)s_written, s_part ? s_part->label : "?");
    return EOS_OK;
}

void eos_ota_mark_running_good(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;

    if (!run) return;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return;

    // This is the moment the rollback is cancelled. Reaching here means the
    // panel came up, the filesystem mounted and the web server is listening -
    // so the image can be updated AGAIN from the web app, which is the property
    // that actually matters. An image that boots but cannot serve its own
    // update page would be a board needing the cable, and the bootloader should
    // take it back.
#ifdef EOS_OTA_NEVER_CONFIRM
    // A TEST HOOK, and the only way to prove the safety net actually catches
    // anything. Built with -DEOS_OTA_NEVER_CONFIRM=1 this image is completely
    // functional and simply never tells the bootloader it works - which is
    // exactly what a genuinely broken update looks like from the bootloader's
    // side, without having to build something broken and risk not getting it
    // back. It boots once, and the reboot after that returns to the slot that
    // was working.
    //
    // Never set in a shipped build. The warning is loud because an image that
    // silently declined to confirm itself would roll back every time and look
    // like a board that cannot be updated at all.
    (void)st;
    ESP_LOGW(TAG, "EOS_OTA_NEVER_CONFIRM is set: NOT confirming this image. "
                  "The next reboot will roll back to the previous slot.");
#else
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        ESP_LOGI(TAG, "this image is confirmed good; rollback cancelled");
    else
        ESP_LOGW(TAG, "could not confirm this image; a reboot will roll it back");
#endif
}

const char *eos_ota_running_slot(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run ? run->label : "?";
}

#else  /* host build */

int  eos_ota_begin(void *ctx, uint32_t total) { (void)ctx; (void)total; return EOS_ERR_UNSUPPORTED; }
int  eos_ota_write(void *ctx, uint32_t off, const void *d, int n)
{ (void)ctx; (void)off; (void)d; (void)n; return EOS_ERR_UNSUPPORTED; }
int  eos_ota_end(void *ctx, bool commit) { (void)ctx; (void)commit; return EOS_ERR_UNSUPPORTED; }
void eos_ota_mark_running_good(void) { }
const char *eos_ota_running_slot(void) { return "host"; }

#endif
