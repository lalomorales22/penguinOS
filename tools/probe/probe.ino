// probe.ino - decide which board this is by looking at the screen.
//
// esptool gets you the chip, the flash size and the MAC. It cannot tell you
// what is on the other end of the SPI bus, and boards/README.md records why
// nothing here should pretend otherwise: the ILI9488 answers register 0xD3
// with 00 7F DF, which matches no known part, and the Waveshare C5 does not
// wire MISO to the panel at all. So after esptool has narrowed the registry to
// two candidates, a human has to look. This sketch is what they look at.
//
// It walks every panel configuration the chip it was built for could be wired
// to, drawing a labelled test card under each. A mismatched driver is never
// subtle - the ILI9488 takes 18-bit pixels and the ST7789 takes 16-bit, so the
// wrong one tears visibly - and the pass that renders a clean card names the
// board.
//
// The one non-obvious constraint: the two wavvy boards are not distinguished by
// their driver, which is the same ILI9488 on both. They are distinguished by
// whether the panel survives an 80MHz pixel clock. So those two passes differ
// only in the clock, and the thing that separates them is the stripe field at
// the bottom of the card: at a clock the panel cannot hold, whole bytes are
// lost mid-block and the stripes shift phase in visible rectangles. A clean
// card with a clean stripe field means that clock is safe on this board.
//
// Everything is constructed at runtime rather than by #define, because the
// point is to compare configurations without reflashing between them.
//
// Build:
//   arduino-cli compile --fqbn esp32:esp32:esp32   tools/probe
//   arduino-cli compile --fqbn esp32:esp32:esp32c5 tools/probe
// Needs the "GFX Library for Arduino" library (Arduino_GFX by moononournation).
//
// tools/flash.sh --probe does the build, the upload and the question.

#include <Arduino_GFX_Library.h>
#include <Wire.h>

// How long each pass holds before moving on. Long enough to read the label and
// look at the stripes, short enough that the whole cycle is not a chore.
static const uint32_t kHoldMs = 10000;

// ------------------------------------------------------------ configurations

enum eos_ctrl {
    EOS_CTRL_ILI9341,
    EOS_CTRL_ILI9488,
    EOS_CTRL_ST7789,
    EOS_CTRL_SSD1306_I2C,
};

typedef struct {
    const char *profile;   // the boards/<id>.json this pass is testing
    const char *label;     // what goes on the screen, big
    enum eos_ctrl ctrl;
    int8_t sck, mosi, miso, dc, cs, rst;
    uint8_t spi_host;
    int32_t clock_hz;
    uint8_t rotation;
    int16_t native_w, native_h;
    uint8_t col_off1, row_off1, col_off2, row_off2;
    bool ips;
    int8_t bl_pin;         // backlight, -1 when the panel has no software control
    bool bl_active_low;
    int8_t sda, scl;       // i2c only
    uint8_t i2c_addr;
    const char *note;      // the one thing that separates this from its twin
} eos_probe_cfg_t;

#if CONFIG_IDF_TARGET_ESP32

// Three esp32 boards in the registry. The CYD is separable from the wavvy pair
// by esptool alone (different chip package, different USB bridge) but it is
// included so the prober is useful on its own, without detect.py.
static const eos_probe_cfg_t kConfigs[] = {
    {
        "cyd-2432s024n", "ILI9341", EOS_CTRL_ILI9341,
        14, 13, 12, 2, 15, -1,          // SCK MOSI MISO DC CS RST
        HSPI, 40000000, 1,              // rotation 1 = 320x240 landscape
        240, 320, 0, 0, 0, 0, false,
        27, false,                      // backlight GPIO27, active high
        -1, -1, 0x00,
        "2.4in Cheap Yellow Display. Backlight is GPIO27; without it the panel is black."
    },
    {
        "wavvy-ili9488-40", "ILI9488 80MHz", EOS_CTRL_ILI9488,
        18, 23, -1, 2, 5, 4,
        VSPI, 80000000, 0,              // rotation 0 = 320x480 portrait
        320, 480, 0, 0, 0, 0, false,
        -1, false,
        -1, -1, 0x00,
        "The 4.0in board. Clean here means this panel holds 80MHz."
    },
    {
        "wavvy-ili9488-35", "ILI9488 40MHz", EOS_CTRL_ILI9488,
        18, 23, -1, 2, 5, 4,
        VSPI, 40000000, 0,
        320, 480, 0, 0, 0, 0, false,
        -1, false,
        -1, -1, 0x00,
        "The 3.5in board. If the 80MHz pass was corrupt and this one is clean, it is this."
    },
};

#elif CONFIG_IDF_TARGET_ESP32C5

// Two C5 boards. esptool reports them identically - same chip, same 4MB, both
// native USB - so this is the only thing that tells them apart short of looking
// at the hardware.
static const eos_probe_cfg_t kConfigs[] = {
    {
        "waveshare-c5-lcd-147", "ST7789", EOS_CTRL_ST7789,
        7, 6, -1, 24, 23, 26,
        FSPI, 40000000, 1,              // rotation 1 = 320x172 landscape
        172, 320,
        // A 172-wide panel sits offset inside the ST7789's 240-wide RAM. At
        // rotation 1 Arduino_TFT takes _xStart from row_offset1 and _yStart
        // from col_offset2, so the 34 goes in the col slots.
        34, 0, 34, 0,
        false,                          // the registry says invert:false; if this
                                        // pass renders as a photographic negative
                                        // it is still a match, the BSP owns
                                        // inversion on the real firmware
        10, false,                      // backlight GPIO10
        -1, -1, 0x00,
        "Long thin colour strip. The BSP owns panel init on the real image; this is the probe's own."
    },
    {
        "wavvy-oled-c5", "SSD1306", EOS_CTRL_SSD1306_I2C,
        -1, -1, -1, -1, -1, -1,
        0, 400000, 0,
        128, 64, 0, 0, 0, 0, false,
        -1, false,
        23, 24, 0x3C,                   // SDA GPIO23, SCL GPIO24
        "The only I2C panel in the fleet. If 0x3C acknowledges, it is this board."
    },
};

#else
#error "probe.ino has no configurations for this chip. Add them, or build for esp32 / esp32c5."
#endif

static const int kConfigCount = (int)(sizeof(kConfigs) / sizeof(kConfigs[0]));

// ------------------------------------------------------------------ helpers

// Defined below, next to the thing it waits for. Declared here because the
// passes call it and .ino prototype generation is not something to rely on.
static void holdOrSkip();

static void backlightOn(const eos_probe_cfg_t *c)
{
    if (c->bl_pin < 0) return;
    pinMode(c->bl_pin, OUTPUT);
    digitalWrite(c->bl_pin, c->bl_active_low ? LOW : HIGH);
}

static void backlightOff(const eos_probe_cfg_t *c)
{
    if (c->bl_pin < 0) return;
    digitalWrite(c->bl_pin, c->bl_active_low ? HIGH : LOW);
}

// An I2C address scan is not identification on its own - every SSD1306 and a
// good number of SH1106 modules answer at 0x3C - but between the two C5 boards
// in the registry it is decisive, because the other one has no I2C panel at all.
static bool i2cAnswers(int8_t sda, int8_t scl, uint32_t hz, uint8_t addr)
{
    Wire.begin(sda, scl);
    Wire.setClock(hz);
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// ---------------------------------------------------------------- test card

// The stripe field is the sensitive part. One-pixel vertical stripes are the
// densest pattern the bus can carry, so a dropped byte shifts every following
// pixel in that block and the eye sees a rectangle of inverted phase. Solid
// fills hide that; gradients hide it; stripes do not.
static void drawStripes(Arduino_GFX *gfx, int16_t y, int16_t h, int16_t w,
                        uint16_t a, uint16_t b)
{
    for (int16_t x = 0; x < w; x++) {
        gfx->drawFastVLine(x, y, h, (x & 1) ? a : b);
    }
}

static void drawColourCard(Arduino_GFX *gfx, const eos_probe_cfg_t *c, int pass)
{
    const int16_t w = gfx->width();
    const int16_t h = gfx->height();

    gfx->fillScreen(RGB565_BLACK);

    // The label. If this is not readable the driver is wrong; stop here.
    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(w >= 300 ? 3 : 2);
    gfx->setCursor(8, 10);
    gfx->println(c->label);

    gfx->setTextSize(1);
    gfx->setTextColor(RGB565(170, 170, 180));
    gfx->setCursor(8, w >= 300 ? 42 : 34);
    gfx->printf("pass %d/%d  %dx%d  %ld MHz", pass, kConfigCount, w, h,
                (long)(c->clock_hz / 1000000));
    gfx->setCursor(8, (w >= 300 ? 42 : 34) + 12);
    gfx->print(c->profile);

    int16_t y = (w >= 300 ? 42 : 34) + 30;

    // Primary bars. A wrong pixel format gives wrong or smeared colours.
    const uint16_t bars[] = {RGB565_RED, RGB565_GREEN, RGB565_BLUE, RGB565_WHITE};
    const int16_t bw = w / 4;
    const int16_t bh = h / 6;
    for (int i = 0; i < 4; i++) {
        gfx->fillRect(i * bw, y, bw, bh, bars[i]);
    }
    y += bh + 4;

    // A grey ramp exposes byte-order and bit-depth mistakes as banding.
    for (int16_t x = 0; x < w; x++) {
        uint8_t v = (uint8_t)((x * 255) / (w - 1));
        gfx->drawFastVLine(x, y, bh / 2, RGB565(v, v, v));
    }
    y += bh / 2 + 4;

    // The stripe field, repeated. Corruption from an over-clocked bus is often
    // intermittent, so one clean frame is not proof; several are.
    const int16_t stripe_h = h - y - 4;
    if (stripe_h > 8) {
        for (int rep = 0; rep < 3; rep++) {
            drawStripes(gfx, y, stripe_h, w, RGB565_WHITE, RGB565_BLACK);
            drawStripes(gfx, y, stripe_h, w, RGB565_BLACK, RGB565_WHITE);
        }
        drawStripes(gfx, y, stripe_h, w, RGB565_WHITE, RGB565_BLACK);
    }

    // A one-pixel border confirms the full addressable area is right. A wrong
    // width or a missing RAM offset clips or wraps it.
    gfx->drawRect(0, 0, w, h, RGB565(255, 140, 0));
}

static void drawMonoCard(Arduino_GFX *gfx, const eos_probe_cfg_t *c, int pass)
{
    const int16_t w = gfx->width();
    const int16_t h = gfx->height();

    gfx->fillScreen(RGB565_BLACK);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(2, 2);
    gfx->println(c->label);

    gfx->setTextSize(1);
    gfx->setCursor(2, 20);
    gfx->printf("pass %d/%d %dx%d", pass, kConfigCount, w, h);
    gfx->setCursor(2, 30);
    gfx->print(c->profile);

    // A checkerboard on a mono panel is the equivalent of the stripe field: an
    // SH1106 driven as an SSD1306 renders it shifted two columns, which is
    // obvious against the border.
    for (int16_t y = 40; y < h - 2; y += 2) {
        for (int16_t x = 2; x < w - 2; x += 2) {
            if (((x >> 1) + (y >> 1)) & 1) gfx->drawPixel(x, y, RGB565_WHITE);
        }
    }
    gfx->drawRect(0, 0, w, h, RGB565_WHITE);
}

// ------------------------------------------------------------------- passes

static void announce(const eos_probe_cfg_t *c, int pass)
{
    Serial.println();
    Serial.printf("[probe] pass %d/%d  %s\n", pass, kConfigCount, c->profile);
    Serial.printf("[probe]   driver %s", c->label);
    if (c->ctrl == EOS_CTRL_SSD1306_I2C) {
        Serial.printf("  i2c sda=%d scl=%d addr=0x%02X %ld kHz\n",
                      c->sda, c->scl, c->i2c_addr, (long)(c->clock_hz / 1000));
    } else {
        Serial.printf("  spi sck=%d mosi=%d dc=%d cs=%d rst=%d  %ld MHz\n",
                      c->sck, c->mosi, c->dc, c->cs, c->rst,
                      (long)(c->clock_hz / 1000000));
    }
    Serial.printf("[probe]   %s\n", c->note);
}

static bool runSpiPass(const eos_probe_cfg_t *c, int pass)
{
    Arduino_DataBus *bus = new Arduino_ESP32SPI(
        c->dc, c->cs, c->sck, c->mosi, c->miso, c->spi_host, false /* not shared */);

    Arduino_GFX *gfx = NULL;
    switch (c->ctrl) {
        case EOS_CTRL_ILI9341:
            gfx = new Arduino_ILI9341(bus, c->rst, c->rotation, c->ips);
            break;
        case EOS_CTRL_ILI9488:
            // 18-bit only. The part has no 16-bit SPI pixel mode, which is the
            // whole reason it is not an ST7796.
            gfx = new Arduino_ILI9488_18bit(bus, c->rst, c->rotation, c->ips);
            break;
        case EOS_CTRL_ST7789:
            gfx = new Arduino_ST7789(bus, c->rst, c->rotation, c->ips,
                                     c->native_w, c->native_h,
                                     c->col_off1, c->row_off1,
                                     c->col_off2, c->row_off2);
            break;
        default:
            delete bus;
            return false;
    }

    backlightOn(c);

    bool ok = gfx->begin(c->clock_hz);
    if (!ok) {
        Serial.printf("[probe]   begin() FAILED - this is not the panel, or the "
                      "wiring is wrong\n");
    } else {
        drawColourCard(gfx, c, pass);
        Serial.printf("[probe]   drew %dx%d. Readable label, four clean bars, a "
                      "smooth ramp and an unbroken stripe field = this is it.\n",
                      gfx->width(), gfx->height());
    }

    holdOrSkip();

    backlightOff(c);
    delete gfx;
    delete bus;
    return ok;
}

static bool runI2cPass(const eos_probe_cfg_t *c, int pass)
{
    if (!i2cAnswers(c->sda, c->scl, c->clock_hz, c->i2c_addr)) {
        Serial.printf("[probe]   nothing acknowledges at 0x%02X on sda=%d scl=%d. "
                      "This is not that board.\n", c->i2c_addr, c->sda, c->scl);
        holdOrSkip();
        return false;
    }
    Serial.printf("[probe]   0x%02X acknowledged.\n", c->i2c_addr);

    // The SSD1306 is an Arduino_G output sink, not a drawing surface: its
    // memory is 8 vertical pixels per byte and it has no primitives. A mono
    // canvas gives it the GFX calls and pushes the whole 1024-byte page on
    // flush, which is exactly what the tier 0 mono1 compositor does too.
    Arduino_DataBus *bus = new Arduino_Wire(c->i2c_addr, 0x00, 0x40, &Wire);
    Arduino_G *panel = new Arduino_SSD1306(bus, c->rst, c->native_w, c->native_h);
    Arduino_GFX *gfx = new Arduino_Canvas_Mono(c->native_w, c->native_h, panel,
                                               0, 0, true /* verticalByte */);

    bool ok = gfx->begin(c->clock_hz);
    if (!ok) {
        Serial.println("[probe]   begin() FAILED after the address answered - "
                       "likely an SH1106 rather than an SSD1306.");
    } else {
        drawMonoCard(gfx, c, pass);
        gfx->flush();
        Serial.println("[probe]   drew the mono card. A checkerboard shifted two "
                       "columns against the border means SH1106, not SSD1306.");
    }

    holdOrSkip();

    delete gfx;
    delete panel;
    delete bus;
    return ok;
}

// Holds this pass on screen. Typing anything on serial moves on early; typing a
// digit selects that pass and prints the machine-readable line flash.sh and any
// other caller can look for.
static void holdOrSkip()
{
    uint32_t until = millis() + kHoldMs;
    while ((int32_t)(millis() - until) < 0) {
        if (Serial.available()) {
            int ch = Serial.read();
            while (Serial.available()) Serial.read();
            if (ch >= '1' && ch <= '9') {
                int want = ch - '1';
                if (want < kConfigCount) {
                    Serial.println();
                    Serial.printf("[probe] PROBE-SELECT %s\n", kConfigs[want].profile);
                    Serial.printf("[probe] pass %d selected. Tell tools/flash.sh, or "
                                  "run: tools/flash.sh --profile %s\n",
                                  want + 1, kConfigs[want].profile);
                }
            }
            return;
        }
        delay(20);
    }
}

// ---------------------------------------------------------------------- main

void setup()
{
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println("[probe] penguinos panel prober");
    Serial.println("[probe] --------------------------------------------------");
    Serial.printf("[probe] %d configuration(s) for this chip. Each holds ~%lus.\n",
                  kConfigCount, (unsigned long)(kHoldMs / 1000));
    Serial.println("[probe] Watch the SCREEN, not this log. The pass that draws a");
    Serial.println("[probe] readable label, four clean colour bars, a smooth grey");
    Serial.println("[probe] ramp and an unbroken stripe field is your board.");
    Serial.println("[probe]");
    Serial.println("[probe] Press a digit to jump ahead and record that choice.");
    Serial.println("[probe] Any other key skips to the next pass.");
    Serial.println("[probe] Nothing here writes to the board. It loops forever.");
    for (int i = 0; i < kConfigCount; i++) {
        Serial.printf("[probe]   %d) %-22s %s\n", i + 1,
                      kConfigs[i].profile, kConfigs[i].label);
    }
}

void loop()
{
    for (int i = 0; i < kConfigCount; i++) {
        const eos_probe_cfg_t *c = &kConfigs[i];
        announce(c, i + 1);
        if (c->ctrl == EOS_CTRL_SSD1306_I2C) {
            runI2cPass(c, i + 1);
        } else {
            runSpiPass(c, i + 1);
        }
    }
    Serial.println();
    Serial.println("[probe] cycle complete, starting over.");
}
