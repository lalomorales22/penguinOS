# ESP-OS firmware. One IDF project; one board per image.
#
# The board is a build-time choice and never a source edit. -DEOS_BOARD_ID=<id>
# picks which boards/generated/<id>.h the kernel compiles against, and the IDF
# target is read back out of that header rather than being stated twice. The one
# non-obvious constraint: the kernel sources are NOT inside this directory. They
# stay in kernel/ and are pulled in by relative path, because a copy under
# firmware/ would silently rot the moment someone edited the original.

cmake_minimum_required(VERSION 3.16)

set(EOS_BOARD_ID "waveshare-c6-lcd-13" CACHE STRING
    "Board id from boards/*.json. Selects boards/generated/<id>.h and the IDF target.")

get_filename_component(EOS_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(EOS_KERNEL_DIR "${EOS_REPO_ROOT}/kernel"            CACHE INTERNAL "")
set(EOS_BOARDS_DIR "${EOS_REPO_ROOT}/boards/generated"  CACHE INTERNAL "")
set(EOS_BOARD_HEADER_PATH "${EOS_BOARDS_DIR}/${EOS_BOARD_ID}.h")

# boards/generated/ is gitignored: the headers are derived from boards/*.json and
# a fresh clone has none. Say so here instead of emitting forty include errors.
if(NOT EXISTS "${EOS_BOARD_HEADER_PATH}")
    file(GLOB _eos_have "${EOS_BOARDS_DIR}/*.h")
    if(_eos_have)
        string(REPLACE "${EOS_BOARDS_DIR}/" "" _eos_have "${_eos_have}")
        string(REPLACE ".h;" ", " _eos_have "${_eos_have}")
        string(REPLACE ".h"  ""   _eos_have "${_eos_have}")
        message(FATAL_ERROR
            "EOS_BOARD_ID=${EOS_BOARD_ID} has no generated header.\n"
            "  looked for: ${EOS_BOARD_HEADER_PATH}\n"
            "  generated:  ${_eos_have}")
    endif()
    message(FATAL_ERROR
        "boards/generated/ is empty. It is derived, not committed. Run:\n"
        "  python3 ${EOS_REPO_ROOT}/tools/gen_board_header.py --all")
endif()

# The header states its own IDF target. Read it rather than keeping a second
# board-to-target table here that could disagree with the registry.
file(READ "${EOS_BOARD_HEADER_PATH}" _eos_hdr)
string(REGEX MATCH "#define[ \t]+EOS_TARGET[ \t]+\"([a-z0-9]+)\"" _eos_junk "${_eos_hdr}")
set(EOS_BOARD_TARGET "${CMAKE_MATCH_1}")
unset(_eos_hdr)
if(NOT EOS_BOARD_TARGET)
    message(FATAL_ERROR "no EOS_TARGET in ${EOS_BOARD_HEADER_PATH}; regenerate it")
endif()

# Only a default. An existing sdkconfig, an explicit -DIDF_TARGET, or IDF_TARGET
# in the environment all win, so `idf.py set-target` keeps behaving normally.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig"
   AND NOT DEFINED IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET})
    set(IDF_TARGET "${EOS_BOARD_TARGET}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-os)

# Flashing an esp32 image built against a C6 pinout is a dead board and a long
# afternoon. Catch it at configure time.
if(NOT IDF_TARGET STREQUAL EOS_BOARD_TARGET)
    message(FATAL_ERROR
        "board/target mismatch: EOS_BOARD_ID=${EOS_BOARD_ID} needs target "
        "'${EOS_BOARD_TARGET}', this build tree is '${IDF_TARGET}'.\n"
        "  idf.py fullclean && idf.py -DEOS_BOARD_ID=${EOS_BOARD_ID} set-target ${EOS_BOARD_TARGET}")
endif()

message(STATUS "ESP-OS board: ${EOS_BOARD_ID} (${EOS_BOARD_TARGET})")
