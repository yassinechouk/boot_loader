#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* =========================================================
 * Protocol version
 * ========================================================= */
#define PROTO_VERSION           1U

/* =========================================================
 * Frame format
 *
 *   Offset  Size  Field
 *   0       2     MAGIC   (0xAA 0x55)
 *   2       1     CMD
 *   3       2     LENGTH  (little-endian)
 *   5       2     SEQ     (little-endian)
 *   7       N     DATA
 *   7+N     4     CRC32   (little-endian)
 *
 * The CRC covers CMD, LENGTH, SEQ and DATA -- not the MAGIC,
 * which validates itself simply by being recognized.
 * ========================================================= */
#define FRAME_MAGIC_0           0xAAU
#define FRAME_MAGIC_1           0x55U

#define FRAME_HEADER_SIZE       7U      /* MAGIC + CMD + LENGTH + SEQ */
#define FRAME_CRC_SIZE          4U
#define FRAME_OVERHEAD          (FRAME_HEADER_SIZE + FRAME_CRC_SIZE)

/* Field offsets within the frame */
#define FRAME_OFF_MAGIC         0U
#define FRAME_OFF_CMD           2U
#define FRAME_OFF_LENGTH        3U
#define FRAME_OFF_SEQ           5U
#define FRAME_OFF_DATA          7U

/* =========================================================
 * Sizes
 *
 * DATA_BLOCK_SIZE is constrained by the flash controller:
 *   - multiple of 8    (programming unit, 64-bit double-word)
 *   - divisor of 2048  (erase unit, page)
 * ========================================================= */
#define DATA_BLOCK_SIZE         256U
#define MAX_PAYLOAD_SIZE        1024U   /* plausibility bound on LENGTH */
#define MAX_FRAME_SIZE          (MAX_PAYLOAD_SIZE + FRAME_OVERHEAD)

/* =========================================================
 * Commands (PC -> bootloader)
 * ========================================================= */
#define CMD_GET_INFO            0x01U
#define CMD_START_UPDATE        0x02U
#define CMD_DATA                0x03U
#define CMD_END_UPDATE          0x04U
#define CMD_ABORT               0x05U

/* =========================================================
 * Responses (bootloader -> PC)
 * Bit 7 marks a response: readable at a glance in a dump.
 * ========================================================= */
#define RSP_INFO                0x81U
#define RSP_ACK                 0x82U
#define RSP_NACK                0x83U

#define IS_RESPONSE(cmd)        (((cmd) & 0x80U) != 0U)

/* =========================================================
 * Error codes (payload of RSP_NACK, 1 byte)
 * ========================================================= */
#define ERR_CRC                 0x01U   /* invalid frame CRC                */
#define ERR_SEQ                 0x02U   /* unexpected sequence number       */
#define ERR_LENGTH              0x03U   /* LENGTH out of bounds             */
#define ERR_FLASH               0x04U   /* write or read-back failure       */
#define ERR_SIZE                0x05U   /* firmware too large               */
#define ERR_SLOT                0x06U   /* wrong target slot                */
#define ERR_STATE               0x07U   /* command in an invalid state      */
#define ERR_GLOBAL_CRC          0x08U   /* invalid full-firmware CRC        */
#define ERR_PROTO_VER           0x09U   /* unsupported protocol version     */

/* =========================================================
 * CMD_START_UPDATE payload (16 bytes)
 *
 * Fields ordered by decreasing size: no padding inserted by the
 * compiler, identical layout regardless of the toolchain.
 *
 * The frame count is not transmitted: it is derived from fw_size
 * and DATA_BLOCK_SIZE. A redundant datum could become inconsistent.
 * ========================================================= */
typedef struct {
    uint32_t fw_size;
    uint32_t fw_crc32;
    uint32_t fw_version;
    uint8_t  target_slot;
    uint8_t  proto_version;
    uint8_t  reserved[2];
} start_update_t;

/* =========================================================
 * RSP_INFO payload (12 bytes)
 *
 * active_slot and free_slot are mutually derivable.
 * This redundancy is intentional: the decision "where to write"
 * belongs to the bootloader, which holds the real state, rather
 * than to the PC tool that would have to infer it.
 * ========================================================= */
typedef struct {
    uint32_t fw_version;
    uint32_t bl_version;
    uint8_t  proto_version;
    uint8_t  active_slot;
    uint8_t  free_slot;
    uint8_t  state;
} info_response_t;

/* =========================================================
 * Timeouts (milliseconds)
 *
 * The timer is armed after a complete frame has been processed,
 * never on receipt of a single byte: a flash write must not
 * trigger a false positive.
 * ========================================================= */
#define TIMEOUT_START_MS        2000U
#define TIMEOUT_DATA_MS         5000U

#endif /* PROTOCOL_H */
