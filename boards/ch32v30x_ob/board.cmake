# ==============================================================================
# boards/ch32v30x_ob/board.cmake
#
# Board-specific build definitions.  Included by the top level CMakeLists.txt
# when BOARD selects this directory.
# ==============================================================================

# ------------------------------------------------------------------------------
# Crystal and clock tree
#
# ch32fun derives the PLL setup, every delay, every baud rate and the USB PHY
# reference from these three numbers, and they describe the hardware fitted on
# the board - which is why they live here and not in funconfig.h (that file
# keeps the ch32fun behaviour switches and repeats these values only as
# fallbacks).
#
#     HSE 24 MHz  ->  PLL x6  ->  SYSCLK = HCLK = 144 MHz, APB1 = HCLK/2 = 72 MHz
#
# A board with a different crystal changes HSE_VALUE here, and the two derived
# numbers with it.  Sanity check after such a change: 115200 baud on
# PCLK1 = 72 MHz is BRR 0x271, and board_time_ms() must count real
# milliseconds.
# ------------------------------------------------------------------------------
add_compile_definitions(
    HSE_VALUE=24000000
    FUNCONF_PLL_MULTIPLIER=6
    FUNCONF_SYSTEM_CORE_CLOCK=144000000
)
