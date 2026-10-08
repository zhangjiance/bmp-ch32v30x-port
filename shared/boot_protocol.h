/*
 * boot_protocol.h
 *
 * Layout / hand-shake contract shared by the DFU bootloader and the
 * application.  Deliberately SDK-free so it can be included from both projects
 * with a plain -I.
 *
 * NOTE ON PORTABILITY
 *   The bootloader region and the hand-shake are fixed contracts.
 *   The *total* flash size is a chip property: it defaults to 128 KB (CH32V305)
 *   and can be overridden per board/chip on the compiler command line with
 *     -DBOOT_FLASH_SIZE=$((288*1024))     # e.g. CH32V307
 */
#ifndef BOOT_PROTOCOL_H
#define BOOT_PROTOCOL_H

#include <stdint.h>

/* ------------------------------------------------------------------ *
 * Flash partition
 *   [0x00000000 .. 0x00008000)  bootloader  (32 KB)
 *   [0x00008000 .. end)         application (rest of flash)
 * ------------------------------------------------------------------ */
#define BOOT_PARTITION_SIZE  (32UL * 1024UL)          /* bootloader region   */
#define BOOT_APP_OFFSET      (BOOT_PARTITION_SIZE)    /* 0x00008000          */
#define BOOT_FLASH_BASE      (0x08000000UL)           /* physical alias      */

#ifndef BOOT_FLASH_SIZE
#define BOOT_FLASH_SIZE      (128UL * 1024UL)         /* CH32V305; see note  */
#endif

#define BOOT_APP_START       (BOOT_FLASH_BASE + BOOT_APP_OFFSET)   /* 0x08008000 */
#define BOOT_APP_SIZE        (BOOT_FLASH_SIZE - BOOT_APP_OFFSET)

/* ------------------------------------------------------------------ *
 * DFU / DfuSe
 * ------------------------------------------------------------------ */
#define BOOT_DFU_XFER_SIZE   (4096U)    /* == wTransferSize                */
#define BOOT_DFU_SECTOR_SIZE (4096U)    /* DfuSe memory-map sector         */

/* ------------------------------------------------------------------ *
 * APP -> boot hand-shake
 *
 * CH32V30x has no HPM-style BGPR/PDGO retention register, so the request is
 * kept in a BKP data register which survives a software reset (but not a
 * power cycle).  The application writes BOOT_TRIGGER_MAGIC before resetting;
 * the bootloader reads and clears it.  The concrete access lives in
 * port/<chip>/boot_trigger_<chip>.c.
 * ------------------------------------------------------------------ */
#define BOOT_TRIGGER_BKP_DR  ((uint16_t)0x0004) /* == BKP_DR1 (ch32v30x_bkp.h) */
#define BOOT_TRIGGER_MAGIC   ((uint16_t)0xDF01) /* "DFU" request marker         */

#endif /* BOOT_PROTOCOL_H */
