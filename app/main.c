#include <stdint.h>
#include "protocol.h"
#include "metadata.h"

/* ---------- Adresses de base ---------- */
#define RCC_BASE        0x40021000UL
#define GPIOA_BASE      0x48000000UL
#define USART2_BASE     0x40004400UL

/* ---------- Registres RCC ---------- */
#define RCC_AHB2ENR     (*(volatile uint32_t *)(RCC_BASE + 0x4C))
#define RCC_APB1ENR1    (*(volatile uint32_t *)(RCC_BASE + 0x58))

/* ---------- Registres GPIOA ---------- */
#define GPIOA_MODER     (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_ODR       (*(volatile uint32_t *)(GPIOA_BASE + 0x14))
#define GPIOA_AFRL      (*(volatile uint32_t *)(GPIOA_BASE + 0x20))

/* ---------- Registres USART2 ---------- */
#define USART2_CR1      (*(volatile uint32_t *)(USART2_BASE + 0x00))
#define USART2_BRR      (*(volatile uint32_t *)(USART2_BASE + 0x0C))
#define USART2_ISR      (*(volatile uint32_t *)(USART2_BASE + 0x1C))
#define USART2_RDR      (*(volatile uint32_t *)(USART2_BASE + 0x24))
#define USART2_TDR      (*(volatile uint32_t *)(USART2_BASE + 0x28))

#define LED_PIN         5

static void delay(volatile uint32_t count)
{
    while (count--) {
        __asm__("nop");
    }
}

static void uart_init(void)
{
    RCC_AHB2ENR  |= (1U << 0);
    RCC_APB1ENR1 |= (1U << 17);

    GPIOA_MODER &= ~((3U << (2 * 2)) | (3U << (3 * 2)));
    GPIOA_MODER |=  ((2U << (2 * 2)) | (2U << (3 * 2)));

    GPIOA_AFRL &= ~((0xFU << (2 * 4)) | (0xFU << (3 * 4)));
    GPIOA_AFRL |=  ((7U   << (2 * 4)) | (7U   << (3 * 4)));

    USART2_BRR = 4000000UL / 115200UL;
    USART2_CR1 = (1U << 3) | (1U << 2) | (1U << 0);
}

static void uart_send_char(char c)
{
    while (!(USART2_ISR & (1U << 7)));
    USART2_TDR = c;
}

static void uart_send_string(const char *s)
{
    while (*s) {
        uart_send_char(*s++);
    }
}

int main(void)
{
    RCC_AHB2ENR |= (1U << 0);
    GPIOA_MODER &= ~(3U << (LED_PIN * 2));
    GPIOA_MODER |=  (1U << (LED_PIN * 2));

    uart_init();

    uart_send_string("\r\n### APPLICATION ###\r\n");

    while (1) {
        GPIOA_ODR ^= (1U << LED_PIN);
        uart_send_string("app\r\n");
        delay(100000);      /* 5x plus rapide que le bootloader */
    }
}