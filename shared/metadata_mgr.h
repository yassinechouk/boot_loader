#ifndef METADATA_MGR_H
#define METADATA_MGR_H

#include <stdint.h>
#include "metadata.h"

/*
 * Persistent metadata management for the bootloader.
 *
 * Dual-page mechanism
 * -------------------
 * The structure is written in duplicate across two distinct flash
 * pages. On each update, only the inactive page is erased and
 * rewritten: the other remains intact and readable throughout the
 * operation.
 *
 * Consequence: a power cut can never destroy both copies
 * simultaneously. On reboot, at least one valid copy always
 * remains — the one from before the update if the cut occurred
 * during the write.
 *
 * A copy is retained if its magic matches AND its CRC is correct.
 * The magic alone is not sufficient: it shares its 8-byte block
 * with the counter, so a power cut after the first write would
 * leave a valid magic in front of fields still at 0xFF. The CRC
 * covering the first 28 bytes rejects any partial alteration.
 *
 * Between two valid copies, the one with the higher counter takes
 * precedence.
 *
 * Equal counter case
 * ------------------
 * This situation can only result from corruption or a bug.
 * The module then chooses page 0, deterministically, rather than
 * declaring the board blank.
 *
 * Refusing to boot would be the wrong choice: two valid copies
 * hold two intact datasets, most likely describing the same state.
 * There is no danger in booting, whereas falling into receive mode
 * would immobilise a device that was working. We only refuse to
 * operate when continuing would be dangerous.
 *
 * The anomaly resolves itself: the next write carries a strictly
 * higher counter.
 *
 * Copy rather than pointer
 * ------------------------
 * metadata_read() copies data into RAM instead of returning a
 * pointer into flash. Such a pointer would silently become invalid
 * as soon as metadata_write() erased the target page: fields would
 * start reading as 0xFF without any warning.
 *
 * The cost is 32 bytes out of 96 KB. The benefit is that the caller
 * owns its data and can modify it before rewriting it.
 *
 * NOT REENTRANT: relies on flash.c and crc.c, which are not either.
 */

typedef enum {
    META_OK = 0,
    META_ERR_ERASE,       /* failed to erase the inactive page         */
    META_ERR_WRITE,       /* programming failure                        */
    META_ERR_VERIFY,      /* inconsistent read-back after write         */
    META_ERR_ARG          /* null argument                              */
} metadata_status_t;

/*
 * Reads the most recent copy and copies it into dest.
 *
 * Returns 1 if a valid copy was found, 0 otherwise — in which case
 * dest is not modified. The absence of a valid copy means either a
 * blank board or destroyed metadata: in both cases, the bootloader
 * must enter receive mode.
 */
int metadata_read(metadata_t *dest);

/*
 * Writes a new version into the inactive page.
 *
 * The magic, counter, reserved and meta_crc32 fields of src are
 * ignored: the module computes them itself. The caller therefore
 * cannot produce a structure with a wrong CRC or an inconsistent
 * counter, even if trying.
 *
 * Only the following are carried over: fw_size, fw_crc32,
 * fw_version, active_slot, state, boot_fail_count.
 */
metadata_status_t metadata_write(const metadata_t *src);

/*
 * Erases both pages. Returns the board to a blank state.
 * Intended for testing and deliberate factory reset.
 */
metadata_status_t metadata_erase_all(void);

/*
 * Indicates whether a given copy is structurally valid.
 * Exposed for testing; the module uses it internally.
 */
int metadata_is_valid(const metadata_t *meta);

#endif /* METADATA_MGR_H */
