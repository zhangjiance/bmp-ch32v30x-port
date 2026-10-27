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
#   boards/<board>/        board BSP: board_config.h (BOARD_* pins/features) +
#                          board.c (primitives); the probe's SWD/JTAG pins live
#                          in bmp_port/jtag_port.h
#   shared/                boot_protocol.h, the bootloader <-> app contract
#   linkfile/              application linker script (0x00008000, 96 KB)
#   third_party_components/blackmagic/  submodule: BMP core + target drivers
#   third_party_components/ch32fun/     submodule: startup, clock tree, vectors
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

# ------------------------------------------------------------------------------
# Peripheral backend: ch32fun
#
# ch32fun supplies the startup, the clock tree, the vector table and the
# register headers; there is no vendor peripheral library in this project.
#
# ch32fun's ch32fun.h defines its own jmp_buf, which collides with newlib's when
# both are visible in one translation unit (blackmagic's exception code pulls in
# <setjmp.h>).  patches/ch32fun.patch guards that definition with
# FUNCONF_NO_JMP_BUF and is applied to a private copy of the sources, so the
# submodule itself stays pristine - same idea as the blackmagic slim patch.
# ------------------------------------------------------------------------------
set(_ch32fun_submodule "${CMAKE_CURRENT_SOURCE_DIR}/third_party_components/ch32fun")
set(_ch32fun_patch "${CMAKE_CURRENT_SOURCE_DIR}/patches/ch32fun.patch")
set(CH32FUN_ROOT "${CMAKE_BINARY_DIR}/ch32fun-patched")
set(_ch32fun_stamp "${CH32FUN_ROOT}/.patch-applied")

if(NOT EXISTS "${_ch32fun_patch}")
    message(FATAL_ERROR "Missing ${_ch32fun_patch}")
endif()

if(NOT EXISTS "${_ch32fun_stamp}" OR "${_ch32fun_patch}" IS_NEWER_THAN "${_ch32fun_stamp}")
    message(STATUS "ch32fun: refreshing patched copy in ${CH32FUN_ROOT}")
    file(REMOVE_RECURSE "${CH32FUN_ROOT}")
    file(COPY "${_ch32fun_submodule}/ch32fun" DESTINATION "${CH32FUN_ROOT}")
    execute_process(
        COMMAND patch -p1 -s -i "${_ch32fun_patch}"
        WORKING_DIRECTORY "${CH32FUN_ROOT}"
        RESULT_VARIABLE _ch32fun_patch_result)
    if(NOT _ch32fun_patch_result EQUAL 0)
        message(FATAL_ERROR "ch32fun: failed to apply ${_ch32fun_patch} (exit ${_ch32fun_patch_result})")
    endif()
    file(TOUCH "${_ch32fun_stamp}")
endif()

# ------------------------------------------------------------------------------
# Board selection
# ------------------------------------------------------------------------------
set(BOARD "ch32v30x_ob" CACHE STRING "Target board directory under boards/")
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
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party_components/blackmagic/src/gdb_main.c"
        "${CHERRYUSB_ROOT}/class/cdc/usbd_cdc_acm.c")
    if(NOT EXISTS "${_submodule_file}")
        message(FATAL_ERROR "Submodule missing (${_submodule_file}); run: git submodule update --init --recursive")
    endif()
endforeach()

if(NOT EXISTS "${CH32FUN_ROOT}/ch32fun/ch32fun.c")
    message(FATAL_ERROR "ch32fun not found at ${CH32FUN_ROOT}; run: git submodule update --init --recursive")
endif()

message(STATUS "Board: ${BOARD}")

# ------------------------------------------------------------------------------
# Build flavour
#
# BMP_SLIM=ON is the size-optimised probe image (RISC-V RV32/HPM targets only,
# no ARM/SWD/ADIv5/ADIv6/SPI/semihosting).  The handful of blackmagic submodule
# changes it needs are kept in patches/blackmagic-slim.patch and applied to a
# private copy of the sources under the build directory, so the submodule stays
# untouched.  See bmp_port/CMakeLists.txt.
#
# BMP_SLIM=OFF builds the full multi-architecture probe from the unmodified
# submodule, exactly as before.
# ------------------------------------------------------------------------------
option(BMP_SLIM "Build the size-optimised RISC-V/HPM-only probe" OFF)
message(STATUS "BMP slim build: ${BMP_SLIM}")

include("cmake/ch32fun_riscv.cmake")
include("${CHERRYUSB_ROOT}/cherryusb.cmake")

# CherryUSB master has no usable CH32V30x USBHS port (port/wch/usbhs targets a
# different USBHS IP), so the CH32V30x controller comes from our CherryUSB fork
# (branch ch32v30x-usbhs).

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
enable_language(C ASM)

project(${CMAKE_PROJECT_NAME})

set(PROJECT_TARGET ${CMAKE_PROJECT_NAME}.elf)
# ch32fun's vector table lives in .init and the entry point is InterruptVector,
# so this script has no .vector output section.
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
