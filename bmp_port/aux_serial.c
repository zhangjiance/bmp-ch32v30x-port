/*
 * aux_serial.c
 *
 * Target UART for the CH32V30x BMP port.
 *
 *   PB10 = USART3 TX (alternate function push-pull)
 *   PB11 = USART3 RX (input with pull-up)
 *
 * Exactly the pins ch32v305_bmp uses, so the same adapter cable works.
 *
 * Both directions go through small ring buffers driven by the USART
 * interrupt: the USB callbacks must never block, and the target UART is far
 * slower than USB bulk transfers.
 */
#include "aux_serial.h"

#include "board.h"
#include "debug.h" /* ch32v30x_conf.h -> GPIO / RCC / USART / misc */

#define AUX_UART          USART3
#define AUX_UART_IRQn     USART3_IRQn
#define AUX_TX_GPIO_PORT  GPIOB
#define AUX_TX_GPIO_PIN   GPIO_Pin_10
#define AUX_RX_GPIO_PORT  GPIOB
#define AUX_RX_GPIO_PIN   GPIO_Pin_11

#define AUX_BUF_SIZE 256U /* must be a power of two */

static volatile uint8_t rx_buf[AUX_BUF_SIZE];
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;

static volatile uint8_t tx_buf[AUX_BUF_SIZE];
static volatile uint16_t tx_head;
static volatile uint16_t tx_tail;

static struct aux_line_coding line_coding = {
    .baudrate = 115200U,
    .data_bits = 8U,
    .parity = 0U,
    .stop_bits = 1U,
};

static bool pins_enabled;

static uint16_t buf_next(uint16_t index)
{
    return (uint16_t)((index + 1U) & (AUX_BUF_SIZE - 1U));
}

static void uart_apply_encoding(void)
{
    USART_InitTypeDef uart = { 0 };

    uart.USART_BaudRate = line_coding.baudrate;
    uart.USART_WordLength = USART_WordLength_8b;
    uart.USART_StopBits = (line_coding.stop_bits == 2U) ? USART_StopBits_2 : USART_StopBits_1;
    switch (line_coding.parity) {
    case 1U:
        uart.USART_Parity = USART_Parity_Odd;
        break;
    case 2U:
        uart.USART_Parity = USART_Parity_Even;
        break;
    default:
        uart.USART_Parity = USART_Parity_No;
        break;
    }
    /* USART_WordLength_8b includes the parity bit, so 8 data bits + parity
     * needs 9 bit frames - keep it simple and use 8N1 unless parity is set. */
    if (uart.USART_Parity != USART_Parity_No) {
        uart.USART_WordLength = USART_WordLength_9b;
    }
    uart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    uart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;

    USART_Init(AUX_UART, &uart);
}

void aux_serial_init(void)
{
    GPIO_InitTypeDef gpio = { 0 };

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);

    /* TX is driven high before the alternate function takes over, so the pin
     * does not glitch low (which the target would see as a start bit). */
    GPIO_SetBits(AUX_TX_GPIO_PORT, AUX_TX_GPIO_PIN);
    gpio.GPIO_Pin = AUX_TX_GPIO_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(AUX_TX_GPIO_PORT, &gpio);

    gpio.GPIO_Pin = AUX_RX_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(AUX_RX_GPIO_PORT, &gpio);

    rx_head = rx_tail = 0U;
    tx_head = tx_tail = 0U;

    uart_apply_encoding();
    USART_ITConfig(AUX_UART, USART_IT_RXNE, ENABLE);
    USART_Cmd(AUX_UART, ENABLE);

    NVIC_EnableIRQ(AUX_UART_IRQn);
    pins_enabled = true;
}

bool aux_serial_pins_enabled(void)
{
    return pins_enabled;
}

void aux_serial_set_encoding(const struct aux_line_coding *coding)
{
    if (coding == NULL) {
        return;
    }
    line_coding = *coding;
    if (line_coding.baudrate == 0U) {
        line_coding.baudrate = 115200U;
    }
    uart_apply_encoding();
}

void aux_serial_get_encoding(struct aux_line_coding *coding)
{
    if (coding != NULL) {
        *coding = line_coding;
    }
}

uint32_t aux_serial_read(uint8_t *dst, uint32_t max)
{
    uint32_t count = 0U;

    while ((count < max) && (rx_tail != rx_head)) {
        dst[count++] = rx_buf[rx_tail];
        rx_tail = buf_next(rx_tail);
    }
    return count;
}

uint32_t aux_serial_write(const uint8_t *src, uint32_t len)
{
    uint32_t queued = 0U;

    while ((queued < len) && (buf_next(tx_head) != tx_tail)) {
        tx_buf[tx_head] = src[queued++];
        tx_head = buf_next(tx_head);
    }
    /* Kick the transmitter: TXE is level triggered, so enabling it here is
     * enough even if the buffer was already running. */
    USART_ITConfig(AUX_UART, USART_IT_TXE, ENABLE);
    return queued;
}

/*
 * blackmagic's "target stdout" hook (declared in general.h): anything the
 * target prints through semihosting / debug_serial_* is forwarded to the aux
 * serial port, i.e. to the second CDC on the host.
 */
void debug_serial_send_stdout(const uint8_t *data, size_t len)
{
    (void)aux_serial_write(data, (uint32_t)len);
}

void USART3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USART3_IRQHandler(void)
{
    if (USART_GetITStatus(AUX_UART, USART_IT_RXNE) != RESET) {
        uint8_t data = (uint8_t)(AUX_UART->DATAR & 0xFFU);

        USART_ClearITPendingBit(AUX_UART, USART_IT_RXNE);
        if (buf_next(rx_head) != rx_tail) {
            rx_buf[rx_head] = data;
            rx_head = buf_next(rx_head);
        }
    }

    if (USART_GetITStatus(AUX_UART, USART_IT_TXE) != RESET) {
        if (tx_tail != tx_head) {
            AUX_UART->DATAR = tx_buf[tx_tail];
            tx_tail = buf_next(tx_tail);
        } else {
            USART_ITConfig(AUX_UART, USART_IT_TXE, DISABLE);
        }
    }
}
