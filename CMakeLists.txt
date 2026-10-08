cmake_minimum_required(VERSION 3.22)

# ==============================================================================
# bmp-ch32v30x-port - Black Magic Probe port for CH32V30x
#
# Application image for the DFU bootloader:
#   * links at 0x00008000, the application partition (32 KB bootloader)
#   * DFU runtime interface: "dfu-util -e" (and "monitor bootloader") resets
#     into the bootloader through the BKP hand-shake
#
# Layout: one bmp_port/ directory holds the whole port including its source list,
# main.c sits at the top level and the board BSP lives in boards/<board>/.
#
#   CMakeLists.txt         this file: toolchain, board selection, link, objcopy
#   main.c                 application entry point
#   bmp_port/              port layer + bmp_port/CMakeLists.txt, which lists the
#                          port sources and the blackmagic core/target sources
#   boards/<board>/        board BSP (pins live in bmp_port/jtag_port.h)
#   shared/                boot_protocol.h, the bootloader <-> app contract
#   linkfile/              application linker script (0x00008000, 96 KB)
#   SDK/                   WCH ch32v30x peripheral library
#   blackmagic/            submodule: BMP core + target drivers
#   third_party_components/CherryUSB/   submodule: USB stack + CH32V30x USBHS
# ==============================================================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# The CH32V305/307 USBHS block runs in high speed.
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
# Board selection
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

foreach(_submodule_file
        "${CMAKE_CURRENT_SOURCE_DIR}/blackmagic/src/gdb_main.c"
        "${CHERRYUSB_ROOT}/class/cdc/usbd_cdc_acm.c")
    if(NOT EXISTS "${_submodule_file}")
        message(FATAL_ERROR "Submodule missing (${_submodule_file}); run: git submodule update --init --recursive")
    endif()
endforeach()

message(STATUS "Board: ${BOARD}")

include("cmake/wch_riscv.cmake")
include("${CHERRYUSB_ROOT}/cherryusb.cmake")

# CherryUSB master has no usable CH32V30x USBHS port (port/wch/usbhs targets a
# different USBHS IP), so the CH32V30x controller comes from our CherryUSB fork
# (branch ch32v30x-usbhs).

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
enable_language(C ASM)

project(${CMAKE_PROJECT_NAME})

set(PROJECT_TARGET ${CMAKE_PROJECT_NAME}.elf)
set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linkfile/flash_dfu.ld)

add_link_options(
    -T ${LINKER_SCRIPT}
    -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map
)

add_executable(${PROJECT_TARGET} main.c)

# Everything else - port sources, blackmagic core and target drivers, the WCH
# peripheral library, the board BSP, include paths and compile definitions -
# is contributed by bmp_port/CMakeLists.txt.
add_subdirectory(bmp_port)

add_custom_command(TARGET ${PROJECT_TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -Oihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf> ${CMAKE_PROJECT_NAME}.bin
)
