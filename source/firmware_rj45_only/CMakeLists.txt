cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PICO_BOARD pico2_w CACHE STRING "Pico 2 W")
include(pico_sdk_import.cmake)

project(networkbox_slip_w5500 C CXX ASM)
pico_sdk_init()

set(IOLIB_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/ioLibrary_Driver)
set(WIZPORT_DIR ${CMAKE_CURRENT_LIST_DIR}/wiznet_port)

add_executable(networkbox_slip_w5500
    networkbox_slip_w5500.cpp
    ${IOLIB_DIR}/Ethernet/socket.c
    ${IOLIB_DIR}/Ethernet/wizchip_conf.c
    ${IOLIB_DIR}/Ethernet/W5500/w5500.c
    ${WIZPORT_DIR}/ioLibrary_Driver/src/w5x00_spi.c
    ${WIZPORT_DIR}/lwip/w5x00_lwip.c
)

target_compile_definitions(networkbox_slip_w5500 PRIVATE
    _WIZCHIP_=W5500
    DEVICE_BOARD_NAME=2
)

target_include_directories(networkbox_slip_w5500 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${IOLIB_DIR}/Ethernet
    ${IOLIB_DIR}/Ethernet/W5500
    ${WIZPORT_DIR}
    ${WIZPORT_DIR}/ioLibrary_Driver/inc
    ${WIZPORT_DIR}/lwip
)

# The W5500 feeds raw Ethernet frames into lwIP in NO_SYS mode.
target_include_directories(pico_lwip INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(networkbox_slip_w5500 PRIVATE
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_clocks
    pico_lwip
    pico_lwip_nosys
)

pico_enable_stdio_uart(networkbox_slip_w5500 0)
pico_enable_stdio_usb(networkbox_slip_w5500 1)
pico_add_extra_outputs(networkbox_slip_w5500)
