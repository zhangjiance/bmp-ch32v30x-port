/*
 * board.c - board primitives.
 *
 * This file is the whole board layer.  It includes the SDK and the board's own
 * headers (board.h, board_config.h) and nothing else: no application, no probe.
 * Every board specific value it needs is a BOARD_* macro from board_config.h.
 *
 * It provides the primitives declared in board.h and owns the one periodic ISR
 * the application may ask for through board_timer_create().  The ISR does not
 * know what the callback does - it just calls it.
 */
#include "board.h"
#include "board_config.h"

#include "debug.h" /* SDK: ch32v30x.h -> RCC / GPIO / TIM / misc */

/* clock frequency the SysTick counter runs at (Hz), set in board_init() */
static uint32_t systick_clock;

/* the single periodic callback the application may install (may be NULL) */
static board_tick_cb tick_cb;

/*
 * SysTick is used as a free running 64bit counter, which gives the millisecond
 * time base without needing an interrupt.  Delays are derived from it too, so
 * nothing else may reconfigure SysTick (Delay_Ms()/Delay_Us() from the WCH SDK
 * do, so they are intentionally NOT used here - call board_delay_ms()).
 */
static void board_init_systick(void)
{
    systick_clock = SystemCoreClock;

    SysTick->CTLR = 0;
    SysTick->SR = 0;
    SysTick->CNT = 0;
    SysTick->CMP = 0xffffffffffffffffull;
    SysTick->CTLR |= (1 << 0); /* STE: start counting */
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
    GPIO_InitTypeDef gpio = { 0 };

#if BOARD_LED_NEEDS_JTAG_DISABLE
    /* e.g. PB4 is JTDO by default: disable JTAG-DP but keep SW-DP alive so the
     * pin becomes GPIO while the debug link stays usable. */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    AFIO->PCFR1 = (AFIO->PCFR1 & 0xF8FFFFFFu) | 0x02000000u;
#endif
    RCC_APB2PeriphClockCmd(BOARD_LED_RCC, ENABLE);

    /* Preload the off level (BOARD_LED_ACTIVE_LOW aware) before the pin becomes
     * an output, so the LED does not flash on during bring-up. */
    board_led_write(0U);

    gpio.GPIO_Pin   = BOARD_LED_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(BOARD_LED_GPIO, &gpio);
}
#endif /* BOARD_HAS_LED */

void board_led_write(uint8_t state)
{
#if BOARD_HAS_LED
    /* state != 0 means "LED on"; the pin may be active low. */
    const uint8_t on = (state != 0U) ? 1U : 0U;
    const uint8_t level = BOARD_LED_ACTIVE_LOW ? (uint8_t)!on : on;

    GPIO_WriteBit(BOARD_LED_GPIO, BOARD_LED_PIN, level ? Bit_SET : Bit_RESET);
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
    GPIO_InitTypeDef gpio = { 0 };

    RCC_APB2PeriphClockCmd(BOARD_BOOT_RCC, ENABLE);

    gpio.GPIO_Pin   = BOARD_BOOT_PIN;
    /* Released level must be the "run the application" level. */
    gpio.GPIO_Mode  = BOARD_BOOT_ACTIVE_LOW ? GPIO_Mode_IPU : GPIO_Mode_IPD;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(BOARD_BOOT_GPIO, &gpio);
}
#endif /* BOARD_HAS_BOOT_BUTTON */

bool board_read_boot_pin(void)
{
#if BOARD_HAS_BOOT_BUTTON
    const BitAction level = GPIO_ReadInputDataBit(BOARD_BOOT_GPIO, BOARD_BOOT_PIN);

    return BOARD_BOOT_ACTIVE_LOW ? (level == Bit_RESET) : (level == Bit_SET);
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
    GPIO_InitTypeDef gpio = { 0 };

    BOARD_APP_UART_GPIO_CLK_ENABLE();
    BOARD_APP_UART_CLK_ENABLE();

    /* TX idle high before the alternate function takes over, so the line does
     * not glitch low (the far end would read that as a start bit). */
    GPIO_SetBits(BOARD_APP_UART_TX_GPIO, BOARD_APP_UART_TX_PIN);
    gpio.GPIO_Pin   = BOARD_APP_UART_TX_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_Init(BOARD_APP_UART_TX_GPIO, &gpio);

    gpio.GPIO_Pin  = BOARD_APP_UART_RX_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(BOARD_APP_UART_RX_GPIO, &gpio);

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
    TIM_TimeBaseInitTypeDef tim = { 0 };

    tick_cb = cb;

    /*
     * TIM3 update interrupt.  The prescaler gives a 10 kHz tick so the period
     * only depends on the milliseconds asked for, independent of SystemCoreClock.
     * APB1 timer clock is HCLK.  Default priority, same as the USBHS interrupt:
     * neither preempts the other.
     */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
    tim.TIM_Prescaler     = (uint16_t)((SystemCoreClock / 10000U) - 1U);
    tim.TIM_Period        = (uint16_t)((10000U * ms / 1000U) - 1U);
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &tim);

    TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
    TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
    NVIC_EnableIRQ(TIM3_IRQn);
    TIM_Cmd(TIM3, ENABLE);
}

void TIM3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void TIM3_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM3, TIM_IT_Update) == RESET) {
        return;
    }
    TIM_ClearITPendingBit(TIM3, TIM_IT_Update);

    if (tick_cb) {
        tick_cb();
    }
}

/* -------------------------------------------------------------------------- */
/* Bring-up                                                                   */
/* -------------------------------------------------------------------------- */
void board_init(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    SystemCoreClockUpdate();
    Delay_Init();

#if BOARD_HAS_LED
    board_init_led();
#endif
#if BOARD_HAS_BOOT_BUTTON
    board_init_boot_button();
#endif

    board_init_systick();
}
