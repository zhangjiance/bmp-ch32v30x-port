/*
 * aux_serial.c
 *
 * Target UART for the CH32V30x BMP port: USART3 on PB10 (TX) / PB11 (RX), the
 * physical side of the second CDC function.
 *
 * The data path is DMA + interrupt driven end to end and never depends on the
 * main loop, which is normally parked inside gdb_if_getchar() waiting for a GDB
 * command:
 *
 *   target -> host   USART3 RX -> DMA1 channel 3, writing rx_buf in a circle.
 *                    The USART IDLE interrupt and the DMA half/full transfer
 *                    interrupts forward whatever arrived straight to the USB
 *                    endpoint through the registered sink, and the endpoint's
 *                    completion callback (aux_serial_usb_ready()) continues
 *                    with the rest.
 *   host -> target   the USB OUT callback queues into tx_buf; DMA1 channel 2
 *                    sends one contiguous run at a time and its transfer
 *                    complete interrupt picks up the next one.
 *
 * Both directions therefore keep flowing while a GDB command is in progress.
 */
#include "aux_serial.h"

#include "debug.h" /* ch32v30x_conf.h -> GPIO / RCC / USART / DMA / misc */

#define AUX_UART          USART3
#define AUX_TX_GPIO_PORT  GPIOB
#define AUX_TX_GPIO_PIN   GPIO_Pin_10
#define AUX_RX_GPIO_PORT  GPIOB
#define AUX_RX_GPIO_PIN   GPIO_Pin_11

/*
 * CH32V30x DMA1 request mapping: USART3_TX is channel 2, USART3_RX is
 * channel 3 (the same fixed mapping STM32F1 uses).
 *
 * Careful with the two macro families: DMA_IT_* are the per-channel CFGR
 * enable bits (TCIE/HTIE/TEIE) that DMA_ITConfig() ORs straight into CFGR,
 * while DMA1_IT_* / DMA1_FLAG_* are the INTFR flag masks used by
 * DMA_GetITStatus()/DMA_ClearFlag().  Passing the latter to DMA_ITConfig()
 * silently reprograms DIR/CIRC/PSIZE instead of enabling an interrupt.
 */
#define AUX_TX_DMA        DMA1_Channel2
#define AUX_TX_DMA_IRQn   DMA1_Channel2_IRQn
#define AUX_TX_DMA_IT     DMA_IT_TC
#define AUX_TX_DMA_FLAGS  (DMA1_FLAG_GL2 | DMA1_FLAG_TC2 | DMA1_FLAG_HT2 | DMA1_FLAG_TE2)
#define AUX_RX_DMA        DMA1_Channel3
#define AUX_RX_DMA_IRQn   DMA1_Channel3_IRQn
#define AUX_RX_DMA_IT     (DMA_IT_HT | DMA_IT_TC)
#define AUX_RX_DMA_FLAGS  (DMA1_FLAG_GL3 | DMA1_FLAG_TC3 | DMA1_FLAG_HT3 | DMA1_FLAG_TE3)

/* Circular DMA destination, must be a power of two. */
#define AUX_RX_BUF_SIZE 256U
#define AUX_RX_BUF_MASK (AUX_RX_BUF_SIZE - 1U)
/* host -> target staging ring, a power of two and at least one HS bulk packet
 * so a full 512 byte OUT transfer always fits. */
#define AUX_TX_BUF_SIZE 1024U
#define AUX_TX_BUF_MASK (AUX_TX_BUF_SIZE - 1U)

/* Bytes handed to the sink at a time; matches the USB transfer buffer size. */
#define AUX_USB_CHUNK 64U

static uint8_t rx_buf[AUX_RX_BUF_SIZE];
/* Position in rx_buf not yet handed to the host.  Only the forwarding ISRs and
 * the USB completion callback touch it, and those never nest. */
static volatile uint16_t rx_tail;

static uint8_t tx_buf[AUX_TX_BUF_SIZE];
static volatile uint16_t tx_head;
static volatile uint16_t tx_tail;
/* Length of the run DMA1 channel 2 is currently sending. */
static volatile uint16_t tx_run;
static volatile bool tx_busy;

static aux_serial_sink_fn usb_sink;

static struct aux_line_coding line_coding = {
    .baudrate = 115200U,
    .data_bits = 8U,
    .parity = 0U,
    .stop_bits = 1U,
};

static bool pins_enabled;

void aux_serial_set_sink(aux_serial_sink_fn sink)
{
    usb_sink = sink;
}

/* ------------------------------------------------------------------ *
 * target -> host
 * ------------------------------------------------------------------ */

/* Where the DMA has written up to, derived from its remaining count. */
static inline uint16_t aux_rx_head(void)
{
    return (uint16_t)(AUX_RX_BUF_SIZE - DMA_GetCurrDataCounter(AUX_RX_DMA)) & AUX_RX_BUF_MASK;
}

/*
 * Hand everything the DMA has collected to the host.  It is called from the
 * USART IDLE / DMA interrupts and from the USB transfer-complete callback, so
 * a burst is delivered as soon as it pauses and a burst longer than one USB
 * packet continues without any help from the main loop.
 */
static void aux_serial_forward(void)
{
    const uint16_t head = aux_rx_head();

    while (rx_tail != head) {
        uint32_t run = (head > rx_tail) ? (uint32_t)(head - rx_tail)
                                        : (uint32_t)(AUX_RX_BUF_SIZE - rx_tail);
        uint32_t accepted;

        if (run > AUX_USB_CHUNK) {
            run = AUX_USB_CHUNK;
        }
        accepted = usb_sink ? usb_sink(&rx_buf[rx_tail], run) : 0U;
        if (accepted == 0U) {
            break; /* Endpoint busy: the completion callback re-enters here. */
        }
        rx_tail = (uint16_t)((rx_tail + accepted) & AUX_RX_BUF_MASK);
    }
}

void aux_serial_usb_ready(void)
{
    aux_serial_forward();
}

void USART3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USART3_IRQHandler(void)
{
    if (USART_GetITStatus(AUX_UART, USART_IT_IDLE) == RESET) {
        return;
    }
    /* Reading the status register and then the data register clears IDLE (and
     * any overrun/framing/noise flags).  The byte itself was already taken by
     * the RX DMA, so this read cannot steal one. */
    (void)AUX_UART->STATR;
    (void)AUX_UART->DATAR;

    aux_serial_forward();
}

void DMA1_Channel3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void DMA1_Channel3_IRQHandler(void)
{
    /* Clears the half transfer and transfer complete flags together. */
    DMA_ClearFlag(AUX_RX_DMA_FLAGS);

    aux_serial_forward();
}

/* ------------------------------------------------------------------ *
 * host -> target
 * ------------------------------------------------------------------ */

/* Start the next run if the channel is idle.  Called from the USB OUT callback
 * (through aux_serial_write), from the transfer complete interrupt and from
 * semihosting stdout, so the check-and-start is protected against the DMA
 * interrupt. */
static void aux_tx_kick(void)
{
    NVIC_DisableIRQ(AUX_TX_DMA_IRQn);

    if (!tx_busy && (tx_head != tx_tail)) {
        /* One contiguous run; a wrap is handled by the next kick. */
        const uint16_t run = (uint16_t)((tx_head > tx_tail) ? (tx_head - tx_tail)
                                                            : (AUX_TX_BUF_SIZE - tx_tail));

        tx_run = run;
        tx_busy = true;
        DMA_Cmd(AUX_TX_DMA, DISABLE);
        DMA_ClearFlag(AUX_TX_DMA_FLAGS);
        AUX_TX_DMA->MADDR = (uint32_t)&tx_buf[tx_tail];
        DMA_SetCurrDataCounter(AUX_TX_DMA, run);
        DMA_Cmd(AUX_TX_DMA, ENABLE);
    }

    NVIC_EnableIRQ(AUX_TX_DMA_IRQn);
}

void DMA1_Channel2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void DMA1_Channel2_IRQHandler(void)
{
    DMA_Cmd(AUX_TX_DMA, DISABLE);
    DMA_ClearFlag(AUX_TX_DMA_FLAGS);

    tx_tail = (uint16_t)((tx_tail + tx_run) & AUX_TX_BUF_MASK);
    tx_busy = false;

    aux_tx_kick();
}

uint32_t aux_serial_write(const uint8_t *src, uint32_t len)
{
    uint32_t queued = 0U;

    if ((src == NULL) || (len == 0U)) {
        return 0U;
    }

    /*
     * The head is also advanced by the USB OUT callback, which runs in the
     * USBHS interrupt, so the update is protected against it.  The tail is only
     * ever moved by the DMA interrupt, which this section still allows.
     */
    NVIC_DisableIRQ(USBHS_IRQn);
    while ((queued < len) && (((tx_head + 1U) & AUX_TX_BUF_MASK) != tx_tail)) {
        tx_buf[tx_head] = src[queued++];
        tx_head = (uint16_t)((tx_head + 1U) & AUX_TX_BUF_MASK);
    }
    NVIC_EnableIRQ(USBHS_IRQn);

    aux_tx_kick();
    return queued;
}

/* ------------------------------------------------------------------ *
 * Configuration
 * ------------------------------------------------------------------ */

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
    /* USART_WordLength_8b counts the parity bit, so 8 data bits + parity needs
     * 9 bit frames.  Only 8 data bits are supported: 7N1 has no encoding on this
     * USART (7 data bits always come with a parity bit), so a request for 7 is
     * honoured as 8 and reported back as 8 by aux_serial_get_encoding(). */
    if (uart.USART_Parity != USART_Parity_No) {
        uart.USART_WordLength = USART_WordLength_9b;
    }
    uart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    uart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;

    USART_Init(AUX_UART, &uart);
}

static void aux_serial_init_gpio(void)
{
    GPIO_InitTypeDef gpio = { 0 };

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

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
}

static void aux_serial_init_rx_dma(void)
{
    DMA_InitTypeDef dma = { 0 };

    DMA_DeInit(AUX_RX_DMA);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&AUX_UART->DATAR;
    dma.DMA_MemoryBaseAddr = (uint32_t)rx_buf;
    dma.DMA_DIR = DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize = AUX_RX_BUF_SIZE;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Circular;
    dma.DMA_Priority = DMA_Priority_High;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(AUX_RX_DMA, &dma);

    DMA_ClearFlag(AUX_RX_DMA_FLAGS);
    DMA_ITConfig(AUX_RX_DMA, AUX_RX_DMA_IT, ENABLE);
    NVIC_EnableIRQ(AUX_RX_DMA_IRQn);
    DMA_Cmd(AUX_RX_DMA, ENABLE);
}

static void aux_serial_init_tx_dma(void)
{
    DMA_InitTypeDef dma = { 0 };

    DMA_DeInit(AUX_TX_DMA);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&AUX_UART->DATAR;
    dma.DMA_MemoryBaseAddr = (uint32_t)tx_buf;
    dma.DMA_DIR = DMA_DIR_PeripheralDST;
    dma.DMA_BufferSize = 0U; /* loaded per run by aux_tx_kick() */
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Normal;
    dma.DMA_Priority = DMA_Priority_High;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(AUX_TX_DMA, &dma);

    DMA_ClearFlag(AUX_TX_DMA_FLAGS);
    DMA_ITConfig(AUX_TX_DMA, AUX_TX_DMA_IT, ENABLE);
    NVIC_EnableIRQ(AUX_TX_DMA_IRQn);
}

void aux_serial_init(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    aux_serial_init_gpio();
    uart_apply_encoding();

    tx_head = tx_tail = 0U;
    rx_tail = 0U;
    tx_run = 0U;
    tx_busy = false;

    aux_serial_init_rx_dma();
    aux_serial_init_tx_dma();

    /* Only the idle line interrupt is needed: the data itself is moved by DMA,
     * the interrupt just says "the burst stopped, forward what you have". */
    USART_ITConfig(AUX_UART, USART_IT_IDLE, ENABLE);
    NVIC_EnableIRQ(USART3_IRQn);

    USART_Cmd(AUX_UART, ENABLE);
    USART_DMACmd(AUX_UART, USART_DMAReq_Tx | USART_DMAReq_Rx, ENABLE);

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
    /* Only 8 data bit frames exist on this USART (see uart_apply_encoding()). */
    line_coding.data_bits = 8U;
    if (pins_enabled) {
        USART_Cmd(AUX_UART, DISABLE);
        uart_apply_encoding();
        USART_Cmd(AUX_UART, ENABLE);
    }
}

void aux_serial_get_encoding(struct aux_line_coding *coding)
{
    if (coding != NULL) {
        *coding = line_coding;
    }
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
