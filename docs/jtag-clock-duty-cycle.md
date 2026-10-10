# JTAG clock duty cycle: why one clock in eight measured 86%

[English](jtag-clock-duty-cycle.md) | [简体中文](jtag-clock-duty-cycle.zh-CN.md)

[Back to the project README](../README.md)

Development note. This is the record of the remaining JTAG clocks that measured
~86% duty cycle after the bulk shift loops had already been squared up to 50%,
where they came from, and what the fix costs. The timing model itself is
summarised in `bmp_port/timing_ch32.c`; the reasoning lives here.

## Symptom

With the shift loops rewritten (see below), most TCK clocks on the scope measured
50%, but some measured **86%** - one clock in about eight - and a few paths were
skewed the other way, 86% **low**. The 86% figure is suspiciously close to
`6 / 7`, which is what a clock looks like when one of its two halves contains no
work at all.

## Root cause

Two independent single-bit paths, skewing in opposite directions. Neither is
rare; together they cover a large share of the clocks in a scan.

### (a) The tail bits of every bulk shift — TCK high ~86%

`jtagtap_tdi_tdo_seq()` and `jtagtap_tdi_seq()` split the transfer into whole
bytes plus a tail of `clock_cycles % 8` bits, and only the byte loops had been
rewritten. The tail still used the per-bit helpers back to back:

```c
/* before */
PIN_JTAG_SHIFT_LOW((last >> bit) & 1U);   /* falling edge */
PIN_JTAG_CLK_HIGH();                      /* rising edge, immediately after */
if (PIN_TDO_IN())
    value |= mask;
```

Two GPIO stores with nothing between them: TCK low for 0-1 cycles, high for 6-7.
The same shape sat on the final clock, which is the one that drives `final_tms`,
and on the tails of `jtagtap_tdi_seq()`.

How often this happens, from the `clock_cycles` the upper layers actually pass
(`adiv5_jtag.c`, `riscv_jtag_dtm.c`):

| Call | Tail clocks | Share of that call |
|---|---|---|
| 32-bit (RISC-V DMI data, IDCODE) | 8 | 25% |
| 35-bit (ADIv5 DP/AP) | 3 | 9% |
| 2-bit (DMI status), 4-5 bit (IR), 5-6 bit (`abits`) | all of them | **100%** |

This port talks to a RISC-V DTM, where one DMI access is `2 + 32 + abits`
clocks — so roughly **40% of the clocks in a DMI access were skewed**, not one in
eight. The "occasional" reading was the scope triggering on a long scan, where
the tail is a small fraction.

### (b) The single-clock paths — TCK low ~86%

`jtagtap_next()` and `jtagtap_cycle()` are called once per clock, so the low half
of their clock is *the caller's*: the function return, whatever the caller does
with the bit, and the call back in all happen with TCK low.

```c
/* before */
PIN_SWCLK_TCK_SET();
const uint16_t result = (uint16_t)PIN_TDO_IN();   /* high half: 2-3 instructions */
PIN_SWCLK_TCK_CLR();
return result != 0;                               /* low half: ~12+ cycles, in the caller */
```

~12 cycles low against 2-3 high. These paths are only used during enumeration
(the 51-clock `jtagtap_cycle()` reset, `jtag_read_irs()`), which is why they were
easy to miss.

## The fix

The general rule, applied to every path: **a JTAG clock has to do a fixed amount
of work, and it is free to choose which half of the period does it.** Building
the word for the *next* falling edge and sampling TDO are both safe to do while
TCK is low — the target launched the TDO bit on the falling edge and holds it
until the next one — so moving them there leaves the rising edge nothing but the
store. The clock rate is unchanged; only the shape changes.

```327:357:bmp_port/jtagtap.c
		do {
			/* Falling edge, TDI as built during the low half of this clock. */
			PIN_JTAG_SHIFT_LOW_STORE(word);
			PIN_STICK_HERE(in);
			/*
			 * Low half: build the word for the next falling edge.  ...
			 */
			in >>= 1U;
			word = PIN_JTAG_SHIFT_LOW_WORD(in);
```

Three specific changes on top of that:

1. **The tail is no longer a special case.** It uses the same structure as the
   byte loop, including the branch-free TDO accumulation and the 32-bit
   accumulator (an 8-bit one makes the compiler emit a `zext.b` once per tail
   bit — one cycle per clock).
2. **The `final_tms` word is built at the top of the function**, before any clock
   of the call, using the new `PIN_JTAG_SHIFT_LOW_TMS_WORD()` macro. It costs 7
   instructions; building it inside the low half of the clock that uses it would
   have made that one low half 7 cycles longer than every other one.
3. **The single-clock paths get pads.** `jtagtap_cycle_no_delay()` pads both
   halves (the loop back edge lands in the low half, so they are not equal);
   `jtagtap_next_no_delay()` pads the high half to roughly match the caller.

## Measured

Counting instructions in `objdump -d` across each rising edge, at 144 MHz:

| Path | low / high | duty |
|---|---|---|
| `tdi_tdo_seq` byte loop | 7 / 7 | 50% |
| `tdi_tdo_seq` tail bits | 9 / 9 | 50% |
| `tdi_tdo_seq` final (`final_tms`) clock | 8 / 8 | 50% |
| `tdi_seq` byte loop | 5 / 5 | 50% |
| `tdi_seq` tail bits | 5 / 5 | 50% |
| `tdi_seq` final clock | 6 / 6 | 50% |
| `tms_seq` | 7 / 7 | 50% |
| `jtagtap_cycle` | 8 / 8 | 50% |
| `jtagtap_next` | caller / ~13 | ~40% (was ~14%) |

Per-clock cost went from 13-14 to 16 instructions on `tdi_tdo_seq`, which is why
`USED_SWD_CYCLES` in `timing_ch32.c` is now 16. The loops are not actually
slower: the old branch that picked the TDI level mispredicted on about half the
bits, and it is gone now.

## Re-tuning

The pads at the top of `jtagtap.c` absorb the 1-3 cycle residue the loop
structure cannot express; they are finer than one `PIN_CLK_DELAY_ITERS()` step
(3 cycles). Recount them from `objdump -d` after touching a loop or the compiler
flags:

```
/opt/Toolchain/RISC-V_Embedded_GCC12/bin/riscv-wch-elf-objdump -d \
  build/ch32v30x_ob-release/CMakeFiles/bmp-ch32v30x-port.elf.dir/bmp_port/jtagtap.c.obj
```

Count the instructions between the store that clears TCK and the store that sets
it, and between that one and the next falling edge; add the difference to the
shorter side.

## Residual risks

- **`jtagtap_next()` is estimated.** Its low half is the caller's, and the
  callers live in the vendor's own code, so `JTAG_NEXT_HIGH_PAD` is sized for a
  typical call site rather than measured. If those clocks still look off on a
  scope, that single constant is the knob. It only runs during enumeration.
- **`swdptap.c` has the same problem, untouched.** `PIN_SWD_SHIFT_LOW()` still
  branches on the data bit, and the SWD loops have tail bits and single-clock
  paths of their own.
- **The pads are compiler-specific.** They were counted under
  `-DCMAKE_BUILD_TYPE=ch32v30x_ob-release`; a different optimisation level moves
  them.
