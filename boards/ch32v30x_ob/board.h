/*
 * board.h - board primitives.
 *
 * The board layer exposes "primitives" only: bring-up, a millisecond time base,
 * status LED and BOOT button access, and one registerable periodic tick.  The
 * policy built on top of them (what the LED shows, what a held BOOT button
 * does) lives on the application side.
 *
 * These primitives deliberately know nothing about the application: pins and
 * polarity are BOARD_* macros in board_config.h, and the tick callback is opaque
 * here (board.c owns the ISR and only calls whatever was registered).
 *
 * A board directory (board_config.h + board.h + board.c) is self-contained, so
 * it can be copied under boards/ and selected with -DBOARD=<name>.
 */
#ifndef _BOARD_H
#define _BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* System clock / time base / LED / BOOT pin bring-up. */
void board_init(void);

/* Free running millisecond time base, derived from SysTick.  The board owns
 * SysTick, so the WCH SDK Delay_Ms()/Delay_Us() (which reconfigure it) must not
 * be used: call board_delay_ms() instead. */
uint32_t board_time_ms(void);
void board_delay_ms(uint32_t ms);

/* Status LED primitive: state != 0 means "LED on".  No-op without an LED. */
void board_led_write(uint8_t state);
void board_led_toggle(void);

/* BOOT button primitive: true while pressed.  Always false without a button. */
bool board_read_boot_pin(void);

/* Application UART hardware bring-up: peripheral/GPIO clocks and pins of the
 * instance the BOARD_APP_UART* macros name (BOARD_APP_UART_BAUDRATE is its
 * default line rate).  The application owns the line format and everything
 * above the wire (DMA, protocols) - and simply never calls this on a board
 * without an application UART, where it is then a no-op. */
void board_init_app_uart(void);

/* Periodic tick: the board owns the timer and its ISR and only calls cb(). */
typedef void (*board_tick_cb)(void);
void board_timer_create(uint32_t ms, board_tick_cb cb);

#ifdef __cplusplus
}
#endif

#endif /* _BOARD_H */
