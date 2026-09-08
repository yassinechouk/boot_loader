#ifndef SYSTICK_H
#define SYSTICK_H

#include <stdint.h>

/*
 * Millisecond time base built on the SysTick.
 *
 * SysTick is a 24-bit counter integrated into the Cortex-M core,
 * present on every chip of this family regardless of manufacturer.
 * Unlike the STM32 TIM peripherals, it is part of the ARM
 * architecture: this module ports as-is to any Cortex-M.
 *
 * Overflow
 * --------
 * The counter wraps to zero after roughly 49 days. This is not a
 * problem as long as duration measurements are written as:
 *
 *     if ((millis() - start) > duration)
 *
 * Unsigned 32-bit arithmetic is modular, so the subtraction gives
 * the real elapsed time even across a rollover:
 * 0 - 4294967000 equals 296.
 *
 * The following form would be wrong, because start + duration
 * can overflow:
 *
 *     if (millis() > start + duration)      // DO NOT WRITE THIS
 */

/* Configures an interrupt every millisecond.
   cpu_hz must match the actual core frequency; at reset,
   the STM32L4 starts on MSI at 4 MHz. */
void systick_init(uint32_t cpu_hz);

/* Milliseconds elapsed since systick_init(). */
uint32_t millis(void);

/* Blocking wait. Accurate, unlike a nop loop whose duration
   depends on the optimisation level and the clock. */
void delay_ms(uint32_t ms);

/* Stops the counter and its interrupt.
   Must be called before jumping to the application, which will
   reconfigure SysTick in its own way: an interrupt firing during
   the transition would find an inconsistent vector table. */
void systick_deinit(void);

#endif /* SYSTICK_H */
