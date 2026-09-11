// eos_ota — the three eos_httpd ports that update penguinOS over the web,
// plus the call that tells the bootloader a freshly written image works.
//
// Every function is safe to call on a board whose partition table has one app
// slot: they answer EOS_ERR_NODEV and nothing is written. That is the honest
// behaviour for an image built before partitions.csv grew a second slot.
#ifndef EOS_OTA_H
#define EOS_OTA_H

#include <stdbool.h>
#include <stdint.h>
#include "eos_board.h"   // eos_err_t

// Opens the slot that is NOT running. total is the whole image length, or 0
// when the client does not know it - with it, an image too big for the slot is
// refused now instead of after a minute of upload, and only the bytes needed
// are erased rather than the whole partition.
int eos_ota_begin(void *ctx, uint32_t total);

// One chunk. Offsets MUST arrive in order: esp_ota_write appends and cannot
// seek, so anything else is EOS_ERR_STATE rather than a quietly wrong image.
int eos_ota_write(void *ctx, uint32_t offset, const void *data, int len);

// Validates, and on success points the bootloader at the new slot. Does not
// restart - the caller still has a response to send. commit false abandons the
// slot and leaves the running image alone.
int eos_ota_end(void *ctx, bool commit);

// Cancels the rollback for the image now running, if it is pending. Call it
// once the board is demonstrably working - panel up, filesystem mounted, web
// server listening - and NOT merely once app_main has started: the property
// worth confirming is that this image can be updated again.
void eos_ota_mark_running_good(void);

// "ota_0" / "ota_1", or "factory" on a single-slot image. For the web app.
const char *eos_ota_running_slot(void);

#endif
