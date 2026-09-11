# The app component: the boot glue, and nothing else.
#
# It depends on eos_kernel and on esp_timer. The board header include path and
# the EOS_BOARD_HEADER definition both arrive through eos_kernel's PUBLIC
# interface, so this file does not repeat the board choice.
#
# The embedded theme is the one non-obvious entry. kernel/theme guarantees that
# a missing or corrupt theme file cannot stop the boot, and the fallback it
# guarantees is the neutral slate default. Linking cyd-amber.json in gives the
# search one more step before that default, so a board with no card and no
# mounted filesystem still comes up looking like penguinOS. ~1.7 KB of rodata, and
# it is the only thing in the image that runs eos_theme_parse() on target.

idf_component_register(
    SRCS
        "main.c"
        "eos_boot_theme.c"
        "eos_brain_bridge.c"
        "eos_buddy_model.c"
        "eos_shell_draw.c"
        "eos_app_registry.c"
        "eos_app_basic.c"
        "eos_app_camera.c"
        "eos_app_chat.c"
        "eos_app_files.c"
        "eos_app_media.c"
        "eos_app_party.c"
        "eos_app_tetris.c"
        "eos_led.c"
        "eos_shell_input.c"
        "eos_setup_screen.c"
        "eos_ota.c"
        "eos_web_embed.c"
        "eos_seed_buddy.c"
        "eos_settings_bind.c"
    REQUIRES
        eos_kernel
        esp_timer
        # esp_ota_ops, for updating penguinOS from the web app. The partition
        # table carries two app slots so there is somewhere to write.
        app_update
        # The WS2812 on GPIO8, which the Media and Party windows drive. It is
        # the RMT transmit driver and nothing else; the LED is one part and its
        # whole frame is 24 bits, so no strip library is pulled in for it.
        esp_driver_rmt
    EMBED_TXTFILES
        "${EOS_KERNEL_DIR}/theme/themes/cyd-amber.json"
    EMBED_FILES
        "${CMAKE_CURRENT_LIST_DIR}/../../web/index.html"
        "${CMAKE_CURRENT_LIST_DIR}/../../web/app.js"
        "${CMAKE_CURRENT_LIST_DIR}/../../web/style.css"
        "${CMAKE_CURRENT_LIST_DIR}/../../web/setup.js"
        "${CMAKE_CURRENT_LIST_DIR}/../../web/voxel-editor.js"
        # The gallery. Each pair is one row in SHIPPED[] in eos_seed_buddy.c,
        # and the two lists have to stay in step: a file embedded with no row
        # is 5 KB of rodata nothing writes, and a row with no file does not
        # link. penguin.vox/buddy.json are Pip and are named for what they
        # were before the gallery existed.
        "${CMAKE_CURRENT_LIST_DIR}/../../assets/buddy/penguin.vox"
        "${CMAKE_CURRENT_LIST_DIR}/../../assets/buddy/buddy.json"
        "${CMAKE_CURRENT_LIST_DIR}/../../assets/buddy/cat.vox"
        "${CMAKE_CURRENT_LIST_DIR}/../../assets/buddy/cat.json"
        "${CMAKE_CURRENT_LIST_DIR}/../../assets/buddy/owl.vox"
        "${CMAKE_CURRENT_LIST_DIR}/../../assets/buddy/owl.json"
        "${CMAKE_CURRENT_LIST_DIR}/../../assets/buddy/robot.vox"
        "${CMAKE_CURRENT_LIST_DIR}/../../assets/buddy/robot.json"
        "${EOS_KERNEL_DIR}/theme/themes/carbon.json"
        "${EOS_KERNEL_DIR}/theme/themes/catppuccin-mocha.json"
        "${EOS_KERNEL_DIR}/theme/themes/ember.json"
        "${EOS_KERNEL_DIR}/theme/themes/goldleaf.json"
        "${EOS_KERNEL_DIR}/theme/themes/gruvbox.json"
        "${EOS_KERNEL_DIR}/theme/themes/tokyonight.json"
)

# Same promise the kernel makes. These files are kernel code that happens to
# live next to app_main, and they compile clean for riscv32 and xtensa.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# A TEST HOOK for the OTA rollback, off unless asked for:
#
#   idf.py -D EOS_OTA_NEVER_CONFIRM=1 build
#
# builds an image that works completely and never tells the bootloader so. That
# is what a genuinely broken update looks like from the bootloader's side, and
# it is the only way to prove the rollback catches anything without building
# something actually broken and risking not getting the board back.
#
# -D on the idf.py line is a CMAKE variable and reaches no C file on its own,
# which is exactly how an earlier attempt at this produced two identical images
# and a test that proved nothing. This is the line that bridges it.
if(EOS_OTA_NEVER_CONFIRM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EOS_OTA_NEVER_CONFIRM=1)
    message(WARNING "EOS_OTA_NEVER_CONFIRM: this image will NOT confirm itself "
                    "and the bootloader will roll it back. Test builds only.")
endif()
