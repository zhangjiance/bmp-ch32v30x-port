cmake_minimum_required(VERSION 3.22)

# ==============================================================================
# bmp-ch32v30x-port - Black Magic Probe port for CH32V30x
#
# Standalone application for the ch32_dfu_boot DFU bootloader:
#   * links at 0x00008000, the application partition (32 KB bootloader)
#   * DFU runtime interface: "dfu-util -e" (and "monitor bootloader") resets
#     into the bootloader through the BKP hand-shake
#
# Layering (bmp-hpm-port style, with the build conventions of ch32_dfu_boot /
# ch32_hello_world):
#   blackmagic/         submodule: BMP core + target drivers
#   port/ch32v30x/      platform layer: USB, SWD/JTAG bit banging, target UART,
#                       timing, boot hand-shake
#   boards/<board>/     board BSP (pins live in port/ch32v30x/jtag_port.h)
#   src/                main + WCH system/startup glue
# ==============================================================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# The CH32V305/307 USBHS block runs in high speed (same as ch32v305_bmp).
add_compile_definitions(-DCONFIG_USB_HS)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

set(CMAKE_PROJECT_NAME bmp-ch32v30x-port)

# CherryUSB device stack: two CDC ACM functions (GDB + target UART).
set(CONFIG_CHERRYUSB_DEVICE 1)
set(CONFIG_CHERRYUSB_DEVICE_CDC_ACM 1)

set(CHERRYUSB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party_components/CherryUSB"
    CACHE PATH "CherryUSB submodule root")
set(SDK_ROOT       "${CMAKE_CURRENT_SOURCE_DIR}/SDK"
    CACHE PATH "WCH ch32v30x peripheral library root")

# ------------------------------------------------------------------------------
# Board / chip selection
# ------------------------------------------------------------------------------
set(BOARD "ch32v30x_bmp" CACHE STRING "Target board directory under boards/")
set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}")

file(GLOB _board_cfgs RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/boards" "${CMAKE_CURRENT_SOURCE_DIR}/boards/*/board_config.h")
set(_available_boards "")
foreach(_cfg ${_board_cfgs})
    get_filename_component(_dir "${_cfg}" DIRECTORY)
    list(APPEND _available_boards "${_dir}")
endforeach()

if(NOT EXISTS "${BOARD_DIR}/board_config.h")
    message(FATAL_ERROR "Unknown BOARD '${BOARD}'. Available boards: ${_available_boards}")
endif()

if(EXISTS "${BOARD_DIR}/board.cmake")
    include("${BOARD_DIR}/board.cmake")
endif()

set(CHIP_PORT "ch32v30x" CACHE STRING "Chip port directory under port/")
set(CHIP_PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/port/${CHIP_PORT}")
if(NOT EXISTS "${CHIP_PORT_DIR}")
    message(FATAL_ERROR "Unknown CHIP_PORT '${CHIP_PORT}'")
endif()

foreach(_submodule_file
        "${CMAKE_CURRENT_SOURCE_DIR}/blackmagic/src/gdb_main.c"
        "${CHERRYUSB_ROOT}/class/cdc/usbd_cdc_acm.c")
    if(NOT EXISTS "${_submodule_file}")
        message(FATAL_ERROR "Submodule missing (${_submodule_file}); run: git submodule update --init --recursive")
    endif()
endforeach()

message(STATUS "Board: ${BOARD}   Chip port: ${CHIP_PORT}")

include("cmake/wch_riscv.cmake")
include("${CHERRYUSB_ROOT}/cherryusb.cmake")
include("cmake/blackmagic.cmake")

# CherryUSB master has no usable CH32V30x USBHS port (port/wch/usbhs targets a
# different USBHS IP), so the CH32V30x controller comes from our CherryUSB fork
# (branch ch32v30x-usbhs), the same driver ch32_dfu_boot and ch32_hello_world
# use.

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
enable_language(C ASM)

project(${CMAKE_PROJECT_NAME})

set(PROJECT_TARGET ${CMAKE_PROJECT_NAME}.elf)
set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linkfile/flash_dfu.ld)

add_link_options(
    -T ${LINKER_SCRIPT}
    -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map
)

add_executable(${PROJECT_TARGET})

# ------------------------------------------------------------------------------
# WCH ch32v30x peripheral library + startup
# ------------------------------------------------------------------------------
add_library(ch32v30x_lib STATIC)

target_include_directories(ch32v30x_lib PUBLIC
    ${SDK_ROOT}/Core
    ${SDK_ROOT}/Debug
    ${SDK_ROOT}/Peripheral/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

file(GLOB SDK_CORE_SRC   ${SDK_ROOT}/Core/*.c)
file(GLOB SDK_DEBUG_SRC  ${SDK_ROOT}/Debug/*.c)
file(GLOB SDK_DEVICE_SRC ${SDK_ROOT}/Peripheral/src/*.c)

target_sources(ch32v30x_lib PRIVATE
    ${SDK_CORE_SRC}
    ${SDK_DEBUG_SRC}
    ${SDK_DEVICE_SRC}
    ${SDK_ROOT}/Startup/startup_ch32v30x_D8C.S
)

# ------------------------------------------------------------------------------
# Application: port layer + blackmagic core
# ------------------------------------------------------------------------------
target_sources(${PROJECT_TARGET} PRIVATE
    ${cherryusb_srcs}
    ${blackmagic_srcs}
    # CH32V30x USBHS device controller, from the CherryUSB fork
    ${CHERRYUSB_ROOT}/port/wch/ch32v30x/usb_dc_ch32v30x.c
    # port layer
    ${CHIP_PORT_DIR}/boot_trigger_${CHIP_PORT}.c
    ${CHIP_PORT_DIR}/boot_usb_${CHIP_PORT}.c
    ${CHIP_PORT_DIR}/platform.c
    ${CHIP_PORT_DIR}/timing_ch32.c
    ${CHIP_PORT_DIR}/swdptap.c
    ${CHIP_PORT_DIR}/jtagtap.c
    ${CHIP_PORT_DIR}/aux_serial.c
    ${CHIP_PORT_DIR}/cdc_acm_dual.c
    # board
    ${BOARD_DIR}/board.c
    # application glue
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/system_ch32v30x.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ch32v30x_it.c
)

target_include_directories(${PROJECT_TARGET} PRIVATE
    ${cherryusb_incs}
    ${CHERRYUSB_ROOT}/port/wch/ch32v30x
    ${blackmagic_incs}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/shared
    ${CMAKE_CURRENT_SOURCE_DIR}/port
    ${BOARD_DIR}
    ${CHIP_PORT_DIR}
)

# Black magic debug configuration: no libopencm3 (CherryUSB provides USB), no
# RTT, 4 byte pointers (rv32), accelerated RISC-V access and no interrupt
# preemption so the bit-banged SWD/JTAG timing stays deterministic.
target_compile_definitions(${PROJECT_TARGET} PRIVATE
    -DCONFIG_BMDA=0
    -DCONFIG_POINTER_SIZE=4
    -DNO_LIBOPENCM3
    -DUSB_HS
    -DADVERTISE_NOACKMODE=1
    -DCONFIG_RISCV=1
    -DCONFIG_RISCV_ACCEL=1
    -DGDB_PACKET_BUFFER_SIZE=2048
    -DDISABLE_IRQ_PREEMPTIVE=1
)

target_link_libraries(${PROJECT_TARGET}
    ch32v30x_lib
)

add_custom_command(TARGET ${PROJECT_TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -Oihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf> ${CMAKE_PROJECT_NAME}.bin
)
