// eos_board — what the OS is running on, as one flat const struct.
//
// This is the runtime mirror of boards/*.json. The generator turns one
// registry entry into one static const eos_board_t in flash, and the board
// component hands the kernel a pointer to it at boot. It is the single place
// pin numbers, panel identity and clock rates live, so no driver below this
// line ever carries a #ifdef for a specific board.
//
// Field for field with the registry, minus the parts that only the flashing
// tools need. Everything here is readable at runtime by settings screens and
// by the display backend deciding how much framebuffer it can afford.
//
// This file is the ONLY definition of eos_board_t and of the tier, SoC, panel,
// bus, touch, LED and audio enums. A generated board header includes this one
// and emits data — the EOS_* macros and one initialiser — and no types at all.
// It used to declare a second, near-identical eos_board_t of its own so that it
// would compile standalone, which meant the boot glue could include the HAL API
// or the board data but never both: C puts all enumerators in one namespace, so
// EOS_BUS_*, EOS_COMP_* and EOS_LED_* collided outright. If a field is missing
// for something the registry expresses, it gets added here. Nothing gets a
// second declaration anywhere else.
//
// The one thing to understand: almost none of this is discoverable. The
// runtime can tell you the SoC, the flash size, whether PSRAM answered, and
// the MAC. That is the entire list. The panel controller cannot be probed —
// the ILI9488 on the wavvy boards returns 00 7F DF for register 0xD3, which
// matches no known part, and nothing in penguinOS reads a controller ID because
// of it. So board identity is the registry plus one human confirmation
// (confirm_prompt), and eos_board_check() only verifies the three things that
// are genuinely verifiable. Anything claiming to autodetect a panel is lying.

#ifndef EOS_BOARD_H
#define EOS_BOARD_H

#include <stdint.h>
#include <stdbool.h>

// eos_rect_t comes from the window manager, which is pure logic with no
// hardware or allocation of its own. The HAL borrows that rectangle rather
// than defining a second identical one and forcing every caller to convert.
#include "eos_wm.h"

// ------------------------------------------------------------------ errors

typedef enum {
    EOS_OK              =   0,
    EOS_ERR_ARG         =  -1,  // caller passed nonsense
    EOS_ERR_NODEV       =  -2,  // the board does not have this at all
    EOS_ERR_IO          =  -3,  // bus or media failure
    EOS_ERR_POOL        =  -4,  // a fixed pool is exhausted (never a failed malloc)
    EOS_ERR_NOTFOUND    =  -5,
    EOS_ERR_EXISTS      =  -6,
    EOS_ERR_UNSUPPORTED =  -7,  // valid call, this tier or backend cannot do it
    EOS_ERR_BUSY        =  -8,
    EOS_ERR_TOOBIG      =  -9,  // would not fit a caller-provided buffer
    EOS_ERR_READONLY    = -10,
    EOS_ERR_STATE       = -11,  // called out of order (init missing, frame not open)
} eos_err_t;

static inline const char *eos_strerr(eos_err_t e)
{
    switch (e) {
    case EOS_OK:              return "ok";
    case EOS_ERR_ARG:         return "bad argument";
    case EOS_ERR_NODEV:       return "no such device on this board";
    case EOS_ERR_IO:          return "io error";
    case EOS_ERR_POOL:        return "pool exhausted";
    case EOS_ERR_NOTFOUND:    return "not found";
    case EOS_ERR_EXISTS:      return "already exists";
    case EOS_ERR_UNSUPPORTED: return "unsupported on this tier";
    case EOS_ERR_BUSY:        return "busy";
    case EOS_ERR_TOOBIG:      return "too big";
    case EOS_ERR_READONLY:    return "read only";
    case EOS_ERR_STATE:       return "wrong state";
    }
    return "unknown";
}

// -------------------------------------------------------------------- tier
//
// The tier is a property of the BOARD, decided once in the registry, and it
// selects which display backend gets linked. It is not negotiated at runtime
// and it is not a quality setting the user can raise.
//
//   SOFT  no PSRAM, tight heap. No LVGL — an 8-bit indexed software
//         compositor, or a 1bpp one on the OLED. The CYD sits here: a 320x240
//         indexed framebuffer is 76,800 bytes and leaves roughly 20KB of heap
//         once WiFi and NimBLE are up, which LVGL would eat instantly.
//   LEAN  enough SRAM to hold LVGL 9 plus a partial draw buffer, still no
//         PSRAM. ESP32-C5 class, 40-row bands. No animation budget.
//   RICH  PSRAM present. LVGL 9, double buffered, animations enabled.

typedef enum {
    EOS_TIER_SOFT = 0,
    EOS_TIER_LEAN = 1,
    EOS_TIER_RICH = 2,
} eos_tier_t;

static inline const char *eos_tier_name(uint8_t t)
{
    switch (t) {
    case EOS_TIER_SOFT: return "soft";
    case EOS_TIER_LEAN: return "lean";
    case EOS_TIER_RICH: return "rich";
    }
    return "?";
}

// Which renderer the registry asked for. The tier implies it, but the OLED and
// the CYD are both tier SOFT with different compositors, so it is stated.
typedef enum {
    EOS_COMP_INDEXED8 = 0,   // 8-bit palette framebuffer
    EOS_COMP_MONO1,          // 1bpp framebuffer, SSD1306
    EOS_COMP_LVGL,           // LVGL 9 owns the buffers
} eos_comp_t;

static inline const char *eos_comp_name(uint8_t c)
{
    switch (c) {
    case EOS_COMP_INDEXED8: return "indexed8";
    case EOS_COMP_MONO1:    return "mono1";
    case EOS_COMP_LVGL:     return "lvgl";
    }
    return "?";
}

// ---------------------------------------------------------------- hardware

typedef int8_t eos_pin_t;
#define EOS_PIN_NONE ((eos_pin_t)-1)

static inline bool eos_pin_ok(eos_pin_t p) { return p >= 0; }

typedef enum {
    EOS_SOC_ESP32 = 0,   // classic WROOM / D0WD-V3
    EOS_SOC_ESP32_S2,
    EOS_SOC_ESP32_S3,
    EOS_SOC_ESP32_C3,
    EOS_SOC_ESP32_C5,
    EOS_SOC_ESP32_C6,
    EOS_SOC_ESP32_P4,
} eos_soc_t;

static inline const char *eos_soc_name(uint8_t s)
{
    switch (s) {
    case EOS_SOC_ESP32:    return "esp32";
    case EOS_SOC_ESP32_S2: return "esp32-s2";
    case EOS_SOC_ESP32_S3: return "esp32-s3";
    case EOS_SOC_ESP32_C3: return "esp32-c3";
    case EOS_SOC_ESP32_C5: return "esp32-c5";
    case EOS_SOC_ESP32_C6: return "esp32-c6";
    case EOS_SOC_ESP32_P4: return "esp32-p4";
    }
    return "?";
}

typedef enum {
    EOS_PANEL_NONE = 0,
    EOS_PANEL_ILI9341,   // CYD, 320x240, RGB565 straight down the wire
    EOS_PANEL_ST7789,    // C5-LCD-1.47, 320x172 IPS, RGB565
    EOS_PANEL_ILI9488,   // wavvy 4.0in and 3.5in, 320x480, NO 16-bit mode: 3 bytes/pixel
    EOS_PANEL_ST7735,
    EOS_PANEL_SSD1306,   // wavvy OLED, 128x64 I2C, 1bpp
} eos_panel_t;

static inline const char *eos_panel_name(uint8_t p)
{
    switch (p) {
    case EOS_PANEL_NONE:    return "none";
    case EOS_PANEL_ILI9341: return "ili9341";
    case EOS_PANEL_ST7789:  return "st7789";
    case EOS_PANEL_ILI9488: return "ili9488";
    case EOS_PANEL_ST7735:  return "st7735";
    case EOS_PANEL_SSD1306: return "ssd1306";
    }
    return "?";
}

typedef enum { EOS_BUS_NONE = 0, EOS_BUS_SPI, EOS_BUS_I2C, EOS_BUS_SDMMC } eos_bus_t;

typedef enum {
    EOS_TOUCH_NONE = 0,  // the CYD "N" variant: looks like a touchscreen, has no chip
    EOS_TOUCH_XPT2046,   // resistive, SPI
    EOS_TOUCH_GT911,     // capacitive, I2C
    EOS_TOUCH_CST816,    // capacitive, I2C
} eos_touch_t;

typedef enum { EOS_LED_NONE = 0, EOS_LED_GPIO_RGB, EOS_LED_WS2812 } eos_led_t;
typedef enum { EOS_AUDIO_NONE = 0, EOS_AUDIO_DAC, EOS_AUDIO_I2S } eos_audio_t;

// ----------------------------------------------------------- render config
//
// Mirrors the registry's "render" block. The display backend reads this rather
// than deciding for itself, because the decision was made once with a heap
// measurement in hand and should not be re-derived at boot.

typedef struct {
    uint8_t  compositor;        // eos_comp_t
    bool     lvgl;
    uint16_t palette_entries;   // 256 for indexed8, 0 for LVGL and mono
    bool     full_framebuffer;  // false means banded: the frame loop runs per strip
    int16_t  band_h;            // rows per band when !full_framebuffer, else 0
    bool     double_buffer;
    bool     animations;

    // The two numbers the tiling window manager needs, copied verbatim into
    // eos_wm_cfg_t.min_tile_w / min_tile_h by whoever builds the shell config.
    // They live on the BOARD and not in the theme because they are a property
    // of the panel: a split that cannot give both children this much space
    // collapses into a tab group, and that rule is the only reason tiling is
    // usable on a 2.4" screen. Both are listed "unverified" in the registry —
    // they are a legibility judgement (roughly thirteen columns and five rows
    // of the default font), not a measurement.
    int16_t  min_tile_w;
    int16_t  min_tile_h;

    // render.heap_budget_bytes: what eos_display_init() may claim at boot. Every
    // board in the registry currently lists this field under "unverified", so it
    // is a sized estimate, not a measurement, and the first thing to check when a
    // board OOMs during display init.
    uint32_t heap_budget;
} eos_board_render_t;

// ------------------------------------------------------------ panel config

typedef struct {
    uint8_t   panel;          // eos_panel_t
    uint8_t   bus;            // eos_bus_t
    int16_t   native_w;       // panel size before rotation
    int16_t   native_h;
    uint8_t   rotation;       // 0..3. Tracks how the panel is MOUNTED, not its size.
    uint8_t   color_depth;    // bits of colour the controller accepts: 1, 16 or 18
    uint8_t   wire_bytes;     // bytes per pixel on the bus: 0 (packed 1bpp), 2 or 3
    bool      bgr;            // panel is wired BGR rather than RGB
    bool      invert;         // controller inversion (most IPS panels want it)
    uint32_t  hz;             // bus clock. The 3.5in ILI9488 corrupts above 40 MHz.
    int16_t   col_offset;     // controller RAM window origin; nonzero on small ST77xx
    int16_t   row_offset;

    // SPI panels
    eos_pin_t sck, mosi, miso, dc, cs, rst;
    uint8_t   spi_host;       // 1 = SPI2/HSPI, 2 = SPI3/VSPI on classic ESP32

    // I2C panels
    eos_pin_t sda, scl;
    uint8_t   i2c_addr;

    // backlight
    eos_pin_t bl;             // EOS_PIN_NONE on the ILI9488 boards and the OLED
    bool      bl_active_low;
    bool      bl_pwm;         // false: on/off only, eos_display_backlight() snaps
} eos_panel_cfg_t;

// ------------------------------------------------------------ input config

#define EOS_MAX_BUTTONS 6

typedef struct {
    eos_pin_t pin;
    bool      active_low;
    bool      pull_up;        // internal pull the driver must enable
    // The EOS_KEY_* usage this button reports. The registry carries no keymap,
    // so the generator emits 0 here and the board component fills it in; it is
    // the one field in the whole descriptor that does not come from the JSON.
    uint8_t   key;
} eos_button_t;

typedef struct {
    eos_button_t buttons[EOS_MAX_BUTTONS];
    uint8_t      button_count;
    bool         ble_keyboard;   // build the NimBLE HID host. Never Bluedroid: 83KB = OOM.
    bool         web_input;      // the phone page may inject events
    uint8_t      touch;          // eos_touch_t
    uint8_t      touch_bus;      // eos_bus_t
    eos_pin_t    touch_sck, touch_mosi, touch_miso, touch_cs, touch_irq;
    eos_pin_t    touch_sda, touch_scl;
    uint8_t      touch_addr;
} eos_board_input_t;

// ---------------------------------------------------------- storage config

typedef struct {
    bool        sd;              // microSD slot exists
    uint8_t     sd_bus;          // eos_bus_t: SPI or SDMMC
    uint8_t     sd_spi_host;     // 1 = SPI2/HSPI, 2 = SPI3/VSPI
    eos_pin_t   sd_sck, sd_mosi, sd_miso, sd_cs;
    // SDMMC slot index when sd_bus is EOS_BUS_SDMMC. No board in the registry
    // uses SDMMC and the JSON has no field for it, so the generator emits 0.
    uint8_t     sd_slot;
    uint32_t    sd_hz;           // 20 MHz is the safe ceiling on these cheap slots
    bool        sd_shares_bus;   // true means take the panel's bus lock around every access
    const char *sd_point;        // mount point, conventionally "/sd"
    const char *int_label;       // LittleFS partition label in the partition table
    const char *int_point;       // mount point, conventionally "/int"
} eos_board_storage_t;

// ----------------------------------------------------------------- extras

typedef struct {
    uint8_t   led;               // eos_led_t
    eos_pin_t led_r, led_g, led_b;   // EOS_LED_GPIO_RGB
    bool      led_active_low;    // true on the CYD: the LED sinks through the pin
    eos_pin_t led_data;          // EOS_LED_WS2812
    uint8_t   led_count;

    uint8_t   audio;             // eos_audio_t
    eos_pin_t audio_pin;         // DAC pin, GPIO25/26 on classic ESP32

    eos_pin_t ldr;               // ambient light on an ADC pin
    uint8_t   ldr_adc_unit;      // 1 or 2
    int8_t    ldr_adc_channel;
} eos_board_extras_t;

// ------------------------------------------------------------------ board

typedef struct {
    const char *id;              // registry key, e.g. "cyd-2432s024n"
    const char *name;            // human label shown in settings
    const char *variant;         // silicon revision string, e.g. "ESP32-D0WD-V3"
    uint8_t     soc;             // eos_soc_t
    uint8_t     cores;
    uint8_t     tier;            // eos_tier_t
    uint32_t    flash_bytes;
    uint32_t    psram_bytes;     // 0 on SOFT and LEAN

    eos_board_render_t  render;
    eos_panel_cfg_t     panel;
    eos_board_input_t   input;
    eos_board_storage_t storage;
    eos_board_extras_t  extras;

    // Identity. auto_detectable is false on every board in the registry and is
    // kept as a field only so a future self-identifying board does not need a
    // struct change. confirm_prompt is the question a first boot asks the
    // human, because that confirmation IS the identification.
    bool        auto_detectable;
    const char *confirm_prompt;

    const char *port;            // host serial device, for the flash tooling only
    uint32_t    upload_baud;     // 230400 on the wavvy CP2102 cable; 921600 fails there
    uint32_t    monitor_baud;
} eos_board_t;

// Each generated board header defines one const eos_board_t in flash, named
// eos_board_<id_with_underscores>, and points the macro EOS_BOARD at it. The
// board component returns &EOS_BOARD. The name carries the board id so that a
// test can hold all six registry entries in one translation unit; a firmware
// image includes exactly one header and never sees the difference.
// Never NULL in a linked image.
const eos_board_t *eos_board_get(void);

// ------------------------------------------------------- derived geometry

// Rotation 1 and 3 swap the axes. This is the size the window manager lays
// out into and the size the display backend reports.
static inline int16_t eos_board_screen_w(const eos_board_t *b)
{
    return (b->panel.rotation & 1) ? b->panel.native_h : b->panel.native_w;
}

static inline int16_t eos_board_screen_h(const eos_board_t *b)
{
    return (b->panel.rotation & 1) ? b->panel.native_w : b->panel.native_h;
}

static inline eos_rect_t eos_board_screen(const eos_board_t *b)
{
    eos_rect_t r;
    r.x = 0;
    r.y = 0;
    r.w = eos_board_screen_w(b);
    r.h = eos_board_screen_h(b);
    return r;
}

static inline bool eos_board_has_lvgl(const eos_board_t *b) { return b->render.lvgl; }
static inline bool eos_board_has_touch(const eos_board_t *b) { return b->input.touch != EOS_TOUCH_NONE; }
static inline bool eos_board_is_mono(const eos_board_t *b) { return b->panel.color_depth == 1; }

// The registry's display.supports_16bit_pixels, which is not a second fact: the
// generator refuses a profile where it disagrees with color_depth. It is here
// because it is the question a driver actually asks — the ILI9488 over SPI has
// no 16-bit pixel mode at all, so RGB565 has to be expanded to three bytes on
// the way out, and that is the whole reason wire_bytes exists.
static inline bool eos_board_panel_16bit(const eos_board_t *b) { return b->panel.color_depth == 16; }

// Bytes a full-screen compositor framebuffer would cost. This is the
// arithmetic behind render.full_framebuffer, kept here so the number can be
// printed in a diagnostic and checked against the registry's decision: the CYD
// is 76,800 and fits, a 320x480 ILI9488 is 153,600 and does not fit next to
// WiFi, which is why those two boards share a tier and not a frame loop.
static inline uint32_t eos_board_fb_bytes(const eos_board_t *b)
{
    uint32_t w = (uint32_t)eos_board_screen_w(b);
    uint32_t h = (uint32_t)eos_board_screen_h(b);
    if (b->render.compositor == EOS_COMP_MONO1) return ((w + 7u) / 8u) * h;
    return w * h;   // one palette index per pixel
}

// Bytes one band costs, which is what a banded backend actually allocates.
static inline uint32_t eos_board_band_bytes(const eos_board_t *b)
{
    if (b->render.full_framebuffer || b->render.band_h <= 0) return eos_board_fb_bytes(b);
    uint32_t w = (uint32_t)eos_board_screen_w(b);
    uint32_t rows = (uint32_t)b->render.band_h;
    if (b->render.compositor == EOS_COMP_MONO1) return ((w + 7u) / 8u) * rows;
    return w * rows;
}

// ------------------------------------------------------------------ probe
//
// Everything the silicon will actually admit to. Nothing here identifies a
// panel, a touch chip, or an SD card slot.

typedef struct {
    uint8_t  soc;                // eos_soc_t
    uint32_t flash_bytes;
    uint32_t psram_bytes;        // 0 when no PSRAM answered
    uint8_t  mac[6];
} eos_probe_t;

eos_err_t eos_board_probe(eos_probe_t *out);

#define EOS_MISMATCH_SOC   0x01
#define EOS_MISMATCH_FLASH 0x02
#define EOS_MISMATCH_PSRAM 0x04

// Returns a bitmask of the checks that failed, 0 when the running silicon
// agrees with the registry entry. A nonzero result means the wrong board
// header was flashed, and the correct response is to stop and say so — not to
// guess a different board.
static inline uint8_t eos_board_check(const eos_board_t *b, const eos_probe_t *p)
{
    uint8_t bad = 0;
    if (b->soc != p->soc)                                  bad |= EOS_MISMATCH_SOC;
    if (b->flash_bytes != p->flash_bytes)                  bad |= EOS_MISMATCH_FLASH;
    if ((b->psram_bytes != 0) != (p->psram_bytes != 0))    bad |= EOS_MISMATCH_PSRAM;
    return bad;
}

#endif // EOS_BOARD_H
