#include <stdint.h>
#include "crc.h"
#include "flash.h"
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
#define GPIOA_AFRL      (*(volatile uint32_t *)(GPIOA_BASE + 0x20))

/* ---------- Registres USART2 ---------- */
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

static void resultat(flash_status_t st, flash_status_t attendu)
{
    uart_hex32((uint32_t)st);
    uart_puts(st == attendu ? "   OK\r\n" : "   ECHEC\r\n");
}


int main(void)
{
    uart_init();
    crc32_init();

    uart_puts("\r\n=== TEST MODULE FLASH ===\r\n");
    uart_puts("cible : slot B (banque 2)\r\n\r\n");

    const uint32_t addr = SLOT_B_ADDR;

    /* --- 1. Effacement d'une page --- */
    uart_puts("erase page      -> ");
    resultat(flash_erase_page(addr), FLASH_OK);

    /* --- 2. La page doit etre vierge --- */
    uart_puts("page vierge     -> ");
    uart_hex32((uint32_t)flash_is_erased(addr, FLASH_PAGE_SIZE));
    uart_puts("   attendu 0x00000001\r\n");

    /* --- 3. Ecriture de 16 octets --- */
    static const uint8_t motif[16] = {
        0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
        0xCA, 0xFE, 0xBA, 0xBE, 0x05, 0x06, 0x07, 0x08
    };

    uart_puts("write 16 o      -> ");
    resultat(flash_write(addr, motif, 16), FLASH_OK);

    /* --- 4. Relecture directe --- */
    uart_puts("relu [0..3]     -> ");
    uart_hex32(*(volatile uint32_t *)addr);
    uart_puts("   attendu 0xEFBEADDE\r\n");

    uart_puts("relu [8..11]    -> ");
    uart_hex32(*(volatile uint32_t *)(addr + 8));
    uart_puts("   attendu 0xBEBAFECA\r\n");

    /* --- 5. CRC du contenu ecrit --- */
    uart_puts("crc du motif    -> ");
    uart_hex32(crc32_compute((const uint8_t *)addr, 16));
    uart_puts("\r\n");

    uart_puts("crc de la source-> ");
    uart_hex32(crc32_compute(motif, 16));
    uart_puts("   doit etre identique\r\n\r\n");

    /* --- 6. Cas d'erreur : adresse non alignee sur 8 --- */
    uart_puts("addr non 8      -> ");
    resultat(flash_write(addr + 1, motif, 8), FLASH_ERR_ALIGN);

    /* --- 7. Cas d'erreur : longueur non multiple de 8 --- */
    uart_puts("len non 8       -> ");
    resultat(flash_write(addr + 64, motif, 5), FLASH_ERR_ALIGN);

    /* --- 8. Cas d'erreur : reecriture sans effacement --- */
    uart_puts("reecriture      -> ");
    resultat(flash_write(addr, motif, 8), FLASH_ERR_PROG);

    /* --- 9. Cas d'erreur : hors flash --- */
    uart_puts("hors flash      -> ");
    resultat(flash_write(0x20000000UL, motif, 8), FLASH_ERR_RANGE);

    /* --- 10. Cas d'erreur : page non alignee --- */
    uart_puts("page non alignee-> ");
    resultat(flash_erase_page(addr + 100), FLASH_ERR_ALIGN);

    uart_puts("\r\n=== FIN ===\r\n");

    while (1);
}