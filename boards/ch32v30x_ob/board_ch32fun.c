/*
 * board_ch32fun.c - board primitives, register level (ch32fun backend).
 *
 * Same interface as board.c (board.h), but nothing here goes through the WCH
 * peripheral library: ch32fun supplies the startup, the clock tree, the vector
 * table and the core intrinsics, and the couple of peripherals the board owns
 * (GPIO, SysTick, TIM3, USART3 pins) are programmed directly.
 *
 * It provides the primitives declared in board.h and owns the one periodic ISR
 * the application may ask for through board_timer_create().  The ISR does not
 * know what the callback does - it just calls it.
 */
#include "board.h"
#include "board_config.h"

#include "ch32fun.h"

/* CFGLR/CFGHR values, one nibble per pin. */
#define BOARD_GPIO_OUT_PP_50MHZ 0x3U
#define BOARD_GPIO_IN_PUPD      0x8U
#define BOARD_GPIO_IN_FLOAT     0x4U
#define BOARD_GPIO_AF_PP_50MHZ  0xBU

/*
 * Program one pin of a WCH GPIO port: four configuration bits, CFGLR for pins
 * 0-7 and CFGHR for 8-15.  `pin` is a GPIO_Pin_x mask.
 */
static void board_gpio_mode(GPIO_TypeDef *const port, const uint16_t pin, const uint32_t mode)
{
    const uint32_t index = (uint32_t)__builtin_ctz((uint32_t)pin);
    volatile uint32_t *const reg = (index < 8U) ? &port->CFGLR : &port->CFGHR;
    const uint32_t shift = 4U * (index & 7U);

    *reg = (*reg & ~(0xFU << shift)) | (mode << shift);
}

/* clock frequency the SysTick counter runs at (Hz), set in board_init() */
static uint32_t systick_clock;

/* the single periodic callback the application may install (may be NULL) */
static board_tick_cb tick_cb;

/*
 * SysTick is used as a free running 64bit counter, which gives the millisecond
 * time base without needing an interrupt.  Delays are derived from it too, so
 * nothing else may reconfigure SysTick (ch32fun's DelaySysTick() does, so it is
 * intentionally NOT used here - call board_delay_ms()).
 */
static void board_init_systick(void)
{
    systick_clock = FUNCONF_SYSTEM_CORE_CLOCK;

    SysTick->CTLR = 0;
    SysTick->SR = 0;
    SysTick->CNT = 0;
    SysTick->CMP = 0xffffffffffffffffull;
    SysTick->CTLR |= (1 << 0) | (1 << 2); /* STE | STCLK: count at HCLK */
}

uint32_t board_time_ms(void)
{
    uint64_t cycles = SysTick->CNT;
    return (uint32_t)(cycles / (uint64_t)(systick_clock / 1000U));
}

void board_delay_ms(uint32_t ms)
{
    uint32_t start = board_time_ms();
    while ((uint32_t)(board_time_ms() - start) < ms) {
        continue;
    }
}

/* -------------------------------------------------------------------------- */
/* Status LED                                                                 */
/* -------------------------------------------------------------------------- */
#if BOARD_HAS_LED
static void board_init_led(void)
{
#if BOARD_LED_NEEDS_JTAG_DISABLE
    /* e.g. PB4 is JTDO by default: disable JTAG-DP but keep SW-DP alive so the
     * pin becomes GPIO while the debug link stays usable. */
    RCC->APB2PCENR |= RCC_APB2Periph_AFIO;
    AFIO->PCFR1 = (AFIO->PCFR1 & 0xF8FFFFFFu) | 0x02000000u;
#endif
    RCC->APB2PCENR |= BOARD_LED_RCC;

    /* Preload the off level (BOARD_LED_ACTIVE_LOW aware) before the pin becomes
     * an output, so the LED does not flash on during bring-up. */
    board_led_write(0U);

    board_gpio_mode(BOARD_LED_GPIO, BOARD_LED_PIN, BOARD_GPIO_OUT_PP_50MHZ);
}
#endif /* BOARD_HAS_LED */

void board_led_write(uint8_t state)
{
#if BOARD_HAS_LED
    /* state != 0 means "LED on"; the pin may be active low. */
    const uint8_t on = (state != 0U) ? 1U : 0U;
    const uint8_t level = BOARD_LED_ACTIVE_LOW ? (uint8_t)!on : on;

    if (level) {
        BOARD_LED_GPIO->BSHR = BOARD_LED_PIN;
    } else {
        BOARD_LED_GPIO->BCR = BOARD_LED_PIN;
    }
#else
    (void)state;
#endif
}

void board_led_toggle(void)
{
#if BOARD_HAS_LED
    static uint8_t on;

    on ^= 1U;
    board_led_write(on);
#endif
}

/* -------------------------------------------------------------------------- */
/* BOOT button                                                                */
/* -------------------------------------------------------------------------- */
#if BOARD_HAS_BOOT_BUTTON
static void board_init_boot_button(void)
{
    RCC->APB2PCENR |= BOARD_BOOT_RCC;

    /* Released level must be the "run the application" level: pull up when the
     * button shorts the pin to ground, pull down otherwise. */
    if (BOARD_BOOT_ACTIVE_LOW) {
        BOARD_BOOT_GPIO->BSHR = BOARD_BOOT_PIN;
    } else {
        BOARD_BOOT_GPIO->BCR = BOARD_BOOT_PIN;
    }
    board_gpio_mode(BOARD_BOOT_GPIO, BOARD_BOOT_PIN, BOARD_GPIO_IN_PUPD);
}
#endif /* BOARD_HAS_BOOT_BUTTON */

bool board_read_boot_pin(void)
{
#if BOARD_HAS_BOOT_BUTTON
    const bool level = (BOARD_BOOT_GPIO->INDR & BOARD_BOOT_PIN) != 0U;

    return BOARD_BOOT_ACTIVE_LOW ? !level : level;
#else
    return false;
#endif
}

/* -------------------------------------------------------------------------- */
/* Application UART                                                           */
/* -------------------------------------------------------------------------- */
void board_init_app_uart(void)
{
#if BOARD_HAS_APP_UART
    BOARD_APP_UART_GPIO_CLK_ENABLE();
    BOARD_APP_UART_CLK_ENABLE();

    /* TX idle high before the alternate function takes over, so the line does
     * not glitch low (the far end would read that as a start bit). */
    BOARD_APP_UART_TX_GPIO->BSHR = BOARD_APP_UART_TX_PIN;
    board_gpio_mode(BOARD_APP_UART_TX_GPIO, BOARD_APP_UART_TX_PIN, BOARD_GPIO_AF_PP_50MHZ);

    /* RX with pull-up. */
    BOARD_APP_UART_RX_GPIO->BSHR = BOARD_APP_UART_RX_PIN;
    board_gpio_mode(BOARD_APP_UART_RX_GPIO, BOARD_APP_UART_RX_PIN, BOARD_GPIO_IN_PUPD);

    /* Hardware side only.  The line format is the application's business: it
     * owns the live one and changes it whenever the far end asks, starting from
     * the board's BOARD_APP_UART_BAUDRATE. */
#endif /* BOARD_HAS_APP_UART */
}

/* -------------------------------------------------------------------------- */
/* Periodic tick                                                              */
/* -------------------------------------------------------------------------- */
void board_timer_create(uint32_t ms, board_tick_cb cb)
{
    tick_cb = cb;

    /*
     * TIM3 update interrupt.  The prescaler gives a 10 kHz tick so the period
     * only depends on the milliseconds asked for, independent of the clock.
     * APB1 timer clock is HCLK.  Default priority, same as the USBHS interrupt:
     * neither preempts the other.
     */
    RCC->APB1PCENR |= RCC_APB1Periph_TIM3;

    TIM3->PSC = (uint16_t)((FUNCONF_SYSTEM_CORE_CLOCK / 10000U) - 1U);
    TIM3->ATRLR = (uint16_t)((10000U * ms / 1000U) - 1U);
    TIM3->CNT = 0;
    TIM3->INTFR = (uint16_t)~TIM_FLAG_Update;
    TIM3->DMAINTENR |= TIM_IT_Update;
    NVIC_EnableIRQ(TIM3_IRQn);
    TIM3->CTLR1 |= TIM_CEN;
}

void TIM3_IRQHandler(void) INTERRUPT_DECORATOR;
void TIM3_IRQHandler(void)
{
    if ((TIM3->INTFR & TIM_FLAG_Update) == 0U) {
        return;
    }
    TIM3->INTFR = (uint16_t)~TIM_FLAG_Update;

    if (tick_cb) {
        tick_cb();
    }
}

/* -------------------------------------------------------------------------- */
/* Bring-up                                                                   */
/* -------------------------------------------------------------------------- */
void board_init(void)
{
    /* Same priority grouping the WCH build's NVIC_PriorityGroupConfig() sets:
     * two pre-emption bits, so USB and the tick keep their default levels. */
    PFIC->CFGR = 0x500U;

#if BOARD_HAS_LED
    board_init_led();
#endif
#if BOARD_HAS_BOOT_BUTTON
    board_init_boot_button();
#endif

    board_init_systick();
}
