/*
 * board_config.h - "ch32v30x_ob"
 *
 * Board Support Package for the CH32V30x onboard target board.
 *
 * Each board lives in its own, self-contained folder under boards/:
 *     boards/<board>/board_config.h   pins / polarity / features (this file)
 *     boards/<board>/board.h          board primitive interface (copy as is)
 *     boards/<board>/board.c          board primitive implementation (copy as is)
 *     boards/<board>/board.cmake      OPTIONAL extra CMake definitions
 *
 * It is the default board of every project that uses it.  Because the whole
 * folder is self-contained, copying it under boards/ of another project and
 * selecting it with -DBOARD=<board> just works.
 *
 *   BOOT button : PA6 to GND, internal pull-up, active low
 *   status LED  : PA5, active low
 *   app UART    : USART3, PB10 (TX) / PB11 (RX)
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#define BOARD_NAME              "ch32v30x_ob"

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

/* ---- application UART ----
 * USART3 on PB10 (TX) / PB11 (RX): the board's UART for application traffic.
 * The BMP build drives it as its USB2UART hardware; the bootloader and the
 * hello-world build leave it unused - they never call board_init_app_uart(),
 * and their own log output is their business (see main.c).
 * The clock helpers are macros because the APB bus differs per USART, which is
 * exactly the kind of board detail board.c must not hardcode. */
#define BOARD_HAS_APP_UART        1
#define BOARD_APP_UART            USART3
#define BOARD_APP_UART_IRQ        USART3_IRQn
#define BOARD_APP_UART_BAUDRATE   115200
#define BOARD_APP_UART_TX_GPIO    GPIOB
#define BOARD_APP_UART_TX_PIN     GPIO_Pin_10
#define BOARD_APP_UART_RX_GPIO    GPIOB
#define BOARD_APP_UART_RX_PIN     GPIO_Pin_11
#define BOARD_APP_UART_CLK_ENABLE()      RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE)
#define BOARD_APP_UART_GPIO_CLK_ENABLE() RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE)

#endif /* BOARD_CONFIG_H */
