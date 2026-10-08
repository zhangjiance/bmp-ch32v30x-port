#ifndef _BOARD_H
#define _BOARD_H

#include <stdint.h>

#define BOARD_NAME "ch32v30x_bmp"

/* set by SET_RUN_STATE()/SET_IDLE_STATE() in platform.h */
extern uint32_t running_status;

void board_init(void);

/* USBHS clock + interrupt, called from board_init() */
void board_init_usb(void);

/* status LED (PA8) */
void board_led_write(uint8_t state);

uint32_t board_time_ms(void);
void board_delay_ms(uint32_t ms);

#endif
