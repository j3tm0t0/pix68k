# RP2040 (Pico W) の無線 LAN 越しの更新のためのブートローダ。../README.md の「無線 LAN 越しの更新 (Pico W)」を参照。
#   PICO_SDK_PATH=~/pico-sdk cmake -S tools/pico_kbd/boot2040 -B build/boot2040 -DPICO_BOARD=pico_w -DPICO_NO_PICOTOOL=1
cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(pico_kbd_boot2040 C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()
if(NOT PICO_RP2040)
  message(FATAL_ERROR "このブートローダは RP2040 のボード (pico_w など) 用")
endif()

add_executable(pico_kbd_boot2040 main.c ../otaboot.c ../core.c)
target_include_directories(pico_kbd_boot2040 PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
target_link_libraries(pico_kbd_boot2040 PRIVATE pico_stdlib hardware_flash hardware_watchdog hardware_sync)
pico_enable_stdio_uart(pico_kbd_boot2040 0)
pico_enable_stdio_usb(pico_kbd_boot2040 0)
pico_add_extra_outputs(pico_kbd_boot2040)
