cmake_minimum_required(VERSION 3.13)

# Raspberry Pi Pico 2 by default; for an original Pico use
#   -DPICO_BOARD=pico -DPICO_PLATFORM=rp2040
set(PICO_BOARD pico2 CACHE STRING "Board")
if (PICO_BOARD STREQUAL "pico" OR PICO_BOARD STREQUAL "pico_w")
    set(PICO_PLATFORM rp2040 CACHE STRING "Platform")
else()
    set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")
endif()

include(pico_sdk_import.cmake)
project(pidp8_panel C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(pidp8_panel main.c usb_descriptors.c)
target_include_directories(pidp8_panel PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(pidp8_panel PRIVATE -O2 -Wall)
target_link_libraries(pidp8_panel PRIVATE
    pico_stdlib pico_multicore pico_unique_id hardware_gpio tinyusb_device tinyusb_board)
pico_set_program_name(pidp8_panel "PiDP-8/I panel interface")
pico_add_extra_outputs(pidp8_panel)
