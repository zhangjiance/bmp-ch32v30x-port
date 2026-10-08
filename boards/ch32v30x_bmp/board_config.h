/*
 * board_config.h
 *
 * Board identity for the CH32V30x Black Magic Probe port.
 *
 * The pins live in bmp_port/jtag_port.h (they are part of the probe's
 * SWD/JTAG bit-banging layer), this header only carries what the build and the
 * USB descriptors need to know.
 *
 * Wiring:
 *   PB14 -> SWCLK / TCK
 *   PB15 -> SWDIO / TMS   (push-pull output, switched to input for SWD reads)
 *   PB13 -> TDI
 *   PB12 -> TDO
 *   PA8  -> status LED
 *   PB10 -> target UART TX (USART3)
 *   PB11 -> target UART RX (USART3)
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#define BOARD_NAME "CH32V30x BMP"

#endif /* BOARD_CONFIG_H */
