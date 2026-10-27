/*
 * jtag_port.c - bring-up of the probe's bit-banged SWD/JTAG pins.
 *
 * The pin macros and the bit accessors live in jtag_port.h; only the one-time
 * GPIO configuration needs code, and it belongs to the probe (the board layer,
 * boards/ch32v30x_ob/, knows nothing about SWD/JTAG).
 *
 * Register level on purpose: the same code serves the WCH SDK and the ch32fun
 * backend, and the CFGLR nibbles are all that is needed.
 */
#include "jtag_port.h"

void jtag_port_init(void)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOB;

    /*
     * SWDIO/TMS: push pull and idle high.
     * jtagtap.c never calls PIN_TMS_SWDIO_SET_OUT(), so JTAG needs this line to
     * be a driven output already; an open-drain pin without a pull-up would
     * float when driving high and break the JTAG reset sequence.
     * swdptap.c switches the mode to input around each SWD turnaround.
     */
    PIN_TMS_GPIO_PORT->BSHR = PIN_TMS_GPIO_PIN;
    PIN_GPIO_MODE(PIN_TMS_GPIO_PORT, PIN_TMS_GPIO_PIN, PIN_GPIO_CFG_OUT_PP_50MHZ);

    /* SWCLK/TCK and TDI: push pull, idle high */
    PIN_TCK_GPIO_PORT->BSHR = PIN_TCK_GPIO_PIN;
    PIN_GPIO_MODE(PIN_TCK_GPIO_PORT, PIN_TCK_GPIO_PIN, PIN_GPIO_CFG_OUT_PP_50MHZ);

    PIN_TDI_GPIO_PORT->BSHR = PIN_TDI_GPIO_PIN;
    PIN_GPIO_MODE(PIN_TDI_GPIO_PORT, PIN_TDI_GPIO_PIN, PIN_GPIO_CFG_OUT_PP_50MHZ);

    /* TDO: input with pull up */
    PIN_TDO_GPIO_PORT->BSHR = PIN_TDO_GPIO_PIN;
    PIN_GPIO_MODE(PIN_TDO_GPIO_PORT, PIN_TDO_GPIO_PIN, PIN_GPIO_CFG_IN_PUPD);
}
