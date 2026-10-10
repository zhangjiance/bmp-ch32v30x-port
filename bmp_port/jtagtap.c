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
	const uint16_t result = (uint16_t)PIN_TDO_IN();
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
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		const bool state = tms_states & 1U;
		PIN_TMS_SWDIO_OUT(state);
		PIN_SWCLK_TCK_SET();
		PIN_CLK_DELAY();
		tms_states >>= 1U;
		PIN_SWCLK_TCK_CLR();
		PIN_CLK_DELAY();
	}
}

static void jtagtap_tms_seq_no_delay(uint32_t tms_states, const size_t clock_cycles)
{
	bool state = tms_states & 1U;
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		PIN_TMS_SWDIO_OUT(state);
		PIN_SWCLK_TCK_SET();
		/* Block the compiler from re-ordering the TMS states calculation to preserve timings */
		tms_states >>= 1U;
		state = tms_states & 1U;
		PIN_SWCLK_TCK_CLR();
	}
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
	uint8_t value = 0;

	/* Complete bytes. */
	for (size_t byte = 0; byte < final_byte; ++byte) {
		const uint8_t in = data_in[byte];
		uint8_t out = 0;
		for (size_t bit = 0; bit < 8U; ++bit) {
			PIN_JTAG_SHIFT_LOW((in >> bit) & 1U);
			PIN_CLK_DELAY();
			PIN_JTAG_CLK_HIGH();
			PIN_CLK_DELAY();
			if (PIN_TDO_IN())
				out |= (uint8_t)(1U << bit);
		}
		data_out[byte] = out;
	}

	/* Bits of the final byte, then the one that carries final_tms. */
	const uint8_t last = data_in[final_byte];
	for (size_t bit = 0; bit < tail_bits; ++bit) {
		PIN_JTAG_SHIFT_LOW((last >> bit) & 1U);
		PIN_CLK_DELAY();
		PIN_JTAG_CLK_HIGH();
		PIN_CLK_DELAY();
		if (PIN_TDO_IN())
			value |= (uint8_t)(1U << bit);
	}
	PIN_JTAG_SHIFT_LOW_TMS((last >> tail_bits) & 1U, final_tms ? 1U : 0U);
	PIN_CLK_DELAY();
	PIN_JTAG_CLK_HIGH();
	PIN_CLK_DELAY();
	if (PIN_TDO_IN())
		value |= (uint8_t)(1U << tail_bits);

	data_out[final_byte] = value;
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
	 * then carries TDI in the same GPIO store, the rising edge is one store,
	 * and the loop needs no bit/byte arithmetic beyond a rotating mask.
	 */
	const size_t final_byte = (clock_cycles - 1U) >> 3U;
	const size_t tail_bits = (clock_cycles - 1U) - (final_byte << 3U);
	uint8_t value = 0;

	/* Complete bytes. */
	for (size_t byte = 0; byte < final_byte; ++byte) {
		uint8_t in = data_in[byte];
		uint8_t out = 0;
		uint32_t mask = 1U;
		do {
			PIN_JTAG_SHIFT_LOW(in & 1U);
			PIN_JTAG_CLK_HIGH();
			if (PIN_TDO_IN())
				out |= (uint8_t)mask;
			in >>= 1U;
			mask <<= 1U;
		} while (mask <= 0x80U);
		data_out[byte] = out;
	}

	/* Bits of the final byte, then the one that carries final_tms. */
	const uint8_t last = data_in[final_byte];
	uint8_t mask = 1U;
	for (size_t bit = 0; bit < tail_bits; ++bit) {
		PIN_JTAG_SHIFT_LOW((last >> bit) & 1U);
		PIN_JTAG_CLK_HIGH();
		if (PIN_TDO_IN())
			value |= mask;
		mask = (uint8_t)(mask << 1U);
	}
	PIN_JTAG_SHIFT_LOW_TMS((last >> tail_bits) & 1U, final_tms ? 1U : 0U);
	PIN_JTAG_CLK_HIGH();
	if (PIN_TDO_IN())
		value |= mask;

	data_out[final_byte] = value;
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

	/* Complete bytes, TMS low throughout. */
	for (size_t byte = 0; byte < final_byte; ++byte) {
		const uint8_t in = data_in[byte];
		for (size_t bit = 0; bit < 8U; ++bit) {
			PIN_JTAG_SHIFT_LOW((in >> bit) & 1U);
			PIN_CLK_DELAY();
			PIN_JTAG_CLK_HIGH();
			PIN_CLK_DELAY();
		}
	}

	/* Bits of the final byte, then the one that carries final_tms. */
	const uint8_t last = data_in[final_byte];
	for (size_t bit = 0; bit < tail_bits; ++bit) {
		PIN_JTAG_SHIFT_LOW((last >> bit) & 1U);
		PIN_CLK_DELAY();
		PIN_JTAG_CLK_HIGH();
		PIN_CLK_DELAY();
	}
	PIN_JTAG_SHIFT_LOW_TMS((last >> tail_bits) & 1U, final_tms ? 1U : 0U);
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

	/* Complete bytes, TMS low throughout. */
	for (size_t byte = 0; byte < final_byte; ++byte) {
		uint8_t in = data_in[byte];
		uint32_t mask = 1U;
		do {
			PIN_JTAG_SHIFT_LOW(in & 1U);
			PIN_JTAG_CLK_HIGH();
			in >>= 1U;
			mask <<= 1U;
		} while (mask <= 0x80U);
	}

	/* Bits of the final byte, then the one that carries final_tms. */
	const uint8_t last = data_in[final_byte];
	for (size_t bit = 0; bit < tail_bits; ++bit) {
		PIN_JTAG_SHIFT_LOW((last >> bit) & 1U);
		PIN_JTAG_CLK_HIGH();
	}
	PIN_JTAG_SHIFT_LOW_TMS((last >> tail_bits) & 1U, final_tms ? 1U : 0U);
	PIN_JTAG_CLK_HIGH();
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
		PIN_SWCLK_TCK_CLR();
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
