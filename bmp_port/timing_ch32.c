/*
 * Timing support for CH32V305.
 *
 * The millisecond time base comes from the free running SysTick counter
 * configured in board.c (see board_time_ms()); this file owns the SWD/JTAG
 * bit-bang clock, which the GDB "monitor frequency" command drives through
 * platform_max_frequency_set().
 */

#include "general.h"
#include "platform.h"
#include "board.h"
#include "system_ch32v30x.h"

/*
 * SWD/JTAG bit-bang clock.
 *
 * target_clk_divider is the iteration count the bit-banging routines spend in
 * their busy-wait loop per clock edge (bmp_port/swdptap.c, bmp_port/jtagtap.c).
 * UINT32_MAX is the sentinel they read as "no delay at all", which is the
 * default here: with no monitor frequency command the probe runs the target
 * link as fast as the loop can toggle the pins.
 *
 * The two constants below come from disassembling those loops for the QingKe
 * RV32 core at 144 MHz: a clock costs USED_SWD_CYCLES instructions of fixed
 * overhead, plus two delay loops of CYCLES_PER_CNT cycles per iteration in the
 * delayed variant (the loop body re-reads the volatile counter, so it is not a
 * single-cycle per iteration).  Measure with a scope before trusting the exact
 * frequency.
 */
#define USED_SWD_CYCLES 12U /* fixed instructions per SWD/JTAG clock           */
#define CYCLES_PER_CNT  6U  /* cycles per delay-loop iteration (volatile r/w)  */

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
    platform_ospeed_update(frequency);

    /* A requested frequency of 0 asks for the slowest rate there is. */
    const uint32_t requested = frequency ? frequency : 1U;

    /* If the bare loop is already slower than the request, add no delay. */
    if ((uint64_t)USED_SWD_CYCLES * requested >= SystemCoreClock) {
        target_clk_divider = UINT32_MAX;
        return;
    }

    /*
     * cycles per clock = USED_SWD_CYCLES + 2 * CYCLES_PER_CNT * target_clk_divider,
     * so invert that for the requested frequency.  The result is bounded by the
     * 1 Hz request above, i.e. by SystemCoreClock / (2 * CYCLES_PER_CNT).
     */
    const uint64_t divider = ((uint64_t)SystemCoreClock - (uint64_t)USED_SWD_CYCLES * requested) /
        (2U * (uint64_t)CYCLES_PER_CNT * requested);
    target_clk_divider = (uint32_t)divider;
}

uint32_t platform_max_frequency_get(void)
{
    if (target_clk_divider == UINT32_MAX)
        return SystemCoreClock / USED_SWD_CYCLES;
    return (uint32_t)((uint64_t)SystemCoreClock /
        ((uint64_t)USED_SWD_CYCLES + 2U * (uint64_t)CYCLES_PER_CNT * target_clk_divider));
}
