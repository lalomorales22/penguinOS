# The app component. It depends on eos_kernel and on nothing else: the board
# header include path and the EOS_BOARD_HEADER definition both arrive through
# eos_kernel's PUBLIC interface, so this file does not repeat the board choice.
idf_component_register(
    SRCS "main.c"
    REQUIRES eos_kernel
)
