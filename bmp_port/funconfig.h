/*
 * funconfig.h
 *
 * ch32fun configuration for the CH32V30x BMP port.
 *
 * The clock tree is a board property: boards/<board>/board.cmake passes
 * HSE_VALUE, FUNCONF_PLL_MULTIPLIER and FUNCONF_SYSTEM_CORE_CLOCK with -D.  The
 * fallbacks below keep a build without a board.cmake on the same tree this
 * board uses:
 *
 *     HSE 24 MHz  ->  PLL x6  ->  SYSCLK = HCLK = 144 MHz, APB1 = HCLK/2 = 72 MHz
 *
 * Pins and polarity stay in boards/<board>/board_config.h.
 */
#ifndef _FUNCONFIG_H
#define _FUNCONFIG_H

#define CH32V30x 1

/* ------------------------------------------------------------------ *
 * Clock tree
 * ------------------------------------------------------------------ */
#ifndef FUNCONF_USE_HSI
#define FUNCONF_USE_HSI 0
#endif
#ifndef FUNCONF_USE_HSE
#define FUNCONF_USE_HSE 1
#endif
#ifndef FUNCONF_USE_PLL
#define FUNCONF_USE_PLL 1
#endif
#ifndef FUNCONF_PLL_MULTIPLIER
#define FUNCONF_PLL_MULTIPLIER 6
#endif
#ifndef FUNCONF_SYSTEM_CORE_CLOCK
#define FUNCONF_SYSTEM_CORE_CLOCK 144000000
#endif
#ifndef HSE_VALUE
#define HSE_VALUE 24000000
#endif

/* ------------------------------------------------------------------ *
 * Core / interrupts
 *
 * -march=rv32imacxw enables the WCH fast interrupt entry (hardware prologue
 * and epilogue); every handler in this port uses INTERRUPT_DECORATOR.
 * ------------------------------------------------------------------ */
#define FUNCONF_ENABLE_HPE 1

/* SysTick counts HCLK cycles; it is the board time base (board_time_ms()). */
#define FUNCONF_SYSTICK_USE_HCLK 1

/* ------------------------------------------------------------------ *
 * printf transport: none.
 *
 * Diagnostics go out over USB (the GDB console channel), and the format
 * strings in blackmagic need the newlib formatter (%zu, %llx, %.*s,
 * %-25s ...).  ch32fun's mini formatter therefore stays out of the picture:
 * its printf family is renamed by cmake/ch32fun_riscv.cmake so the newlib
 * symbols win.
 * ------------------------------------------------------------------ */
#define FUNCONF_USE_DEBUGPRINTF 0
#define FUNCONF_USE_UARTPRINTF 0
#define FUNCONF_USE_USBPRINTF 0

/* ------------------------------------------------------------------ *
 * Core library coexistence
 *
 * blackmagic's exception handling uses newlib's setjmp/longjmp, and newlib's
 * jmp_buf is larger than the one ch32fun defines.  patches/ch32fun.patch makes
 * that definition conditional so the newlib one is used everywhere.
 * ------------------------------------------------------------------ */
#define FUNCONF_NO_JMP_BUF 1
#include <setjmp.h>

/* ------------------------------------------------------------------ *
 * Size knobs
 * ------------------------------------------------------------------ */
#define FUNCONF_DEBUG_HARDFAULT 0
#define FUNCONF_SUPPORT_CONSTRUCTORS 0
#define FUNCONF_ICACHE_EN 0

#endif /* _FUNCONFIG_H */
