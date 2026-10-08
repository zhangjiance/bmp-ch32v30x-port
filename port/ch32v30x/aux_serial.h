/*
 * aux_serial.h
 *
 * Target UART ("aux serial") of the CH32V30x BMP port: USART3 on PB10/PB11,
 * the same pins ch32v305_bmp uses.  It backs the second CDC port so a terminal
 * on the host can talk to the target's UART.
 *
 * The port layer owns this interface (the blackmagic core only knows about it
 * through the platform's USB glue), so the API is deliberately small.
 */
#ifndef AUX_SERIAL_H
#define AUX_SERIAL_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Deliberately free of CherryUSB headers: the USB glue converts
 * struct cdc_line_coding into this plain form, so the UART driver (and the
 * targets that include this header) never depends on the USB stack.
 * bDataBits counts the data bits, parity is 0 = none, 1 = odd, 2 = even and
 * stop_bits is 1 or 2.
 */
struct aux_line_coding {
    uint32_t baudrate;
    uint8_t data_bits;
    uint8_t parity;
    uint8_t stop_bits;
};

void aux_serial_init(void);
bool aux_serial_pins_enabled(void);

void aux_serial_set_encoding(const struct aux_line_coding *coding);
void aux_serial_get_encoding(struct aux_line_coding *coding);

/* target -> host: drain up to max bytes, returns how many were copied */
uint32_t aux_serial_read(uint8_t *dst, uint32_t max);

/* host -> target: queue bytes for transmission, returns how many were queued */
uint32_t aux_serial_write(const uint8_t *src, uint32_t len);

/* Push buffered target data to the host; driven from the GDB idle loops
 * (the CH32V30x device controller does not raise SOF events). */
void aux_serial_poll(void);

#endif /* AUX_SERIAL_H */
