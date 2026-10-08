# ==============================================================================
# blackmagic (Black Magic Debug) sources for the CH32V30x probe port
#
# Structure follows bmp-hpm-port: the core and the target drivers come from the
# blackmagic submodule, everything the probe itself needs (USB, SWD/JTAG bit
# banging, target UART, timing, board) comes from port/ch32v30x.
#
# Deliberately NOT built:
#   src/rtt.c                     this port has no RTT support
#   src/platforms/**              vendored platform ports (bmp-v2, stlink, ...)
#   src/target/hpm_xpi.c          (kept) flash stub for HPM targets
#   src/target/*_generic.c        jtagtap_generic IS used, swdptap_generic is not
#                                 (port/ch32v30x/swdptap.c provides SWD)
#
# The target list is trimmed to the families that matter here (Cortex-M/A/R,
# RISC-V, CH32/WCH); enabling more of them is only a matter of adding files, but
# the application partition is 96 KB.
# ==============================================================================

set(BLACKMAGIC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/blackmagic")
set(BMP_CORE_ROOT   "${BLACKMAGIC_ROOT}/src")

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
    # application partition is 96 KB and this build is already at ~93%.  Add
    # both back if CH32F1 support is needed.
    ${BMP_CORE_ROOT}/target/ch32vx.c
    ${BMP_CORE_ROOT}/target/ch579.c
    # ---- HPM targets (the user's other workspace) --------------------------
    ${BMP_CORE_ROOT}/target/hpm_xpi.c
)
