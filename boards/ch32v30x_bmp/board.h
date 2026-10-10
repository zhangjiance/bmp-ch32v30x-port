/*
 * board.h - board primitives for the CH32V30x Black Magic Probe port.
 *
 * The board layer exposes "primitives" only: bring-up, a millisecond time base,
 * status LED and BOOT button access, and one registerable periodic tick.  The
 * policy built on top of them (what the LED shows, what a held BOOT button
 * does) lives on the application/probe side - see bmp_port/platform.c.
 *
 * This header and its implementation deliberately know nothing about the probe:
 * pins and polarity are BOARD_* macros in board_config.h, and the tick callback
 * is opaque here (board.c owns the ISR and only calls whatever was registered).
 */
#ifndef _BOARD_H
#define _BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* System clock / time base / LED / BOOT pin / console bring-up. */
void board_init(void);

/* Free running millisecond time base, derived from SysTick. */
uint32_t board_time_ms(void);
void board_delay_ms(uint32_t ms);

/* Console bring-up; no-op unless BOARD_HAS_CONSOLE (kept optional on purpose
 * because the WCH printf spins on an uninitialised USART). */
void board_init_console(void);

/* Status LED primitive: state != 0 means "LED on".  No-op without an LED. */
void board_led_write(uint8_t state);
void board_led_toggle(void);

/* BOOT button primitive: true while pressed.  Always false without a button. */
bool board_read_boot_pin(void);

/* Periodic tick: the board owns the timer and its ISR and only calls cb().
 * A board that has no spare timer may ignore this. */
typedef void (*board_tick_cb)(void);
void board_timer_create(uint32_t ms, board_tick_cb cb);

#ifdef __cplusplus
}
#endif

#endif /* _BOARD_H */
