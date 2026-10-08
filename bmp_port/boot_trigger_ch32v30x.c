/*
 * boot_trigger_ch32v30x.c
 *
 * CH32V30x implementation of the cross-reset DFU request.
 *
 * HPM uses BGPR/PDGO retention registers; CH32V30x has none, so a
 * battery-backed BKP data register is used.  It survives NVIC_SystemReset()
 * and is cleared by a power cycle (which is fine: after a power cycle the
 * bootloader re-evaluates the boot pin / application signature anyway).
 */
#include "boot_trigger_port.h"
#include "boot_protocol.h"

#include "debug.h"      /* ch32v30x.h -> PWR / BKP / RCC / core_riscv */

static void bkp_access_enable(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
}

void boot_trigger_request(void)
{
    bkp_access_enable();
    BKP_WriteBackupRegister(BOOT_TRIGGER_BKP_DR, BOOT_TRIGGER_MAGIC);
}

bool boot_trigger_check_and_clear(void)
{
    bool triggered;

    bkp_access_enable();
    triggered = (BKP_ReadBackupRegister(BOOT_TRIGGER_BKP_DR) == BOOT_TRIGGER_MAGIC);
    if (triggered) {
        BKP_WriteBackupRegister(BOOT_TRIGGER_BKP_DR, 0);
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
