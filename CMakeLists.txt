cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w CACHE STRING "Board type")
include(pico_sdk_import.cmake)

project(pico2homekit C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

if (NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/config/config.h)
    message(FATAL_ERROR "config/config.h not found. Copy config/config.example.h to config/config.h and fill it in.")
endif()

add_executable(pico2homekit
    src/main.c
    src/mibeacon.c
    src/sensors.c
)

target_include_directories(pico2homekit PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src      # btstack_config.h, lwipopts.h, mbedtls_config.h
    ${CMAKE_CURRENT_LIST_DIR}/config
)

target_link_libraries(pico2homekit
    pico_stdlib
    pico_cyw43_arch_lwip_poll      # single-threaded: everything runs from the main loop
    pico_btstack_ble
    pico_btstack_cyw43
    pico_mbedtls     # only the parts enabled in src/mbedtls_config.h are compiled in
)

pico_enable_stdio_usb(pico2homekit 1)
pico_enable_stdio_uart(pico2homekit 0)
pico_add_extra_outputs(pico2homekit)
