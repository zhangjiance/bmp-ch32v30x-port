#include "board.h"
#include "ch32v30x.h"
#include "debug.h"
/* cur_target: the status LED only reports a GDB session once a target is attached */
#include "gdb_main.h"
#include "system_ch32v30x.h"
#include "jtag_port.h"
#include "boot_trigger_port.h"

/* clock frequency the SysTick counter runs at (Hz), set in board_init() */
static uint32_t systick_clock;

/*
 * BOOT button watchdog.
 *
 * The probe spends its idle time parked inside gdb_if_getchar() waiting for the
 * next GDB command, so the main loop cannot sample the button like
 * ch32_hello_world does.  TIM3 therefore raises a 100 ms update interrupt that
 * samples PA6 and, once the button has read pressed for long enough, writes the
 * BKP hand-shake and resets into the DFU bootloader.
 *
 * SysTick is already used as a free-running time base (see board_init_systick()
 * below) and must not be given an interrupt, hence the dedicated timer.
 */
#define BOOT_BUTTON_TICK_MS     100U  /* TIM3 update period                     */
#define BOOT_BUTTON_PRESS_TICKS 5U    /* pressed samples => 500 ms before boot  */

uint32_t running_status = 0;

/*
 * SysTick is used as a free running 64bit counter, which gives the millisecond
 * time base without needing an interrupt. Delays are derived from it too, so
 * nothing else may reconfigure SysTick (Delay_Ms()/Delay_Us() from the WCH SDK
 * do, so they are intentionally NOT used here).
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

static void board_init_boot_button(void)
{
    GPIO_InitTypeDef gpio = { 0 };
    TIM_TimeBaseInitTypeDef tim = { 0 };

    /* PA6: input, pull-up when active low (released level = "run the app"). */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin   = PIN_BOOT_GPIO_PIN;
    gpio.GPIO_Mode  = PIN_BOOT_ACTIVE_LOW ? GPIO_Mode_IPU : GPIO_Mode_IPD;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(PIN_BOOT_GPIO_PORT, &gpio);

    /* TIM3: BOOT_BUTTON_TICK_MS update interrupt.  APB1 timer clock is HCLK. */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
    tim.TIM_Prescaler     = (uint16_t)((SystemCoreClock / 10000U) - 1U); /* 10 kHz   */
    tim.TIM_Period        = (uint16_t)((10000U * BOOT_BUTTON_TICK_MS / 1000U) - 1U);
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &tim);

    TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
    TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
    /* Default priority, same as the USBHS interrupt: neither preempts the
     * other, and this ISR is only a GPIO read plus a counter. */
    NVIC_EnableIRQ(TIM3_IRQn);
    TIM_Cmd(TIM3, ENABLE);
}

/*
 * Status LED.
 *
 * The board has a single LED and it reports the GDB session:
 *   - no target attached                     -> dark
 *   - attached and stopped, which includes
 *     while a GDB command is being executed   -> solid on
 *   - attached and running                    -> blinking
 * The BOOT button tick below drives it, which is why no extra timer or
 * interrupt is needed.
 */
#define LED_BLINK_TICKS 1U /* timer ticks per toggle: 1 => 100 ms */

static void board_led_tick(void)
{
    static uint32_t led_ticks;
    static uint32_t led_on;

    if (!cur_target) {
        /* Nothing attached, the LED has nothing to report. */
        led_ticks = 0U;
        led_on = 0U;
        board_led_write(0U);
        return;
    }

    if (!running_status) {
        /* Attached and stopped: solid on. */
        led_ticks = 0U;
        led_on = 1U;
        board_led_write(1U);
        return;
    }

    if (++led_ticks >= LED_BLINK_TICKS) {
        led_ticks = 0U;
        led_on = !led_on;
        board_led_write((uint8_t)led_on);
    }
}

/*
 * Samples the BOOT button from the timer interrupt and ticks the status LED: an
 * ISR is the only place that still runs while the main loop is blocked on the
 * GDB endpoint.  The ISR stays minimal (a GPIO read, a counter and one LED
 * write) so the bit-banged SWD/JTAG timing only sees a very short, infrequent
 * interruption.
 */
void TIM3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void TIM3_IRQHandler(void)
{
    static uint32_t pressed;

    if (TIM_GetITStatus(TIM3, TIM_IT_Update) == RESET) {
        return;
    }
    TIM_ClearITPendingBit(TIM3, TIM_IT_Update);

    board_led_tick();

    if (PIN_BOOT_PRESSED()) {
        if (++pressed >= BOOT_BUTTON_PRESS_TICKS) {
            /* Writes the BKP hand-shake and resets; never returns. */
            boot_trigger_reboot_to_boot();
        }
    } else {
        pressed = 0U;
    }
}

static void board_init_gpio(void)
{
    GPIO_InitTypeDef gpio = { 0 };

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    /*
     * SWDIO/TMS: push pull and idle high.
     * jtagtap.c never calls PIN_TMS_SWDIO_SET_OUT(), so JTAG needs this line to
     * be a driven output already; an open-drain pin without a pull-up would
     * float when driving high and break the JTAG reset sequence.
     * swdptap.c switches the mode to input around each SWD turnaround.
     */
    GPIO_SetBits(PIN_TMS_GPIO_PORT, PIN_TMS_GPIO_PIN);
    gpio.GPIO_Pin = PIN_TMS_GPIO_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(PIN_TMS_GPIO_PORT, &gpio);

    /* SWCLK/TCK and TDI: push pull, idle high */
    GPIO_SetBits(PIN_TCK_GPIO_PORT, PIN_TCK_GPIO_PIN);
    gpio.GPIO_Pin = PIN_TCK_GPIO_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(PIN_TCK_GPIO_PORT, &gpio);

    GPIO_SetBits(PIN_TDI_GPIO_PORT, PIN_TDI_GPIO_PIN);
    gpio.GPIO_Pin = PIN_TDI_GPIO_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(PIN_TDI_GPIO_PORT, &gpio);

    /* TDO: input with pull up */
    gpio.GPIO_Pin = PIN_TDO_GPIO_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(PIN_TDO_GPIO_PORT, &gpio);

    /* status LED: preload the off level before the pin becomes an output */
    board_led_write(0U);
    gpio.GPIO_Pin = PIN_LED_GPIO_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(PIN_LED_GPIO_PORT, &gpio);
}

static void usbhs_rcc_init(void)
{
    RCC_USBCLK48MConfig(RCC_USBCLK48MCLKSource_USBPHY);
    RCC_USBHSPLLCLKConfig(RCC_HSBHSPLLCLKSource_HSE);
    RCC_USBHSConfig(RCC_USBPLL_Div6);
    RCC_USBHSPLLCKREFCLKConfig(RCC_USBHSPLLCKREFCLK_4M);
    RCC_USBHSPHYPLLALIVEcmd(ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBHS, ENABLE);
}

void board_init_usb(void)
{
    usbhs_rcc_init();
    NVIC_EnableIRQ(USBHS_IRQn);
}

void board_led_write(uint8_t state)
{
    /*
     * state != 0 means "LED on".  The hardware is active low (see
     * PIN_LED_ACTIVE_LOW in jtag_port.h), so "on" is the pin driven low.
     */
    const uint32_t lit = (state != 0U) ? 1U : 0U;
    const uint32_t low = PIN_LED_ACTIVE_LOW ? lit : !lit;

    if (low) {
        PIN_LED_GPIO_PORT->BCR = PIN_LED_GPIO_PIN;
    } else {
        PIN_LED_GPIO_PORT->BSHR = PIN_LED_GPIO_PIN;
    }
}

void board_init(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    SystemCoreClockUpdate();
    Delay_Init();

    board_init_gpio();
    board_init_usb();
    board_init_systick();
    board_init_boot_button();
}

/*
 * The CherryUSB low-level hook (usb_dc_low_level_init(uint8_t busid)) lives in
 * bmp_port/boot_usb_ch32v30x.c, next to the PHY/PLL recipe it needs.
 */
