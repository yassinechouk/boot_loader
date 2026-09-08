#include <stdint.h>

#include "uart.h"
#include "crc.h"
#include "flash.h"
#include "systick.h"
#include "metadata.h"
#include "metadata_mgr.h"
#include "protocol.h"
#include "protocol_mgr.h"

/*
 * Bootloader — entry point.
 *
 * Boot sequence
 * -------------
 *   1. Initialize peripherals
 *   2. Read metadata
 *   3. Decide: jump to application, or wait for an update
 *   4. If an update is requested, handle it then reboot
 *
 * The decision in step 3 depends on the recorded state:
 *
 *   No metadata      -> receive mode: nothing to execute
 *   EMPTY            -> receive mode
 *   IN_PROGRESS      -> receive mode: a transfer was
 *                       interrupted, the target slot contains a
 *                       partial image
 *   VALID            -> jump immediately
 *   TESTING          -> jump, after incrementing the failure
 *                       counter. If the threshold is reached,
 *                       rollback to the other slot.
 *
 * The TESTING mechanism
 * ---------------------
 * A correct CRC proves the integrity of an image, not that it
 * works correctly: a firmware transferred without any corruption
 * can crash within its first second.
 *
 * The bootloader therefore jumps to a TESTING image after
 * incrementing its failure counter. The application, if it boots
 * correctly, must reset this counter to zero and set the state to
 * VALID. If it crashes, the watchdog triggers a reset and the
 * counter has not been reset: beyond the threshold, the bootloader
 * switches back to the previous slot.
 *
 * The application therefore carries a responsibility. Without its
 * confirmation, rollback is impossible.
 */

#define BOOT_WAIT_MS        2000U   /* startup wait window */
#define MSI_DEFAULT_HZ      4000000UL

#define SCB_VTOR            (*(volatile uint32_t *)0xE000ED08UL)

/* RCC registers, needed to return peripherals to reset state
   before handing off control. */
#define RCC_BASE            0x40021000UL
#define RCC_AHB1ENR         (*(volatile uint32_t *)(RCC_BASE + 0x48))
#define RCC_AHB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x4C))
#define RCC_APB1ENR1        (*(volatile uint32_t *)(RCC_BASE + 0x58))

#define NVIC_ICER0          (*(volatile uint32_t *)0xE000E180UL)
#define NVIC_ICER1          (*(volatile uint32_t *)0xE000E184UL)
#define NVIC_ICPR0          (*(volatile uint32_t *)0xE000E280UL)
#define NVIC_ICPR1          (*(volatile uint32_t *)0xE000E284UL)


/* ----------------------------------------------------------------
 * Logging
 * ---------------------------------------------------------------- */

static void log_state(uint8_t s)
{
    switch (s) {
    case STATE_EMPTY:       uart_puts("EMPTY");       break;
    case STATE_IN_PROGRESS: uart_puts("IN_PROGRESS"); break;
    case STATE_TESTING:     uart_puts("TESTING");     break;
    case STATE_VALID:       uart_puts("VALID");       break;
    default:                uart_hex8(s);             break;
    }
}


/* ----------------------------------------------------------------
 * Jump to application
 * ---------------------------------------------------------------- */

static int slot_looks_bootable(uint32_t base)
{
    uint32_t sp    = *(volatile uint32_t *)base;
    uint32_t reset = *(volatile uint32_t *)(base + 4U);

    /* The first word is the initial stack pointer: it must point to
       an address in RAM. The second is the reset handler address,
       which must fall within flash.

       A missing image leaves 0xFFFFFFFF in both fields, which these
       tests reject. Jumping to a non-existent image would cause a
       silent HardFault with no message. */
    if ((sp & 0xFFF00000UL) != 0x20000000UL) {
        return 0;
    }
    if (reset < FLASH_BASE_ADDR ||
        reset >= (FLASH_BASE_ADDR + FLASH_TOTAL_SIZE)) {
        return 0;
    }

    /* Bit 0 must be 1: Cortex-M only executes Thumb code, and this
       bit signals that to the processor. An even address would cause
       an immediate HardFault. */
    if ((reset & 1U) == 0U) {
        return 0;
    }

    return 1;
}


static void jump_to_application(uint32_t base)
{
    uint32_t app_sp    = *(volatile uint32_t *)base;
    uint32_t app_reset = *(volatile uint32_t *)(base + 4U);

    uart_puts("\r\nJumping to ");
    uart_hex32(base);
    uart_puts("\r\n\r\n");
    uart_flush();

    __asm__ volatile ("cpsid i");

    /* Return peripherals to a state close to reset.
       The application will reconfigure them, but it assumes they
       are clean: an already-active USART or an armed interrupt
       would produce inexplicable behavior. */
    systick_deinit();

    NVIC_ICER0 = 0xFFFFFFFFUL;      /* disable all IRQs */
    NVIC_ICER1 = 0xFFFFFFFFUL;
    NVIC_ICPR0 = 0xFFFFFFFFUL;      /* clear pending ones */
    NVIC_ICPR1 = 0xFFFFFFFFUL;

    RCC_APB1ENR1 &= ~(1U << 17);    /* USART2 */
    RCC_AHB1ENR  &= ~(1U << 12);    /* CRC    */

    /* Relocate the vector table. Without this line the application
       starts, then crashes on its first interrupt: the processor
       would look for the handler in the bootloader's table. */
    SCB_VTOR = base;

    __asm__ volatile ("dsb");
    __asm__ volatile ("isb");

    /* Load the application's stack pointer before jumping.
       The bootloader's stack pointer is meaningless on the other side. */
    __asm__ volatile ("msr msp, %0" : : "r" (app_sp) : );

    __asm__ volatile ("cpsie i");

    /* The jump itself. This function never returns. */
    ((void (*)(void))app_reset)();

    /* Unreachable */
    while (1) {
    }
}


/* ----------------------------------------------------------------
 * Image verification before execution
 * ---------------------------------------------------------------- */

static int image_is_intact(const metadata_t *meta, uint8_t slot)
{
    const slot_info_t *info = &meta->slot[slot];

    if (info->size == 0U || info->size > SLOT_SIZE) {
        return 0;
    }

    uint32_t base = SLOT_ADDR(slot);

    if (!slot_looks_bootable(base)) {
        return 0;
    }

    /* CRC recomputed at every boot. A flash cell can degrade over
       time; better to discover that here than to execute corrupt code. */
    uint32_t computed = crc32_compute((const uint8_t *)base, info->size);

    return (computed == info->crc32);
}


/* ----------------------------------------------------------------
 * Receive mode
 * ---------------------------------------------------------------- */

static void update_mode(uint32_t timeout_ms)
{
    uart_puts("Receive mode");
    if (timeout_ms > 0U) {
        uart_puts(" (");
        uart_dec(timeout_ms);
        uart_puts(" ms)");
    }
    uart_puts("\r\n");

    uint32_t start = millis();

    while (1) {
        uint32_t now = millis();

        protocol_poll(now);

        if (protocol_update_complete()) {
            uart_puts("\r\nUpdate complete, rebooting\r\n");
            uart_flush();
            delay_ms(50);

            /* Software reset via AIRCR. Starting fresh guarantees a
               clean state rather than jumping from a bootloader whose
               peripherals are already configured. */
            *(volatile uint32_t *)0xE000ED0CUL = 0x05FA0004UL;

            while (1) {
            }
        }

        /* A wait window only expires if nothing has started:
           an in-progress transfer must not be interrupted by the
           startup timeout. */
        if (timeout_ms > 0U &&
            protocol_get_state() == PROTO_IDLE &&
            (now - start) > timeout_ms) {
            return;
        }
    }
}


/* ----------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------- */

int main(void)
{
    systick_init(MSI_DEFAULT_HZ);
    uart_init(115200);
    crc32_init();
    protocol_init();

    uart_puts("\r\n\r\n========================================\r\n");
    uart_puts("  BOOTLOADER v");
    uart_dec((BOOTLOADER_VERSION >> 16) & 0xFFU);
    uart_putc('.');
    uart_dec((BOOTLOADER_VERSION >> 8) & 0xFFU);
    uart_putc('.');
    uart_dec(BOOTLOADER_VERSION & 0xFFU);
    uart_puts("\r\n========================================\r\n");

    metadata_t meta;

    if (!metadata_read(&meta)) {
        uart_puts("No valid metadata\r\n");
        update_mode(0);             /* wait indefinitely */
        while (1) { }
    }

    uint8_t active = meta.active_slot;

    uart_puts("Active slot : ");
    uart_putc((char)('A' + active));
    uart_puts("\r\nState       : ");
    log_state(meta.slot[active].state);
    uart_puts("\r\nSize        : ");
    uart_dec(meta.slot[active].size);
    uart_puts(" bytes\r\nVersion     : ");
    uart_hex32(meta.slot[active].version);
    uart_puts("\r\nBoot fails  : ");
    uart_dec(meta.boot_fail_count);
    uart_puts("\r\n");

    /* The other slot is also displayed: it is the one that will serve
       as fallback in case of rollback, and knowing what it contains
       avoids discovering too late that it is empty. */
    uint8_t other_slot = OTHER_SLOT(active);
    uart_puts("Fallback    : slot ");
    uart_putc((char)('A' + other_slot));
    uart_puts(", ");
    log_state(meta.slot[other_slot].state);
    uart_puts(", ");
    uart_dec(meta.slot[other_slot].size);
    uart_puts(" bytes\r\n\r\n");

    switch (meta.slot[active].state) {

    case STATE_VALID:
        if (image_is_intact(&meta, active)) {
            update_mode(BOOT_WAIT_MS);   /* give the PC a chance */
            jump_to_application(SLOT_ADDR(active));
        }
        uart_puts("Image invalid despite VALID state\r\n");
        break;

    case STATE_TESTING:
        if (meta.boot_fail_count >= MAX_BOOT_FAILURES) {
            /* The application never confirmed successful boot.
               Roll back to the previous slot. */
            uart_puts("Failure threshold reached, rolling back\r\n");

            uint8_t other = OTHER_SLOT(active);

            /* Verify the fallback BEFORE switching: rejecting the
               current image and ending up with nothing would be
               worse than continuing to try it. */
            if (meta.slot[other].state == STATE_EMPTY ||
                !image_is_intact(&meta, other)) {
                uart_puts("No usable fallback image\r\n");
                break;
            }

            /* Only active_slot, the counter and the state of the
               rejected slot change. The descriptions of both images
               remain intact — that is the whole point of keeping
               them separate. */
            metadata_t prev = meta;
            prev.slot[active].state = STATE_EMPTY;
            prev.active_slot        = other;
            prev.boot_fail_count    = 0;

            if (metadata_write(&prev) == META_OK &&
                slot_looks_bootable(SLOT_ADDR(other))) {
                uart_puts("Switching back to slot ");
                uart_putc((char)('A' + other));
                uart_puts("\r\n");
                jump_to_application(SLOT_ADDR(other));
            }
            uart_puts("No usable fallback image\r\n");
            break;
        }

        if (image_is_intact(&meta, active)) {
            /* Increment BEFORE jumping. If the application crashes,
               the counter will already have advanced at the next reset.
               Incrementing after would have no effect, since we never
               return from the jump. */
            metadata_t trial = meta;
            trial.boot_fail_count = (uint8_t)(meta.boot_fail_count + 1U);
            metadata_write(&trial);

            uart_puts("Trial ");
            uart_dec(trial.boot_fail_count);
            uart_putc('/');
            uart_dec(MAX_BOOT_FAILURES);
            uart_puts("\r\n");

            update_mode(BOOT_WAIT_MS);
            jump_to_application(SLOT_ADDR(active));
        }
        uart_puts("Testing image is invalid\r\n");
        break;

    case STATE_IN_PROGRESS:
        uart_puts("Interrupted transfer detected\r\n");
        break;

    case STATE_EMPTY:
    default:
        uart_puts("No firmware installed\r\n");
        break;
    }

    /* All paths that did not jump end up here: there is nothing
       executable, wait indefinitely for an update. */
    update_mode(0);

    while (1) {
    }
}
