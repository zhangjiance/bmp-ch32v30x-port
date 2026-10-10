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
 * The two constants below are counted from the release disassembly of those
 * loops for the QingKe RV32 core at 144 MHz (one instruction per cycle; the
 * measured no-delay TCK rate confirms it).  Each figure is one clock's whole
 * loop body with the loop-closing branch included, so it is directly
 * comparable with the per-iteration delay-loop cost:
 *
 *   fixed cost per clock, in instructions (GPIO accesses in brackets)
 *     swdptap_seq_out          8   [TCK+SWDIO, TCK]
 *     swdptap_seq_in           9   [INDR read, TCK, TCK]
 *     jtagtap_tdi_tdo_seq     16   [TCK+TMS+TDI, TCK, INDR read]
 *     jtagtap_tdi_seq         12   [TCK+TMS+TDI, TCK]
 *   delay-loop body           3   [JTAG bnez / addi / j; SWD 2, addi / bnez]
 *
 * The JTAG figures went up from 13-14 to 16 when the shift loops were
 * rearranged for a 50% duty cycle: the per-bit work is now split evenly across
 * the rising edge instead of sitting on one side of it (see jtagtap.c), and a
 * couple of nops trim the last cycle of difference.  The old loops were not
 * really faster: the branch that picked the TDI level mispredicted on roughly
 * half the bits.  What did change is the shape of the clock: TCK used to be low
 * for about 1 cycle out of 14, it is now low for 8 out of 16.
 *
 * TCK, TMS and TDI all live on GPIOB and are driven through a single BSHR store
 * per edge (see jtag_port.h), so a JTAG scan needs two writes and one read per
 * clock instead of the five GPIO accesses the per-pin helpers cost.
 *
 * USED_SWD_CYCLES carries the JTAG bulk-shift figure: that is the loop a scan
 * actually spends its time in, so it is the worst case, and SWD's leaner loop
 * then comes out faster than the model predicts.  A clock costs
 *
 *     USED_SWD_CYCLES + 2 * CYCLES_PER_CNT * target_clk_divider
 *
 * cycles, one delay loop per clock edge.  Measure with a scope before trusting
 * the exact frequency.
 */
#define USED_SWD_CYCLES 16U /* fixed instructions per clock (JTAG bulk shift)  */
#define CYCLES_PER_CNT  3U  /* cycles per delay-loop iteration                 */

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
