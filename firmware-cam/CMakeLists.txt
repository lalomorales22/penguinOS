# penguinOS camera node.
#
# A SECOND ESP-IDF project, deliberately. The XIAO ESP32-S3 Sense has no screen,
# and penguinOS's main.c brings a display up before anything else - so this is
# not a board profile with the display switched off, it is a different program
# that happens to share most of an operating system.
#
# What it shares is the part that matters: kernel/svc/eos_net.c and
# kernel/svc/eos_httpd.c have NO display dependency at all (eos_net has zero
# references, eos_httpd's single one is a comment). So the camera node gets the
# same Wi-Fi provisioning, the same captive portal, the same mDNS name and the
# same HTTP conventions as every other board in the fleet, rather than
# reinventing them badly.
cmake_minimum_required(VERSION 3.16)

set(EOS_ROOT       "${CMAKE_CURRENT_LIST_DIR}/..")
set(EOS_KERNEL_DIR "${EOS_ROOT}/kernel")

# NOT firmware/components. That directory holds eos_kernel, which builds the
# whole OS - window manager, shell, display backend - and force-includes a
# generated board header. This node has no board profile, so merely having that
# component visible makes every file compile with EOS_BOARD_HEADER=".h" and
# fail. main/CMakeLists.txt compiles the two services this node needs directly.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(penguinos-cam)
