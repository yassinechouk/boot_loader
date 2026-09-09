/* =========================================================
 * Boot request flag
 *
 * A soft reset (AIRCR) preserves RAM. The application writes
 * BOOT_REQUEST_MAGIC to the flag word just before triggering a
 * reset; the bootloader reads it on the next boot to decide whether
 * to open a long update window instead of its usual short one.
 *
 * Address selection
 * -----------------
 * The flag lives in the last word of RAM, and its address comes from
 * the linker rather than from a constant written here:
 *
 *     _boot_request = ORIGIN(RAM) + LENGTH(RAM) - 4;
 *     _estack       = ORIGIN(RAM) + LENGTH(RAM) - 8;
 *
 * declared in all three linker scripts. The stack top is lowered
 * past the flag, so neither the stack nor any compiler-allocated
 * object can reach it, and both binaries agree on the address
 * because both take it from the same symbol.
 *
 * Hard-coding 0x20017FFC in C would have worked exactly as long as
 * nobody changed LENGTH(RAM). The day someone did, the flag would
 * have landed inside the live stack and this file would have become
 * a source of random corruption rather than a broken build. The
 * same reasoning as the field offsets in protocol.h: the layout is
 * a contract, so it is stated once in the place that owns it.
 *
 * Eight bytes rather than four are taken from the stack so that
 * _estack stays 8-byte aligned, as AAPCS requires.
 *
 *   RAM:   0x20000000 -- 0x20018000  (96 KB)
 *   Flag:  0x20017FFC               (last word)
 *   Stack: grows down from 0x20017FF8
 *
 * Which resets preserve it
 * ------------------------
 * SRAM1 survives every system reset -- AIRCR, the NRST pin and the
 * watchdog alike -- and is only randomised by a power cycle. The
 * flag is deliberately placed in SRAM1 and not in SRAM2, which an
 * option byte can be configured to erase on every reset.
 *
 * Safety
 * ------
 * The bootloader clears the flag as soon as it reads it, before
 * acting on it. Clearing afterwards would mean a reset during the
 * transfer left the request set, and the board would re-enter update
 * mode on every boot from then on.
 *
 * A power cycle leaves RAM in an indeterminate state; the magic
 * value is chosen so that it is vanishingly unlikely to appear by
 * chance.
 * ========================================================= */

#ifndef BOOT_REQUEST_H
#define BOOT_REQUEST_H

#include <stdint.h>

/* Defined by the linker script, not by any translation unit. Only
   its address is ever used. */
extern uint32_t _boot_request;

#define BOOT_REQUEST_ADDR   ((volatile uint32_t *)&_boot_request)

/* Sentinel value written by the application before resetting. */
#define BOOT_REQUEST_MAGIC  0xDEADBEEFUL

/* Write the flag. Call this just before triggering a soft reset. */
static inline void boot_request_set(void)
{
    *BOOT_REQUEST_ADDR = BOOT_REQUEST_MAGIC;
}

/* Returns non-zero if the flag is set (i.e. an OTA was requested). */
static inline int boot_request_pending(void)
{
    return (*BOOT_REQUEST_ADDR == BOOT_REQUEST_MAGIC);
}

/* Clear the flag. The bootloader must call this immediately after
   detecting it, before acting on it. */
static inline void boot_request_clear(void)
{
    *BOOT_REQUEST_ADDR = 0U;
}

#endif /* BOOT_REQUEST_H */
