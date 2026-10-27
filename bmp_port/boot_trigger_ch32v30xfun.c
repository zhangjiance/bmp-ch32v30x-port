/*
 * boot_trigger_ch32v30xfun.c
 *
 * CH32V30x implementation of the cross-reset DFU request, register level
 * (ch32fun backend).
 *
 * CH32V30x has no general-purpose retention register in a backup domain, so a
 * battery-backed BKP data register is used.  It survives NVIC_SystemReset() and
 * is cleared by a power cycle (which is fine: after a power cycle the bootloader
 * re-evaluates the boot pin / application signature anyway).
 */
#include "boot_trigger_port.h"
#include "boot_protocol.h"

#include "ch32fun.h"

/* BOOT_TRIGGER_BKP_DR is the WCH "BKP_DR1" style offset (BKP base + 4), i.e.
 * the first data register, which ch32fun names DATAR1 directly. */
_Static_assert(BOOT_TRIGGER_BKP_DR == 0x0004, "boot_trigger_ch32v30xfun.c hardcodes BKP->DATAR1");

static void bkp_access_enable(void)
{
    RCC->APB1PCENR |= RCC_APB1Periph_PWR | RCC_APB1Periph_BKP;
    PWR->CTLR |= (1U << 8); /* BKPWEN: backup domain write enable */
}

void boot_trigger_request(void)
{
    bkp_access_enable();
    BKP->DATAR1 = BOOT_TRIGGER_MAGIC;
}

bool boot_trigger_check_and_clear(void)
{
    bool triggered;

    bkp_access_enable();
    triggered = (BKP->DATAR1 == BOOT_TRIGGER_MAGIC);
    if (triggered) {
        BKP->DATAR1 = 0;
    }
    return triggered;
}

void boot_system_reset(void)
{
    __asm volatile("fence.i");
    NVIC_SystemReset();
    while (1) {
    }
}

void boot_trigger_reboot_to_boot(void)
{
    boot_trigger_request();
    boot_system_reset();
}
