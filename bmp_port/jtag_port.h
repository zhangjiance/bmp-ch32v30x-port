/*
 * SWD/JTAG pin layer for the CH32V305/CH32V30x port.
 *
 * Only these PIN_* / TIMESTAMP_GET macros are platform specific: swdptap.c and
 * jtagtap.c contain the protocol and only call into this header.  The debug
 * pins belong to the probe, so their bring-up is jtag_port_init() (jtag_port.c);
 * the board layer (boards/ch32v30x_ob/) knows nothing about SWD/JTAG.
 *
 * Pin mapping:
 *   PB14 -> SWCLK/TCK
 *   PB15 -> SWDIO/TMS   (push-pull, switched to input for SWD reads)
 *   PB13 -> TDI
 *   PB12 -> TDO
 *
 * The status LED (PA5) and BOOT button (PA6) are board pins: they are BOARD_*
 * macros in boards/ch32v30x_ob/board_config.h and are reached through the
 * board primitives board_led_write() / board_read_boot_pin().
 *
 * Ordering: no barrier is needed around the pin accesses.  They all go through
 * __IO (volatile) registers of the WCH GPIO struct, so the compiler may neither
 * reorder nor elide them.  A barrier only stopped it from keeping the port
 * address in a register, which cost several instructions per clock.
 *
 * TCK, TMS and TDI share GPIOB, and BSHR sets the pins named in its low half
 * while clearing the ones named in its high half.  The combined helpers below
 * exploit that: one store lowers TCK, holds TMS low and presents TDI together,
 * turning three APB accesses per clock into one.
 */
#ifndef __JTAG_PORT_H__
#define __JTAG_PORT_H__

#include "ch32v30x.h"
#include "ch32v30x_gpio.h"
#include "timing.h"

#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE __attribute__((always_inline)) static inline
#endif
#ifndef __WEAK
#define __WEAK __attribute__((weak))
#endif

/* Configure the SWD/JTAG pins (PB12/PB13/PB14/PB15) as outputs, idle high. */
void jtag_port_init(void);

/* ---------------- pin assignment ---------------- */
#define PIN_TCK_GPIO_PORT GPIOB
#define PIN_TCK_GPIO_PIN  GPIO_Pin_14

#define PIN_TMS_GPIO_PORT GPIOB
#define PIN_TMS_GPIO_PIN  GPIO_Pin_15

#define PIN_TDI_GPIO_PORT GPIOB
#define PIN_TDI_GPIO_PIN  GPIO_Pin_13

#define PIN_TDO_GPIO_PORT GPIOB
#define PIN_TDO_GPIO_PIN  GPIO_Pin_12

/* ---------------- TCK / SWCLK ---------------- */
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_SET(void)
{
    PIN_TCK_GPIO_PORT->BSHR = PIN_TCK_GPIO_PIN;
}

__STATIC_FORCEINLINE void PIN_SWCLK_TCK_CLR(void)
{
    PIN_TCK_GPIO_PORT->BCR = PIN_TCK_GPIO_PIN;
}

/* ---------------- TMS / SWDIO ---------------- */
/* ---------------- constant-time pin drive ----------------
 * Put `pin` in the set (bits 0-15) or the reset (bits 16-31) half of BSHR
 * according to `bit`, in three instructions and without a branch.
 *
 * The plain `if (bit)` form needs a data dependent branch: it costs 1-3 cycles
 * and a different number of cycles for a 0 than for a 1, so the TCK pulse
 * width jitters with the data being shifted and the measured duty cycle moves
 * with the payload.  Shifting the pin into the wanted half is branch free, so
 * every clock takes exactly the same number of cycles.
 */
#define PIN_BSHR_HALF(pin, bit) ((uint32_t)(pin) << ((((bit) & 1U) << 4U) ^ 16U))

__STATIC_FORCEINLINE uint32_t PIN_TMS_SWDIO_IN(void)
{
    return (PIN_TMS_GPIO_PORT->INDR & PIN_TMS_GPIO_PIN) ? 1U : 0U;
}

__STATIC_FORCEINLINE void PIN_TMS_SWDIO_OUT(const uint32_t bit)
{
    PIN_TMS_GPIO_PORT->BSHR = PIN_BSHR_HALF(PIN_TMS_GPIO_PIN, bit);
}

/*
 * Direction switching is done by changing the GPIO mode:
 * jtagtap.c only calls PIN_TMS_SWDIO_OUT() and never SET_OUT(), so TMS has to
 * be a real push-pull output by default or JTAG cannot drive the line at all.
 * swdptap.c calls SET_OUT()/SET_IN() around each turnaround, so SWD gets the
 * input mode it needs.
 */
__STATIC_FORCEINLINE void PIN_TMS_SWDIO_SET_OUT(void)
{
    GPIO_InitTypeDef gpio = { 0 };
    gpio.GPIO_Pin = PIN_TMS_GPIO_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(PIN_TMS_GPIO_PORT, &gpio);
}

__STATIC_FORCEINLINE void PIN_TMS_SWDIO_SET_IN(void)
{
    GPIO_InitTypeDef gpio = { 0 };
    gpio.GPIO_Pin = PIN_TMS_GPIO_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(PIN_TMS_GPIO_PORT, &gpio);
}

/* ---------------- TDI ---------------- */
__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void)
{
    return (PIN_TDI_GPIO_PORT->INDR & PIN_TDI_GPIO_PIN) ? 1U : 0U;
}

__STATIC_FORCEINLINE void PIN_TDI_OUT(const uint32_t bit)
{
    PIN_TDI_GPIO_PORT->BSHR = PIN_BSHR_HALF(PIN_TDI_GPIO_PIN, bit);
}

/* ---------------- TDO ---------------- */
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void)
{
    return (PIN_TDO_GPIO_PORT->INDR & PIN_TDO_GPIO_PIN) ? 1U : 0U;
}

/* ---------------- combined clock edges ----------------
 * One store per JTAG clock edge: the falling edge lower TCK, hold TMS low and
 * present TDI; the rising edge only drives TCK.
 *
 * Building the falling edge word is split from storing it: the words for two
 * consecutive clocks then overlap, which lets the shift loop spend the work of
 * building the next one inside the low half of the current clock instead of
 * leaving it all on one side of the rising edge (see jtagtap.c).
 */
#define PIN_JTAG_TCK_TMS_LOW (((uint32_t)PIN_TCK_GPIO_PIN | (uint32_t)PIN_TMS_GPIO_PIN) << 16U)

/*
 * Falling edge word: TCK low, TMS low, TDI = tdi_bit.
 *
 * Selecting between the two whole words with `0U - bit` (0 or all ones) rather
 * than shifting the TDI pin into one half of BSHR costs one instruction less in
 * the shift loops, which is one cycle per JTAG clock.
 */
#define PIN_JTAG_SHIFT_LOW_CLEAR (PIN_JTAG_TCK_TMS_LOW | ((uint32_t)PIN_TDI_GPIO_PIN << 16U))
#define PIN_JTAG_SHIFT_LOW_DIFF  ((uint32_t)PIN_TDI_GPIO_PIN | ((uint32_t)PIN_TDI_GPIO_PIN << 16U))
#define PIN_JTAG_SHIFT_LOW_WORD(tdi_bit) \
    (PIN_JTAG_SHIFT_LOW_CLEAR ^ (PIN_JTAG_SHIFT_LOW_DIFF & (0U - ((tdi_bit) & 1U))))

/* Same, but for the one clock that drives TMS to tms_bit.  Split the same way
 * so the shift loops can build it before the clock that uses it instead of
 * inside the low half of that clock. */
#define PIN_JTAG_SHIFT_LOW_TMS_DIFF ((uint32_t)PIN_TMS_GPIO_PIN | ((uint32_t)PIN_TMS_GPIO_PIN << 16U))
#define PIN_JTAG_SHIFT_LOW_TMS_WORD(tdi_bit, tms_bit) \
    (PIN_JTAG_SHIFT_LOW_WORD(tdi_bit) ^ (PIN_JTAG_SHIFT_LOW_TMS_DIFF & (0U - ((tms_bit) & 1U))))

__STATIC_FORCEINLINE void PIN_JTAG_SHIFT_LOW_STORE(const uint32_t word)
{
    PIN_TCK_GPIO_PORT->BSHR = word;
}

__STATIC_FORCEINLINE void PIN_JTAG_SHIFT_LOW(const uint32_t tdi_bit)
{
    PIN_JTAG_SHIFT_LOW_STORE(PIN_JTAG_SHIFT_LOW_WORD(tdi_bit));
}

/* Same, but for the one clock that drives TMS to tms_bit. */
__STATIC_FORCEINLINE void PIN_JTAG_SHIFT_LOW_TMS(const uint32_t tdi_bit, const uint32_t tms_bit)
{
    PIN_JTAG_SHIFT_LOW_STORE((uint32_t)PIN_TCK_GPIO_PIN << 16U | PIN_BSHR_HALF(PIN_TDI_GPIO_PIN, tdi_bit) |
                             PIN_BSHR_HALF(PIN_TMS_GPIO_PIN, tms_bit));
}

__STATIC_FORCEINLINE void PIN_JTAG_CLK_HIGH(void)
{
    PIN_TCK_GPIO_PORT->BSHR = PIN_TCK_GPIO_PIN;
}

/* SWD: the falling edge and the data bit go out in a single store too. */
__STATIC_FORCEINLINE void PIN_SWD_SHIFT_LOW(const uint32_t bit)
{
    if (bit) {
        PIN_TCK_GPIO_PORT->BSHR = ((uint32_t)PIN_TCK_GPIO_PIN << 16U) | (uint32_t)PIN_TMS_GPIO_PIN;
    } else {
        PIN_TCK_GPIO_PORT->BSHR = ((uint32_t)PIN_TCK_GPIO_PIN | (uint32_t)PIN_TMS_GPIO_PIN) << 16U;
    }
}

/* ---------------- bit-clock delay ----------------
 * One delay edge, `iteration` iterations of target_clk_divider.  A plain
 * counter plus an empty compiler barrier is used instead of a volatile
 * counter: the volatile form made the loop spill and reload the counter every
 * iteration (6 cycles) where this one costs 2-3.
 */
#define PIN_CLK_DELAY_ITERS(iterations) \
    do { \
        for (uint32_t pin_clk_delay_counter_ = (iterations); pin_clk_delay_counter_ > 0U; \
             --pin_clk_delay_counter_) \
            __asm__ volatile("" ::: "memory"); \
    } while (0)

#define PIN_CLK_DELAY() PIN_CLK_DELAY_ITERS(target_clk_divider)

/* ---------------- duty cycle trim ----------------
 * `cycles` nops, one cycle each (7 ns at 144 MHz), for the half of a TCK period
 * that the release disassembly shows to be the shorter one.  It has to be nops:
 * the imbalance left over once the per-bit work is split evenly is 1-3 cycles,
 * and one PIN_CLK_DELAY_ITERS() iteration already costs 3.
 *
 * The count is pasted straight into an assembler `.rept`, so it has to be a
 * bare decimal number: no U suffix, no arithmetic.
 */
/*
 * Tie a value to its place in the instruction stream.  The compiler is free to
 * hoist plain arithmetic across the GPIO stores, which silently moves work from
 * one half of the TCK period to the other; this says "the value changed here",
 * so nothing that uses it can be moved above this point and nothing that
 * produced it can be moved below.  It emits no code.
 */
#define PIN_STICK_HERE(var) __asm__ volatile("" : "+r"(var))

#define PIN_CLK_BALANCE_(cycles) __asm__ volatile(".rept " #cycles "\nnop\n.endr")
/* Two levels so a macro such as JTAG_SHIFT_LOW_PAD is expanded, not spelled out. */
#define PIN_CLK_BALANCE(cycles) PIN_CLK_BALANCE_(cycles)

/* ---------------- nTRST / nRESET (not wired on this board) ---------------- */
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN(void)
{
    return 0U;
}

__STATIC_FORCEINLINE void PIN_nTRST_OUT(uint32_t bit)
{
    (void)bit;
}

__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void)
{
    return 0U;
}

__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit)
{
    (void)bit;
}

__STATIC_INLINE uint32_t TIMESTAMP_GET(void)
{
    return platform_time_ms();
}

#endif /* __JTAG_PORT_H__ */
