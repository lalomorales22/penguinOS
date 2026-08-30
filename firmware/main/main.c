// app_main — a placeholder that proves the image is the right image.
//
// This is the smallest thing that is still worth flashing: it says which board
// descriptor got compiled in, asks the silicon what it actually is, and prints
// the heap. Nothing is initialised, nothing is drawn, and app_main returns.
//
// The one non-obvious constraint: it goes through eos_board_get() rather than
// touching EOS_BOARD from the generated header, even though this file could
// reach either. That is the whole layering in one line — above the HAL, the
// board is a pointer, and no source outside the eos_kernel component ever
// includes a boards/generated header. The boot glue replaces this file; it
// should inherit that rule and not the convenience.
//
// The mismatch check at the end is the one thing here that earns its flash. A
// board's identity is not probeable, so flashing a C6 image onto a C5 produces
// a panel that never leaves reset and no other symptom. Three fields — SoC,
// flash size, PSRAM presence — are all the silicon will confirm, and confirming
// them costs a boot line.

#include <inttypes.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "eos_board.h"

static const char *TAG = "eos";

void app_main(void)
{
    const eos_board_t *b = eos_board_get();
    eos_probe_t probe;
    uint8_t bad;

    ESP_LOGI(TAG, "ESP-OS placeholder image. Nothing is drawn yet.");

    ESP_LOGI(TAG, "board   %s (%s)", b->id, b->name);
    ESP_LOGI(TAG, "chip    %s %s, %u core, flash %" PRIu32 " KB, psram %" PRIu32 " KB",
             eos_soc_name(b->soc), b->variant, (unsigned)b->cores,
             b->flash_bytes / 1024u, b->psram_bytes / 1024u);
    ESP_LOGI(TAG, "tier    %s, compositor %s, lvgl %d",
             eos_tier_name(b->tier), eos_comp_name(b->render.compositor),
             (int)b->render.lvgl);
    ESP_LOGI(TAG, "panel   %s %dx%d rot %u, %u bpp wire, %" PRIu32 " Hz, invert %d",
             eos_panel_name(b->panel.panel),
             (int)eos_board_screen_w(b), (int)eos_board_screen_h(b),
             (unsigned)b->panel.rotation, (unsigned)b->panel.wire_bytes,
             b->panel.hz, (int)b->panel.invert);
    ESP_LOGI(TAG, "pins    sck %d mosi %d cs %d dc %d rst %d bl %d",
             (int)b->panel.sck, (int)b->panel.mosi, (int)b->panel.cs,
             (int)b->panel.dc, (int)b->panel.rst, (int)b->panel.bl);
    ESP_LOGI(TAG, "render  %s, band %d rows, frame %" PRIu32 " B, band %" PRIu32 " B, budget %" PRIu32 " B",
             b->render.full_framebuffer ? "full frame" : "banded",
             (int)b->render.band_h, eos_board_fb_bytes(b), eos_board_band_bytes(b),
             b->render.heap_budget);
    ESP_LOGI(TAG, "storage internal fs \"%s\" at %s", b->storage.int_label, b->storage.int_point);

    ESP_LOGI(TAG, "heap    free %u, largest block %u, total %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "heap    internal free %u, a full %dx%d RGB565 frame needs %d",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (int)eos_board_screen_w(b), (int)eos_board_screen_h(b),
             (int)eos_board_screen_w(b) * (int)eos_board_screen_h(b) * 2);

    if (eos_board_probe(&probe) != EOS_OK) {
        ESP_LOGE(TAG, "probe  failed");
        return;
    }
    ESP_LOGI(TAG, "probe   %s, flash %" PRIu32 " KB, psram %" PRIu32 " KB, "
                  "mac %02x:%02x:%02x:%02x:%02x:%02x",
             eos_soc_name(probe.soc), probe.flash_bytes / 1024u, probe.psram_bytes / 1024u,
             probe.mac[0], probe.mac[1], probe.mac[2],
             probe.mac[3], probe.mac[4], probe.mac[5]);

    bad = eos_board_check(b, &probe);
    if (bad == 0) {
        ESP_LOGI(TAG, "check   silicon agrees with the board descriptor");
    } else {
        // Not a warning. The wrong header was flashed, and every pin number
        // above is then a guess about someone else's board.
        ESP_LOGE(TAG, "check   MISMATCH 0x%02x:%s%s%s - wrong board header flashed", bad,
                 (bad & EOS_MISMATCH_SOC)   ? " soc"   : "",
                 (bad & EOS_MISMATCH_FLASH) ? " flash" : "",
                 (bad & EOS_MISMATCH_PSRAM) ? " psram" : "");
        ESP_LOGE(TAG, "check   rebuild with -DEOS_BOARD_ID=<the board you are holding>");
    }
}
