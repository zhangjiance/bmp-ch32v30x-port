/*
 * Timing support for CH32V305.
 *
 * The millisecond time base comes from the free running SysTick counter
 * configured in board.c (see board_time_ms()).
 */

#include "general.h"
#include "platform.h"
#include "board.h"
#include "system_ch32v30x.h"

uint32_t target_clk_divider = UINT32_MAX;

void platform_timing_init(void)
{
}

void platform_delay(uint32_t ms)
{
    board_delay_ms(ms);
}

void sys_tick_handler(void)
{
}

uint32_t platform_time_ms(void)
{
    return board_time_ms();
}

__attribute__((weak)) void platform_ospeed_update(const uint32_t frequency)
{
    (void)frequency;
}

void platform_max_frequency_set(const uint32_t frequency)
{
    (void)frequency;
}

uint32_t platform_max_frequency_get(void)
{
    return SystemCoreClock;
}
