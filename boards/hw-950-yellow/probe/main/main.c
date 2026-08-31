// probe_lcd — identifies the HW-950 "yellow board" (a Cheap Yellow Display).
//
// This board is the first here that can ANSWER the question rather than being
// guessed at: unlike the C6 and S3 boards, the CYD family wires the panel's
// MISO (GPIO12), so the controller's own ID register is readable. That turns
// "which controller is this" from an inference into a measurement.
//
// It is also the first board that CANNOT be scanned over JTAG. The original
// ESP32 has no USB-serial-JTAG peripheral - it talks through a CH340 bridge -
// so the register-reading procedure used on the C6 boards needs external
// hardware here. Reading the panel ID is the substitute, and it is better.
//
// The pin set below is the standard CYD assignment, taken from the existing
// boards/cyd-2432s024n.json profile, which describes the same ESP32-D0WD-V3.
// If the ID read comes back as zeros or 0xFF, the pins are wrong and nothing
// below is trustworthy.

#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#define PIN_SCLK 14
#define PIN_MOSI 13
#define PIN_MISO 12     // wired on CYD boards. This is what makes the ID read possible.
#define PIN_CS   15
#define PIN_DC    2
#define PIN_RST  -1     // CYD ties panel reset to the board's own reset
#define PIN_BL   27     // 27 on the 2432S024N profile; some variants use 21

#define W 240           // ILI9341 native is 240x320 PORTRAIT, which is what was asked for
#define H 320
#define BAND 40         // 240x40x2 = 19,200 B. The ESP32 has ~300KB; a whole
                        // 240x320 frame is 153,600 B and is not worth the risk.

static const char *TAG = "probe";
// Width stays at 240 - the SAFE value for both candidates. The previous
// attempt painted 320 wide and the picture SHEARED: the controller clamped
// its column window to a narrower panel and the surplus 80 pixels of each
// row spilled into the next one, stepping the image diagonally. That is
// itself evidence the panel is 240 columns, but it also made the height
// question unreadable. Overrunning WIDTH shears; overrunning HEIGHT just
// gets discarded, so height can be tested cleanly on its own.
#define PW 240
#define PH 320          // MEASURED: 320 rows, by the clamp hairline test
static uint16_t band[PW * BAND];

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{   // Host order. Whether the wire wants it swapped is a board fact - see the
    // byte_swap field - and the quadrant test below is what settles it.
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Name the controller from its ID register. Only these three are plausible on
// a CYD; anything else is reported raw rather than guessed at.
static const char *panel_name(const uint8_t *id)
{
    if (id[1] == 0x93 && id[2] == 0x41) return "ILI9341  (240x320)";
    if (id[1] == 0x77 && id[2] == 0x96) return "ST7796   (320x480)";
    if (id[1] == 0x94 && id[2] == 0x88) return "ILI9488  (320x480)";
    if (id[1] == 0x85 && id[2] == 0x52) return "ST7789V  (240x320)";
    return "UNRECOGNISED - write the raw bytes down, do not guess";
}

void app_main(void)
{
    // Backlight first: a correct panel on a dark backlight looks identical to a
    // dead one, and that has cost time on every board so far.
    ledc_timer_config_t lt = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                               .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = { .gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
                                 .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 1023 };
    ledc_channel_config(&lc);
    ESP_LOGI(TAG, "backlight on GPIO%d at full", PIN_BL);

    spi_bus_config_t bus = { .sclk_io_num = PIN_SCLK, .mosi_io_num = PIN_MOSI,
                             .miso_io_num = PIN_MISO, .quadwp_io_num = -1,
                             .quadhd_io_num = -1, .max_transfer_sz = W * BAND * 2 + 8 };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    // A SLOW io handle purely for the ID read. Controllers clock their read path
    // far below their write path, and an ID read at 40MHz returns plausible
    // garbage rather than failing - which is worse than failing.
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t iocfg = { .dc_gpio_num = PIN_DC, .cs_gpio_num = PIN_CS,
        .pclk_hz = 8 * 1000 * 1000, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &iocfg, &io));

    struct { uint8_t cmd; int n; const char *what; } reads[] = {
        { 0x04, 4, "RDDID   manufacturer / version / driver" },
        { 0xD3, 4, "RDID4   the one that names ILI9341/ST7796/ILI9488" },
        { 0x09, 5, "RDDST   display status" },
        { 0x0A, 2, "RDDPM   power mode" },
        // The resolution question, answered by the panel itself. Display
        // Function Control's NL field is the number of driven lines as
        // 8*(NL+1): 0x27 -> 320 lines (a 240x320 part), 0x3B -> 480 lines (a
        // 320x480 part). It MUST be read before esp_lcd_panel_init(), which
        // writes B6h itself and would overwrite the power-on value.
        { 0xB6, 5, "DISFCTRL  param 3 is the LINE COUNT - the resolution answer" },
    };
    uint8_t disp_fn[5] = {0};
    uint8_t id4[4] = {0};
    for (unsigned i = 0; i < sizeof(reads)/sizeof(reads[0]); i++) {
        uint8_t buf[8] = {0};
        esp_err_t e = esp_lcd_panel_io_rx_param(io, reads[i].cmd, buf, reads[i].n);
        if (e != ESP_OK) { ESP_LOGW(TAG, "read 0x%02X failed: %s", reads[i].cmd, esp_err_to_name(e)); continue; }
        char hex[32]; int p = 0;
        for (int b = 0; b < reads[i].n; b++) p += snprintf(hex + p, sizeof(hex) - p, "%02X ", buf[b]);
        ESP_LOGI(TAG, "PANEL 0x%02X -> %-14s %s", reads[i].cmd, hex, reads[i].what);
        if (reads[i].cmd == 0xD3) memcpy(id4, buf, 4);
        if (reads[i].cmd == 0xB6) memcpy(disp_fn, buf, 5);
    }
    ESP_LOGI(TAG, "PANEL identified as: %s", panel_name(id4));
    // Byte 0 of a read is a dummy on these parts, so the four real parameters
    // are bytes 1..4 and NL is the third of them.
    for (int k = 1; k <= 2; k++) {
        unsigned nl = disp_fn[k + 2] & 0x3Fu;      // try both dummy-offset readings
        ESP_LOGI(TAG, "RESOLUTION if param3 is byte %d: 0x%02X -> NL=%u -> %u lines%s",
                 k + 2, disp_fn[k + 2], nl, 8u * (nl + 1u),
                 (8u * (nl + 1u) == 480u) ? "   <== 320x480, the 3.5 inch board"
                 : (8u * (nl + 1u) == 320u) ? "   <== 240x320, the smaller board" : "");
    }
    if (id4[1] == 0 && id4[2] == 0)
        ESP_LOGW(TAG, "PANEL all zeros - MISO is probably NOT on GPIO%d, or the pins are wrong", PIN_MISO);

    ESP_LOGI(TAG, "HEAP free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // Now drive it. ILI9341 is the assumption; the ID above is what confirms it.
    esp_lcd_panel_handle_t p = NULL;
    esp_lcd_panel_dev_config_t pc = { .reset_gpio_num = PIN_RST,
        // RGB here DELIBERATELY: with BGR forced on, a channel-order error and a
        // byte-order error are not separable. Driving RGB makes the three-primary
        // test below name the model outright.
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &pc, &p));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(p));
    ESP_ERROR_CHECK(esp_lcd_panel_init(p));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(p, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(p, true));
    ESP_LOGI(TAG, "panel up: %dx%d PORTRAIT, sck %d mosi %d miso %d cs %d dc %d bl %d",
             W, H, PIN_SCLK, PIN_MOSI, PIN_MISO, PIN_CS, PIN_DC, PIN_BL);

    // THREE PRIMARIES, and the panel is driven in RGB order so that byte order
    // and channel order are separable. Each of the four possible models
    // produces a DIFFERENT sequence, so simply naming the three bands top to
    // bottom identifies the model outright - no arithmetic, no judgement call:
    //
    //     no swap + RGB  ->  RED    GREEN  BLUE
    //     no swap + BGR  ->  BLUE   GREEN  RED
    //     swap    + RGB  ->  BLUE   RED    GREEN
    //     swap    + BGR  ->  RED    BLUE   GREEN
    //
    // Pure primaries are used because they are the only colours nobody has to
    // interpret. Gold was the right instrument for separating swap from
    // inversion on the S3; it is the WRONG one here, because under swap+BGR it
    // lands on a yellow-green that reads as "yellowish gold" and confirms the
    // wrong model - which is exactly what happened.
    const uint16_t BANDS[3] = { 0xF800, 0x07E0, 0x001F };   /* R, G, B as stored */
    ESP_LOGI(TAG, "THREE PRIMARY TEST, panel in RGB order, no inversion");
    ESP_LOGI(TAG, "  stored top..bottom: 0xF800 (red) 0x07E0 (green) 0x001F (blue)");
    ESP_LOGI(TAG, "  R,G,B=no-swap+RGB   B,G,R=no-swap+BGR   B,R,G=swap+RGB   R,B,G=swap+BGR");

    for (int y0 = 0; y0 < PH; y0 += BAND) {
        int rows = (y0 + BAND > PH) ? (PH - y0) : BAND;
        for (int y = 0; y < rows; y++) {
            int gy = y0 + y, b = gy / 107;
            if (b > 2) b = 2;
            for (int x = 0; x < PW; x++) band[y * PW + x] = BANDS[b];
        }
        esp_lcd_panel_draw_bitmap(p, 0, y0, PW, y0 + rows, band);
    }
    ESP_LOGI(TAG, "painted three full-width bands at %dx%d", PW, PH);

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
