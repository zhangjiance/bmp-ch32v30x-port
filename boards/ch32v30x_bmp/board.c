#include "board.h"
#include "ch32v30x.h"
#include "debug.h"
#include "system_ch32v30x.h"
#include "jtag_port.h"

/* clock frequency the SysTick counter runs at (Hz), set in board_init() */
static uint32_t systick_clock;

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

    /* status LED */
    GPIO_ResetBits(PIN_LED_GPIO_PORT, PIN_LED_GPIO_PIN);
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
    if (state) {
        PIN_LED_GPIO_PORT->BSHR = PIN_LED_GPIO_PIN;
    } else {
        PIN_LED_GPIO_PORT->BCR = PIN_LED_GPIO_PIN;
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
}

/*
 * The CherryUSB low-level hook (usb_dc_low_level_init(uint8_t busid)) lives in
 * port/ch32v30x/boot_usb_ch32v30x.c, next to the PHY/PLL recipe it shares with
 * ch32_dfu_boot and ch32_hello_world.
 */
