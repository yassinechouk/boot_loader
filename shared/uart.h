#ifndef UART_H
#define UART_H

#include <stdint.h>

/*
 * USART2 driver — polling TX, interrupt-driven RX.
 *
 * On the Nucleo-L476RG, USART2 is connected to the ST-LINK
 * virtual COM port via PA2 (TX) and PA3 (RX), with solder bridges
 * SB13/SB14 closed at the factory. No wiring is required.
 *
 * Why interrupt-driven reception
 * --------------------------------
 * At 115200 baud, one byte arrives every ~87 µs. A 256-byte flash
 * write takes a few hundred µs, a page erase roughly twenty ms —
 * enough to miss over two hundred bytes if the CPU polled the
 * register in a loop.
 *
 * The protocol operates in strict request-response mode: the PC
 * does not transmit while the bootloader is writing. But this
 * guarantee relies on the sender's discipline. A timeout expiring
 * at the wrong moment, a poorly timed retransmission, and bytes
 * would be lost. A bootloader must not depend on its peer's
 * good behavior.
 *
 * The ISR simply stores the byte in a circular buffer:
 * a few microseconds, well within the 87 µs interval.
 *
 * Lock-free circular buffer
 * --------------------------
 * Two indices, head and tail. The ISR writes head and reads tail;
 * the main context writes tail and reads head. Each index has a
 * single writer, eliminating any race condition without needing a
 * critical section.
 *
 * One slot is sacrificed to distinguish a full buffer from an
 * empty one: head == tail means empty, (head + 1) % size == tail
 * means full. The alternative — an element counter — would be
 * written by both contexts and would require disabling interrupts
 * on every access.
 *
 * The size is a power of two: modulo becomes a bit mask, one
 * instruction instead of a division costing a dozen cycles, in
 * code called on every byte.
 *
 * Overflow
 * --------
 * A byte arriving on a full buffer is DROPPED, not substituted for
 * the oldest. In a CRC-verified frame protocol, dropping the new
 * byte corrupts the current frame: the CRC detects it and the PC
 * retransmits. Overwriting the oldest would corrupt a frame
 * already received and potentially being processed.
 *
 * The event is counted, not signalled: a non-zero counter at the
 * end of a transfer indicates an undersized buffer or processing
 * that is too slow — information that would otherwise be invisible.
 *
 * NOT REENTRANT on TX: uart_puts() must not be called from an
 * interrupt while it is executing in the main context.
 */

#define UART_RX_BUFFER_SIZE     512U    /* must be a power of two */

/* Initialises the clock, PA2/PA3 pins, baud rate and interrupt. */
void uart_init(uint32_t baudrate);

/* --- Transmission (polling) --- */
void uart_putc(char c);
void uart_puts(const char *s);
void uart_write(const uint8_t *data, uint32_t len);

/* Waits for the actual end of transmission (TC flag).
   Must be called before cutting the USART clock or jumping to the
   application, otherwise the last character would be truncated. */
void uart_flush(void);

/* --- Formatting helpers, useful for diagnostics --- */
void uart_hex32(uint32_t v);
void uart_hex8(uint8_t v);
void uart_dec(uint32_t v);

/* --- Reception (interrupt + circular buffer) --- */

/* Number of available bytes. */
uint32_t uart_available(void);

/* Removes one byte from the buffer. Returns 1 if a byte was present. */
int uart_getc(uint8_t *out);

/* Removes up to max bytes. Returns the number actually read. */
uint32_t uart_read(uint8_t *dest, uint32_t max);

/* Flushes the buffer. Useful for resynchronising after an error. */
void uart_rx_flush(void);

/* --- Diagnostics --- */
uint32_t uart_overrun_count(void);      /* software buffer full      */
uint32_t uart_hw_overrun_count(void);   /* USART ORE flag            */
uint32_t uart_framing_error_count(void);/* FE flag                   */
uint32_t uart_noise_error_count(void);  /* NE flag                   */
void     uart_reset_counters(void);

#endif /* UART_H */
