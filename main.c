#include <stdint.h>

/* ---------- Adresses de base (RM0351, section 2.2.2) ---------- */
#define RCC_BASE        0x40021000UL
#define GPIOA_BASE      0x48000000UL

/* ---------- Registres utilisés ---------- */
#define RCC_AHB2ENR     (*(volatile uint32_t *)(RCC_BASE   + 0x4C))
#define GPIOA_MODER     (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_ODR       (*(volatile uint32_t *)(GPIOA_BASE + 0x14))

/* ---------- LD2 est câblée sur PA5 ---------- */
#define LED_PIN         5

static void delay(volatile uint32_t count)
{
    while (count--) {
        __asm__("nop");
    }
}

int main(void)
{
    /* 1. Activer l'horloge de GPIOA */
    RCC_AHB2ENR |= (1U << 0);

    /* 2. PA5 en sortie : 01 dans les bits [11:10] de MODER */
    GPIOA_MODER &= ~(3U << (LED_PIN * 2));
    GPIOA_MODER |=  (1U << (LED_PIN * 2));

    /* 3. Clignoter */
    while (1) {
        GPIOA_ODR ^= (1U << LED_PIN);
delay(500000);    }
}