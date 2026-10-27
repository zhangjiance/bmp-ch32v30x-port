/*
 * aux_serial_ch32fun.c
 *
 * Target UART for the CH32V30x BMP port, register level: the board's
 * application UART (BOARD_APP_UART*, see
 * boards/ch32v30x_ob/board_config.h), the physical side of the second CDC
 * function.  The board layer owns the hardware - instance, pins and clocks,
 * all through board_init_app_uart() - while this file owns the line format and
 * the DMA data path on top of it.
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

#include "board.h" /* BOARD_APP_UART*: the USB2UART hardware */
#include "ch32fun.h"

#if !BOARD_HAS_APP_UART
#error "aux_serial needs an application UART: the board must provide BOARD_APP_UART*"
#endif

/* The UART is the board's application UART.  Its instance, pins and clocks are
 * BOARD_APP_UART* macros, applied by the board in board_init_app_uart(); the
 * probe only builds the data path on top. */
#define AUX_UART         BOARD_APP_UART
#define AUX_UART_IRQn    BOARD_APP_UART_IRQ

/*
 * APB1 runs at HCLK / 2 on this port (ch32fun's clock tree), so the USART
 * divisor is derived from that.  If the APB1 prescaler ever changes, update
 * this along with FUNCONF_SYSTEM_CORE_CLOCK usage in funconfig.h.
 */
#define AUX_PCLK1 (FUNCONF_SYSTEM_CORE_CLOCK / 2U)

/*
 * CH32V30x DMA1 request mapping: USART3_TX is channel 2, USART3_RX is
 * channel 3 (the same fixed mapping STM32F1 uses).
 */
#define AUX_TX_DMA        DMA1_Channel2
#define AUX_TX_DMA_IRQn   DMA1_Channel2_IRQn
#define AUX_RX_DMA        DMA1_Channel3
#define AUX_RX_DMA_IRQn   DMA1_Channel3_IRQn

/* DMA CFGR bits used below (same layout as the WCH library defines). */
#define AUX_DMA_CFGR_EN      0x0001U
#define AUX_DMA_CFGR_TCIE    0x0002U
#define AUX_DMA_CFGR_HTIE    0x0004U
#define AUX_DMA_CFGR_DIR_P2M 0x0000U
#define AUX_DMA_CFGR_DIR_M2P 0x0010U
#define AUX_DMA_CFGR_CIRC    0x0020U
#define AUX_DMA_CFGR_MINC    0x0080U
#define AUX_DMA_CFGR_PL_HIGH 0x2000U

/* DMA1->INTFR flags, per channel (GIF/TCIF/HTIF/TEIF). */
#define AUX_TX_DMA_FLAGS (0x1U << 4)  /* GL2 */
#define AUX_RX_DMA_FLAGS (0x1U << 8)  /* GL3 */

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
    .baudrate = BOARD_APP_UART_BAUDRATE,
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
    return (uint16_t)(AUX_RX_BUF_SIZE - AUX_RX_DMA->CNTR) & AUX_RX_BUF_MASK;
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

void USART3_IRQHandler(void) INTERRUPT_DECORATOR;
void USART3_IRQHandler(void)
{
    if ((AUX_UART->STATR & USART_STATR_IDLE) == 0U) {
        return;
    }
    /* Reading the status register and then the data register clears IDLE (and
     * any overrun/framing/noise flags).  The byte itself was already taken by
     * the RX DMA, so this read cannot steal one. */
    (void)AUX_UART->STATR;
    (void)AUX_UART->DATAR;

    aux_serial_forward();
}

void DMA1_Channel3_IRQHandler(void) INTERRUPT_DECORATOR;
void DMA1_Channel3_IRQHandler(void)
{
    /* Clears the global (half transfer + transfer complete) flags together. */
    DMA1->INTFR = AUX_RX_DMA_FLAGS;

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
        AUX_TX_DMA->CFGR &= ~AUX_DMA_CFGR_EN;
        DMA1->INTFR = AUX_TX_DMA_FLAGS;
        AUX_TX_DMA->MADDR = (uint32_t)&tx_buf[tx_tail];
        AUX_TX_DMA->CNTR = run;
        AUX_TX_DMA->CFGR |= AUX_DMA_CFGR_EN;
    }

    NVIC_EnableIRQ(AUX_TX_DMA_IRQn);
}

void DMA1_Channel2_IRQHandler(void) INTERRUPT_DECORATOR;
void DMA1_Channel2_IRQHandler(void)
{
    AUX_TX_DMA->CFGR &= ~AUX_DMA_CFGR_EN;
    DMA1->INTFR = AUX_TX_DMA_FLAGS;

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
    /* Keep every control bit that is not part of the frame format (IDLEIE,
     * DMA enables, ...) and only rewrite M/PCE/PS/TE/RE. */
    uint16_t ctlr1 = AUX_UART->CTLR1;

    ctlr1 &= (uint16_t)~(0x1000U | 0x0400U | 0x0200U | 0x0008U | 0x0004U); /* M PCE PS TE RE */
    ctlr1 |= USART_Mode_Rx | USART_Mode_Tx;

    /* USART_WordLength_8b counts the parity bit, so 8 data bits + parity needs
     * 9 bit frames.  Only 8 data bits are supported: 7N1 has no encoding on this
     * USART (7 data bits always come with a parity bit), so a request for 7 is
     * honoured as 8 and reported back as 8 by aux_serial_get_encoding(). */
    switch (line_coding.parity) {
    case 1U:
        ctlr1 |= 0x0600U; /* PCE | PS: odd */
        ctlr1 |= 0x1000U; /* M: 9 bit frames for 8 data + parity */
        break;
    case 2U:
        ctlr1 |= 0x0400U; /* PCE: even */
        ctlr1 |= 0x1000U;
        break;
    default:
        break;
    }

    AUX_UART->CTLR1 = ctlr1;
    AUX_UART->CTLR2 = (line_coding.stop_bits == 2U) ? 0x2000U : 0x0000U;

    /*
     * BRR: the WCH divisor, 16x oversampling, PCLK1 as the source:
     *   usartdiv = 25 * PCLK1 / (4 * baud)  (in hundredths of a bit period)
     *   BRR = mantissa << 4 | round(fraction * 16)
     */
    const uint32_t usartdiv = (25U * AUX_PCLK1) / (4U * line_coding.baudrate);
    uint32_t brr = (usartdiv / 100U) << 4;
    uint32_t fraction = usartdiv - (100U * (brr >> 4));
    fraction = ((fraction * 16U) + 50U) / 100U;
    if (fraction > 0x0FU) {
        brr += 1U << 4;
    } else {
        brr |= fraction & 0x0FU;
    }
    AUX_UART->BRR = (uint16_t)brr;
}

static void aux_serial_init_rx_dma(void)
{
    AUX_RX_DMA->CFGR = 0;
    AUX_RX_DMA->CNTR = AUX_RX_BUF_SIZE;
    AUX_RX_DMA->PADDR = (uint32_t)&AUX_UART->DATAR;
    AUX_RX_DMA->MADDR = (uint32_t)rx_buf;
    AUX_RX_DMA->CFGR = AUX_DMA_CFGR_MINC | AUX_DMA_CFGR_CIRC | AUX_DMA_CFGR_PL_HIGH |
        AUX_DMA_CFGR_HTIE | AUX_DMA_CFGR_TCIE;

    DMA1->INTFR = AUX_RX_DMA_FLAGS;
    NVIC_EnableIRQ(AUX_RX_DMA_IRQn);
    AUX_RX_DMA->CFGR |= AUX_DMA_CFGR_EN;
}

static void aux_serial_init_tx_dma(void)
{
    AUX_TX_DMA->CFGR = 0;
    AUX_TX_DMA->CNTR = 0; /* loaded per run by aux_tx_kick() */
    AUX_TX_DMA->PADDR = (uint32_t)&AUX_UART->DATAR;
    AUX_TX_DMA->MADDR = (uint32_t)tx_buf;
    AUX_TX_DMA->CFGR = AUX_DMA_CFGR_DIR_M2P | AUX_DMA_CFGR_MINC | AUX_DMA_CFGR_PL_HIGH | AUX_DMA_CFGR_TCIE;

    DMA1->INTFR = AUX_TX_DMA_FLAGS;
    NVIC_EnableIRQ(AUX_TX_DMA_IRQn);
}

void aux_serial_init(void)
{
    RCC->AHBPCENR |= RCC_AHBPeriph_DMA1;

    /* Board side: instance, pins, clocks and the power-on line format. */
    board_init_app_uart();
    /* Probe side: the live line format the CDC host asked for. */
    uart_apply_encoding();

    tx_head = tx_tail = 0U;
    rx_tail = 0U;
    tx_run = 0U;
    tx_busy = false;

    aux_serial_init_rx_dma();
    aux_serial_init_tx_dma();

    /* Only the idle line interrupt is needed: the data itself is moved by DMA,
     * the interrupt just says "the burst stopped, forward what you have". */
    AUX_UART->CTLR1 |= USART_CTLR1_IDLEIE;
    NVIC_EnableIRQ(AUX_UART_IRQn);

    AUX_UART->CTLR1 |= USART_CTLR1_UE;
    AUX_UART->CTLR3 |= USART_CTLR3_DMAT | USART_CTLR3_DMAR;

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
        AUX_UART->CTLR1 &= (uint16_t)~USART_CTLR1_UE;
        uart_apply_encoding();
        AUX_UART->CTLR1 |= USART_CTLR1_UE;
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
