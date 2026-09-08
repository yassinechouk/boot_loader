#include <stdint.h>

#include "crc.h"
#include "flash.h"
#include "metadata.h"
#include "metadata_mgr.h"

/*
 * Application — firmware update cycle demonstration.
 *
 * Self-confirmation
 * -----------------
 * The bootloader jumps to a TESTING image after incrementing
 * its failure counter. If the application does nothing, the
 * counter reaches the threshold after a few reboots and the
 * bootloader switches back to the previous slot.
 *
 * This is the rollback mechanism itself: a correct CRC proves
 * the integrity of the image, not that it works correctly. A
 * firmware transferred without any corruption can crash within
 * its first second.
 *
 * The application must therefore confirm its own successful boot:
 * set the state to VALID and reset the counter to zero. Without
 * this confirmation, no rollback is possible — but without it,
 * no firmware survives beyond three boots either.
 *
 * When to confirm
 * ---------------
 * Confirming at the first line of main() would defeat the
 * mechanism: an application that crashes mid-run would still
 * be declared valid.
 *
 * What constitutes a successful boot depends on the product. Here
 * we wait until one full application cycle has completed. A real
 * system would confirm after checking its peripherals, obtaining
 * a communication link, or reaching any other relevant criterion.
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
#define USART2_TDR          (*(volatile uint32_t *)(USART2_BASE + 0x28))

#define SCB_VTOR            (*(volatile uint32_t *)0xE000ED08UL)

#define LED_PIN             5

/* Number of application cycles before declaring healthy. */
#define CYCLES_BEFORE_CONFIRM   3


/* ----------------------------------------------------------------
 * Minimal UART — the application only needs to transmit
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


/* ----------------------------------------------------------------
 * Determining the current execution slot
 * ---------------------------------------------------------------- */

static uint8_t current_slot(void)
{
    /* SCB_VTOR holds the address of the vector table, which the
       bootloader positioned before jumping. It therefore points to
       the start of the currently executing slot.

       This introspection lets the application know where it is
       running without being told — useful for verifying that the
       correct binary was sent to the correct location. */
    return (SCB_VTOR >= SLOT_B_ADDR) ? SLOT_B : SLOT_A;
}


/* ----------------------------------------------------------------
 * Boot confirmation
 * ---------------------------------------------------------------- */

static void confirm_boot(void)
{
    metadata_t meta;

    if (!metadata_read(&meta)) {
        uart_puts("  metadata unreadable, cannot confirm\r\n");
        return;
    }

    uint8_t me = current_slot();

    if (meta.slot[me].state == STATE_VALID && meta.boot_fail_count == 0U) {
        uart_puts("  already confirmed\r\n");
        return;
    }

    /* Only MY slot's state changes. The other slot describes an image
       I don't know about and must remain usable as a fallback. */
    meta.slot[me].state = STATE_VALID;
    meta.boot_fail_count = 0;

    if (metadata_write(&meta) == META_OK) {
        uart_puts("  boot confirmed: state VALID, counter reset to zero\r\n");
    } else {
        uart_puts("  failed to write metadata\r\n");
    }
}


/* ----------------------------------------------------------------
 * Self-integrity check
 * ---------------------------------------------------------------- */

static void verify_image(const metadata_t *meta)
{
    uint8_t me = current_slot();
    uint32_t base = SLOT_ADDR(me);
    const slot_info_t *info = &meta->slot[me];

    if (info->size == 0U || info->size > SLOT_SIZE) {
        uart_puts("  invalid size, verification skipped\r\n");
        return;
    }

    uint32_t computed = crc32_compute((const uint8_t *)base, info->size);

    uart_puts("  Computed CRC : ");
    uart_hex32(computed);
    uart_puts("\r\n  Expected CRC : ");
    uart_hex32(info->crc32);
    uart_puts(computed == info->crc32 ? "   match\r\n"
                                      : "   MISMATCH\r\n");
}


/* ----------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------- */

int main(void)
{
    /* LED on PA5 */
    RCC_AHB2ENR |= (1U << 0);
    GPIOA_MODER &= ~(3U << (LED_PIN * 2));
    GPIOA_MODER |=  (1U << (LED_PIN * 2));

    uart_init();
    crc32_init();

    uint8_t slot = current_slot();

    uart_puts("\r\n########################################\r\n");
    uart_puts("  APPLICATION — slot ");
    uart_putc((char)('A' + slot));
    uart_puts("\r\n########################################\r\n");

    uart_puts("VTOR        : ");
    uart_hex32(SCB_VTOR);
    uart_puts("\r\n");

    metadata_t meta;
    if (metadata_read(&meta)) {
        uart_puts("State       : ");
        switch (meta.slot[current_slot()].state) {
        case STATE_EMPTY:       uart_puts("EMPTY");       break;
        case STATE_IN_PROGRESS: uart_puts("IN_PROGRESS"); break;
        case STATE_TESTING:     uart_puts("TESTING");     break;
        case STATE_VALID:       uart_puts("VALID");       break;
        default:                uart_dec(meta.slot[current_slot()].state); break;
        }
        uart_puts("\r\nVersion     : ");
        uart_hex32(meta.slot[current_slot()].version);
        uart_puts("\r\nBoot fails  : ");
        uart_dec(meta.boot_fail_count);
        uart_puts("\r\n\r\nImage verification:\r\n");
        verify_image(&meta);
    } else {
        uart_puts("No readable metadata\r\n");
    }

    uart_puts("\r\nApplication cycles before confirmation: ");
    uart_dec(CYCLES_BEFORE_CONFIRM);
    uart_puts("\r\n\r\n");

    uint32_t cycle = 0;
    int confirmed = 0;

    while (1) {
        GPIOA_ODR ^= (1U << LED_PIN);
        delay(300000);

        cycle++;

        uart_puts("cycle ");
        uart_dec(cycle);
        uart_puts("\r\n");

        /* Confirmation only happens after several complete cycles.
           Confirming at the first line of main() would validate an
           application that crashes right after. */
        if (!confirmed && cycle >= CYCLES_BEFORE_CONFIRM) {
            uart_puts("\r\nConfirmation:\r\n");
            confirm_boot();
            uart_puts("\r\n");
            confirmed = 1;
        }
    }
}
