/*
 * Platform hooks required by platform_support.h, CH32V30x flavour.
 *
 * The board exposes no target power switch, no Vref ADC and no SPI, so the
 * optional hooks stay no-ops / fixed values (same as bmp-hpm-port and
 * ch32v305_bmp).  RTT is deliberately not enabled in this port.
 */

#include "general.h"
#include "platform.h"
#include "board.h"
#include "boot_trigger_port.h"

/* set by SET_IDLE_STATE() in platform.h */
volatile bool platform_gdb_idle = false;

int platform_hwversion(void)
{
    return 0;
}

void platform_init(void)
{
}

void platform_nrst_set_val(bool assert)
{
    (void)assert;
}

bool platform_nrst_get_val(void)
{
    return false;
}

const char *platform_target_voltage(void)
{
    return "Unknown";
}

/*
 * "monitor bootloader" (and DFU_DETACH from dfu-util -e): hand the device over
 * to ch32_dfu_boot.  The BKP register hand-shake survives the reset, so the
 * bootloader stays in DFU mode instead of jumping straight back into this
 * application.
 */
void platform_request_boot(void)
{
    boot_trigger_reboot_to_boot();
}

bool platform_target_get_power(void)
{
    return true;
}

bool platform_target_set_power(const bool power)
{
    (void)power;
    return true;
}

void platform_target_clk_output_enable(bool enable)
{
    (void)enable;
}

bool platform_spi_init(const spi_bus_e bus)
{
    (void)bus;
    return false;
}

bool platform_spi_deinit(const spi_bus_e bus)
{
    (void)bus;
    return false;
}

bool platform_spi_chip_select(const uint8_t device_select)
{
    (void)device_select;
    return false;
}

uint8_t platform_spi_xfer(const spi_bus_e bus, const uint8_t value)
{
    (void)bus;
    return value;
}
