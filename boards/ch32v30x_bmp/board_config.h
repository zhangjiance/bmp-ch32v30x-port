/*
 * board_config.h - "ch32v30x_bmp"
 *
 * Board Support Package for the CH32V30x Black Magic Probe port.
 *
 * This header carries only board identity, pins, polarity and features - all as
 * BOARD_* macros, with the same names the bootloader / hello-world boards use
 * (boards/ch32v30x_ob).  The board primitives live in board.c, and the probe's
 * own SWD/JTAG pin layer lives in bmp_port/jtag_port.h.
 *
 *   BOOT button : PA6 to GND, internal pull-up, active low
 *   status LED  : PA5, active low
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#define BOARD_NAME              "ch32v30x_bmp"

/* ---- BOOT button: PA6 to GND, pull-up, active low ----
 * Set BOARD_HAS_BOOT_BUTTON to 0 on a board without a button: DFU is then
 * entered only through the application detach path (dfu-util -e). */
#define BOARD_HAS_BOOT_BUTTON   1
#define BOARD_BOOT_GPIO         GPIOA
#define BOARD_BOOT_PIN          GPIO_Pin_6
#define BOARD_BOOT_RCC          RCC_APB2Periph_GPIOA
#define BOARD_BOOT_ACTIVE_LOW   1

/* ---- status LED: PA5 (plain GPIO, no JTAG remap needed) ----
 * BOARD_LED_ACTIVE_LOW: 1 = LED on when the pin is low. */
#define BOARD_HAS_LED           1
#define BOARD_LED_GPIO          GPIOA
#define BOARD_LED_PIN           GPIO_Pin_5
#define BOARD_LED_RCC           RCC_APB2Periph_GPIOA
#define BOARD_LED_ACTIVE_LOW    1
#define BOARD_LED_NEEDS_JTAG_DISABLE 0

#endif /* BOARD_CONFIG_H */
