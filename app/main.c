#include <stdint.h>

#include "crc.h"
#include "flash.h"
#include "metadata.h"
#include "metadata_mgr.h"
#include "iwdg.h"
#include "boot_request.h"

/*
 * Application -- demonstrates the update lifecycle.
 *
 * Self-confirmation
 * -----------------
 * The bootloader jumps to a TESTING image after incrementing its
 * failure counter. If the application does nothing, that counter
 * reaches the threshold after a few restarts and the bootloader
 * switches back to the previous slot.
 *
 * That is the rollback mechanism itself: a correct CRC proves the
 * image's integrity, not its correctness. A firmware transmitted
 * without a single corrupted bit can still crash within its first
 * second.
 *
 * The application must therefore confirm its own start-up: set the
 * state to VALID and clear the counter. Without that confirmation no
 * rollback is possible -- but without it, no firmware survives past
 * three boots either.
 *
 * When to confirm
 * ---------------
 * Confirming on the first line of main() would empty the mechanism
 * of meaning: an application that crashes moments later would still
 * have been declared healthy.
 *
 * What counts as a successful start depends on the product. Here a
 * few complete application cycles are required. A real system would
 * confirm after checking its peripherals, establishing a link, or
 * meeting whatever criterion is meaningful for it.
 *
 * Inherited watchdog
 * ------------------
 * The bootloader started the IWDG before jumping, and it can no
 * longer be stopped. This application must therefore refresh it, or
 * it will be reset after three seconds -- which advances the failure
 * counter and, after three attempts, triggers rollback.
 *
 * That is the mechanism working as intended: an application that
 * hangs has no way to keep refreshing, so its failure is detected
 * without it having to report anything.
 *
 * Refreshing here is conditional on the application cycle having
 * advanced. An unconditional refresh would only catch a complete
 * hang: a program looping over a section that happens to contain the
 * call would keep the watchdog quiet while doing nothing useful.
 */

#define RCC_BASE            0x40021000UL
#define GPIOA_BASE          0x48000000UL
#define USART2_BASE         0x40004400UL

#define RCC_AHB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x4C))
#define RCC_APB1ENR1        (*(volatile uint32_t *)(RCC_BASE + 0x58))

#define GPIOA_MODER         (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_ODR           (*(volatile uint32_t *)(GPIOA_BASE + 0x14))
#define GPIOA_AFRL          (*(volatile uint32_t *)(GPIOA_BASE + 0x20))

#define USART2_CR1          (*(volatile uint32_t *)(USART2_BASE + 0x00))
#define USART2_BRR          (*(volatile uint32_t *)(USART2_BASE + 0x0C))
#define USART2_ISR          (*(volatile uint32_t *)(USART2_BASE + 0x1C))
#define USART2_RDR          (*(volatile uint32_t *)(USART2_BASE + 0x24))
#define USART2_TDR          (*(volatile uint32_t *)(USART2_BASE + 0x28))

#define SCB_VTOR            (*(volatile uint32_t *)0xE000ED08UL)

#define LED_PIN             5

/* Application cycles required before declaring the image healthy. */
#define CYCLES_AVANT_CONFIRMATION   3


/* ----------------------------------------------------------------
 * Minimal UART -- the application never needs to receive
 * ---------------------------------------------------------------- */

static void uart_init(void)
{
    RCC_AHB2ENR  |= (1U << 0);
    RCC_APB1ENR1 |= (1U << 17);

    GPIOA_MODER &= ~((3U << 4) | (3U << 6));
    GPIOA_MODER |=  ((2U << 4) | (2U << 6));
    GPIOA_AFRL  &= ~((0xFU << 8) | (0xFU << 12));
    GPIOA_AFRL  |=  ((7U << 8)   | (7U << 12));

    USART2_BRR = 4000000UL / 115200UL;
    USART2_CR1 = (1U << 3) | (1U << 2) | (1U << 0);
}

static void uart_putc(char c)
{
    while (!(USART2_ISR & (1U << 7))) { }
    USART2_TDR = (uint32_t)(uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}

static void uart_hex32(uint32_t v)
{
    const char *d = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uart_putc(d[(v >> i) & 0xFU]);
    }
}

static void uart_dec(uint32_t v)
{
    char tmp[10];
    int n = 0;
    if (v == 0U) {
        uart_putc('0');
        return;
    }
    while (v > 0U) {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (n-- > 0) {
        uart_putc(tmp[n]);
    }
}

static void delay(volatile uint32_t n)
{
    while (n--) {
        __asm__("nop");
    }
}

/* Non-blocking UART receive. Returns 1 and stores the byte if one is
 * available, 0 otherwise. RXNE is bit 5 of USART2_ISR. */
static int uart_getc(uint8_t *c)
{
    if (USART2_ISR & (1U << 5)) {
        *c = (uint8_t)(USART2_RDR & 0xFFU);
        return 1;
    }
    return 0;
}

/* Software reset via AIRCR -- identical to what the bootloader uses.
 * The CPU restarts from the reset vector; RAM is preserved. */
static void software_reset(void)
{
    *(volatile uint32_t *)0xE000ED0CUL = 0x05FA0004UL;
    while (1) { }    /* unreachable, silences compiler warning */
}


/* ----------------------------------------------------------------
 * Determining which slot is executing
 * ---------------------------------------------------------------- */

static uint8_t slot_courant(void)
{
    /* SCB_VTOR holds the vector table address, set by the bootloader
       just before jumping. It therefore points at the start of the
       slot currently executing.

       This introspection lets the application know where it runs
       without being told -- useful to confirm that the right binary
       reached the right slot. */
    return (SCB_VTOR >= SLOT_B_ADDR) ? SLOT_B : SLOT_A;
}


/* ----------------------------------------------------------------
 * Start-up confirmation
 * ---------------------------------------------------------------- */

static void confirmer_demarrage(void)
{
    metadata_t meta;

    if (!metadata_read(&meta)) {
        uart_puts("  metadata unreadable, cannot confirm\r\n");
        return;
    }

    uint8_t moi = slot_courant();

    if (meta.slot[moi].state == STATE_VALID && meta.boot_fail_count == 0U) {
        uart_puts("  already confirmed\r\n");
        return;
    }

    /* Only MY slot's state changes. The other one describes an image
       this build knows nothing about, and which must stay usable as a
       fallback. */
    meta.slot[moi].state = STATE_VALID;
    meta.boot_fail_count = 0;

    if (metadata_write(&meta) == META_OK) {
        uart_puts("  start-up confirmed: state VALID, counter cleared\r\n");
    } else {
        uart_puts("  metadata write failed\r\n");
    }
}


/* ----------------------------------------------------------------
 * Self-integrity check
 * ---------------------------------------------------------------- */

static void verifier_image(const metadata_t *meta)
{
    uint8_t moi = slot_courant();
    uint32_t base = SLOT_ADDR(moi);
    const slot_info_t *info = &meta->slot[moi];

    if (info->size == 0U || info->size > SLOT_SIZE) {
        uart_puts("  invalid size, check skipped\r\n");
        return;
    }

    uint32_t calcule = crc32_compute((const uint8_t *)base, info->size);

    uart_puts("  computed CRC : ");
    uart_hex32(calcule);
    uart_puts("\r\n  expected CRC : ");
    uart_hex32(info->crc32);
    uart_puts(calcule == info->crc32 ? "   match\r\n"
                                     : "   MISMATCH\r\n");
}


/* ----------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------- */

int main(void)
{
    /* User LED on PA5 */
    RCC_AHB2ENR |= (1U << 0);
    GPIOA_MODER &= ~(3U << (LED_PIN * 2));
    GPIOA_MODER |=  (1U << (LED_PIN * 2));

    uart_init();
    crc32_init();

    uint8_t slot = slot_courant();

    uart_puts("\r\n########################################\r\n");
    uart_puts("  APPLICATION -- slot ");
    uart_putc((char)('A' + slot));
    uart_puts("\r\n########################################\r\n");

    uart_puts("VTOR        : ");
    uart_hex32(SCB_VTOR);
    uart_puts("\r\n");

    metadata_t meta;
    if (metadata_read(&meta)) {
        uart_puts("State       : ");
        switch (meta.slot[slot_courant()].state) {
        case STATE_EMPTY:       uart_puts("EMPTY");       break;
        case STATE_IN_PROGRESS: uart_puts("IN_PROGRESS"); break;
        case STATE_TESTING:     uart_puts("TESTING");     break;
        case STATE_VALID:       uart_puts("VALID");       break;
        default:                uart_dec(meta.slot[slot_courant()].state); break;
        }
        uart_puts("\r\nVersion     : ");
        uart_hex32(meta.slot[slot_courant()].version);
        uart_puts("\r\nBoot fails  : ");
        uart_dec(meta.boot_fail_count);
        uart_puts("\r\n\r\nImage verification:\r\n");
        verifier_image(&meta);
    } else {
        uart_puts("No readable metadata\r\n");
    }

    uart_puts("\r\nApplication cycles before confirming: ");
    uart_dec(CYCLES_AVANT_CONFIRMATION);
    uart_puts("\r\n\r\n");

    uint32_t cycle = 0;
    uint32_t dernier_cycle_vu = 0;
    int confirmee = 0;

    while (1) {
        GPIOA_ODR ^= (1U << LED_PIN);
        delay(300000);

        cycle++;

        /* Conditional refresh: the watchdog is only reassured if the
           cycle counter has genuinely advanced since the last pass.
           The criterion is trivial here, but the shape is the right
           one -- under an RTOS a supervisor task would check in the
           same way that every other task has made progress. */
        iwdg_feed_if(cycle != dernier_cycle_vu);
        dernier_cycle_vu = cycle;

        uart_puts("cycle ");
        uart_dec(cycle);
        uart_puts("\r\n");

        /* OTA trigger: if the host sends the character 'U' over UART,
         * the application sets the boot request flag and resets.
         * The bootloader will detect the flag and enter update mode
         * immediately, without any time window constraint.
         *
         * Usage from a PC terminal:
         *   echo -n 'U' > /dev/ttyACM0
         *   flash                         (run immediately after) */
        uint8_t rx;
        if (uart_getc(&rx) && rx == 'U') {
            uart_puts("\r\nOTA request received -- resetting to bootloader\r\n");
            boot_request_set();
            software_reset();
        }

        /* Confirmation only happens after several complete cycles.
           Confirming on the first line of main() would validate an
           application that crashes immediately afterwards. */
        if (!confirmee && cycle >= CYCLES_AVANT_CONFIRMATION) {
            uart_puts("\r\nConfirmation:\r\n");
            confirmer_demarrage();
            uart_puts("\r\n");
            confirmee = 1;
        }
    }
}
