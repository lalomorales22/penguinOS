// Touch-controller probe for the 4.0in CYD (ESP32-4832S040).
//
// The vendor firmware dump proved the touch controller is SPI, not I2C - the
// build retains 701 ESP_LOG strings including five from spi_master.c, and not
// one from the I2C driver. What the dump would NOT give up is the pins: a
// stripped Arduino build scatters them as immediates through inlined code with
// nothing to anchor them to. Two attempts to recover them statically produced
// false positives, so this asks the chip instead.
//
// The test is the XPT2046's TEMP0 channel (control byte 0x87): a single-ended
// read against the internal reference that returns a stable mid-scale value
// whether or not anyone is touching the panel. That matters because every
// touch-dependent register reads as zero on an idle screen, which is exactly
// what a disconnected pin also reads. A floating MISO gives 0x000 or 0xFFF;
// a real part gives neither.
//
// It never drives the display pins as a group, so the panel keeps whatever the
// last firmware left on it.

#include <Arduino.h>

// Display pins, already proven on this board - the touch controller usually
// shares this bus and only takes a CS of its own.
static const int LCD_SCK = 14, LCD_MOSI = 13, LCD_MISO = 12, LCD_CS = 15;

struct PinSet { const char *name; int clk, mosi, miso, cs, irq; };

static const PinSet SETS[] = {
    // The classic 2.4in CYD wiring: touch on its own bit-banged bus.
    { "separate bus (2.4in CYD classic)", 25, 32, 39, 33, 36 },
    // Same bus as the panel, separate CS - the common 4in arrangement.
    { "shared bus, CS 33", LCD_SCK, LCD_MOSI, LCD_MISO, 33, 36 },
    { "shared bus, CS 21", LCD_SCK, LCD_MOSI, LCD_MISO, 21, 22 },
    { "shared bus, CS 5",  LCD_SCK, LCD_MOSI, LCD_MISO,  5, 34 },
};

static bool is_input_only(int p) { return p >= 34 && p <= 39; }

// Bit-bang one 8-bit command then 16 bits back. Mode 0, MSB first.
static uint16_t xpt_xfer(const PinSet &s, uint8_t cmd)
{
    digitalWrite(s.cs, LOW);
    for (int i = 7; i >= 0; i--) {
        digitalWrite(s.mosi, (cmd >> i) & 1);
        digitalWrite(s.clk, HIGH);
        delayMicroseconds(2);
        digitalWrite(s.clk, LOW);
        delayMicroseconds(2);
    }
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) {
        digitalWrite(s.clk, HIGH);
        delayMicroseconds(2);
        v = (uint16_t)((v << 1) | (digitalRead(s.miso) & 1));
        digitalWrite(s.clk, LOW);
        delayMicroseconds(2);
    }
    digitalWrite(s.cs, HIGH);
    return (uint16_t)(v >> 3);   // 16 clocks, 12 valid bits, 3 trailing
}

static void claim(const PinSet &s)
{
    pinMode(s.clk, OUTPUT);
    pinMode(s.mosi, OUTPUT);
    pinMode(s.miso, INPUT);
    pinMode(s.cs, OUTPUT);
    digitalWrite(s.cs, HIGH);
    digitalWrite(s.clk, LOW);
    if (s.irq >= 0) pinMode(s.irq, is_input_only(s.irq) ? INPUT : INPUT_PULLUP);
}

// Every input-capable pin that could plausibly carry PENIRQ, minus the ones
// this board already spoken for and minus the flash pins. The IRQ is whichever
// one sits high at rest and goes low only under a press - so the probe records
// all of them and lets the change identify the pin, rather than assuming 36
// because the 2.4in board uses it.
static const int CAND[] = { 4, 5, 16, 17, 18, 19, 21, 22, 23, 25, 26, 32, 34, 35, 36, 39 };

static int g_found = -1;

void setup()
{
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println("=== XPT2046 pin probe ===");

    // Park the panel's own CS high so bit-banging its bus cannot address it.
    pinMode(LCD_CS, OUTPUT);
    digitalWrite(LCD_CS, HIGH);

    for (unsigned i = 0; i < sizeof SETS / sizeof *SETS; i++) {
        const PinSet &s = SETS[i];
        if (is_input_only(s.clk) || is_input_only(s.mosi) || is_input_only(s.cs)) {
            Serial.printf("  [%u] %-34s SKIPPED (output on input-only pin)\n", i, s.name);
            continue;
        }
        claim(s);
        // Two reads: the first wakes the part out of power-down.
        xpt_xfer(s, 0x87);
        delay(2);
        uint16_t t = xpt_xfer(s, 0x87);
        bool plausible = (t > 64 && t < 4032);
        Serial.printf("  [%u] %-34s clk=%2d mosi=%2d miso=%2d cs=%2d  TEMP0=0x%03X %s\n",
                      i, s.name, s.clk, s.mosi, s.miso, s.cs, t,
                      plausible ? "<-- ANSWERS" : "(no reply)");
        if (plausible && g_found < 0) g_found = (int)i;
    }

    if (g_found < 0) {
        Serial.println("\nNo candidate answered. The touch bus is on pins not in the list.");
        Serial.println("Report this and I will widen the scan to a full CS sweep.");
    } else {
        for (unsigned i = 0; i < sizeof CAND / sizeof *CAND; i++)
            pinMode(CAND[i], is_input_only(CAND[i]) ? INPUT : INPUT_PULLUP);
        Serial.print("PINS column order: ");
        for (unsigned i = 0; i < sizeof CAND / sizeof *CAND; i++)
            Serial.printf("%d ", CAND[i]);
        Serial.println();
        Serial.printf("\nLive readings from set [%d]. Touch the screen.\n", g_found);
        Serial.println("Z1 rises under a press; X/Y should track where you touch.\n");
    }
}

void loop()
{
    if (g_found < 0) { delay(1000); return; }
    const PinSet &s = SETS[g_found];
    // PD0 MUST be 0. The XPT2046 disables PENIRQ whenever PD0 is 1, so the
    // 0xB1/0xD1/0x91 used in the first pass held the pin high by construction
    // and proved nothing about which GPIO the interrupt lands on.
    uint16_t z1 = xpt_xfer(s, 0xB0);
    uint16_t x  = xpt_xfer(s, 0xD0);
    uint16_t y  = xpt_xfer(s, 0x90);
    delayMicroseconds(200);   // let the part power down and re-assert PENIRQ

    char bits[sizeof CAND / sizeof *CAND + 1];
    for (unsigned i = 0; i < sizeof CAND / sizeof *CAND; i++)
        bits[i] = digitalRead(CAND[i]) ? '1' : '0';
    bits[sizeof CAND / sizeof *CAND] = 0;

    Serial.printf("Z1=%4u  X=%4u  Y=%4u  PINS=%s  %s\n",
                  z1, x, y, bits, z1 > 100 ? "TOUCH" : "");
    delay(120);
}
