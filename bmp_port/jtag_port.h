/*
 * SWD/JTAG pin layer for the CH32V305/CH32V30x port.
 *
 * Only these PIN_* / LED_* / TIMESTAMP_GET macros are platform specific:
 * swdptap.c and jtagtap.c contain the protocol and only call into this header.
 *
 * Pin mapping:
 *   PB14 -> SWCLK/TCK
 *   PB15 -> SWDIO/TMS   (driven open-drain, see board.c)
 *   PB13 -> TDI
 *   PB12 -> TDO
 *   PA5  -> status LED
 *   PA6  -> BOOT button (to GND, active low)
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

/* ---------------- pin assignment ---------------- */
#define PIN_TCK_GPIO_PORT GPIOB
#define PIN_TCK_GPIO_PIN  GPIO_Pin_14

#define PIN_TMS_GPIO_PORT GPIOB
#define PIN_TMS_GPIO_PIN  GPIO_Pin_15

#define PIN_TDI_GPIO_PORT GPIOB
#define PIN_TDI_GPIO_PIN  GPIO_Pin_13

#define PIN_TDO_GPIO_PORT GPIOB
#define PIN_TDO_GPIO_PIN  GPIO_Pin_12

/*
 * Status LED: PA5 sits on the cathode side of the LED, whose anode goes to 3V3
 * through its series resistor, so the pin sinks the current - driving it low
 * lights the LED and driving it high turns it off.
 */
#define PIN_LED_GPIO_PORT  GPIOA
#define PIN_LED_GPIO_PIN   GPIO_Pin_5
#define PIN_LED_ACTIVE_LOW 1

/* BOOT button: PA6 to GND with the internal pull-up, so pressed reads low. */
#define PIN_BOOT_GPIO_PORT       GPIOA
#define PIN_BOOT_GPIO_PIN        GPIO_Pin_6
#define PIN_BOOT_ACTIVE_LOW      1

/* Compiler barrier, keeps the GPIO accesses in program order. */
#define PIN_BARRIER() __asm__ volatile("" ::: "memory")

/* ---------------- TCK / SWCLK ---------------- */
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_SET(void)
{
    PIN_TCK_GPIO_PORT->BSHR = PIN_TCK_GPIO_PIN;
    PIN_BARRIER();
}

__STATIC_FORCEINLINE void PIN_SWCLK_TCK_CLR(void)
{
    PIN_TCK_GPIO_PORT->BCR = PIN_TCK_GPIO_PIN;
    PIN_BARRIER();
}

/* ---------------- TMS / SWDIO ----------------
 * SWDIO is driven open-drain: writing 1 releases the line so the target can
 * drive it, which means switching direction needs no GPIO mode change.
 */
__STATIC_FORCEINLINE uint32_t PIN_TMS_SWDIO_IN(void)
{
    uint32_t sta = (PIN_TMS_GPIO_PORT->INDR & PIN_TMS_GPIO_PIN) ? 1U : 0U;
    PIN_BARRIER();
    return sta;
}

__STATIC_FORCEINLINE void PIN_TMS_SWDIO_OUT(uint32_t bit)
{
    if (bit) {
        PIN_TMS_GPIO_PORT->BSHR = PIN_TMS_GPIO_PIN;
    } else {
        PIN_TMS_GPIO_PORT->BCR = PIN_TMS_GPIO_PIN;
    }
    PIN_BARRIER();
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
    PIN_BARRIER();
}

__STATIC_FORCEINLINE void PIN_TMS_SWDIO_SET_IN(void)
{
    GPIO_InitTypeDef gpio = { 0 };
    gpio.GPIO_Pin = PIN_TMS_GPIO_PIN;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(PIN_TMS_GPIO_PORT, &gpio);
    PIN_BARRIER();
}

/* ---------------- TDI ---------------- */
__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void)
{
    uint32_t sta = (PIN_TDI_GPIO_PORT->INDR & PIN_TDI_GPIO_PIN) ? 1U : 0U;
    PIN_BARRIER();
    return sta;
}

__STATIC_FORCEINLINE void PIN_TDI_OUT(uint32_t bit)
{
    if (bit) {
        PIN_TDI_GPIO_PORT->BSHR = PIN_TDI_GPIO_PIN;
    } else {
        PIN_TDI_GPIO_PORT->BCR = PIN_TDI_GPIO_PIN;
    }
    PIN_BARRIER();
}

/* ---------------- TDO ---------------- */
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void)
{
    uint32_t sta = (PIN_TDO_GPIO_PORT->INDR & PIN_TDO_GPIO_PIN) ? 1U : 0U;
    PIN_BARRIER();
    return sta;
}

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

/*
 * ---------------- LED ----------------
 * The single status LED is owned by the board layer - board_led_write() in
 * boards/ch32v30x_bmp/board.c, driven from the 100 ms timer so it is dark
 * without an attached target, solid while one is stopped and blinking while it
 * runs.  Only the pin definition and PIN_LED_ACTIVE_LOW above are needed here.
 */

__STATIC_INLINE uint32_t TIMESTAMP_GET(void)
{
    return platform_time_ms();
}

/* ---------------- BOOT button ----------------
 * Sampled by the 100 ms TIM3 interrupt in board.c while the main loop is parked
 * in gdb_if_getchar() waiting for a GDB command. */
__STATIC_INLINE uint32_t PIN_BOOT_PRESSED(void)
{
    uint32_t sta = (PIN_BOOT_GPIO_PORT->INDR & PIN_BOOT_GPIO_PIN) ? 1U : 0U;
    PIN_BARRIER();
    return PIN_BOOT_ACTIVE_LOW ? (sta == 0U) : (sta != 0U);
}

#endif /* __JTAG_PORT_H__ */
