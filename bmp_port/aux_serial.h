/*
 * aux_serial.h
 *
 * Target UART ("aux serial") of the CH32V30x BMP port: USART3 on PB10/PB11.
 * It backs the second CDC port so a terminal on the host can talk to the
 * target's UART.
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

/*
 * target -> host sink, supplied by the USB glue.  It copies the bytes into the
 * USB transfer buffer and starts the endpoint; it is called from interrupt
 * context and returns how many bytes it accepted, 0 meaning "the endpoint still
 * holds the previous transfer, ask me again later".
 */
typedef uint32_t (*aux_serial_sink_fn)(const uint8_t *data, uint32_t len);

/* Register the sink.  Must be called before aux_serial_init(). */
void aux_serial_set_sink(aux_serial_sink_fn sink);

/*
 * Bring up USART3, its DMA channels (RX circular, TX on demand) and the
 * interrupts that carry the data.  From here on both directions run on
 * interrupts alone - the main loop is never involved.
 */
void aux_serial_init(void);
bool aux_serial_pins_enabled(void);

/*
 * Called by the USB glue when the IN endpoint finished a transfer, so buffered
 * target data can be forwarded without waiting for more UART traffic.
 */
void aux_serial_usb_ready(void);

void aux_serial_set_encoding(const struct aux_line_coding *coding);
void aux_serial_get_encoding(struct aux_line_coding *coding);

/* host -> target: queue bytes for transmission, returns how many were queued */
uint32_t aux_serial_write(const uint8_t *src, uint32_t len);

#endif /* AUX_SERIAL_H */
