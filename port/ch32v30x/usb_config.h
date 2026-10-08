/*
 * usb_config.h
 *
 * CherryUSB device configuration for the CH32V30x Black Magic Probe port.
 *
 * The device is a composite:
 *   interface 0/1  GDB server            (CDC ACM, WinUSB)
 *   interface 2/3  target UART (aux)     (CDC ACM, WinUSB)
 *   interface 4    DFU runtime           (custom handler: hands over to
 *                                         ch32_dfu_boot on DFU_DETACH, so
 *                                         "dfu-util -e" reaches the bootloader)
 *
 * USBHS is high speed, like ch32v305_bmp / ch32_dfu_boot / ch32_hello_world.
 */
#ifndef USB_CONFIG_H
#define USB_CONFIG_H

/* The WCH newlib printf() spins forever on an uninitialised USART, so the
 * CherryUSB logging must stay silent. */
#define CONFIG_USB_PRINTF(...) ((void)0)
#define CONFIG_USB_DBG_LEVEL   0
#define CONFIG_USB_ALIGN_SIZE  4
#define USB_NOCACHE_RAM_SECTION

#define CONFIG_USB_DEVICE          1
#define CONFIG_USB_DEVICE_CDC_ACM  1

#define CONFIG_USBDEV_MAX_BUS             1
#define CONFIG_USBDEV_ADVANCE_DESC        1
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN  512
#define CONFIG_USBDEV_EP_NUM              8

/* The upstream Black Magic Probe USB id, so the host tooling (and the
 * udev/WinUSB rules that ship with it) recognise the device. */
#define USBD_VID        0x1D50
#define USBD_PID        0x6018
#define USBD_MAX_POWER  200

#endif /* USB_CONFIG_H */
