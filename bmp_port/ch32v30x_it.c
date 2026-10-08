/********************************** (C) COPYRIGHT *******************************
* File Name          : ch32v30x_it.c
* Description        : Interrupt service routines for the CH32V30x BMP port.
*
* The USBHS vector only forwards into the CherryUSB device controller
* (USBD_IRQHandler); everything else keeps the WCH defaults.  The USART3
* handler for the target UART lives next to its driver in
* bmp_port/aux_serial.c.
*******************************************************************************/
#include "ch32v30x_it.h"

void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

void NMI_Handler(void)
{
  while (1)
  {
  }
}

void HardFault_Handler(void)
{
  NVIC_SystemReset();
  while (1)
  {
  }
}

/* The USBHS vector is served by the CherryUSB CH32V30x port itself
 * (USBHS_IRQHandler in port/wch/ch32v30x/usb_dc_ch32v30x.c). */
