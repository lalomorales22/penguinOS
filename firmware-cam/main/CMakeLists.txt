# The camera node compiles the penguinOS services it needs DIRECTLY, rather
# than depending on the eos_kernel component.
#
# eos_kernel builds the whole OS - window manager, shell, display backend,
# avatar - and requires a board profile to compile against. This node has no
# screen and no board profile, so that component cannot build for it.
#
# What it needs is a small subset, and the subset is clean by construction:
# eos_net.c has ZERO references to eos_board, and eos_httpd.c deliberately
# spells its own error values locally rather than including it (see the comment
# at eos_httpd.c:553). Neither knows a display exists.
#
# eos_ble.c is DELIBERATELY absent. eos_net calls no BLE function at all - only
# eos_radio's lock - and the NimBLE stack costs about 37 KB. A node with no
# keyboard should not pay for one.
set(EOS_KERNEL_DIR "${CMAKE_CURRENT_LIST_DIR}/../../kernel")

idf_component_register(
    SRCS
        "main.c"
        "eos_cam.c"
        "eos_ble_absent.c"
        "${EOS_KERNEL_DIR}/svc/eos_net.c"
        "${EOS_KERNEL_DIR}/svc/eos_httpd.c"
        "${EOS_KERNEL_DIR}/svc/eos_radio.c"
    INCLUDE_DIRS
        "."
        "${EOS_KERNEL_DIR}/svc/include"
        "${EOS_KERNEL_DIR}/hal/include"
        # eos_net.c includes eos_ble.h for its types (it calls no BLE function),
        # and that reaches eos_board.h, which reaches eos_wm.h. Headers only -
        # no window manager is compiled into this image.
        "${EOS_KERNEL_DIR}/wm/include"
    REQUIRES
        esp_psram
        esp_driver_gpio
        esp_driver_ledc
        esp_timer
        esp_http_server
        esp_netif
        esp_wifi
        esp_event
        nvs_flash
        lwip
        espressif__mdns
        espressif__esp32-camera
)
