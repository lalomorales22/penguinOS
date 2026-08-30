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
# mounted filesystem still comes up looking like ESP-OS. ~1.7 KB of rodata, and
# it is the only thing in the image that runs eos_theme_parse() on target.

idf_component_register(
    SRCS
        "main.c"
        "eos_boot_theme.c"
        "eos_shell_draw.c"
        "eos_shell_input.c"
        "eos_setup_screen.c"
        "eos_web_embed.c"
    REQUIRES
        eos_kernel
        esp_timer
    EMBED_TXTFILES
        "${EOS_KERNEL_DIR}/theme/themes/cyd-amber.json"
    EMBED_FILES
        "${CMAKE_CURRENT_LIST_DIR}/../../web/index.html"
        "${CMAKE_CURRENT_LIST_DIR}/../../web/app.js"
        "${CMAKE_CURRENT_LIST_DIR}/../../web/style.css"
        "${CMAKE_CURRENT_LIST_DIR}/../../web/setup.js"
        "${CMAKE_CURRENT_LIST_DIR}/../../web/voxel-editor.js"
)

# Same promise the kernel makes. These five files are kernel code that happens
# to live next to app_main, and they compile clean for riscv32 and xtensa.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
