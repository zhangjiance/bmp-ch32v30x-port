# ==============================================================================
# bmp_port/CMakeLists.txt
#
# The whole CH32V30x port: this file lists the port sources *and* the blackmagic
# core/target sources, and
# sets the application's include paths and compile definitions.  The top level
# CMakeLists.txt creates the executable (from main.c), calls
# add_subdirectory(bmp_port) and does the final link and objcopy, so this file
# relies on these variables being set there:
#   PROJECT_TARGET, BOARD_DIR, SDK_ROOT, CHERRYUSB_ROOT,
#   cherryusb_srcs, cherryusb_incs
#
# Layering:
#   bmp_port/       USB (CherryUSB + CH32V30x device controller), GPIO bit
#                   banged SWD/JTAG, target UART, timing, boot hand-shake,
#                   MCU glue (system/startup + interrupt vectors)
#   third_party_components/
#     blackmagic/     submodule: BMP core + target drivers
#     CherryUSB/      submodule: USB stack + CH32V30x USBHS device controller
#   boards/<board>/ board BSP: board_config.h (BOARD_* pins/features) + board.c
#                   (primitives); the probe's SWD/JTAG pins live in jtag_port.h
# ==============================================================================

set(PROJECT_ROOT   "${CMAKE_SOURCE_DIR}")
set(BMP_PORT_DIR   "${CMAKE_CURRENT_SOURCE_DIR}")
set(BMP_SUBMODULE  "${PROJECT_ROOT}/third_party_components/blackmagic")

if(BMP_SLIM)
    # --------------------------------------------------------------------------
    # Size-optimised build: work on a private copy of the blackmagic sources and
    # apply patches/blackmagic-slim.patch to it, so the submodule stays pristine
    # and can be updated at will.  The copy lives in the build directory and is
    # refreshed whenever the patch changes.
    # --------------------------------------------------------------------------
    set(BMP_ROOT "${CMAKE_BINARY_DIR}/blackmagic-slim")
    set(BMP_SLIM_PATCH "${PROJECT_ROOT}/patches/blackmagic-slim.patch")
    if(NOT EXISTS "${BMP_SLIM_PATCH}")
        message(FATAL_ERROR "BMP_SLIM=ON requires ${BMP_SLIM_PATCH}")
    endif()

    set(_bmp_stamp "${BMP_ROOT}/.patch-applied")
    if(NOT EXISTS "${_bmp_stamp}" OR "${BMP_SLIM_PATCH}" IS_NEWER_THAN "${_bmp_stamp}")
        message(STATUS "BMP_SLIM: refreshing patched copy in ${BMP_ROOT}")
        file(REMOVE_RECURSE "${BMP_ROOT}")
        file(COPY "${BMP_SUBMODULE}/src" DESTINATION "${BMP_ROOT}")
        execute_process(
            COMMAND patch -p1 -s -i "${BMP_SLIM_PATCH}"
            WORKING_DIRECTORY "${BMP_ROOT}"
            RESULT_VARIABLE _bmp_patch_result)
        if(NOT _bmp_patch_result EQUAL 0)
            message(FATAL_ERROR "BMP_SLIM: failed to apply ${BMP_SLIM_PATCH} (exit ${_bmp_patch_result})")
        endif()
        file(TOUCH "${_bmp_stamp}")
    endif()
else()
    set(BMP_ROOT "${BMP_SUBMODULE}")
endif()

set(BMP_CORE_ROOT  "${BMP_ROOT}/src")

# ------------------------------------------------------------------------------
# WCH ch32v30x peripheral library + startup
#
# Only built for the WCH backend; the ch32fun backend brings its own startup,
# clock tree and register headers.
# ------------------------------------------------------------------------------
if(NOT BMP_CH32FUN)
    add_library(ch32v30x_lib STATIC)

    target_include_directories(ch32v30x_lib PUBLIC
        ${SDK_ROOT}/Core
        ${SDK_ROOT}/Debug
        ${SDK_ROOT}/Peripheral/inc
        ${BMP_PORT_DIR}
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
endif()

# ------------------------------------------------------------------------------
# Port layer
#
# The CH32V30x USBHS device controller comes from our CherryUSB fork (branch
# ch32v30x-usbhs): CherryUSB master has no usable CH32V30x USBHS port, as
# port/wch/usbhs targets a different USBHS IP.
#
# SWD/JTAG are pure GPIO bit banging - no peripheral acceleration - so the
# timing is fully deterministic; only jtag_port.h is CH32V30x specific.
#
# The *_ch32fun.c files are the register-level counterparts of the files they
# replace; both backends expose the same board/platform interfaces.
# ------------------------------------------------------------------------------
if(BMP_CH32FUN)
    set(bmp_port_srcs
        # platform: LED, boot hand-shake, target clock output control
        ${BMP_PORT_DIR}/platform.c
        ${BMP_PORT_DIR}/timing_ch32.c
        # GPIO bit banged debug interfaces
        ${BMP_PORT_DIR}/jtag_port.c
        ${BMP_PORT_DIR}/jtagtap.c
        # USB: composite CDC ACM (GDB server + target UART) and the target UART
        ${BMP_PORT_DIR}/cdc_acm_dual.c
        ${BMP_PORT_DIR}/aux_serial_ch32fun.c
        ${CHERRYUSB_ROOT}/port/wch/ch32v30x/usb_dc_ch32v30x.c
        # bootloader integration: BKP hand-shake and reset into the bootloader
        ${BMP_PORT_DIR}/boot_trigger_ch32v30xfun.c
        ${BMP_PORT_DIR}/boot_usb_ch32v30xfun.c
        # exception handlers
        ${BMP_PORT_DIR}/ch32v30x_it_ch32fun.c
        # startup, clock tree, vector table and core intrinsics
        ${CH32FUN_ROOT}/ch32fun/ch32fun.c
    )

    # ch32fun ships its own small printf family.  blackmagic's format strings
    # need the newlib formatter (%zu, %llx, %.*s, %-25s ...), so rename the
    # ch32fun ones out of the way and let the library symbols win.
    set_source_files_properties(${CH32FUN_ROOT}/ch32fun/ch32fun.c PROPERTIES
        COMPILE_OPTIONS "-Dprintf=ch32fun_printf;-Dvprintf=ch32fun_vprintf;-Dsnprintf=ch32fun_snprintf;-Dsprintf=ch32fun_sprintf")
else()
    set(bmp_port_srcs
        # platform: LED, boot hand-shake, target clock output control
        ${BMP_PORT_DIR}/platform.c
        ${BMP_PORT_DIR}/timing_ch32.c
        # GPIO bit banged debug interfaces
        ${BMP_PORT_DIR}/jtag_port.c
        ${BMP_PORT_DIR}/jtagtap.c
        # USB: composite CDC ACM (GDB server + target UART) and the target UART
        ${BMP_PORT_DIR}/cdc_acm_dual.c
        ${BMP_PORT_DIR}/aux_serial.c
        ${CHERRYUSB_ROOT}/port/wch/ch32v30x/usb_dc_ch32v30x.c
        # bootloader integration: BKP hand-shake and reset into the bootloader
        ${BMP_PORT_DIR}/boot_trigger_ch32v30x.c
        ${BMP_PORT_DIR}/boot_usb_ch32v30x.c
        # MCU glue: WCH system (clocks) and the interrupt vector implementations
        ${BMP_PORT_DIR}/system_ch32v30x.c
        ${BMP_PORT_DIR}/ch32v30x_it.c
    )
endif()

if(NOT BMP_SLIM)
    # SWD bit banging is only part of the full build; the slim build keeps JTAG
    # (which is what RISC-V/HPM targets use) and drops the ADIv5/SWD stack.
    list(APPEND bmp_port_srcs ${BMP_PORT_DIR}/swdptap.c)
endif()

# ------------------------------------------------------------------------------
# blackmagic (Black Magic Debug) core and target drivers
#
# The core and the target drivers come from the blackmagic submodule;
# everything the probe itself needs comes from bmp_port/.
#
# Two configurations:
#
#   BMP_SLIM=OFF (default)  full build: ARM (Cortex-M/A/R), RISC-V, CH32/WCH and
#                           HPM targets, unmodified blackmagic submodule.
#   BMP_SLIM=ON             size-optimised on-board probe for RISC-V (RV32) HPM
#                           parts only: no ARM, no SWD/ADIv5/ADIv6, no SPI
#                           flash scan, no semihosting, and the BMP remote
#                           protocol keeps only its JTAG/general/high-level/
#                           RISC-V sub-protocols.  The submodule changes this
#                           needs live in patches/blackmagic-slim.patch and are
#                           applied to a private copy (see the top of this
#                           file), so the submodule itself is never modified.
#
# Deliberately NOT built in either configuration:
#   src/rtt.c                     this port has no RTT support
#   src/platforms/**              vendored platform ports (bmp-v2, stlink, ...)
#   src/target/swdptap_generic.c  bmp_port/swdptap.c provides SWD
#
# The slim build relies on the weak no-op stubs in src/target/target_probe.c for
# the probes it leaves out; the full build simply builds every driver.
# ------------------------------------------------------------------------------
set(blackmagic_incs
    ${BMP_CORE_ROOT}
    ${BMP_CORE_ROOT}/include
    ${BMP_CORE_ROOT}/target
)

# Core, generic target infrastructure and HPM support are shared.
set(blackmagic_srcs
    # ---- core ---------------------------------------------------------------
    ${BMP_CORE_ROOT}/command.c
    ${BMP_CORE_ROOT}/crc32.c
    ${BMP_CORE_ROOT}/exception.c
    ${BMP_CORE_ROOT}/gdb_main.c
    ${BMP_CORE_ROOT}/gdb_packet.c
    ${BMP_CORE_ROOT}/hex_utils.c
    ${BMP_CORE_ROOT}/maths_utils.c
    ${BMP_CORE_ROOT}/morse.c
    ${BMP_CORE_ROOT}/remote.c
    ${BMP_CORE_ROOT}/timing.c
    # ---- target infrastructure ---------------------------------------------
    ${BMP_CORE_ROOT}/target/gdb_reg.c
    ${BMP_CORE_ROOT}/target/jtag_devs.c
    ${BMP_CORE_ROOT}/target/jtag_scan.c
    ${BMP_CORE_ROOT}/target/jtagtap_generic.c
    ${BMP_CORE_ROOT}/target/target.c
    ${BMP_CORE_ROOT}/target/target_flash.c
    ${BMP_CORE_ROOT}/target/target_probe.c
    # ---- HPM targets -------------------------------------------------------
    ${BMP_CORE_ROOT}/target/hpm_xpi.c
)

if(BMP_SLIM)
    list(APPEND blackmagic_srcs
        # ---- RISC-V targets (JTAG DTM only) --------------------------------
        ${BMP_CORE_ROOT}/target/riscv_debug.c
        ${BMP_CORE_ROOT}/target/riscv_jtag_dtm.c
        ${BMP_CORE_ROOT}/target/riscv32.c
    )
else()
    list(APPEND blackmagic_srcs
        # ---- generic debug access / discovery ------------------------------
        ${BMP_CORE_ROOT}/target/adi.c
        ${BMP_CORE_ROOT}/target/adiv5.c
        ${BMP_CORE_ROOT}/target/adiv5_jtag.c
        ${BMP_CORE_ROOT}/target/adiv5_swd.c
        ${BMP_CORE_ROOT}/target/adiv6.c
        ${BMP_CORE_ROOT}/target/arm_coresight_cti.c
        ${BMP_CORE_ROOT}/target/cortex.c
        ${BMP_CORE_ROOT}/target/cortexar.c
        ${BMP_CORE_ROOT}/target/cortexm.c
        ${BMP_CORE_ROOT}/target/icepick.c
        ${BMP_CORE_ROOT}/target/onboard_flash.c
        ${BMP_CORE_ROOT}/target/semihosting.c
        ${BMP_CORE_ROOT}/target/sfdp.c
        ${BMP_CORE_ROOT}/target/spi.c
        # ---- RISC-V targets ------------------------------------------------
        ${BMP_CORE_ROOT}/target/riscv_adi_dtm.c
        ${BMP_CORE_ROOT}/target/riscv_debug.c
        ${BMP_CORE_ROOT}/target/riscv_gateway_dtm.c
        ${BMP_CORE_ROOT}/target/riscv_jtag_dtm.c
        ${BMP_CORE_ROOT}/target/riscv32.c
        ${BMP_CORE_ROOT}/target/riscv64.c
        # ---- WCH / CH32 targets --------------------------------------------
        ${BMP_CORE_ROOT}/target/ch32vx.c
        ${BMP_CORE_ROOT}/target/ch579.c
    )
endif()

# ------------------------------------------------------------------------------
# Application
# ------------------------------------------------------------------------------
if(BMP_CH32FUN)
    set(board_srcs ${BOARD_DIR}/board_ch32fun.c)
else()
    set(board_srcs ${BOARD_DIR}/board.c)
endif()

target_sources(${PROJECT_TARGET} PRIVATE
    ${cherryusb_srcs}
    ${bmp_port_srcs}
    ${blackmagic_srcs}
    # board BSP
    ${board_srcs}
)

target_include_directories(${PROJECT_TARGET} PRIVATE
    ${cherryusb_incs}
    ${CHERRYUSB_ROOT}/port/wch/ch32v30x
    ${blackmagic_incs}
    ${BMP_PORT_DIR}
    ${PROJECT_ROOT}/shared
    ${BOARD_DIR}
)

if(BMP_CH32FUN)
    target_include_directories(${PROJECT_TARGET} PRIVATE ${CH32FUN_ROOT}/ch32fun)
endif()

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
    # GDB packet size: also the PacketSize announced to GDB, and what bounds a
    # single 'm' read (GDB_PACKET_BUFFER_SIZE / 2 in gdb_main.c).
    #
    # 496 is the IN-side workaround: a whole reply is at most 496 + 4 = 500
    # bytes, so it fits in one 512 byte USB packet and gdb_in_send() never has
    # to continue a transfer across interrupts (README, "long GDB replies
    # stalled").  A larger value chunks replies over several packets and makes
    # every one of them depend on that continuation path.
    -DGDB_PACKET_BUFFER_SIZE=496
    -DDISABLE_IRQ_PREEMPTIVE=1
)

if(BMP_CH32FUN)
    target_compile_definitions(${PROJECT_TARGET} PRIVATE -DBMP_USE_CH32FUN=1)
else()
    target_link_libraries(${PROJECT_TARGET} ch32v30x_lib)
endif()
