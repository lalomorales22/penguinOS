# eos_kernel — the ESP-OS kernel, registered as an IDF component.
#
# The sources are NOT here. They stay under kernel/ and are listed by relative
# path, because the same files are compiled by six host test suites with a plain
# cc and must keep passing; a copy inside firmware/ would be two files with one
# name and would rot the first time someone fixed a bug in the wrong one.
#
# The one non-obvious constraint: everything in SRCS below is portable C99 with
# no allocation and no hardware, except the tail of eos_brain.c, which sits
# behind #ifdef ESP_PLATFORM and is the only reason this component REQUIRES a
# network stack. That section needs mdns, which left IDF core in v5.0 and is a
# managed component now — see idf_component.yml.

idf_component_register(
    SRCS
        "eos_board_active.c"
        "${EOS_KERNEL_DIR}/wm/eos_wm.c"
        "${EOS_KERNEL_DIR}/theme/eos_theme.c"
        "${EOS_KERNEL_DIR}/shell/eos_bar.c"
        "${EOS_KERNEL_DIR}/shell/eos_keys.c"
        "${EOS_KERNEL_DIR}/avatar/eos_vox.c"
        "${EOS_KERNEL_DIR}/avatar/eos_buddy.c"
        "${EOS_KERNEL_DIR}/svc/eos_brain.c"
    INCLUDE_DIRS
        "${EOS_KERNEL_DIR}/hal/include"
        "${EOS_KERNEL_DIR}/wm/include"
        "${EOS_KERNEL_DIR}/theme/include"
        "${EOS_KERNEL_DIR}/shell/include"
        "${EOS_KERNEL_DIR}/avatar/include"
        "${EOS_KERNEL_DIR}/svc/include"
        "${EOS_BOARDS_DIR}"
    REQUIRES
        esp_hw_support   # esp_chip_info.h, esp_mac.h
        spi_flash        # esp_flash_get_size
        esp_timer        # eos_brain's clock
        esp_netif        # esp_ip4_addr_t, IPSTR/IP2STR
        lwip             # BSD sockets under ESP_PLATFORM
        nvs_flash        # eos_brain caches the discovered host
)

# Which boards/generated/<id>.h everything downstream compiles against. PUBLIC,
# so the display backend and the boot glue get it without repeating the choice.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    EOS_BOARD_HEADER="${EOS_BOARD_ID}.h")

# The kernel is required to build clean under these for both riscv32 and xtensa
# and the host suites already do. PRIVATE, so the promise is not imposed on IDF's
# own components or on app code that has not opted in.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
