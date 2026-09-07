#include <stdint.h>
#include "crc.h"

#define RCC_BASE        0x40021000UL
#define GPIOA_BASE      0x48000000UL
#define USART2_BASE     0x40004400UL

#define RCC_AHB2ENR     (*(volatile uint32_t *)(RCC_BASE + 0x4C))
#define RCC_APB1ENR1    (*(volatile uint32_t *)(RCC_BASE + 0x58))

#define GPIOA_MODER     (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_AFRL      (*(volatile uint32_t *)(GPIOA_BASE + 0x20))

#define USART2_CR1      (*(volatile uint32_t *)(USART2_BASE + 0x00))
#define USART2_BRR      (*(volatile uint32_t *)(USART2_BASE + 0x0C))
#define USART2_ISR      (*(volatile uint32_t *)(USART2_BASE + 0x1C))
#define USART2_TDR      (*(volatile uint32_t *)(USART2_BASE + 0x28))

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
    while (!(USART2_ISR & (1U << 7)));
    USART2_TDR = c;
}

static void uart_puts(const char *s)
{
    while (*s) uart_putc(*s++);
}

static void uart_hex32(uint32_t v)
{
    const char *d = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uart_putc(d[(v >> i) & 0xF]);
    }
}

int main(void)
{
    uart_init();
    crc32_init();

    uart_puts("\r\n=== TEST MODULE CRC ===\r\n");

    uart_puts("une passe      -> ");
    uart_hex32(crc32_compute((const uint8_t *)"123456789", 9));
    uart_puts("   attendu 0x9B63D02C\r\n");

    /* API incrementale : le meme calcul en deux morceaux */
    crc32_reset();
    crc32_update((const uint8_t *)"12345", 5);
    crc32_update((const uint8_t *)"6789", 4);
    uart_puts("deux appels    -> ");
    uart_hex32(crc32_get());
    uart_puts("   attendu 0x9B63D02C\r\n");

    uart_puts("quatre zeros   -> ");
    uart_hex32(crc32_compute((const uint8_t[]){0,0,0,0}, 4));
    uart_puts("   attendu 0xC704DD7B\r\n");

    while (1);
}