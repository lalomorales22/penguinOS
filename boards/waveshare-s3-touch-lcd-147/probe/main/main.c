// probe_lcd — brings up the Waveshare ESP32-S3-Touch-LCD-1.47.
//
// This is the first board in the fleet whose pinout did NOT have to be recovered
// over JTAG: the vendor publishes a complete pin definition, so every constant
// below is read from documentation. That makes this probe a VERIFICATION rather
// than a discovery — it exists to catch a wrong datasheet, not to find the pins.
//
// It also settles the three things the datasheet does not state:
//   1. the touch controller's I2C address, by scanning the bus;
//   2. whether the panel needs the 34-column gap the other 172x320 boards need;
//   3. what the heap actually looks like with 8MB of octal PSRAM mapped in.
//
// If the pinout is right you get eight colour bars, a white border, a cyan line
// sweeping down, and a RED square in the TOP-LEFT corner. Red proves inversion
// is correct (it renders cyan if wrong — exact complements admit no other
// reading) and the corner proves the column gap.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"

// Display — vendor pin definition, "Pin Definition" panel of the listing.
#define PIN_SCLK 38     // LCD_CLK
#define PIN_MOSI 39     // LCD_DIN
#define PIN_CS   21     // LCD_CS
#define PIN_DC   45     // LCD_DC
#define PIN_RST  40     // LCD_RST
#define PIN_BL   46     // LCD_BL

// Touch — shares the header I2C bus (GPIO41 SCL / GPIO42 SDA).
#define PIN_TP_SCL 41
#define PIN_TP_SDA 42
#define PIN_TP_RST 47
#define PIN_TP_INT 48

// microSD. The vendor documents the slot as 4-bit SDIO (D0-D3 + CMD + CLK),
// which is the fast way to drive it. penguinOS mounts it in SPI mode instead,
// because an SD card speaks both and the registry has nowhere to record SDIO
// pin names — see the sd-is-sdio-run-as-spi gotcha. The mapping below is the
// standard one and uses the same physical pins:
#define PIN_SD_CLK  16      // SD_CLK  -> SCK
#define PIN_SD_CMD  15      // SD_CMD  -> MOSI
#define PIN_SD_D0   17      // SD_D0   -> MISO
#define PIN_SD_D3   14      // SD_D3   -> CS

#define W 320          // landscape: swap_xy turns the native 172x320 on its side
#define H 172

static const char *TAG = "probe";


static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{   // MEASURED on this board: NO byte swap. The four-stripe test put yellow at
    // the bottom, which only happens if the panel reads the uint16_t as stored.
    // The C6 boards need the swap; this one does not, and swapping here while
    // also inverting is exactly what rendered pure red as yellow.
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Name the chip if the address is one we recognise. An address alone is not an
// identification, so anything unfamiliar is reported as unknown rather than
// guessed at.
static const char *touch_name(uint8_t addr)
{
    switch (addr) {
        case 0x15: return "CST816S / CST816T (Hynitron)";
        case 0x38: return "FT6236 / FT6336 (FocalTech)";
        case 0x5D: return "GT911 (Goodix, alt addr 0x14)";
        case 0x14: return "GT911 (Goodix)";
        case 0x63: return "AXS5106L (Axsemi)";
        case 0x2E: return "AXS15231B (Axsemi)";
        default:   return "unknown — look it up before trusting it";
    }
}

static void probe_sdcard(void)
{
    // SPI3, so the card never contends with the panel on SPI2 and no bus lock
    // is needed on this board — unlike the CYD, where they share.
    spi_bus_config_t sb = { .sclk_io_num = PIN_SD_CLK, .mosi_io_num = PIN_SD_CMD,
                            .miso_io_num = PIN_SD_D0, .quadwp_io_num = -1,
                            .quadhd_io_num = -1, .max_transfer_sz = 4096 };
    if (spi_bus_initialize(SPI3_HOST, &sb, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "SD  SPI3 bus init FAILED");
        return;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_D3;
    slot.host_id = SPI3_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,     // never reformat a user's card
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t e = esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &mcfg, &card);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "SD  mount FAILED (%s) — no card inserted, or not FAT",
                 esp_err_to_name(e));
        spi_bus_free(SPI3_HOST);
        return;
    }
    ESP_LOGI(TAG, "SD  MOUNTED  name=%s  %lluMB  speed=%dkHz",
             card->cid.name,
             ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024),
             card->max_freq_khz);
    esp_vfs_fat_sdcard_unmount("/sd", card);
    spi_bus_free(SPI3_HOST);
}

static void scan_i2c(void)
{
    // The touch chip holds its bus lines released while in reset, so a scan with
    // TP_RST still low finds an empty bus and looks exactly like bad wiring.
    gpio_config_t rst = { .pin_bit_mask = 1ULL << PIN_TP_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(80));      // controllers want tens of ms to boot

    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_TP_SCL,
        .sda_io_num = PIN_TP_SDA,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init FAILED on scl=%d sda=%d", PIN_TP_SCL, PIN_TP_SDA);
        return;
    }

    ESP_LOGI(TAG, "I2C scan on scl=%d sda=%d (TP_RST=%d released)",
             PIN_TP_SCL, PIN_TP_SDA, PIN_TP_RST);
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "I2C   FOUND 0x%02X  %s", a, touch_name(a));
            found++;
        }
    }
    if (!found)
        ESP_LOGW(TAG, "I2C   no devices answered — touch is NOT on this bus as wired");
    i2c_del_master_bus(bus);
}

void app_main(void)
{
    ESP_LOGI(TAG, "PSRAM %s, size=%u",
             esp_psram_is_initialized() ? "up" : "DOWN",
             (unsigned)esp_psram_get_size());

    scan_i2c();
    probe_sdcard();

    ledc_timer_config_t lt = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                               .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = { .gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
                                 .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 1023 };
    ledc_channel_config(&lc);

    spi_bus_config_t bus = { .sclk_io_num = PIN_SCLK, .mosi_io_num = PIN_MOSI, .miso_io_num = -1,
                             .quadwp_io_num = -1, .quadhd_io_num = -1,
                             .max_transfer_sz = W * H * 2 + 8 };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t iocfg = { .dc_gpio_num = PIN_DC, .cs_gpio_num = PIN_CS,
        .pclk_hz = 40 * 1000 * 1000, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &iocfg, &io));

    esp_lcd_panel_handle_t p = NULL;
    esp_lcd_panel_dev_config_t pc = { .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pc, &p));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(p));
    ESP_ERROR_CHECK(esp_lcd_panel_init(p));
    // 172 columns centred in the ST7789's 240-column window: (240-172)/2 = 34.
    // Same trap as the C5 and LAFVIN profiles document — without the gap the
    // picture looks entirely fine and sits 34 px sideways.
    // EXACTLY what eos_display_st7789.c does for rotation 1: swap_xy plus one
    // mirror. swap_xy alone is a transpose - a reflection, not a rotation - and
    // that is what the corner test caught: two corners pinned, two exchanged.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(p, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(p, false, false));  // back to the config that read correctly
    // With swap_xy the panel's 34-column gap lands on the Y axis.
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(p, 0, 34));
    // Inversion is deliberately OFF for this test. The four stripes below each
    // encode pure red a different way; whichever one actually looks red tells
    // us the byte order AND whether the panel inverts natively. Guessing these
    // two independently is what turns red into yellow.
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(p, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(p, true));
    ESP_LOGI(TAG, "panel up: ST7789 %dx%d  sclk=%d mosi=%d cs=%d dc=%d rst=%d bl=%d",
             W, H, PIN_SCLK, PIN_MOSI, PIN_CS, PIN_DC, PIN_RST, PIN_BL);

    // heap_budget_bytes must be measured, not guessed.
    ESP_LOGI(TAG, "HEAP internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "HEAP spiram   free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    // Tier 2 wants TWO full frames. On every previous board that was out of the
    // question; here it should be comfortable.
    void *fb1 = heap_caps_malloc(W*H*2, MALLOC_CAP_SPIRAM);
    void *fb2 = heap_caps_malloc(W*H*2, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "HEAP double-buffer (2 x %u B) in PSRAM: %s",
             (unsigned)(W*H*2), (fb1 && fb2) ? "SUCCEEDED" : "FAILED");
    if (fb1) free(fb1);
    if (fb2) free(fb2);

    // GOLD, four ways. The red test settled on "no swap" and that turned out to
    // be wrong: penguinOS renders the goldleaf theme's #d4af37 as green, which
    // is exactly what 0xD566 looks like when its bytes arrive reversed. Red was
    // a poor probe colour precisely because 0xF800 and 0x00F8 are both plausible
    // as "some colour" - gold is not, and neither is green.
    //
    // Panel inversion is OFF, so quadrants 3 and 4 pre-invert to simulate it.
    //   TL  plain 0xD566              -> no swap, no invert
    //   TR  swapped 0x66D5            -> swap needed
    //   BL  complement 0x2A99         -> panel inverts
    //   BR  swapped complement 0x992A -> both
    // Exactly one quadrant will be GOLD. Orientation is already settled as
    // identity, so buffer top-left really is screen top-left.
    uint16_t *fb = heap_caps_malloc(W * H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!fb) { ESP_LOGE(TAG, "no framebuffer"); return; }

    const uint16_t GOLD = 0xD566;                 /* #d4af37 in host-order 565 */
    const uint16_t Q[4] = { GOLD,
                            (uint16_t)((GOLD >> 8) | (GOLD << 8)),
                            (uint16_t)~GOLD,
                            (uint16_t)(((uint16_t)~GOLD >> 8) | ((uint16_t)~GOLD << 8)) };
    static const char *WHAT[4] = { "plain (no swap, no invert)", "byte-swapped",
                                   "complement (panel inverts)", "swapped complement" };
    for (int i = 0; i < 4; i++)
        ESP_LOGI(TAG, "quadrant %d = 0x%04X  %s", i + 1, Q[i], WHAT[i]);

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int q = (y >= H / 2 ? 2 : 0) + (x >= W / 2 ? 1 : 0);
            fb[y * W + x] = Q[q];
        }
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(p, 0, 0, W, H, fb));
    ESP_LOGI(TAG, "gold quadrants drawn, %dx%d", W, H);

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
