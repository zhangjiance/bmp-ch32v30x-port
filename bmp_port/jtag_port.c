/*
 * jtag_port.c - bring-up of the probe's bit-banged SWD/JTAG pins.
 *
 * The pin macros and the bit accessors live in jtag_port.h; only the one-time
 * GPIO configuration needs code, and it belongs to the probe (the board layer,
 * boards/ch32v30x_bmp/, knows nothing about SWD/JTAG).
 */
#include "jtag_port.h"

void jtag_port_init(void)
{
    GPIO_InitTypeDef gpio = { 0 };

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    /*
     * SWDIO/TMS: push pull and idle high.
     * jtagtap.c never calls PIN_TMS_SWDIO_SET_OUT(), so JTAG needs this line to
     * be a driven output already; an open-drain pin without a pull-up would
     * float when driving high and break the JTAG reset sequence.
     * swdptap.c switches the mode to input around each SWD turnaround.
     */
    GPIO_SetBits(PIN_TMS_GPIO_PORT, PIN_TMS_GPIO_PIN);
    gpio.GPIO_Pin   = PIN_TMS_GPIO_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(PIN_TMS_GPIO_PORT, &gpio);

    /* SWCLK/TCK and TDI: push pull, idle high */
    GPIO_SetBits(PIN_TCK_GPIO_PORT, PIN_TCK_GPIO_PIN);
    gpio.GPIO_Pin   = PIN_TCK_GPIO_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(PIN_TCK_GPIO_PORT, &gpio);

    GPIO_SetBits(PIN_TDI_GPIO_PORT, PIN_TDI_GPIO_PIN);
    gpio.GPIO_Pin   = PIN_TDI_GPIO_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(PIN_TDI_GPIO_PORT, &gpio);

    /* TDO: input with pull up */
    gpio.GPIO_Pin   = PIN_TDO_GPIO_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_IPU;
    GPIO_Init(PIN_TDO_GPIO_PORT, &gpio);
}
