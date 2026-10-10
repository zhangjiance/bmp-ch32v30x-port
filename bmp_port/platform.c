/*
 * Platform hooks required by platform_support.h, CH32V30x flavour.
 *
 * The board exposes no target power switch, no Vref ADC and no SPI, so the
 * optional hooks stay no-ops / fixed values.  RTT is deliberately not enabled
 * in this port.
 */

#include "general.h"
#include "platform.h"
#include "board.h"
#include "boot_trigger_port.h"
#include "gdb_main.h" /* cur_target, for the status LED policy */

/* set by SET_IDLE_STATE() in platform.h */
volatile bool platform_gdb_idle = false;

/* set by SET_RUN_STATE() in platform.h */
uint32_t running_status = 0;

/*
 * Probe policy, run from the board's periodic tick.
 *
 * The board layer only offers primitives (board_led_write(), board_read_boot_pin())
 * and a registerable tick (board_timer_create()); what the LED reports and what
 * a held BOOT button does is decided here, on the application side.  The tick
 * is the only thing that still runs while the main loop is parked in
 * gdb_if_getchar() waiting for a GDB command.
 */
#define BOOT_BUTTON_TICK_MS     100U /* board tick period                      */
#define BOOT_BUTTON_PRESS_TICKS 5U   /* pressed ticks => 500 ms before boot    */
#define LED_BLINK_TICKS         1U   /* ticks per LED toggle: 1 => 100 ms      */

/*
 * Status LED:
 *   - no target attached                       -> dark
 *   - attached and stopped, which includes
 *     while a GDB command is being executed     -> solid on
 *   - attached and running                      -> blinking
 */
static void probe_status_tick(void)
{
    static uint32_t pressed;
    static uint32_t led_ticks;
    static uint32_t led_on;

    if (!cur_target) {
        /* Nothing attached, the LED has nothing to report. */
        led_ticks = 0U;
        led_on = 0U;
        board_led_write(0U);
    } else if (!running_status) {
        /* Attached and stopped: solid on. */
        led_ticks = 0U;
        led_on = 1U;
        board_led_write(1U);
    } else if (++led_ticks >= LED_BLINK_TICKS) {
        led_ticks = 0U;
        led_on = !led_on;
        board_led_write((uint8_t)led_on);
    }

    /* BOOT button: held long enough resets into the DFU bootloader. */
    if (board_read_boot_pin()) {
        if (++pressed >= BOOT_BUTTON_PRESS_TICKS) {
            /* Writes the BKP hand-shake and resets; never returns. */
            boot_trigger_reboot_to_boot();
        }
    } else {
        pressed = 0U;
    }
}

int platform_hwversion(void)
{
    return 0;
}

void platform_init(void)
{
    board_timer_create(BOOT_BUTTON_TICK_MS, probe_status_tick);
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
 * to the bootloader.  The BKP register hand-shake survives the reset, so the
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
