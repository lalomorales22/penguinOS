# eos_kernel — the penguinOS kernel, registered as an IDF component.
#
# The sources are NOT here. They stay under kernel/ and are listed by relative
# path, because the same files are compiled by six host test suites with a plain
# cc and must keep passing; a copy inside firmware/ would be two files with one
# name and would rot the first time someone fixed a bug in the wrong one.
#
# The one non-obvious constraint: everything in SRCS below is portable C99 with
# no allocation and no hardware, except two files. The tail of eos_brain.c sits
# behind #ifdef ESP_PLATFORM and is the only reason this component REQUIRES a
# network stack; that section needs mdns, which left IDF core in v5.0 and is a
# managed component now — see idf_component.yml. And eos_display_st7789.c is
# the ST7789 backend: it is where the esp_lcd, spi, ledc and gpio requirements
# come from and it makes the single heap allocation in the whole image. It
# still compiles for the host with ESP_PLATFORM unset, which is how its 109
# checks run. eos_storage_idf.c is the third: its mount and unmount call
# joltwallet's littlefs component, which is where the REQUIRES on
# joltwallet__littlefs comes from. Everything else in that file is POSIX and is
# what the host suite runs against a directory in /tmp.

set(_eos_kernel_srcs
    "${EOS_KERNEL_DIR}/wm/eos_wm.c"
    "${EOS_KERNEL_DIR}/theme/eos_theme.c"
    "${EOS_KERNEL_DIR}/shell/eos_bar.c"
    "${EOS_KERNEL_DIR}/shell/eos_keys.c"
    "${EOS_KERNEL_DIR}/shell/eos_launcher.c"
    "${EOS_KERNEL_DIR}/shell/eos_pointer.c"
    "${EOS_KERNEL_DIR}/avatar/eos_vox.c"
    "${EOS_KERNEL_DIR}/avatar/eos_buddy.c"
    "${EOS_KERNEL_DIR}/svc/eos_brain.c"
    "${EOS_KERNEL_DIR}/svc/eos_radio.c"
    "${EOS_KERNEL_DIR}/svc/eos_net.c"
    "${EOS_KERNEL_DIR}/svc/eos_ble.c"
    "${EOS_KERNEL_DIR}/svc/eos_httpd.c"
    "${EOS_KERNEL_DIR}/svc/eos_apps.c"
    "${EOS_KERNEL_DIR}/svc/eos_settings.c"
    "${EOS_KERNEL_DIR}/font/eos_font.c"
    "${EOS_KERNEL_DIR}/qr/eos_qr.c"
    "${EOS_KERNEL_DIR}/hal/eos_input.c"
    "${EOS_KERNEL_DIR}/hal/backend/esp_lcd/eos_display_st7789.c"
    "${EOS_KERNEL_DIR}/hal/backend/storage/eos_storage_idf.c")

idf_component_register(
    SRCS
        "eos_board_active.c"
        ${_eos_kernel_srcs}
    INCLUDE_DIRS
        "${EOS_KERNEL_DIR}/hal/include"
        "${EOS_KERNEL_DIR}/wm/include"
        "${EOS_KERNEL_DIR}/theme/include"
        "${EOS_KERNEL_DIR}/shell/include"
        "${EOS_KERNEL_DIR}/avatar/include"
        "${EOS_KERNEL_DIR}/svc/include"
        "${EOS_KERNEL_DIR}/font/include"
        "${EOS_KERNEL_DIR}/qr/include"
        "${EOS_BOARDS_DIR}"
    REQUIRES
        esp_hw_support   # esp_chip_info.h, esp_mac.h
        spi_flash        # esp_flash_get_size
        esp_timer        # eos_brain's clock
        esp_netif        # esp_ip4_addr_t, IPSTR/IP2STR
        lwip             # BSD sockets under ESP_PLATFORM
        nvs_flash        # eos_brain caches the host; eos_net keeps credentials
        esp_wifi         # eos_net: STA, SoftAP, scanning
        esp_event        # eos_net's join and got-ip handlers
        esp_http_server  # eos_httpd. No DNS here: the captive responder is eos_net's.
        esp_lcd          # esp_lcd_new_panel_st7789 and the panel ops
        esp_driver_spi   # driver/spi_master.h, the panel's bus
        esp_driver_ledc  # driver/ledc.h, the backlight
        esp_driver_gpio  # the panel's CS/DC/RST pins, and eos_input's buttons
        bt               # NimBLE for eos_ble. Never Bluedroid: 83KB vs 19KB is an OOM.
        joltwallet__littlefs   # eos_storage's "/int" mount. Third party, see idf_component.yml.
        vfs              # eos_storage_idf.c is POSIX: open/read/opendir go through esp_vfs
)

# Which boards/generated/<id>.h everything downstream compiles against. PUBLIC,
# so the display backend and the boot glue get it without repeating the choice.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    EOS_BOARD_HEADER="${EOS_BOARD_ID}.h")

# eos_net's radio lock needs no gate any more. It used to: the lock lived at
# the tail of eos_ble.h and eos_net had to be told with a compile definition to
# bind to it, which meant an image built without eos_ble.c was silently
# unserialised while still claiming a radio it shared. The lock is now
# kernel/svc/eos_radio.c, in SRCS above and owned by neither stack, and
# eos_net.c binds to eos_radio.h with __has_include. Both scans take the same
# lock in every build there is.

# The kernel is required to build clean under these for both riscv32 and xtensa
# and the host suites already do. PRIVATE, so the promise is not imposed on IDF's
# own components or on app code that has not opted in.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
