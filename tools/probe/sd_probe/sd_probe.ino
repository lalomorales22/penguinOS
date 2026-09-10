// microSD probe for the 4.0in CYD (ESP32-4832S040).
//
// The board profile has the slot as present:false with the note that the SPI
// header silkscreen reads 18/23/19 but the card's chip select is unconfirmed.
// This settles it by asking the card, which is the one peripheral on this board
// that can answer for itself.
//
// A card in SPI mode replies to CMD0 (GO_IDLE_STATE) with R1 = 0x01 and to CMD8
// with a five-byte R7 echoing the check pattern 0x01AA. Neither value can be
// produced by an absent card: an unselected bus reads 0xFF because MISO idles
// pulled up, and a shorted or wrongly-driven one reads 0x00. So unlike the
// touch controller - where an idle panel and a missing chip both read zero -
// there is no ambiguous case here and no human has to watch anything.
//
// Both plausible buses are tried, because "the silkscreen says 18/23/19" is
// exactly the kind of claim that turned out to be wrong for the touch pinout.

#include <Arduino.h>

struct Bus { const char *name; int sck, mosi, miso; };

static const Bus BUSES[] = {
    { "SPI header (silkscreen 18/23/19)", 18, 23, 19 },
    { "display bus, shared",              14, 13, 12 },
};

// Every output-capable pin not already spoken for. 15 is the panel CS, 33 the
// touch CS, 2 the panel DC, 27 the backlight, 6-11 the flash; 34-39 are
// input-only and cannot drive a chip select at all.
static const int CS_CAND[] = { 5, 4, 21, 22, 16, 17, 26, 25, 32 };

static int g_sck, g_mosi, g_miso;

static uint8_t xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        digitalWrite(g_mosi, (out >> i) & 1);
        digitalWrite(g_sck, HIGH);
        delayMicroseconds(2);
        in = (uint8_t)((in << 1) | (digitalRead(g_miso) & 1));
        digitalWrite(g_sck, LOW);
        delayMicroseconds(2);
    }
    return in;
}

// Send a command and return R1. 0xFF means the card never answered.
static uint8_t cmd(int cs, uint8_t index, uint32_t arg, uint8_t crc)
{
    xfer(0xFF);
    digitalWrite(cs, LOW);
    xfer((uint8_t)(0x40 | index));
    xfer((uint8_t)(arg >> 24));
    xfer((uint8_t)(arg >> 16));
    xfer((uint8_t)(arg >> 8));
    xfer((uint8_t)arg);
    xfer(crc);
    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && r == 0xFF; i++) r = xfer(0xFF);
    return r;
}

static void release(int cs) { digitalWrite(cs, HIGH); xfer(0xFF); }

void setup()
{
    Serial.begin(115200);
    delay(400);
    Serial.println("\n=== microSD probe ===");

    // Park the two chip selects that are already known, so nothing else on the
    // shared bus answers while the candidates are being tried.
    pinMode(15, OUTPUT); digitalWrite(15, HIGH);   // panel
    pinMode(33, OUTPUT); digitalWrite(33, HIGH);   // touch

    bool any = false;
    for (unsigned b = 0; b < sizeof BUSES / sizeof *BUSES; b++) {
        const Bus &bus = BUSES[b];
        g_sck = bus.sck; g_mosi = bus.mosi; g_miso = bus.miso;
        pinMode(g_sck, OUTPUT);
        pinMode(g_mosi, OUTPUT);
        pinMode(g_miso, INPUT_PULLUP);
        digitalWrite(g_sck, LOW);
        Serial.printf("\n%s  (sck=%d mosi=%d miso=%d)\n",
                      bus.name, bus.sck, bus.mosi, bus.miso);

        for (unsigned c = 0; c < sizeof CS_CAND / sizeof *CS_CAND; c++) {
            int cs = CS_CAND[c];
            if (cs == bus.sck || cs == bus.mosi || cs == bus.miso) continue;
            pinMode(cs, OUTPUT);
            digitalWrite(cs, HIGH);

            // At least 74 clocks with CS high to put the card into SPI mode.
            for (int i = 0; i < 12; i++) xfer(0xFF);

            uint8_t r1 = cmd(cs, 0, 0, 0x95);          // CMD0 GO_IDLE_STATE
            if (r1 != 0x01) { release(cs); 
                Serial.printf("   CS %-2d  CMD0 -> 0x%02X\n", cs, r1);
                continue; }

            uint8_t r7 = cmd(cs, 8, 0x000001AA, 0x87); // CMD8 SEND_IF_COND
            uint8_t e[4] = {0,0,0,0};
            if (r7 <= 1) for (int i = 0; i < 4; i++) e[i] = xfer(0xFF);
            release(cs);

            Serial.printf("   CS %-2d  CMD0 -> 0x01  CMD8 -> 0x%02X echo %02X%02X%02X%02X"
                          "   <== CARD ANSWERS\n", cs, r7, e[0], e[1], e[2], e[3]);
            any = true;
        }
    }
    if (!any)
        Serial.println("\nNo card answered on any bus/CS pair.\n"
                       "Either no card is inserted, or the slot is wired "
                       "somewhere not in the candidate list.");
}

void loop() { delay(1000); }
