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

set(CMAKE_C_FLAGS_DEBUG "-Os -g3")

if(BMP_SLIM)
    # The slim image is size constrained (96 KB application partition) and is
    # used as a BMP with RISC-V/HPM targets only; -Oz + LTO is worth ~6.5 KB.
    #
    # LTO used to be unsafe because usb_dc_low_level_init() was a __WEAK
    # definition inside the CherryUSB driver that main.c overrode with a strong
    # one: LTO inlined the weak body into usb_dc_init() and the override was
    # silently dropped.  The CH32V30x driver now only *declares* the hook
    # (extern) and bmp_port/boot_usb_ch32v30x.c provides the single strong
    # definition, so there is no weak/strong pair left for LTO to mishandle.
    set(CMAKE_C_FLAGS_RELEASE "-Oz -flto -g0")
else()
    # Full build: keep the configuration this port was validated with.
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

if(BMP_SLIM)
    add_link_options(-flto)
endif()