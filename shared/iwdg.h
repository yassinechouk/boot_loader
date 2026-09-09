#ifndef IWDG_H
#define IWDG_H

#include <stdint.h>

/*
 * Independent watchdog (IWDG) driver.
 *
 * The IWDG is a free-running down-counter clocked by the LSI, an
 * internal RC oscillator independent of the system clock. It keeps
 * counting even if the PLL stops, the core hangs, or the device
 * enters a low-power mode. When the counter reaches zero, it forces
 * a hardware reset.
 *
 * Why the IWDG and not the WWDG
 * -----------------------------
 * The window watchdog also detects refreshes that come too *early*,
 * which catches a program looping over a section that happens to
 * contain the refresh call. That is a real failure mode, but the
 * WWDG is clocked from APB and tops out around 50 ms — far too short
 * to cover an application start-up, and it stops if the system clock
 * fails. Since the point here is to survive a firmware that breaks
 * the clock configuration, the IWDG is the correct choice.
 *
 * Started before the jump, not by the application
 * ----------------------------------------------
 * The bootloader starts the watchdog *before* jumping. An
 * application that enabled it from its own main() would leave a
 * blind spot: a crash inside startup.s, or a HardFault on the very
 * first instruction, would never be detected. The board would hang
 * with no reset and no rollback.
 *
 * Surveillance must begin before the thing it watches.
 *
 * Once running, the IWDG cannot be stopped (RM0351 36.3.1). Every
 * application deployed on this board must therefore refresh it. That
 * is not a side effect but the mechanism itself: without the
 * obligation, a half-working application could run indefinitely
 * unnoticed.
 *
 * Timing
 * ------
 *     T = (RLR + 1) x prescaler / f_LSI
 *
 * With PR = 3 (divide by 32) and RLR = 2999 at a nominal 32 kHz:
 *
 *     T = 3000 x 32 / 32000 = 3.00 s
 *
 * The LSI is specified at +/-5% (DS10198), so the real timeout lies
 * between 2.86 s and 3.16 s.
 *
 * The figure that matters is the shorter one: 2.86 s is the minimum
 * an application is given to reach its first refresh. Current
 * start-up takes a few milliseconds, three orders of magnitude
 * below.
 *
 * Three seconds is generous, deliberately so. The two failure modes
 * are not symmetric: too short causes a false positive — a healthy
 * firmware reset in a loop and rolled back for no reason — while too
 * long only delays detection. This system drives no actuator, so a
 * few seconds of undefined behaviour during an update carries no
 * risk. A motor controller would need tens of milliseconds and a
 * windowed watchdog.
 */

/* Timeout in milliseconds. Kept as a named constant so the
   justification above stays attached to the value. */
#define IWDG_TIMEOUT_MS     3000U

/*
 * Starts the watchdog. Cannot be undone except by a hardware reset.
 *
 * timeout_ms is clamped to what the peripheral can express: roughly
 * 0.5 ms to 32.8 s at nominal LSI.
 */
void iwdg_start(uint32_t timeout_ms);

/*
 * Refreshes the counter. Must be reached often enough that the
 * interval between two calls stays well below the timeout.
 *
 * Calling this unconditionally only detects a full hang. An
 * application that loops without doing useful work would still
 * refresh, and the watchdog would see nothing. Detecting *that*
 * requires refreshing only once the system has been checked healthy
 * — see iwdg_feed_if() and the note in the implementation.
 */
void iwdg_feed(void);

/*
 * Refreshes only if the condition holds.
 *
 * This is the form that turns a liveness check into a health check.
 * The caller passes the result of whatever it considers proof that
 * the system is working: a task counter that has advanced, a sensor
 * that answered, a queue that drained.
 */
void iwdg_feed_if(int healthy);

/* True if the last reset was caused by the watchdog rather than by
   power-on or the reset button. Lets the bootloader distinguish a
   crashed application from a deliberate restart. */
int iwdg_caused_last_reset(void);

/* Clears the reset-cause flags. Call after reading them, otherwise
   they persist across resets and every later boot looks like a
   watchdog reset. */
void iwdg_clear_reset_flags(void);

/*
 * Freezes the counter while the core is halted by a debugger.
 *
 * Without this, setting a breakpoint resets the board a few seconds
 * later, making step-by-step debugging impossible. The freeze is a
 * debug convenience and has no effect on normal operation, but it
 * must not be left enabled on a shipped product.
 */
void iwdg_freeze_on_debug(void);

#endif /* IWDG_H */
