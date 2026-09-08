#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>

/*
 * Flash controller driver for the STM32L476RG embedded flash.
 *
 * Hardware constraints (RM0351 section 3.3.7):
 *
 *   - Flash is programmed in 72-bit double-words: 64 bits of data
 *     plus 8 ECC bits. Every write must therefore cover 8 bytes at
 *     an 8-byte-aligned address. A byte or half-word access raises
 *     SIZERR; a misaligned address raises PGAERR.
 *
 *   - Programming an already-programmed address raises PROGERR,
 *     unless the value being written is entirely zero. ECC bits
 *     cannot be recomputed without a prior erase.
 *
 *   - The erase unit is a 2 KB page. The 1 MB flash is organized
 *     in two banks of 256 pages each, numbered 0 to 255 per bank.
 *     The BKER bit in FLASH_CR selects the bank.
 *
 * Design choices
 * --------------
 * Functions take ADDRESSES, never page numbers. The address ->
 * (bank, page) conversion is done here, at the only place that
 * knows about the dual-bank layout. The rest of the firmware
 * reasons in addresses, as defined by metadata.h.
 *
 * The controller is unlocked then re-locked inside each operation.
 * Flash is therefore writable only during the few microseconds of
 * an erase or a program, never during frame parsing or idle waiting.
 * A stray pointer at that moment cannot overwrite the bootloader.
 * flash_unlock() is intentionally not exported: an API that makes
 * the error impossible is better than one that merely discourages it.
 *
 * Read-back is integrated into flash_write() and is not optional.
 * In a bootloader, no write can be exempt from verification: every
 * byte written is part of a firmware that must be exact. Making
 * verification separate would create the possibility of forgetting
 * it without ever providing a benefit.
 *
 * Non-aligned lengths and addresses are REJECTED rather than
 * compensated for. The protocol uses 256-byte blocks, chosen
 * precisely so that this constraint never arises. A misaligned
 * call therefore signals a caller bug, which is better seen
 * immediately than silently masked.
 *
 * NOT REENTRANT: the controller is a stateful shared resource.
 */

typedef enum {
    FLASH_OK = 0,
    FLASH_ERR_ALIGN,      /* address or length not a multiple of 8    */
    FLASH_ERR_RANGE,      /* outside flash bounds                     */
    FLASH_ERR_LOCKED,     /* unlock sequence failed                   */
    FLASH_ERR_BUSY,       /* operation still in progress at startup   */
    FLASH_ERR_PROG,       /* error reported by the controller         */
    FLASH_ERR_VERIFY      /* read-back differs from what was written  */
} flash_status_t;

/*
 * Erases the 2 KB page containing the given address.
 *
 * The address must be aligned to a page boundary, otherwise
 * FLASH_ERR_ALIGN is returned: erasing 2 KB from an arbitrary
 * address would destroy data the caller did not intend to target.
 *
 * An erased page contains 0xFF throughout.
 */
flash_status_t flash_erase_page(uint32_t address);

/*
 * Writes len bytes at the given address, then reads back to verify.
 *
 * address and len must both be multiples of 8. The target area
 * must have been erased beforehand, otherwise the controller raises
 * PROGERR and FLASH_ERR_PROG is returned.
 */
flash_status_t flash_write(uint32_t address, const uint8_t *data, uint32_t len);

/*
 * Checks that a region is fully erased (0xFF everywhere).
 * Useful before writing, to distinguish a blank slot from a
 * partially programmed one.
 */
int flash_is_erased(uint32_t address, uint32_t len);

/*
 * Flash is memory-mapped: reading requires no special function,
 * a dereference is sufficient. This helper exists for readability
 * in the calling code.
 */
static inline const uint8_t *flash_ptr(uint32_t address)
{
    return (const uint8_t *)address;
}

#endif /* FLASH_H */
