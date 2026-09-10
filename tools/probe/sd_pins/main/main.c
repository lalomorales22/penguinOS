// Finds the microSD pins on a board whose profile does not know them.
//
// WHY THIS EXISTS. boards/waveshare-c6-lcd-13.json carries the slot as
// present:false with the note that "CS is a guess from GPIO4 having been an
// output sitting high during the JTAG scan... MISO is worse: it is an input, so
// it never appears in GPIO_ENABLE and the scan is blind to it." An output-only
// scan cannot see an input, and no amount of re-reading it will help. So this
// asks the CARD instead, which is the one part of the arrangement that can
// answer for itself.
//
// WHY THE ANSWER IS NOT AMBIGUOUS. A card in SPI mode replies to CMD0
// (GO_IDLE_STATE) with R1 = 0x01 and to CMD8 with a five-byte R7 echoing the
// check pattern 0x01AA. Neither can be produced by an absent card: an
// unselected bus reads 0xFF because MISO idles pulled up, and a shorted or
// wrongly-driven one reads 0x00. That is what separates this from the touch
// probe, where an idle panel and a missing chip both read zero and a human had
// to look at the screen. Here the wrong pins are silent and the right ones say
// 0x01.
//
// THE SWEEP. SCK and MOSI are taken as known - they are the display's, because
// a slot on a board like this shares the panel's bus and the profile already
// records those two as measured. What is unknown is MISO and CS, so every
// ordered pair of the free GPIOs is tried. That is n*(n-1) attempts, which for
// a dozen candidates is a few seconds and no human input at all.
//
// A CARD MUST BE IN THE SLOT. With an empty slot every pair is correctly
// silent and the probe reports nothing, which is indistinguishable from wrong
// candidates. It says so rather than leaving that to be worked out.

#include <stdio.h>
#include <string.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "sdpins";

// The display's, from boards/waveshare-c6-lcd-13.json - measured, not guessed.
#ifndef PROBE_SCK
#define PROBE_SCK 7
#endif
#ifndef PROBE_MOSI
#define PROBE_MOSI 6
#endif

// Everything the board is not already using. The display takes 6, 7, 14, 15,
// 21 and 22; the WS2812 takes 8; USB takes 12 and 13; UART0 takes 16 and 17.
// GPIO9 is a strapping pin and is left out - driving it during a reset is a
// different kind of afternoon.
static const int CAND[] = { 0, 1, 2, 3, 4, 5, 10, 11, 18, 19, 20, 23 };
#define NCAND ((int)(sizeof CAND / sizeof CAND[0]))

// 400 kHz. The card is required to accept CMD0 anywhere from 100 to 400 kHz and
// nothing here is in a hurry; a fast clock on a pin that turns out to be wrong
// is just a fast wrong answer.
#define PROBE_HZ 400000

static spi_device_handle_t s_dev;

static uint8_t xfer(uint8_t out)
{
    spi_transaction_t t = { 0 };
    uint8_t rx = 0xFF;
    t.length    = 8;
    t.tx_buffer = &out;
    t.rx_buffer = &rx;
    if (spi_device_polling_transmit(s_dev, &t) != ESP_OK) return 0xFF;
    return rx;
}

// One command, and the R1 that comes back. The card may take up to eight bytes
// to answer, and answers with the top bit clear - 0xFF is "still thinking", not
// a reply.
static uint8_t cmd(uint8_t index, uint32_t arg, uint8_t crc)
{
    uint8_t r = 0xFF;
    int i;

    xfer(0xFF);
    xfer((uint8_t)(0x40 | index));
    xfer((uint8_t)(arg >> 24));
    xfer((uint8_t)(arg >> 16));
    xfer((uint8_t)(arg >> 8));
    xfer((uint8_t)arg);
    xfer(crc);
    for (i = 0; i < 10; i++) {
        r = xfer(0xFF);
        if (!(r & 0x80)) break;
    }
    return r;
}

// Returns 1 when a card answered on this pair, 0 otherwise.
static int try_pair(int miso, int cs)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PROBE_MOSI, .miso_io_num = miso, .sclk_io_num = PROBE_SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 64,
    };
    spi_device_interface_config_t dev = {
        .clock_speed_hz = PROBE_HZ, .mode = 0, .spics_io_num = -1, .queue_size = 1,
    };
    uint8_t r1, r7[4];
    int i, ok = 0;

    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED) != ESP_OK) return 0;
    if (spi_bus_add_device(SPI2_HOST, &dev, &s_dev) != ESP_OK) {
        spi_bus_free(SPI2_HOST);
        return 0;
    }

    // CS by hand, because the card wants it held low across a whole command and
    // esp_driver_spi would drop it between transactions.
    gpio_config_t g = { .pin_bit_mask = 1ULL << cs, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&g);
    gpio_set_level((gpio_num_t)cs, 1);

    // 80 clocks with CS high is how a card is told it is in SPI mode at all.
    for (i = 0; i < 10; i++) xfer(0xFF);

    gpio_set_level((gpio_num_t)cs, 0);
    r1 = cmd(0, 0, 0x95);                 // CMD0, the only command with a fixed CRC
    if (r1 == 0x01) {
        // Confirm with CMD8: a card echoes 0x01AA in the last two bytes of R7.
        uint8_t r = cmd(8, 0x000001AA, 0x87);
        for (i = 0; i < 4; i++) r7[i] = xfer(0xFF);
        ok = 1;
        printf("  MISO=%-2d CS=%-2d  CMD0 -> 0x%02x   CMD8 -> 0x%02x %02x %02x %02x %02x%s\n",
               miso, cs, r1, r, r7[0], r7[1], r7[2], r7[3],
               (r7[2] == 0x01 && r7[3] == 0xAA) ? "   <== SDHC/SDXC, CONFIRMED"
                                                : "   <== card answered CMD0");
    }
    gpio_set_level((gpio_num_t)cs, 1);

    spi_bus_remove_device(s_dev);
    spi_bus_free(SPI2_HOST);
    gpio_reset_pin((gpio_num_t)cs);
    return ok;
}

void app_main(void)
{
    int i, j, found = 0;

    printf("\n=== penguinOS microSD pin finder ===\n");
    printf("SCK=%d MOSI=%d (the display's, from the board profile)\n",
           PROBE_SCK, PROBE_MOSI);
    printf("sweeping %d candidates as MISO x CS = %d pairs\n\n",
           NCAND, NCAND * (NCAND - 1));
    printf("A CARD MUST BE IN THE SLOT. With an empty slot every pair is\n"
           "correctly silent and this finds nothing, which looks exactly like\n"
           "the wrong candidates.\n\n");

    for (i = 0; i < NCAND; i++) {
        for (j = 0; j < NCAND; j++) {
            if (i == j) continue;
            found += try_pair(CAND[i], CAND[j]);
            vTaskDelay(1);
        }
    }

    printf("\n%d pair%s answered.\n", found, found == 1 ? "" : "s");
    if (!found)
        printf("Nothing answered. Either there is no card in the slot, or MISO\n"
               "and CS are not both in the candidate list above - widen CAND[]\n"
               "and run it again.\n");
    else
        printf("Put the winning pair into the board profile's peripherals.sdcard\n"
               "and flip present to true.\n");
    ESP_LOGI(TAG, "done");
}
