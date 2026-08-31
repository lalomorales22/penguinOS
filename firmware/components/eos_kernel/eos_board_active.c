// eos_board_active — the one board this image was built for, and the only three
// facts the silicon will admit to about itself.
//
// eos_board.h declares eos_board_get() and eos_board_probe() and implements
// neither, because neither can be written above the HAL line: the descriptor is
// chosen when the image is built, and probing needs IDF. Both bodies live here.
//
// The one non-obvious constraint: this is the only file in the firmware that
// names a board header, and it does not name one either — it includes whatever
// EOS_BOARD_HEADER expands to, which CMake sets from EOS_BOARD_ID. There is no
// #ifdef per board anywhere in penguinOS, and adding one here would be the first
// crack in the reason the registry exists at all.
//
// eos_board_probe() reports chip model, flash size, PSRAM size and MAC. That is
// the complete list of what is discoverable. It does not identify the panel,
// and it must not learn to: the ILI9488 on the wavvy boards answers register
// 0xD3 with 00 7F DF, which matches no part, so a controller ID read would
// return confident nonsense. Identity is the registry plus one human
// confirmation; eos_board_check() only verifies what is verifiable.

#include <string.h>

#include EOS_BOARD_HEADER

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

const eos_board_t *eos_board_get(void)
{
    return &EOS_BOARD;
}

// esp_chip_model_t is an IDF enum with its own numbering and gaps; eos_soc_t is
// ours and is dense. The mapping is explicit so that an unrecognised model
// falls out as a SOC mismatch in eos_board_check() rather than aliasing onto
// whichever of our enumerators happens to share its integer value.
static int soc_from_model(esp_chip_model_t m, uint8_t *out)
{
    switch (m) {
    case CHIP_ESP32:    *out = EOS_SOC_ESP32;    return 0;
    case CHIP_ESP32S2:  *out = EOS_SOC_ESP32_S2; return 0;
    case CHIP_ESP32S3:  *out = EOS_SOC_ESP32_S3; return 0;
    case CHIP_ESP32C3:  *out = EOS_SOC_ESP32_C3; return 0;
    case CHIP_ESP32C5:  *out = EOS_SOC_ESP32_C5; return 0;
    case CHIP_ESP32C6:  *out = EOS_SOC_ESP32_C6; return 0;
    case CHIP_ESP32P4:  *out = EOS_SOC_ESP32_P4; return 0;
    default:            return -1;
    }
}

eos_err_t eos_board_probe(eos_probe_t *out)
{
    esp_chip_info_t info;
    uint32_t flash = 0;

    if (!out) return EOS_ERR_ARG;
    memset(out, 0, sizeof *out);

    esp_chip_info(&info);
    // An unknown model is left as the value memset put there, which is
    // EOS_SOC_ESP32. That is deliberate: every mismatch path in
    // eos_board_check() ends in "stop and tell the human", and reporting a
    // wrong-but-real SoC gets there faster than inventing an EOS_SOC_UNKNOWN
    // that four switch statements would then have to carry.
    (void)soc_from_model(info.model, &out->soc);

    // The default chip, which is the one the bootloader came out of. A failure
    // here leaves flash_bytes 0, and 0 never equals a registry flash size, so
    // the caller sees EOS_MISMATCH_FLASH rather than a plausible number.
    if (esp_flash_get_size(NULL, &flash) != ESP_OK) flash = 0;
    out->flash_bytes = flash;

#if CONFIG_SPIRAM
    out->psram_bytes = (uint32_t)esp_psram_get_size();
#else
    out->psram_bytes = 0;
#endif

    // The factory-programmed base MAC, not the Wi-Fi station MAC: this must
    // read the same with the radios down, which is how the board is running
    // during bring-up.
    if (esp_read_mac(out->mac, ESP_MAC_BASE) != ESP_OK) memset(out->mac, 0, sizeof out->mac);

    return EOS_OK;
}
