set(CMAKE_SYSTEM_NAME               Generic)

set(CMAKE_C_COMPILER_FORCED TRUE)
set(CMAKE_CXX_COMPILER_FORCED TRUE)
set(CMAKE_C_COMPILER_ID GNU)
set(CMAKE_CXX_COMPILER_ID GNU)

# Some default GCC settings
# riscv-none-elf- must be part of path environment
set(TOOLCHAIN_PREFIX                riscv-wch-elf-)

set(CMAKE_C_COMPILER                ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER              ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER              ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_AR                        ${TOOLCHAIN_PREFIX}ar)
set(CMAKE_OBJCOPY                   ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_SIZE                      ${TOOLCHAIN_PREFIX}size)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -Os, matching the configuration this bootloader was originally validated
# with. -Oz and -fno-unwind-tables were tried for size but are backed out here
# while the USB bring-up problem is being isolated.
set(CMAKE_C_FLAGS_DEBUG "-Os -g3")
set(CMAKE_C_FLAGS_RELEASE "-Os -g0")

# MCU specific flags
#
# NOTE: no -flto here, unlike ch32v305_bmp.
# usb_dc_low_level_init() is a __WEAK function in the CherryUSB ch32hs driver
# that main.c overrides with a strong definition. With LTO the weak definition
# gets inlined into usb_dc_init() before the linker ever sees the two, so the
# override is silently dropped: USB clocks and the NVIC line stay unconfigured
# and the device never enumerates. The bootloader is small enough that -Oz
# alone keeps it well inside its 28 KB partition.
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

add_link_options(
        -march=rv32imacxw -mabi=ilp32 
        -msmall-data-limit=8 -msave-restore
        -nostartfiles
        -Xlinker
        --gc-sections
        -lm
        -fno-ident
        -Wl,--print-memory-usage
        # -Wl,-Map,${PROJECT_BINARY_DIR}/${PROJECT_NAME}.map
        # -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.map
        --specs=nano.specs --specs=nosys.specs
)