# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(networkbox_unified C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

set(IOLIB_DIR ${CMAKE_CURRENT_LIST_DIR}/ioLibrary_Driver)
set(WIZPORT_DIR ${CMAKE_CURRENT_LIST_DIR}/wiznet_port)

add_executable(networkbox_unified
        networkbox_slip_wifi.cpp
        third_party/dhcpserver/dhcpserver.c
        ${IOLIB_DIR}/Ethernet/socket.c
        ${IOLIB_DIR}/Ethernet/wizchip_conf.c
        ${IOLIB_DIR}/Ethernet/W5500/w5500.c
        ${WIZPORT_DIR}/ioLibrary_Driver/src/w5x00_spi.c
        ${WIZPORT_DIR}/lwip/w5x00_lwip.c
        )

pico_set_program_name(networkbox_unified "networkbox_unified")
pico_set_program_version(networkbox_unified "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(networkbox_unified 0)
pico_enable_stdio_usb(networkbox_unified 1)

# Add the standard library to the build
target_link_libraries(networkbox_unified
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(networkbox_unified PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/third_party/dhcpserver
        ${IOLIB_DIR}/Ethernet
        ${IOLIB_DIR}/Ethernet/W5500
        ${WIZPORT_DIR}
        ${WIZPORT_DIR}/ioLibrary_Driver/inc
        ${WIZPORT_DIR}/lwip
)

target_compile_definitions(networkbox_unified PRIVATE
        _WIZCHIP_=W5500
        DEVICE_BOARD_NAME=2
)

# Add any user requested libraries
target_link_libraries(networkbox_unified
        pico_cyw43_arch_lwip_threadsafe_background
        pico_flash
        hardware_spi
        hardware_dma
        hardware_clocks
        )

pico_add_extra_outputs(networkbox_unified)

