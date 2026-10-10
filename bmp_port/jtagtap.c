/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2022-2023 1BitSquared <info@1bitsquared.com>
 * Modified by Rachel Mant <git@dragonmux.network>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This file implements the low-level JTAG TAP interface.  */

#include <stdio.h>

#include "general.h"
#include "platform.h"
#include "jtagtap.h"
#include "adiv5.h"
#include "board.h"
#include "jtag_port.h"

jtag_proc_s jtag_proc;

/*
 * Duty cycle trim for the shift loops below, in nops (one cycle, 7 ns at
 * 144 MHz), added to whichever half of the TCK period the release disassembly
 * shows to be the shorter one.
 *
 * The loops themselves do the real work: every bit costs a fixed number of
 * instructions and moving half of them across the rising edge is free, so the
 * duty cycle is squared up without lengthening the period.  These constants
 * only absorb the 1-3 cycle residue that is left, which is finer than one
 * PIN_CLK_DELAY_ITERS() step (3 cycles) can express.  Count them again from
 * `objdump -d` after touching a loop or the compiler flags.  Bare decimals:
 * PIN_CLK_BALANCE() pastes them into an assembler `.rept`.
 */
#define JTAG_SHIFT_LOW_PAD       0 /* jtagtap_tdi_tdo_seq_no_delay(): low half  */
#define JTAG_SHIFT_HIGH_PAD      1 /* jtagtap_tdi_tdo_seq_no_delay(): high half */
#define JTAG_TAIL_LOW_PAD        2 /* ... its tail bits, high half below        */
#define JTAG_TAIL_HIGH_PAD       0
#define JTAG_TAIL_FINAL_LOW_PAD  4 /* ... the clock that carries final_tms      */
#define JTAG_TAIL_FINAL_HIGH_PAD 3
#define JTAG_TDI_HIGH_PAD        3 /* jtagtap_tdi_seq_no_delay(): high half     */
#define JTAG_TDI_TAIL_HIGH_PAD   1 /* ... its tail bits                         */
#define JTAG_TDI_FINAL_HIGH_PAD  5 /* ... the clock that carries final_tms      */
#define JTAG_TMS_LOW_PAD         1 /* jtagtap_tms_seq_no_delay(): low half      */
#define JTAG_TMS_HIGH_PAD        2 /* ... high half                             */
/* Single-clock paths.  Here the low half is the caller's: everything between
 * two clocks - function return, the caller's work, the call back in - is spent
 * with TCK low, which is why these used to measure ~86% low.  The pads hold
 * TCK high to match; they are sized for the callers in the vendor's own code
 * and only need to be roughly right. */
#define JTAG_NEXT_HIGH_PAD       10 /* jtagtap_next_no_delay(): high half       */
#define JTAG_CYCLE_LOW_PAD        4 /* jtagtap_cycle_no_delay(): low half       */
#define JTAG_CYCLE_HIGH_PAD       8 /* ... high half, incl. the loop back below */

static void jtagtap_reset(void);
static void jtagtap_tms_seq(uint32_t tms_states, size_t clock_cycles);
static void jtagtap_tdi_tdo_seq(uint8_t *data_out, bool final_tms, const uint8_t *data_in, size_t clock_cycles);
static void jtagtap_tdi_seq(bool final_tms, const uint8_t *data_in, size_t clock_cycles);
static bool jtagtap_next(bool tms, bool tdi);
static void jtagtap_cycle(bool tms, bool tdi, size_t clock_cycles);


void jtagtap_init(void)
{
	platform_target_clk_output_enable(true);

	jtag_proc.jtagtap_reset = jtagtap_reset;
	jtag_proc.jtagtap_next = jtagtap_next;
	jtag_proc.jtagtap_tms_seq = jtagtap_tms_seq;
	jtag_proc.jtagtap_tdi_tdo_seq = jtagtap_tdi_tdo_seq;
	jtag_proc.jtagtap_tdi_seq = jtagtap_tdi_seq;
	jtag_proc.jtagtap_cycle = jtagtap_cycle;
	jtag_proc.tap_idle_cycles = 1;

	/* Ensure we're in JTAG mode. Start by issuing a complete SWD reset of at least 50 reset cycles */
	jtagtap_cycle(true, false, 51U);
	/* Having achieved reset, try the deprecated 16-bit SWD-to-JTAG sequence */
	jtagtap_tms_seq(ADIV5_SWD_TO_JTAG_SELECT_SEQUENCE, 16U);
	// while(1);
	/* Next, to complete that sequence, do a full 50+ cycle reset again */
	jtagtap_cycle(true, false, 51U);
	/*
	 * For parts that implement the old sequence, we're done.. however, for parts that do not, we
	 * now need to do SWD-to-Dormant-State
	 */
	jtagtap_tms_seq(ADIV5_SWD_TO_DORMANT_SEQUENCE, 16U);
	/* Having achieved this state, we now have to signal we want to change states with the alert sequence */
	jtagtap_tms_seq(0xffU, 8U); /* 8 reset cycles used to ensure the target's in a happy place */
	/* 128-bit Selection Alert sequence */
	jtagtap_tms_seq(ADIV5_SELECTION_ALERT_SEQUENCE_0, 32U);
	jtagtap_tms_seq(ADIV5_SELECTION_ALERT_SEQUENCE_1, 32U);
	jtagtap_tms_seq(ADIV5_SELECTION_ALERT_SEQUENCE_2, 32U);
	jtagtap_tms_seq(ADIV5_SELECTION_ALERT_SEQUENCE_3, 32U);
	/*
	 * Now ask for JTAG please
	 * We combine the last two sequences in a single jtagtap_tms_seq as an optimization
	 *
	 * Send 4 SWCLKTCK cycles with SWDIOTMS LOW
	 * Send the required 8 bit activation code sequence on SWDIOTMS
	 *
	 * The bits are shifted out to the right, so we shift the second sequence left by the size of the first sequence
	 * The first sequence is 4 bits and the second 8 bits, totaling 12 bits in the combined sequence
	 */
	jtagtap_tms_seq(ADIV5_ACTIVATION_CODE_ARM_JTAG_DP << 4U, 12U);
	/* At this point we are definitely in JTAG mode - let the scan logic reset the state machine into a good state. */
}

static void jtagtap_reset(void)
{
#ifdef PIN_JTAG_TRST
	PIN_nTRST_OUT(0);
	for (volatile size_t i = 0; i < 10000U; i++)
		continue;
	PIN_nTRST_OUT(1);
#endif
	jtagtap_soft_reset();
}

static bool jtagtap_next_clk_delay()
{
	PIN_SWCLK_TCK_SET();
	PIN_CLK_DELAY();
	const uint16_t result = (uint16_t)PIN_TDO_IN();
	PIN_SWCLK_TCK_CLR();;
	PIN_CLK_DELAY();
	return result != 0;
}

static bool jtagtap_next_no_delay()
{
	PIN_SWCLK_TCK_SET();
	/*
	 * This clock's low half is the caller's: the return, whatever the caller
	 * does with the bit, and the call back in all happen with TCK low, which is
	 * a dozen cycles or so against two here - the worst duty cycle of any path
	 * in this file, ~86% low.  Hold TCK high for a comparable time to even it
	 * out; the pad is a guess at the callers in the vendor's own code, so it is
	 * worth a look with a scope if you care about this one clock.
	 */
	const uint16_t result = (uint16_t)PIN_TDO_IN();
	PIN_CLK_BALANCE(JTAG_NEXT_HIGH_PAD);
	PIN_SWCLK_TCK_CLR();
	return result != 0;
}

static bool jtagtap_next(const bool tms, const bool tdi)
{
	PIN_TMS_SWDIO_OUT(tms);
	PIN_TDI_OUT(tdi);
	if (target_clk_divider != UINT32_MAX)
		return jtagtap_next_clk_delay();
	else // NOLINT(readability-else-after-return)
		return jtagtap_next_no_delay();
}

static void jtagtap_tms_seq_clk_delay(uint32_t tms_states, const size_t clock_cycles)
{
	uint32_t state = tms_states & 1U;
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		/* Falling edge first, so the loop bookkeeping lands in the high half:
		 * driving TMS is then the only thing the low half has to do. */
		PIN_SWCLK_TCK_CLR();
		/* TMS goes out at the start of the low half, so the delay that follows
		 * is setup time for the target's rising-edge sample, not hold time. */
		PIN_TMS_SWDIO_OUT(state);
		PIN_CLK_DELAY();
		PIN_SWCLK_TCK_SET();
		PIN_CLK_DELAY();
		tms_states >>= 1U;
		state = tms_states & 1U;
	}
	PIN_SWCLK_TCK_CLR();
}

static void jtagtap_tms_seq_no_delay(uint32_t tms_states, const size_t clock_cycles)
{
	uint32_t state = tms_states & 1U;
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		PIN_SWCLK_TCK_CLR();
		/* Build the TMS word here: it is the only work the low half has. */
		PIN_STICK_HERE(state);
		PIN_TMS_SWDIO_OUT(state);
		PIN_CLK_BALANCE(JTAG_TMS_LOW_PAD);
		PIN_SWCLK_TCK_SET();
		/* High half: the next TMS value and the loop. */
		tms_states >>= 1U;
		state = tms_states & 1U;
		PIN_CLK_BALANCE(JTAG_TMS_HIGH_PAD);
	}
	PIN_SWCLK_TCK_CLR();
}

static void jtagtap_tms_seq(const uint32_t tms_states, const size_t clock_cycles)
{
	PIN_TDI_OUT(1);
	if (target_clk_divider != UINT32_MAX)
		jtagtap_tms_seq_clk_delay(tms_states, clock_cycles);
	else
		jtagtap_tms_seq_no_delay(tms_states, clock_cycles);
}

static void jtagtap_tdi_tdo_seq_clk_delay(
	const uint8_t *const data_in, uint8_t *const data_out, const bool final_tms, const size_t clock_cycles)
{
	if (!clock_cycles)
		return;

	/*
	 * The last bit is the only clock whose TMS level is not known up front, so
	 * it is peeled out of the loops: everything before it runs with TMS low,
	 * which lets the falling clock edge carry TDI in a single GPIO store.
	 */
	const size_t final_byte = (clock_cycles - 1U) >> 3U;
	const size_t tail_bits = (clock_cycles - 1U) - (final_byte << 3U);
	/* 32-bit: an 8-bit accumulator makes the compiler zero extend it once per
	 * tail bit, one cycle per clock. */
	uint32_t value = 0;
	/* Built before any clock of this call, as in the no-delay loop below. */
	const uint32_t final_word =
		PIN_JTAG_SHIFT_LOW_TMS_WORD((data_in[final_byte] >> tail_bits) & 1U, final_tms ? 1U : 0U);

	/* Complete bytes: same shape as the no-delay loop below, with the busy wait
	 * added to each half.  See the comments there. */
	for (size_t byte = 0; byte < final_byte; ++byte) {
		uint32_t in = data_in[byte];
		uint32_t out = 0;
		uint32_t count = 8U;
		uint32_t word = PIN_JTAG_SHIFT_LOW_WORD(in);
		do {
			PIN_JTAG_SHIFT_LOW_STORE(word);
			PIN_STICK_HERE(in);
			in >>= 1U;
			word = PIN_JTAG_SHIFT_LOW_WORD(in);
			uint32_t tdo = PIN_TDO_GPIO_PORT->INDR & PIN_TDO_GPIO_PIN;
			--count;
			PIN_CLK_DELAY();
			PIN_CLK_BALANCE(JTAG_SHIFT_LOW_PAD);
			PIN_JTAG_CLK_HIGH();
			PIN_CLK_DELAY();
			PIN_CLK_BALANCE(JTAG_SHIFT_HIGH_PAD);
			PIN_STICK_HERE(tdo);
			out = (out >> 1U) | ((uint32_t)(tdo != 0U) << 7U);
		} while (count);
		data_out[byte] = (uint8_t)out;
	}

	/* Bits of the final byte, then the one that carries final_tms: same shape
	 * as the no-delay loop below. */
	uint32_t tail_in = data_in[final_byte];
	uint32_t mask = 1U;
	uint32_t word = PIN_JTAG_SHIFT_LOW_WORD(tail_in);
	for (uint32_t bit = 0; bit < tail_bits; ++bit) {
		PIN_JTAG_SHIFT_LOW_STORE(word);
		PIN_STICK_HERE(tail_in);
		tail_in >>= 1U;
		word = PIN_JTAG_SHIFT_LOW_WORD(tail_in);
		uint32_t tdo = PIN_TDO_GPIO_PORT->INDR & PIN_TDO_GPIO_PIN;
		PIN_CLK_DELAY();
		PIN_JTAG_CLK_HIGH();
		PIN_CLK_DELAY();
		PIN_STICK_HERE(tdo);
		value |= mask & (0U - (tdo != 0U));
		mask <<= 1U;
	}
	PIN_JTAG_SHIFT_LOW_STORE(final_word);
	uint32_t tdo = PIN_TDO_GPIO_PORT->INDR & PIN_TDO_GPIO_PIN;
	PIN_CLK_DELAY();
	PIN_JTAG_CLK_HIGH();
	PIN_CLK_DELAY();
	PIN_STICK_HERE(tdo);
	value |= mask & (0U - (tdo != 0U));

	data_out[final_byte] = (uint8_t)value;
	PIN_SWCLK_TCK_CLR();
}

static void jtagtap_tdi_tdo_seq_no_delay(
	const uint8_t *const data_in, uint8_t *const data_out, const bool final_tms, const size_t clock_cycles)
{
	if (!clock_cycles)
		return;

	/*
	 * Only the last clock can carry final_tms, so it is peeled out of the
	 * loops.  Everything before it runs with TMS low: the falling clock edge
	 * then carries TDI in the same GPIO store and the rising edge is one store.
	 *
	 * Within a byte the loop shifts the result in from the top rather than
	 * OR-ing a rotating mask, which is one instruction less per bit and keeps
	 * the clock edges data independent.
	 */
	const size_t final_byte = (clock_cycles - 1U) >> 3U;
	const size_t tail_bits = (clock_cycles - 1U) - (final_byte << 3U);
	/* 32-bit: an 8-bit accumulator makes the compiler zero extend it once per
	 * tail bit, one cycle per clock. */
	uint32_t value = 0;
	/*
	 * Word for the one clock that drives TMS, built here, before any clock of
	 * this call: building it inside that clock's low half would make it 7
	 * instructions longer than any other low half.
	 */
	const uint32_t final_word =
		PIN_JTAG_SHIFT_LOW_TMS_WORD((data_in[final_byte] >> tail_bits) & 1U, final_tms ? 1U : 0U);

	/* Complete bytes. */
	for (size_t byte = 0; byte < final_byte; ++byte) {
		/* 32-bit on purpose: an 8-bit counter makes the compiler zero extend it
		 * once per shift, one cycle per clock. */
		uint32_t in = data_in[byte];
		uint32_t out = 0;
		uint32_t count = 8U;
		uint32_t word = PIN_JTAG_SHIFT_LOW_WORD(in);
		do {
			/* Falling edge, TDI as built during the low half of this clock. */
			PIN_JTAG_SHIFT_LOW_STORE(word);
			PIN_STICK_HERE(in);
			/*
			 * Low half: build the word for the next falling edge.  That is the
			 * only work a JTAG clock actually has to do while TCK is low, so
			 * putting it here - rather than leaving it on the far side of the
			 * rising edge - is what squares up the duty cycle.  The work itself
			 * is the same either way, so the clock rate is unaffected.
			 */
			in >>= 1U;
			word = PIN_JTAG_SHIFT_LOW_WORD(in);
			/*
			 * Sample TDO in the low half.  The target launched this bit on the
			 * falling edge above, so it is valid from there to the next falling
			 * edge: reading it here leaves the rising edge with nothing to do
			 * but the sample itself, which is the point of the exercise.
			 */
			uint32_t tdo = PIN_TDO_GPIO_PORT->INDR & PIN_TDO_GPIO_PIN;
			--count;
			PIN_JTAG_CLK_HIGH();
			PIN_CLK_BALANCE(JTAG_SHIFT_HIGH_PAD);
			PIN_STICK_HERE(tdo);
			/*
			 * High half: fold the sample in and close the loop.  Shifting into
			 * the top of `out` instead of OR-ing a rotating mask keeps this
			 * branch free and saves the mask update.
			 */
			out = (out >> 1U) | ((uint32_t)(tdo != 0U) << 7U);
		} while (count);
		data_out[byte] = (uint8_t)out;
	}

	/*
	 * Bits of the final byte, then the one that carries final_tms.  These are
	 * the leftovers a call length leaves over - up to eight clocks, and the
	 * whole call for the short shifts (a 2-bit or 5-bit transfer is all tail) -
	 * so they get the same treatment as the bulk loop above rather than a
	 * one-liner per bit.  Driving TDI and raising TCK back to back is what used
	 * to leave TCK low for about one cycle in eight here.
	 */
	uint32_t tail_in = data_in[final_byte];
	uint32_t mask = 1U;
	uint32_t word = PIN_JTAG_SHIFT_LOW_WORD(tail_in);
	for (uint32_t bit = 0; bit < tail_bits; ++bit) {
		PIN_JTAG_SHIFT_LOW_STORE(word);
		PIN_STICK_HERE(tail_in);
		tail_in >>= 1U;
		word = PIN_JTAG_SHIFT_LOW_WORD(tail_in);
		uint32_t tdo = PIN_TDO_GPIO_PORT->INDR & PIN_TDO_GPIO_PIN;
		PIN_CLK_BALANCE(JTAG_TAIL_LOW_PAD);
		PIN_JTAG_CLK_HIGH();
		PIN_CLK_BALANCE(JTAG_TAIL_HIGH_PAD);
		PIN_STICK_HERE(tdo);
		value |= mask & (0U - (tdo != 0U));
		mask <<= 1U;
	}
	/* The clock that carries final_tms: no next word to build, so the pads on
	 * both sides make up the difference. */
	PIN_JTAG_SHIFT_LOW_STORE(final_word);
	uint32_t tdo = PIN_TDO_GPIO_PORT->INDR & PIN_TDO_GPIO_PIN;
	PIN_CLK_BALANCE(JTAG_TAIL_FINAL_LOW_PAD);
	PIN_JTAG_CLK_HIGH();
	PIN_CLK_BALANCE(JTAG_TAIL_FINAL_HIGH_PAD);
	PIN_STICK_HERE(tdo);
	value |= mask & (0U - (tdo != 0U));

	data_out[final_byte] = (uint8_t)value;
	PIN_SWCLK_TCK_CLR();
}

static void jtagtap_tdi_tdo_seq(
	uint8_t *const data_out, const bool final_tms, const uint8_t *const data_in, size_t clock_cycles)
{
	PIN_TMS_SWDIO_OUT(0);
	PIN_TDI_OUT(0);
	if (target_clk_divider != UINT32_MAX)
		jtagtap_tdi_tdo_seq_clk_delay(data_in, data_out, final_tms, clock_cycles);
	else
		jtagtap_tdi_tdo_seq_no_delay(data_in, data_out, final_tms, clock_cycles);
}

static void jtagtap_tdi_seq_clk_delay(const uint8_t *const data_in, const bool final_tms, size_t clock_cycles)
{
	if (!clock_cycles)
		return;

	const size_t final_byte = (clock_cycles - 1U) >> 3U;
	const size_t tail_bits = (clock_cycles - 1U) - (final_byte << 3U);
	/* Built before any clock of this call, as in the read-write loops. */
	const uint32_t final_word =
		PIN_JTAG_SHIFT_LOW_TMS_WORD((data_in[final_byte] >> tail_bits) & 1U, final_tms ? 1U : 0U);

	/* Complete bytes, TMS low throughout. */
	for (size_t byte = 0; byte < final_byte; ++byte) {
		uint32_t in = data_in[byte];
		uint32_t word = PIN_JTAG_SHIFT_LOW_WORD(in);
		uint32_t count = 8U;
		do {
			PIN_JTAG_SHIFT_LOW_STORE(word);
			PIN_STICK_HERE(in);
			in >>= 1U;
			word = PIN_JTAG_SHIFT_LOW_WORD(in);
			PIN_CLK_DELAY();
			PIN_JTAG_CLK_HIGH();
			PIN_CLK_DELAY();
			PIN_CLK_BALANCE(JTAG_TDI_HIGH_PAD);
		} while (--count);
	}

	/* Bits of the final byte, then the one that carries final_tms: same shape
	 * as the no-delay loop below. */
	uint32_t tail_in = data_in[final_byte];
	uint32_t word = PIN_JTAG_SHIFT_LOW_WORD(tail_in);
	for (uint32_t bit = 0; bit < tail_bits; ++bit) {
		PIN_JTAG_SHIFT_LOW_STORE(word);
		PIN_STICK_HERE(tail_in);
		tail_in >>= 1U;
		word = PIN_JTAG_SHIFT_LOW_WORD(tail_in);
		PIN_CLK_DELAY();
		PIN_JTAG_CLK_HIGH();
		PIN_CLK_DELAY();
	}
	PIN_JTAG_SHIFT_LOW_STORE(final_word);
	PIN_CLK_DELAY();
	PIN_JTAG_CLK_HIGH();
	PIN_CLK_DELAY();
	PIN_SWCLK_TCK_CLR();
}

static void jtagtap_tdi_seq_no_delay(const uint8_t *const data_in, const bool final_tms, size_t clock_cycles)
{
	if (!clock_cycles)
		return;

	const size_t final_byte = (clock_cycles - 1U) >> 3U;
	const size_t tail_bits = (clock_cycles - 1U) - (final_byte << 3U);
	/* Built before any clock of this call, as in the read-write loops. */
	const uint32_t final_word =
		PIN_JTAG_SHIFT_LOW_TMS_WORD((data_in[final_byte] >> tail_bits) & 1U, final_tms ? 1U : 0U);

	/* Complete bytes, TMS low throughout. */
	for (size_t byte = 0; byte < final_byte; ++byte) {
		uint32_t in = data_in[byte];
		uint32_t word = PIN_JTAG_SHIFT_LOW_WORD(in);
		uint32_t count = 8U;
		do {
			PIN_JTAG_SHIFT_LOW_STORE(word);
			PIN_STICK_HERE(in);
			/* Low half: the next word, as in the read-write loop. */
			in >>= 1U;
			word = PIN_JTAG_SHIFT_LOW_WORD(in);
			PIN_JTAG_CLK_HIGH();
			/* High half: nothing but the loop bookkeeping, so it gets the pad. */
			PIN_CLK_BALANCE(JTAG_TDI_HIGH_PAD);
		} while (--count);
	}

	/*
	 * Bits of the final byte, then the one that carries final_tms.  Same shape
	 * as the bulk loop: the tail is all there is for the short shifts the IR
	 * and DMI scans use (4-bit IR, 2-bit DMI status, 5-6 bit address), so this
	 * is not the rare case the name suggests.
	 */
	uint32_t tail_in = data_in[final_byte];
	uint32_t word = PIN_JTAG_SHIFT_LOW_WORD(tail_in);
	for (uint32_t bit = 0; bit < tail_bits; ++bit) {
		PIN_JTAG_SHIFT_LOW_STORE(word);
		PIN_STICK_HERE(tail_in);
		tail_in >>= 1U;
		word = PIN_JTAG_SHIFT_LOW_WORD(tail_in);
		PIN_JTAG_CLK_HIGH();
		PIN_CLK_BALANCE(JTAG_TDI_TAIL_HIGH_PAD);
	}
	PIN_JTAG_SHIFT_LOW_STORE(final_word);
	PIN_CLK_BALANCE(JTAG_TDI_FINAL_HIGH_PAD);
	PIN_JTAG_CLK_HIGH();
	PIN_CLK_BALANCE(JTAG_TDI_FINAL_HIGH_PAD);
	PIN_SWCLK_TCK_CLR();
}

static void jtagtap_tdi_seq(const bool final_tms, const uint8_t *const data_in, const size_t clock_cycles)
{
	PIN_TMS_SWDIO_OUT(0);
	if (target_clk_divider != UINT32_MAX)
		jtagtap_tdi_seq_clk_delay(data_in, final_tms, clock_cycles);
	else
		jtagtap_tdi_seq_no_delay(data_in, final_tms, clock_cycles);
}

static void jtagtap_cycle_clk_delay(const size_t clock_cycles)
{
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		PIN_SWCLK_TCK_SET();
		PIN_CLK_DELAY();
		PIN_SWCLK_TCK_CLR();
		PIN_CLK_DELAY();
	}
}

static void jtagtap_cycle_no_delay(const size_t clock_cycles)
{
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		PIN_SWCLK_TCK_SET();
		PIN_CLK_BALANCE(JTAG_CYCLE_HIGH_PAD);
		PIN_SWCLK_TCK_CLR();
		/* The loop back is in the low half, so the low half gets less pad. */
		PIN_CLK_BALANCE(JTAG_CYCLE_LOW_PAD);
	}
}

static void jtagtap_cycle(const bool tms, const bool tdi, const size_t clock_cycles)
{
	jtagtap_next(tms, tdi);
	if (target_clk_divider != UINT32_MAX)
		jtagtap_cycle_clk_delay(clock_cycles - 1U);
	else
		jtagtap_cycle_no_delay(clock_cycles - 1U);
}
