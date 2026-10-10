# ==============================================================================
# boards/ch32v30x_ob/board.cmake
#
# Board-specific build definitions.  Included by the top level CMakeLists.txt
# when BOARD selects this directory.
# ==============================================================================

# ------------------------------------------------------------------------------
# HSE crystal
#
# The WCH library defaults HSE_VALUE to 8 MHz, and every frequency this firmware
# computes is derived from that single number: SYSCLK, HCLK and PCLK1 in
# RCC_GetClocksFreq() / SystemCoreClockUpdate(), and from those the USART baud
# rate divisor, Delay_Init(), board_time_ms() and platform_delay().
#
# This board does not carry the 8 MHz the library assumes.  Its system clock
# setup (SetSysClockTo48_HSE(), the project's system_ch32v30x.c) runs the main
# PLL at HSE * 6, so with this 24 MHz crystal the real SYSCLK is 3x what those
# functions report.  That is exactly the 3x the target UART was out by: a host
# request for 115200 left BRR at 0x0D0, whose real rate was 72000000 / 208 =
# 346153 baud rather than the 115384 the firmware printed, so PB11 decoded a
# genuine 115200 target stream three samples per bit.
#
# Only products of HSE with the decoded PLL multiplier and the prescalers are
# ever used, so correcting the assumed HSE is enough to make every derived
# number right; the clock tree itself is untouched.
#
# Confirm this against the crystal fitted on the board: with a real 24 MHz HSE
# the divisor for 115200 becomes 0x271 and the aux port reports
# "actual=115200 pclk1=72000000".
# ------------------------------------------------------------------------------
add_compile_definitions(HSE_VALUE=24000000)
