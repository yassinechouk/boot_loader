#ifndef METADATA_H
#define METADATA_H

#include <stdint.h>

/* =========================================================
 * Flash memory layout -- STM32L476RG
 *
 * 1 MB of flash, organized in two 512 KB banks,
 * uniform 2 KB pages.
 *
 *   0x08000000  +------------------+
 *               |   BOOTLOADER     |  32 KB
 *   0x08008000  +------------------+
 *               |     SLOT A       |  480 KB
 *   0x08080000  +------------------+
 *               |     SLOT B       |  480 KB
 *   0x080FF000  +------------------+
 *               |  METADATA A      |  2 KB
 *   0x080FF800  +------------------+
 *               |  METADATA B      |  2 KB
 *   0x08100000  +------------------+
 * ========================================================= */
#define FLASH_BASE_ADDR         0x08000000UL
#define FLASH_TOTAL_SIZE        (1024U * 1024U)
#define FLASH_PAGE_SIZE         2048U

#define BOOTLOADER_ADDR         0x08000000UL
#define BOOTLOADER_SIZE         (32U * 1024U)

#define SLOT_A_ADDR             0x08008000UL
#define SLOT_B_ADDR             0x08080000UL
#define SLOT_SIZE               (480U * 1024U)

#define META_PAGE_A_ADDR        0x080FF000UL
#define META_PAGE_B_ADDR        0x080FF800UL

/* =========================================================
 * Slot identifiers
 * ========================================================= */
#define SLOT_A                  0U
#define SLOT_B                  1U

#define SLOT_ADDR(s)            (((s) == SLOT_A) ? SLOT_A_ADDR : SLOT_B_ADDR)
#define OTHER_SLOT(s)           (((s) == SLOT_A) ? SLOT_B : SLOT_A)

/* =========================================================
 * Firmware states
 *
 * STATE_TESTING is the intermediate state that makes rollback
 * possible: a valid CRC proves the integrity of the image, not
 * that it works correctly. The application must confirm its own
 * successful boot by transitioning to STATE_VALID.
 * ========================================================= */
typedef enum {
    STATE_EMPTY       = 0x00U,   /* slot empty or never programmed        */
    STATE_IN_PROGRESS = 0x01U,   /* transfer started, not yet complete    */
    STATE_TESTING     = 0x02U,   /* installed, awaiting confirmation      */
    STATE_VALID       = 0x03U    /* valid, bootable without reservation   */
} fw_state_t;

/* =========================================================
 * Per-slot descriptor (16 bytes, no padding)
 *
 * Each slot carries ITS OWN size, CRC and version.
 *
 * An earlier version of this structure described only the active
 * firmware. The flaw only appeared at rollback: when switching
 * to the other slot, the metadata retained the size and CRC of
 * the rejected image. The bootloader would then read VALID,
 * recompute the CRC of the fallback slot against the wrong value,
 * observe a mismatch and refuse to boot -- even though the board
 * contained a working firmware.
 *
 * The rollback had saved the board once, then immobilised it on
 * the next reboot. Describing both slots independently eliminates
 * the problem: switching only requires changing active_slot.
 * ========================================================= */
typedef struct {
    uint32_t size;              /* image size, 0 if absent               */
    uint32_t crc32;             /* expected CRC32 of this image          */
    uint32_t version;           /* firmware version                      */
    uint8_t  state;             /* fw_state_t                            */
    uint8_t  reserved[3];       /* future extension, always zero         */
} slot_info_t;                  /* 16 bytes */

/* =========================================================
 * Metadata structure (48 bytes, no padding)
 *
 * Written in duplicate across two distinct pages. On each update,
 * only the inactive page is erased: the other remains intact and
 * readable, ensuring that a power cut never destroys both copies.
 *
 * A copy is valid if its magic matches AND its CRC is correct.
 * The magic alone is not sufficient: it shares its 8-byte block
 * with the counter, so a power cut after the first write would
 * leave a valid magic in front of fields still at 0xFF. size
 * would then be 0xFFFFFFFF (4 GB), and the bootloader would
 * exceed flash bounds when trying to verify the image.
 *
 * The CRC covers the preceding 44 bytes. It detects both
 * incomplete writes and cell degradation over time.
 *
 * The authoritative copy is the one with the highest counter
 * among the valid copies.
 *
 * The 48-byte size is a multiple of 8, the flash controller's
 * programming unit. The structure is therefore written as-is,
 * with no gap between sizeof() and what ends up in flash.
 *
 * Field layout
 * ------------
 * size, crc32, version and state are PER-SLOT: each image carries
 * its own permanently, including the one that is not active.
 *
 * active_slot, boot_fail_count and counter are GLOBAL. The failure
 * counter in particular describes only one slot at a time: only
 * one can be in STATE_TESTING at a time, since the bootloader only
 * jumps to active_slot. Duplicating it per slot would leave a field
 * permanently unused and imply a concurrency that never occurs.
 *
 * This last property would no longer hold with more than two slots,
 * or if multiple images could be evaluated in parallel. The counter
 * would then need to become per-slot.
 *
 * 32-bit counter overflow is ignored: flash endurance, roughly
 * 10,000 cycles per page, is the effective limit, five orders of
 * magnitude lower.
 * ========================================================= */
typedef struct {
    uint32_t    magic;          /* METADATA_MAGIC if the copy is valid   */
    uint32_t    counter;        /* incremented on each write             */
    slot_info_t slot[2];        /* [SLOT_A] and [SLOT_B], independent    */
    uint8_t     active_slot;    /* SLOT_A or SLOT_B                      */
    uint8_t     boot_fail_count;/* boots without confirmation            */
    uint8_t     reserved[2];    /* future extension, always zero         */
    uint32_t    meta_crc32;     /* CRC32 of the preceding 44 bytes       */
} metadata_t;                   /* 48 bytes */

#define METADATA_MAGIC          0x424C4D44UL   /* "BLMD" */
#define METADATA_SIZE           sizeof(metadata_t)

/* =========================================================
 * Rollback policy
 *
 * Beyond this number of consecutive failed boots without
 * application confirmation, the bootloader switches to the
 * previous slot.
 * ========================================================= */
#define MAX_BOOT_FAILURES       3U

/* =========================================================
 * Bootloader version
 * Format: 0x00MMmmpp  (major, minor, patch)
 *
 * 0.2.0 added the boot request flag. The bump is not cosmetic: a
 * 0.1.0 bootloader has _estack at the very top of RAM, so its own
 * first stack push lands on the flag word and destroys the request
 * before it can be read. The host tool reports bl_version so that
 * "the trigger does nothing" has a visible cause.
 * ========================================================= */
#define BOOTLOADER_VERSION      0x00000200UL   /* 0.2.0 */

#endif /* METADATA_H */
