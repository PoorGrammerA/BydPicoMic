cmake_minimum_required(VERSION 3.13)

if (DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()

if (NOT PICO_SDK_PATH)
    message(FATAL_ERROR "PICO_SDK_PATH is not set. Export PICO_SDK_PATH=/path/to/pico-sdk")
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pa10_rp2040_mic C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Pico SDK 1.5.1 bundles a UAC2-only audio device implementation.  Keep a
# project-local TinyUSB checkout so this target can advertise a real UAC1
# microphone to vehicle head units that do not enable UAC2 capture streams.
set(PICO_TINYUSB_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/tinyusb-uac1" CACHE PATH "" FORCE)

pico_sdk_init()

add_executable(pa10_rp2040_mic
    src/main.c
    src/inmp441_i2s.c
    src/usb_descriptors.c
)

pico_generate_pio_header(pa10_rp2040_mic
    ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio
)
pico_generate_pio_header(pa10_rp2040_mic
    ${CMAKE_CURRENT_LIST_DIR}/src/inmp441_i2s.pio
)

target_include_directories(pa10_rp2040_mic PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_link_libraries(pa10_rp2040_mic PRIVATE
    pico_stdlib
    hardware_dma
    hardware_irq
    hardware_pio
    tinyusb_device
    tinyusb_board
)

# The native USB controller is reserved for our UAC1 + HID composite device.
pico_enable_stdio_usb(pa10_rp2040_mic 0)
pico_enable_stdio_uart(pa10_rp2040_mic 1)

pico_add_extra_outputs(pa10_rp2040_mic)
