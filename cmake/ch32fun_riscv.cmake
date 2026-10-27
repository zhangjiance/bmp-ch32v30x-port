# ==============================================================================
# Toolchain and flags for the CH32V30x BMP port (ch32fun backend)
#
# No vendor startup file and no vendor library: ch32fun.c brings handle_reset()
# and the vector table (in .init), which is why the entry point is
# InterruptVector rather than a _start stub.
# ==============================================================================
set(CMAKE_SYSTEM_NAME               Generic)

set(CMAKE_C_COMPILER_FORCED TRUE)
set(CMAKE_CXX_COMPILER_FORCED TRUE)
set(CMAKE_C_COMPILER_ID GNU)
set(CMAKE_CXX_COMPILER_ID GNU)

# riscv-wch-elf- must be part of path environment
set(TOOLCHAIN_PREFIX                riscv-wch-elf-)

set(CMAKE_C_COMPILER                ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER              ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER              ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_AR                        ${TOOLCHAIN_PREFIX}ar)
set(CMAKE_OBJCOPY                   ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_SIZE                      ${TOOLCHAIN_PREFIX}size)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS_DEBUG "-Os -g3")

if(BMP_SLIM)
    # The slim image is size constrained (96 KB application partition); -Oz +
    # LTO is worth ~6.5 KB.  LTO is safe here: usb_dc_low_level_init() is only
    # *declared* by the CherryUSB driver and defined once, strongly, in
    # bmp_port/boot_usb_ch32v30xfun.c.
    set(CMAKE_C_FLAGS_RELEASE "-Oz -flto=auto -g0")
else()
    set(CMAKE_C_FLAGS_RELEASE "-Os -g0")
endif()

add_compile_options(
        -fmessage-length=0
        -fsigned-char
        -ffunction-sections
        -fdata-sections
        -fno-common
        -fno-ident
        -Wunused
        -Wuninitialized
)

# -march=rv32imacxw: the "xw" extension is what makes the WCH fast interrupt
# entry (INTERRUPT_DECORATOR with FUNCONF_ENABLE_HPE) and the hardware prologue
# painless, and every handler in this port uses that form.
add_compile_options(-march=rv32imacxw -mabi=ilp32)

add_link_options(
        -march=rv32imacxw -mabi=ilp32
        -msmall-data-limit=8 -msave-restore
        -nostartfiles
        -Xlinker
        --gc-sections
        -lm
        -fno-ident
        -Wl,--print-memory-usage
        --specs=nano.specs --specs=nosys.specs
)

if(BMP_SLIM)
    add_link_options(-flto=auto)
endif()
