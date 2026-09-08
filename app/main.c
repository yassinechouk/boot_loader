#include <stdint.h>

#include "crc.h"
#include "flash.h"
#include "metadata.h"
#include "metadata_mgr.h"

/*
 * Application — demonstration du cycle de mise a jour.
 *
 * Auto-confirmation
 * -----------------
 * Le bootloader saute vers une image en TESTING apres avoir
 * incremente son compteur d'echecs. Si l'application ne fait rien,
 * ce compteur atteint le seuil au bout de quelques redemarrages et
 * le bootloader bascule sur le slot precedent.
 *
 * C'est le mecanisme meme du rollback : un CRC correct prouve
 * l'integrite de l'image, pas son bon fonctionnement. Un firmware
 * transmis sans la moindre corruption peut planter des sa premiere
 * seconde.
 *
 * L'application doit donc confirmer elle-meme qu'elle demarre :
 * passer l'etat a VALID et remettre le compteur a zero. Sans cette
 * confirmation, aucun rollback n'est possible — mais sans elle non
 * plus, aucun firmware ne tient au-dela de trois demarrages.
 *
 * Quand confirmer
 * ---------------
 * Confirmer des la premiere ligne du main() viderait le mecanisme de
 * son sens : une application qui plante en cours de route serait
 * quand meme declaree valide.
 *
 * Ce qui constitue un demarrage reussi depend du produit. Ici on
 * attend qu'un cycle applicatif complet se soit deroule. Un vrai
 * systeme confirmerait apres avoir verifie ses peripheriques, obtenu
 * une communication, ou atteint tout autre critere pertinent.
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

/* Nombre de cycles applicatifs avant de se declarer sain. */
#define CYCLES_AVANT_CONFIRMATION   9999999


/* ----------------------------------------------------------------
 * UART minimal — l'application n'a pas besoin de recevoir
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
 * Determination du slot d'execution
 * ---------------------------------------------------------------- */

static uint8_t slot_courant(void)
{
    /* SCB_VTOR contient l'adresse de la table des vecteurs, que le
       bootloader a positionnee avant de sauter. Elle designe donc le
       debut du slot en cours d'execution.
    
       Cette introspection permet a l'application de savoir ou elle
       tourne sans que personne ne le lui dise — utile pour verifier
       que le bon binaire a ete envoye au bon emplacement. */
    return (SCB_VTOR >= SLOT_B_ADDR) ? SLOT_B : SLOT_A;
}


/* ----------------------------------------------------------------
 * Confirmation de bon demarrage
 * ---------------------------------------------------------------- */

static void confirmer_demarrage(void)
{
    metadata_t meta;

    if (!metadata_read(&meta)) {
        uart_puts("  metadonnees illisibles, confirmation impossible\r\n");
        return;
    }

    uint8_t moi = slot_courant();

    if (meta.slot[moi].state == STATE_VALID && meta.boot_fail_count == 0U) {
        uart_puts("  deja confirmee\r\n");
        return;
    }

    /* Seul l'etat de MON slot change. Celui de l'autre decrit une
       image que je ne connais pas et qui doit rester utilisable
       comme repli. */
    meta.slot[moi].state = STATE_VALID;
    meta.boot_fail_count = 0;

    if (metadata_write(&meta) == META_OK) {
        uart_puts("  demarrage confirme : etat VALID, compteur remis a zero\r\n");
    } else {
        uart_puts("  echec d'ecriture des metadonnees\r\n");
    }
}


/* ----------------------------------------------------------------
 * Verification de sa propre integrite
 * ---------------------------------------------------------------- */

static void verifier_image(const metadata_t *meta)
{
    uint8_t moi = slot_courant();
    uint32_t base = SLOT_ADDR(moi);
    const slot_info_t *info = &meta->slot[moi];

    if (info->size == 0U || info->size > SLOT_SIZE) {
        uart_puts("  taille invalide, verification ignoree\r\n");
        return;
    }

    uint32_t calcule = crc32_compute((const uint8_t *)base, info->size);

    uart_puts("  CRC calcule : ");
    uart_hex32(calcule);
    uart_puts("\r\n  CRC attendu : ");
    uart_hex32(info->crc32);
    uart_puts(calcule == info->crc32 ? "   concordant\r\n"
                                     : "   DIVERGENT\r\n");
}


/* ----------------------------------------------------------------
 * Point d'entree
 * ---------------------------------------------------------------- */

int main(void)
{
    /* LED sur PA5 */
    RCC_AHB2ENR |= (1U << 0);
    GPIOA_MODER &= ~(3U << (LED_PIN * 2));
    GPIOA_MODER |=  (1U << (LED_PIN * 2));

    uart_init();
    crc32_init();

    uint8_t slot = slot_courant();

    uart_puts("\r\n########################################\r\n");
    uart_puts("  APPLICATION — slot ");
    uart_putc((char)('A' + slot));
    uart_puts("\r\n########################################\r\n");

    uart_puts("VTOR        : ");
    uart_hex32(SCB_VTOR);
    uart_puts("\r\n");

    metadata_t meta;
    if (metadata_read(&meta)) {
        uart_puts("Etat        : ");
        switch (meta.slot[slot_courant()].state) {
        case STATE_EMPTY:       uart_puts("EMPTY");       break;
        case STATE_IN_PROGRESS: uart_puts("IN_PROGRESS"); break;
        case STATE_TESTING:     uart_puts("TESTING");     break;
        case STATE_VALID:       uart_puts("VALID");       break;
        default:                uart_dec(meta.slot[slot_courant()].state); break;
        }
        uart_puts("\r\nVersion     : ");
        uart_hex32(meta.slot[slot_courant()].version);
        uart_puts("\r\nEchecs boot : ");
        uart_dec(meta.boot_fail_count);
        uart_puts("\r\n\r\nVerification de l'image :\r\n");
        verifier_image(&meta);
    } else {
        uart_puts("Aucune metadonnee lisible\r\n");
    }

    uart_puts("\r\nCycles applicatifs avant confirmation : ");
    uart_dec(CYCLES_AVANT_CONFIRMATION);
    uart_puts("\r\n\r\n");

    uint32_t cycle = 0;
    int confirmee = 0;

    while (1) {
        GPIOA_ODR ^= (1U << LED_PIN);
        delay(300000);

        cycle++;

        uart_puts("cycle ");
        uart_dec(cycle);
        uart_puts("\r\n");

        /* La confirmation n'intervient qu'apres plusieurs cycles
           complets. Confirmer des la premiere ligne du main()
           validerait une application qui plante juste apres. */
        if (!confirmee && cycle >= CYCLES_AVANT_CONFIRMATION) {
            uart_puts("\r\nConfirmation :\r\n");
            confirmer_demarrage();
            uart_puts("\r\n");
            confirmee = 1;
        }
    }
}
