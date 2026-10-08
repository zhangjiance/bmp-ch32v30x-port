/*
 * boot_trigger_port.h
 *
 * Chip-agnostic hand-shake for requesting DFU mode across a reset.
 * The implementation is chip specific (port/<chip>/boot_trigger_<chip>.c).
 */
#ifndef BOOT_TRIGGER_PORT_H
#define BOOT_TRIGGER_PORT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set the request flag (does not reset). */
void boot_trigger_request(void);

/* Read and clear the request flag. */
bool boot_trigger_check_and_clear(void);

/* Set the flag and reset the MCU -> next boot enters the DFU bootloader.
 * Never returns. */
void boot_trigger_reboot_to_boot(void);

/* Plain system reset (used after a successful DFU download). */
void boot_system_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_TRIGGER_PORT_H */
