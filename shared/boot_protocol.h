/*
 * boot_protocol.h
 *
 * Layout / hand-shake contract shared by the DFU bootloader and the
 * application.  Deliberately SDK-free so it can be included from both projects
 * with a plain -I.  Keep the bootloader's and the application's copy identical.
 *
 * NOTE ON PORTABILITY
 *   The bootloader region and the hand-shake are fixed contracts.
 *   The *total* flash size is a chip property: it defaults to 128 KB (CH32V305)
 *   and can be overridden per board/chip on the compiler command line with
 *     -DBOOT_FLASH_SIZE=$((64*1024))      # e.g. CH32L103
 */
#ifndef BOOT_PROTOCOL_H
#define BOOT_PROTOCOL_H

#include <stdint.h>

/* ------------------------------------------------------------------ *
 * DFU / DfuSe
 * ------------------------------------------------------------------ */
#define BOOT_DFU_XFER_SIZE   (4096U)    /* == wTransferSize                */
#define BOOT_DFU_SECTOR_SIZE (4096U)    /* DfuSe memory-map sector         */

/* ------------------------------------------------------------------ *
 * Flash partition
 *   [0x00000000 .. 0x00003000)  bootloader  (12 KB)
 *   [0x00003000 .. end)         application (rest of flash)
 *
 * 12 KB is what the bootloader image needs plus headroom (~9.8 KB release), and
 * it is the smallest size that keeps the application region a whole number of
 * DfuSe sectors on both a 128 KB part (116 KB) and a 64 KB CH32L103 (52 KB).
 * It used to be 32 KB, which is half of the L103's flash.  0x3000 is also a
 * multiple of the 256 byte flash page and of the 4 KB DfuSe sector, so neither
 * the bootloader nor the DfuSe map straddles a boundary.
 * ------------------------------------------------------------------ */
#define BOOT_PARTITION_SIZE  (12UL * 1024UL)          /* bootloader region   */
#define BOOT_APP_OFFSET      (BOOT_PARTITION_SIZE)    /* 0x00003000          */
#define BOOT_FLASH_BASE      (0x08000000UL)           /* physical alias      */

#ifndef BOOT_FLASH_SIZE
#define BOOT_FLASH_SIZE      (128UL * 1024UL)         /* CH32V305; see note  */
#endif

/* DfuSe lists the application in whole BOOT_DFU_SECTOR_SIZE sectors, so the
 * size is rounded down to a sector boundary: 12 KB of bootloader leaves 116 KB
 * of a 128 KB part (29 sectors) and 52 KB of a 64 KB part (13 sectors).  Both
 * are already aligned; the mask only keeps it that way if the numbers change. */
#define BOOT_APP_START       (BOOT_FLASH_BASE + BOOT_APP_OFFSET)   /* 0x08003000 */
#define BOOT_APP_SIZE        ((BOOT_FLASH_SIZE - BOOT_APP_OFFSET) & ~(BOOT_DFU_SECTOR_SIZE - 1UL))

/* ------------------------------------------------------------------ *
 * APP -> boot hand-shake
 *
 * CH32V30x has no general-purpose retention register in a backup domain, so the
 * request is kept in a BKP data register which survives a software reset (but
 * not a power cycle).  The application writes BOOT_TRIGGER_MAGIC before
 * resetting; the bootloader reads and clears it.  The concrete access lives in
 * port/<chip>/boot_trigger_<chip>.c.
 * ------------------------------------------------------------------ */
#define BOOT_TRIGGER_BKP_DR  ((uint16_t)0x0004) /* == BKP_DR1 (ch32v30x_bkp.h) */
#define BOOT_TRIGGER_MAGIC   ((uint16_t)0xDF01) /* "DFU" request marker         */

#endif /* BOOT_PROTOCOL_H */
