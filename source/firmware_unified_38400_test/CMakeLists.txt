cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Board type")
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(networkbox_38400_test C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

set(BASE_DIR ${CMAKE_CURRENT_LIST_DIR}/../firmware_unified)
set(IOLIB_DIR ${BASE_DIR}/ioLibrary_Driver)
set(WIZPORT_DIR ${BASE_DIR}/wiznet_port)

add_executable(networkbox_38400_test
    networkbox_slip_wifi_38400.cpp
    ${BASE_DIR}/third_party/dhcpserver/dhcpserver.c
    ${IOLIB_DIR}/Ethernet/socket.c
    ${IOLIB_DIR}/Ethernet/wizchip_conf.c
    ${IOLIB_DIR}/Ethernet/W5500/w5500.c
    ${WIZPORT_DIR}/ioLibrary_Driver/src/w5x00_spi.c
    ${WIZPORT_DIR}/lwip/w5x00_lwip.c
)

pico_set_program_name(networkbox_38400_test "NetworkBox 38400 test")
pico_enable_stdio_uart(networkbox_38400_test 0)
pico_enable_stdio_usb(networkbox_38400_test 1)

target_include_directories(networkbox_38400_test PRIVATE
    ${BASE_DIR}
    ${BASE_DIR}/third_party/dhcpserver
    ${IOLIB_DIR}/Ethernet
    ${IOLIB_DIR}/Ethernet/W5500
    ${WIZPORT_DIR}
    ${WIZPORT_DIR}/ioLibrary_Driver/inc
    ${WIZPORT_DIR}/lwip
)

target_compile_definitions(networkbox_38400_test PRIVATE
    _WIZCHIP_=W5500
    DEVICE_BOARD_NAME=2
)

target_link_libraries(networkbox_38400_test PRIVATE
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    pico_flash
    hardware_spi
    hardware_dma
    hardware_clocks
)

pico_add_extra_outputs(networkbox_38400_test)
