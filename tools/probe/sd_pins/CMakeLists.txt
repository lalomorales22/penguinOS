# penguinOS microSD pin finder.
#
# A separate ESP-IDF project on purpose. It could have been a routine inside
# penguinOS behind a flag, but this has to drive candidate pins hard and in
# combinations that are wrong by definition - most of the sweep IS the wrong
# answer - and doing that with a display driver, a radio and a filesystem live
# on the same chip is asking for a confusing result. Nothing here comes up
# except a clock and a chip select.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sd-pins)
