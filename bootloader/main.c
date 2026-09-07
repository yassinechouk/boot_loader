#include <stdint.h>
#include "crc.h"
#include "flash.h"
#include "metadata.h"
#include "metadata_mgr.h"

/* ---------- Adresses de base ---------- */
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

static void uart_dec(uint32_t v)
{
    char buf[11];
    int i = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) uart_putc(buf[i]);
}

static int nb_ok = 0, nb_ko = 0;

static void chk(const char *nom, int cond)
{
    uart_puts(cond ? "  ok    " : "  ECHEC ");
    uart_puts(nom);
    uart_puts("\r\n");
    if (cond) nb_ok++; else nb_ko++;
}


int main(void)
{
    uart_init();
    crc32_init();

    uart_puts("\r\n=== TEST METADATA ===\r\n");

    metadata_t m;
    metadata_t r;

    /* --- Etat vierge --- */
    uart_puts("\r\n-- effacement initial --\r\n");
    chk("erase_all", metadata_erase_all() == META_OK);
    chk("aucune copie valide", metadata_read(&r) == 0);

    /* --- Premiere ecriture --- */
    uart_puts("\r\n-- premiere ecriture --\r\n");
    for (unsigned i = 0; i < sizeof(m); i++) ((uint8_t *)&m)[i] = 0;
    m.fw_size     = 12345;
    m.fw_crc32    = 0xAABBCCDD;
    m.fw_version  = 0x00010203;
    m.active_slot = SLOT_A;
    m.state       = STATE_VALID;

    chk("ecriture acceptee", metadata_write(&m) == META_OK);
    chk("relecture ok", metadata_read(&r) == 1);
    chk("compteur = 1", r.counter == 1);
    chk("fw_size conserve", r.fw_size == 12345);
    chk("fw_crc32 conserve", r.fw_crc32 == 0xAABBCCDD);
    chk("magic pose", r.magic == METADATA_MAGIC);
    chk("reserved a zero", r.reserved[0] == 0 && r.reserved[4] == 0);
    chk("page A ecrite", !flash_is_erased(META_PAGE_A_ADDR, 32));
    chk("page B vierge", flash_is_erased(META_PAGE_B_ADDR, 32));

    /* --- Alternance --- */
    uart_puts("\r\n-- alternance des pages --\r\n");
    m.fw_size = 200;
    chk("2e ecriture", metadata_write(&m) == META_OK);
    metadata_read(&r);
    chk("compteur = 2", r.counter == 2);
    chk("fw_size = 200", r.fw_size == 200);
    chk("page B ecrite", !flash_is_erased(META_PAGE_B_ADDR, 32));

    m.fw_size = 300;
    chk("3e ecriture", metadata_write(&m) == META_OK);
    metadata_read(&r);
    chk("compteur = 3", r.counter == 3);
    chk("fw_size = 300", r.fw_size == 300);

    /* --- Les champs imposes par le module --- */
    uart_puts("\r\n-- champs non falsifiables --\r\n");
    m.counter    = 9999;
    m.magic      = 0x11111111;
    m.meta_crc32 = 0xDEADBEEF;
    m.fw_size    = 444;
    chk("ecriture", metadata_write(&m) == META_OK);
    metadata_read(&r);
    chk("counter ignore", r.counter == 4);
    chk("magic corrige", r.magic == METADATA_MAGIC);
    chk("copie valide", metadata_is_valid(&r));
    chk("fw_size repris", r.fw_size == 444);

    /* --- Persistance apres reset --- */
    uart_puts("\r\n-- etat courant --\r\n");
    uart_puts("  compteur    : ");  uart_dec(r.counter);      uart_puts("\r\n");
    uart_puts("  fw_size     : ");  uart_dec(r.fw_size);      uart_puts("\r\n");
    uart_puts("  fw_crc32    : ");  uart_hex32(r.fw_crc32);   uart_puts("\r\n");
    uart_puts("  slot actif  : ");  uart_putc((char)('A' + r.active_slot));
    uart_puts("\r\n");
    uart_puts("  etat        : ");  uart_dec(r.state);        uart_puts("\r\n");

    /* --- Arguments nuls --- */
    uart_puts("\r\n-- arguments nuls --\r\n");
    chk("read(NULL)", metadata_read(0) == 0);
    chk("write(NULL)", metadata_write(0) == META_ERR_ARG);

    /* --- Bilan --- */
    uart_puts("\r\n=== ");
    uart_dec((uint32_t)nb_ok);
    uart_puts(" reussis, ");
    uart_dec((uint32_t)nb_ko);
    uart_puts(" echecs ===\r\n");

    while (1);
}