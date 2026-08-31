// probe_lcd — confirms the LAFVIN ESP32-C6-1.47inch pinout by driving the panel.
//
// Every pin below was read off the running factory firmware over JTAG, not taken
// from a datasheet: FSPICLK_OUT was found on GPIO7, FSPID_OUT on GPIO6,
// LEDC_LS_SIG_OUT0 on GPIO22 and RMT_SIG_OUT0 on GPIO8. CS/DC/RST are plain
// GPIOs, identified by which ones toggled while the panel was being written.
//
// If the pinout is right you get eight colour bars, a white border, and a cyan
// line sweeping down the screen. If DC and RST were swapped you get nothing at
// all, because the panel never leaves reset.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#define PIN_SCLK 7
#define PIN_MOSI 6
#define PIN_CS   14
#define PIN_DC   15
#define PIN_RST  21
#define PIN_BL   22

#define W 172
#define H 320
#define BAND 40                       // 240x40x2 = 19200 B, banded like the OS

static const char *TAG = "probe";
static uint16_t band[W * BAND];

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{   // ST7789 takes big-endian RGB565 over SPI
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((c >> 8) | (c << 8));
}

void app_main(void)
{
    ledc_timer_config_t lt = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                               .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = { .gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
                                 .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 1023 };
    ledc_channel_config(&lc);

    spi_bus_config_t bus = { .sclk_io_num = PIN_SCLK, .mosi_io_num = PIN_MOSI, .miso_io_num = -1,
                             .quadwp_io_num = -1, .quadhd_io_num = -1,
                             .max_transfer_sz = W * BAND * 2 + 8 };
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
        // 172 columns centred in the ST7789's 240-column window. Without the gap
    // the picture looks fine and sits 34 px sideways, which is the exact trap
    // the C5-LCD-1.47 profile documents for the same panel size.
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(p, 34, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(p, true));    // IPS panels want this
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(p, true));
    ESP_LOGI(TAG, "panel up: ST7789 %dx%d  sclk=%d mosi=%d cs=%d dc=%d rst=%d bl=%d",
             W, H, PIN_SCLK, PIN_MOSI, PIN_CS, PIN_DC, PIN_RST, PIN_BL);

    // heap_budget_bytes must be measured, not guessed - STATUS.md calls the
    // guessed value the likeliest thing to bite on real silicon.
    ESP_LOGI(TAG, "HEAP free=%u largest=%u total=%u  (radios still down)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "HEAP internal free=%u  a 240x240 RGB565 frame needs %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)(W*H*2));
    void *fb = heap_caps_malloc(W*H*2, MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "HEAP full-framebuffer alloc: %s", fb ? "SUCCEEDED" : "FAILED");
    if (fb) { ESP_LOGI(TAG, "HEAP free with frame held=%u",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT)); free(fb); }

    static const uint16_t BAR[8] = {0};
    uint16_t bars[8] = { rgb(255,255,255), rgb(255,255,0), rgb(0,255,255), rgb(0,255,0),
                         rgb(255,0,255), rgb(255,0,0), rgb(0,0,255), rgb(20,20,20) };
    (void)BAR;

    int sweep = 0;
    while (1) {
        for (int y0 = 0; y0 < H; y0 += BAND) {
            for (int y = 0; y < BAND; y++) {
                int gy = y0 + y;
                for (int x = 0; x < W; x++) {
                    uint16_t c = bars[(x * 8) / W];
                    if (gy < 2 || gy >= H - 2 || x < 2 || x >= W - 2) c = rgb(255,255,255);
                    if (gy >= sweep && gy < sweep + 4)                 c = rgb(0,220,255);
                    if (gy < 60 && x < 60)                             c = rgb(255,0,0);
                    band[y * W + x] = c;
                }
            }
            esp_lcd_panel_draw_bitmap(p, 0, y0, W, y0 + BAND, band);
        }
        sweep = (sweep + 6) % H;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
