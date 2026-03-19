cmake_minimum_required(VERSION 3.13)

# ================================================================
# CORE MAIN
# ================================================================
project(core_main C CXX ASM)
set(CMAKE_C_STANDARD 11)

# Point this at your pico-sdk location
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
pico_sdk_init()

# PicoDVI library — https://github.com/Wren6991/PicoDVI
# Clone it alongside this project and set PICODVI_PATH
set(PICODVI_PATH ${CMAKE_CURRENT_SOURCE_DIR}/PicoDVI CACHE PATH "PicoDVI root")

# ---- CORE MAIN executable ----
add_executable(core_main
    core_main.c
)

target_include_directories(core_main PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(core_main
    pico_stdlib
    pico_multicore
    hardware_uart
    hardware_gpio
    hardware_clocks
    tinyusb_host        # TinyUSB host stack
    tinyusb_board
)

# USB host config — create a tusb_config.h in this directory
# (see tusb_config.h below)
target_include_directories(core_main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

pico_set_binary_type(core_main copy_to_ram)
pico_add_extra_outputs(core_main)

# ================================================================
# CORE VIDEO  (separate project — build in core_video/ subfolder)
# ================================================================
project(core_video C CXX ASM)

add_executable(core_video
    core_video.c
)

target_include_directories(core_video PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PICODVI_PATH}/software/include
)

target_link_libraries(core_video
    pico_stdlib
    pico_multicore
    hardware_uart
    hardware_spi
    hardware_dma
    hardware_clocks
    hardware_structs
    libdvi              # from PicoDVI
)

pico_set_binary_type(core_video copy_to_ram)
pico_add_extra_outputs(core_video)
