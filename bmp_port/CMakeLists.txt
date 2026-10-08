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
#   boards/<board>/ board BSP (pins live in bmp_port/jtag_port.h)
# ==============================================================================

set(PROJECT_ROOT   "${CMAKE_SOURCE_DIR}")
set(BMP_PORT_DIR   "${CMAKE_CURRENT_SOURCE_DIR}")
set(BMP_ROOT       "${PROJECT_ROOT}/third_party_components/blackmagic")
set(BMP_CORE_ROOT  "${BMP_ROOT}/src")

# ------------------------------------------------------------------------------
# WCH ch32v30x peripheral library + startup
# ------------------------------------------------------------------------------
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

# ------------------------------------------------------------------------------
# Port layer
#
# The CH32V30x USBHS device controller comes from our CherryUSB fork (branch
# ch32v30x-usbhs): CherryUSB master has no usable CH32V30x USBHS port, as
# port/wch/usbhs targets a different USBHS IP.
#
# SWD/JTAG are pure GPIO bit banging - no peripheral acceleration - so the
# timing is fully deterministic; only jtag_port.h is CH32V30x specific.
# ------------------------------------------------------------------------------
set(bmp_port_srcs
    # platform: LED, boot hand-shake, target clock output control
    ${BMP_PORT_DIR}/platform.c
    ${BMP_PORT_DIR}/timing_ch32.c
    # GPIO bit banged debug interfaces
    ${BMP_PORT_DIR}/swdptap.c
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

# ------------------------------------------------------------------------------
# blackmagic (Black Magic Debug) core and target drivers
#
# The core and the target drivers come from the blackmagic submodule;
# everything the probe itself needs comes from bmp_port/.
#
# Deliberately NOT built:
#   src/rtt.c                     this port has no RTT support
#   src/platforms/**              vendored platform ports (bmp-v2, stlink, ...)
#   src/target/*_generic.c        jtagtap_generic IS used, swdptap_generic is
#                                 not (bmp_port/swdptap.c provides SWD)
#
# The target list is trimmed to the families that matter here (Cortex-M/A/R,
# RISC-V, CH32/WCH); enabling more of them is only a matter of adding files, but
# the application partition is 96 KB.
# ------------------------------------------------------------------------------
set(blackmagic_incs
    ${BMP_CORE_ROOT}
    ${BMP_CORE_ROOT}/include
    ${BMP_CORE_ROOT}/target
)

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
    # ---- generic debug access / discovery ----------------------------------
    ${BMP_CORE_ROOT}/target/adi.c
    ${BMP_CORE_ROOT}/target/adiv5.c
    ${BMP_CORE_ROOT}/target/adiv5_jtag.c
    ${BMP_CORE_ROOT}/target/adiv5_swd.c
    ${BMP_CORE_ROOT}/target/adiv6.c
    ${BMP_CORE_ROOT}/target/arm_coresight_cti.c
    ${BMP_CORE_ROOT}/target/cortex.c
    ${BMP_CORE_ROOT}/target/cortexar.c
    ${BMP_CORE_ROOT}/target/cortexm.c
    ${BMP_CORE_ROOT}/target/gdb_reg.c
    ${BMP_CORE_ROOT}/target/icepick.c
    ${BMP_CORE_ROOT}/target/jtag_devs.c
    ${BMP_CORE_ROOT}/target/jtag_scan.c
    ${BMP_CORE_ROOT}/target/jtagtap_generic.c
    ${BMP_CORE_ROOT}/target/onboard_flash.c
    ${BMP_CORE_ROOT}/target/semihosting.c
    ${BMP_CORE_ROOT}/target/sfdp.c
    ${BMP_CORE_ROOT}/target/spi.c
    ${BMP_CORE_ROOT}/target/target.c
    ${BMP_CORE_ROOT}/target/target_flash.c
    ${BMP_CORE_ROOT}/target/target_probe.c
    # ---- RISC-V targets ----------------------------------------------------
    ${BMP_CORE_ROOT}/target/riscv_adi_dtm.c
    ${BMP_CORE_ROOT}/target/riscv_debug.c
    ${BMP_CORE_ROOT}/target/riscv_gateway_dtm.c
    ${BMP_CORE_ROOT}/target/riscv_jtag_dtm.c
    ${BMP_CORE_ROOT}/target/riscv32.c
    ${BMP_CORE_ROOT}/target/riscv64.c
    # ---- WCH / CH32 targets ------------------------------------------------
    # ch32f1.c (Cortex-M3 CH32F1) is left out together with stm32f1.c: the
    # application partition is 96 KB and this build is already at ~92%.  Add
    # both back if CH32F1 support is needed.
    ${BMP_CORE_ROOT}/target/ch32vx.c
    ${BMP_CORE_ROOT}/target/ch579.c
    # ---- HPM targets (the user's other workspace) --------------------------
    ${BMP_CORE_ROOT}/target/hpm_xpi.c
)

# ------------------------------------------------------------------------------
# Application
# ------------------------------------------------------------------------------
target_sources(${PROJECT_TARGET} PRIVATE
    ${cherryusb_srcs}
    ${bmp_port_srcs}
    ${blackmagic_srcs}
    # board BSP
    ${BOARD_DIR}/board.c
)

target_include_directories(${PROJECT_TARGET} PRIVATE
    ${cherryusb_incs}
    ${CHERRYUSB_ROOT}/port/wch/ch32v30x
    ${blackmagic_incs}
    ${BMP_PORT_DIR}
    ${PROJECT_ROOT}/shared
    ${BOARD_DIR}
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
    # GDB packet size: also the PacketSize announced to GDB, and what bounds a
    # single 'm' read (GDB_PACKET_BUFFER_SIZE / 2 in gdb_main.c).
    -DGDB_PACKET_BUFFER_SIZE=2048
    -DDISABLE_IRQ_PREEMPTIVE=1
)

target_link_libraries(${PROJECT_TARGET}
    ch32v30x_lib
)
