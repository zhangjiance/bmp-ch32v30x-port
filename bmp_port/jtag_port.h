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
__STATIC_FORCEINLINE uint32_t PIN_TMS_SWDIO_IN(void)
{
    return (PIN_TMS_GPIO_PORT->INDR & PIN_TMS_GPIO_PIN) ? 1U : 0U;
}

__STATIC_FORCEINLINE void PIN_TMS_SWDIO_OUT(uint32_t bit)
{
    if (bit) {
        PIN_TMS_GPIO_PORT->BSHR = PIN_TMS_GPIO_PIN;
    } else {
        PIN_TMS_GPIO_PORT->BCR = PIN_TMS_GPIO_PIN;
    }
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

__STATIC_FORCEINLINE void PIN_TDI_OUT(uint32_t bit)
{
    if (bit) {
        PIN_TDI_GPIO_PORT->BSHR = PIN_TDI_GPIO_PIN;
    } else {
        PIN_TDI_GPIO_PORT->BCR = PIN_TDI_GPIO_PIN;
    }
}

/* ---------------- TDO ---------------- */
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void)
{
    return (PIN_TDO_GPIO_PORT->INDR & PIN_TDO_GPIO_PIN) ? 1U : 0U;
}

/* ---------------- combined clock edges ----------------
 * One store per JTAG clock edge: the falling edge lower TCK, hold TMS low and
 * present TDI; the rising edge only drives TCK.
 */
__STATIC_FORCEINLINE void PIN_JTAG_SHIFT_LOW(const uint32_t tdi_bit)
{
    /* BSHR: low half sets, high half clears. */
    const uint32_t tck_tms_low = ((uint32_t)PIN_TCK_GPIO_PIN | (uint32_t)PIN_TMS_GPIO_PIN) << 16U;
    if (tdi_bit) {
        PIN_TCK_GPIO_PORT->BSHR = tck_tms_low | (uint32_t)PIN_TDI_GPIO_PIN;
    } else {
        PIN_TCK_GPIO_PORT->BSHR = tck_tms_low | ((uint32_t)PIN_TDI_GPIO_PIN << 16U);
    }
}

/* Same, but for the one clock that drives TMS to tms_bit. */
__STATIC_FORCEINLINE void PIN_JTAG_SHIFT_LOW_TMS(const uint32_t tdi_bit, const uint32_t tms_bit)
{
    uint32_t value = (uint32_t)PIN_TCK_GPIO_PIN << 16U;
    if (tdi_bit) {
        value |= (uint32_t)PIN_TDI_GPIO_PIN;
    } else {
        value |= (uint32_t)PIN_TDI_GPIO_PIN << 16U;
    }
    if (tms_bit) {
        value |= (uint32_t)PIN_TMS_GPIO_PIN;
    } else {
        value |= (uint32_t)PIN_TMS_GPIO_PIN << 16U;
    }
    PIN_TCK_GPIO_PORT->BSHR = value;
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
