/*
 * boot_usb_ch32v30x.c
 *
 * CH32V30x USBHS device-controller port glue:
 *   - the CherryUSB low-level hooks (usb_dc_low_level_init / _deinit)
 *   - the teardown used before jumping to the application
 *
 * Same PHY/PLL recipe as ch32v305_uf2 / ch32v305_bmp (USBHS, high speed).
 */
#include "boot_usb_port.h"

#include "debug.h"          /* ch32v30x.h -> RCC / USBHS */
#include "ch32v30x_usb.h"

void usb_dc_low_level_init(uint8_t busid)
{
    (void)busid;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    RCC_USBCLK48MConfig(RCC_USBCLK48MCLKSource_USBPHY);
    RCC_USBHSPLLCLKConfig(RCC_HSBHSPLLCLKSource_HSE);
    RCC_USBHSConfig(RCC_USBPLL_Div6);
    RCC_USBHSPLLCKREFCLKConfig(RCC_USBHSPLLCKREFCLK_4M);
    RCC_USBHSPHYPLLALIVEcmd(ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBHS, ENABLE);

    NVIC_EnableIRQ(USBHS_IRQn);
}

void usb_dc_low_level_deinit(uint8_t busid)
{
    (void)busid;
    NVIC_DisableIRQ(USBHS_IRQn);
}

void boot_usb_port_deinit(void)
{
    NVIC_DisableIRQ(USBHS_IRQn);

    /* Drop the D+ pull-up so the host sees a disconnect and re-enumerates the
     * application instead of reusing the bootloader's stale state. */
    USBHSD->CONTROL = 0;
    USBHSD->HOST_CTRL = 0x10;       /* USBHS_PHY_SUSPENDM */
    USBHSD->INT_EN = 0;
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBHS, DISABLE);

    /* >10 ms so the host definitely samples the disconnect. */
    for (volatile uint32_t settle = 0U; settle < 200000U; ++settle) {
    }
}
