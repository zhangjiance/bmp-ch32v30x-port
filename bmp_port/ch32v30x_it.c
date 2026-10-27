/*
 * ch32v30x_it_ch32fun.c
 *
 * Exception handlers for the CH32V30x BMP port.
 *
 * ch32fun's vector table carries weak aliases for every handler, so these
 * strong definitions replace the defaults.  The USBHS and USART3/DMA vectors
 * are served by their drivers (CherryUSB port and aux_serial_ch32fun.c).
 */
#include "ch32fun.h"

void NMI_Handler(void) INTERRUPT_DECORATOR;
void HardFault_Handler(void) INTERRUPT_DECORATOR;

void NMI_Handler(void)
{
    while (1) {
    }
}

void HardFault_Handler(void)
{
    NVIC_SystemReset();
    while (1) {
    }
}
