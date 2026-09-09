#ifndef CRC_H
#define CRC_H

#include <stdint.h>

/*
 * Hardware CRC peripheral driver for the STM32L4.
 *
 * Configuration used:
 *   POL      = 0x04C11DB7  (CRC-32 Ethernet, reset value)
 *   INIT     = 0xFFFFFFFF  (reset value)
 *   REV_IN   = 01          (bit reversal per byte)
 *   REV_OUT  = 0
 *   Final XOR: none
 *
 * REV_IN corrects the little-endian memory order: the peripheral
 * interprets a 32-bit word in big-endian internally, whereas the
 * Cortex-M stores bytes in little-endian. The reversal is done in
 * hardware, at no software cost.
 *
 * This configuration produces the same values as tools/crc32.py,
 * verified on the STM32L476RG:
 *     "123456789"  ->  0x9B63D02C
 *     4 zeros      ->  0xC704DD7B
 *     "STM32"      ->  0xF4F0FF62
 *
 * NOT REENTRANT
 * -------------
 * The CRC peripheral is a shared stateful resource. A computation
 * launched from an interrupt context while another is in progress
 * in the main context would corrupt both results.
 *
 * No protection is implemented: in this bootloader, only the main
 * context computes CRCs. The UART interrupt reception merely fills
 * a buffer. Adding a critical section in crc32_update() would
 * increase interrupt latency on a loop called hundreds of times
 * per transfer, at the risk of dropping bytes -- a real problem
 * created to avoid one that does not exist.
 *
 * If a call from an interrupt ever became necessary, this decision
 * would need to be revisited explicitly.
 */

/* Enables the peripheral clock and applies the configuration.
   Must be called once at startup, before any other call. */
void crc32_init(void);

/* ----------------------------------------------------------------
 * Incremental API
 *
 * Allows feeding the computation in chunks, without ever holding
 * the entire data in memory at once.
 * ---------------------------------------------------------------- */

/* Reloads the initial value. Must be called before each new computation. */
void crc32_reset(void);

/* Adds len bytes to the ongoing computation. */
void crc32_update(const uint8_t *data, uint32_t len);

/* Returns the CRC accumulated since the last crc32_reset(). */
uint32_t crc32_get(void);

/* ----------------------------------------------------------------
 * One-shot convenience wrapper
 * ---------------------------------------------------------------- */
uint32_t crc32_compute(const uint8_t *data, uint32_t len);

#endif /* CRC_H */
