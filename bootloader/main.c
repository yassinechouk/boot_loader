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
 * Bootloader — point d'entree.
 *
 * Sequence de demarrage
 * ---------------------
 *   1. Initialiser les peripheriques
 *   2. Lire les metadonnees
 *   3. Decider : sauter vers l'application, ou attendre une mise a jour
 *   4. Si mise a jour demandee, la traiter puis redemarrer
 *
 * La decision de l'etape 3 depend de l'etat enregistre :
 *
 *   Aucune metadonnee   -> mode reception : rien a executer
 *   EMPTY               -> mode reception
 *   IN_PROGRESS         -> mode reception : un transfert a ete
 *                          interrompu, le slot cible contient une
 *                          image partielle
 *   VALID               -> saut immediat
 *   TESTING             -> saut, apres incrementation du compteur
 *                          d'echecs. Si le seuil est atteint,
 *                          rollback vers l'autre slot.
 *
 * Le mecanisme TESTING
 * --------------------
 * Un CRC correct prouve l'integrite d'une image, pas son bon
 * fonctionnement : un firmware transmis sans la moindre corruption
 * peut planter des sa premiere seconde.
 *
 * Le bootloader saute donc vers une image en TESTING apres avoir
 * incremente son compteur d'echecs. L'application, si elle demarre
 * correctement, doit remettre ce compteur a zero et passer l'etat a
 * VALID. Si elle plante, le watchdog provoque un reset et le
 * compteur n'a pas ete remis a zero : au-dela du seuil, le
 * bootloader bascule sur le slot precedent.
 *
 * L'application porte donc une responsabilite. Sans sa confirmation,
 * le rollback est impossible.
 */

#define BOOT_WAIT_MS        2000U   /* fenetre d'attente au demarrage */
#define MSI_DEFAULT_HZ      4000000UL

#define SCB_VTOR            (*(volatile uint32_t *)0xE000ED08UL)

/* Registres RCC, necessaires pour rendre les peripheriques a l'etat
   de reset avant de ceder la main. */
#define RCC_BASE            0x40021000UL
#define RCC_AHB1ENR         (*(volatile uint32_t *)(RCC_BASE + 0x48))
#define RCC_AHB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x4C))
#define RCC_APB1ENR1        (*(volatile uint32_t *)(RCC_BASE + 0x58))

#define NVIC_ICER0          (*(volatile uint32_t *)0xE000E180UL)
#define NVIC_ICER1          (*(volatile uint32_t *)0xE000E184UL)
#define NVIC_ICPR0          (*(volatile uint32_t *)0xE000E280UL)
#define NVIC_ICPR1          (*(volatile uint32_t *)0xE000E284UL)


/* ----------------------------------------------------------------
 * Journalisation
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
 * Saut vers l'application
 * ---------------------------------------------------------------- */

static int slot_looks_bootable(uint32_t base)
{
    uint32_t sp    = *(volatile uint32_t *)base;
    uint32_t reset = *(volatile uint32_t *)(base + 4U);

    /* Le premier mot est le stack pointer initial : il doit designer
       une adresse en RAM. Le second est l'adresse du reset handler,
       qui doit tomber dans la flash.

       Une image absente laisse 0xFFFFFFFF dans les deux, ce que ces
       tests rejettent. Sauter sur une image inexistante provoquerait
       un HardFault silencieux, sans aucun message. */
    if ((sp & 0xFFF00000UL) != 0x20000000UL) {
        return 0;
    }
    if (reset < FLASH_BASE_ADDR ||
        reset >= (FLASH_BASE_ADDR + FLASH_TOTAL_SIZE)) {
        return 0;
    }

    /* Le bit 0 doit valoir 1 : les Cortex-M n'executent que du Thumb,
       et ce bit le signale au processeur. Une adresse paire
       provoquerait un HardFault immediat. */
    if ((reset & 1U) == 0U) {
        return 0;
    }

    return 1;
}


static void jump_to_application(uint32_t base)
{
    uint32_t app_sp    = *(volatile uint32_t *)base;
    uint32_t app_reset = *(volatile uint32_t *)(base + 4U);

    uart_puts("\r\nSaut vers ");
    uart_hex32(base);
    uart_puts("\r\n\r\n");
    uart_flush();

    __asm__ volatile ("cpsid i");

    /* Rendre les peripheriques a un etat proche du reset.
       L'application les reconfigurera, mais elle part du principe
       qu'ils sont vierges : un USART deja actif ou une interruption
       encore armee produirait des comportements inexplicables. */
    systick_deinit();

    NVIC_ICER0 = 0xFFFFFFFFUL;      /* desarmer toutes les IRQ */
    NVIC_ICER1 = 0xFFFFFFFFUL;
    NVIC_ICPR0 = 0xFFFFFFFFUL;      /* effacer celles en attente */
    NVIC_ICPR1 = 0xFFFFFFFFUL;

    RCC_APB1ENR1 &= ~(1U << 17);    /* USART2 */
    RCC_AHB1ENR  &= ~(1U << 12);    /* CRC    */

    /* Deplacer la table des vecteurs. Sans cette ligne l'application
       demarre, puis plante a sa premiere interruption : le processeur
       chercherait le handler dans la table du bootloader. */
    SCB_VTOR = base;

    __asm__ volatile ("dsb");
    __asm__ volatile ("isb");

    /* Charger le stack pointer de l'application avant de sauter.
       Celui du bootloader n'a plus de sens de l'autre cote. */
    __asm__ volatile ("msr msp, %0" : : "r" (app_sp) : );

    __asm__ volatile ("cpsie i");

    /* Le saut lui-meme. La fonction ne revient jamais. */
    ((void (*)(void))app_reset)();

    /* Inatteignable */
    while (1) {
    }
}


/* ----------------------------------------------------------------
 * Verification d'une image avant execution
 * ---------------------------------------------------------------- */

static int image_is_intact(const metadata_t *meta)
{
    if (meta->fw_size == 0U || meta->fw_size > SLOT_SIZE) {
        return 0;
    }

    uint32_t base = SLOT_ADDR(meta->active_slot);

    if (!slot_looks_bootable(base)) {
        return 0;
    }

    /* Recalcul du CRC a chaque demarrage. Une cellule flash peut se
       degrader avec le temps ; mieux vaut le decouvrir ici que
       d'executer du code corrompu. */
    uint32_t calcule = crc32_compute((const uint8_t *)base, meta->fw_size);

    return (calcule == meta->fw_crc32);
}


/* ----------------------------------------------------------------
 * Mode reception
 * ---------------------------------------------------------------- */

static void update_mode(uint32_t limite_ms)
{
    uart_puts("Mode reception");
    if (limite_ms > 0U) {
        uart_puts(" (");
        uart_dec(limite_ms);
        uart_puts(" ms)");
    }
    uart_puts("\r\n");

    uint32_t depart = millis();

    while (1) {
        uint32_t maintenant = millis();

        protocol_poll(maintenant);

        if (protocol_update_complete()) {
            uart_puts("\r\nMise a jour terminee, redemarrage\r\n");
            uart_flush();
            delay_ms(50);

            /* Reset logiciel via AIRCR. Repartir du debut garantit un
               etat propre plutot que de sauter depuis un bootloader
               dont les peripheriques sont deja configures. */
            *(volatile uint32_t *)0xE000ED0CUL = 0x05FA0004UL;

            while (1) {
            }
        }

        /* Une fenetre d'attente n'expire que si rien n'a commence :
           un transfert en cours ne doit pas etre interrompu par le
           delai de demarrage. */
        if (limite_ms > 0U &&
            protocol_get_state() == PROTO_IDLE &&
            (maintenant - depart) > limite_ms) {
            return;
        }
    }
}


/* ----------------------------------------------------------------
 * Point d'entree
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
        uart_puts("Aucune metadonnee valide\r\n");
        update_mode(0);             /* attente sans limite */
        while (1) { }
    }

    uart_puts("Slot actif  : ");
    uart_putc((char)('A' + meta.active_slot));
    uart_puts("\r\nEtat        : ");
    log_state(meta.state);
    uart_puts("\r\nTaille      : ");
    uart_dec(meta.fw_size);
    uart_puts(" octets\r\nVersion     : ");
    uart_hex32(meta.fw_version);
    uart_puts("\r\nEchecs boot : ");
    uart_dec(meta.boot_fail_count);
    uart_puts("\r\n\r\n");

    switch (meta.state) {

    case STATE_VALID:
        if (image_is_intact(&meta)) {
            update_mode(BOOT_WAIT_MS);   /* laisser une chance au PC */
            jump_to_application(SLOT_ADDR(meta.active_slot));
        }
        uart_puts("Image invalide malgre l'etat VALID\r\n");
        break;

    case STATE_TESTING:
        if (meta.boot_fail_count >= MAX_BOOT_FAILURES) {
            /* L'application n'a jamais confirme son bon
               fonctionnement. On revient au slot precedent. */
            uart_puts("Seuil d'echecs atteint, rollback\r\n");

            uint8_t autre = OTHER_SLOT(meta.active_slot);
            metadata_t precedent = meta;
            precedent.active_slot     = autre;
            precedent.state           = STATE_VALID;
            precedent.boot_fail_count = 0;

            if (metadata_write(&precedent) == META_OK &&
                slot_looks_bootable(SLOT_ADDR(autre))) {
                uart_puts("Retour au slot ");
                uart_putc((char)('A' + autre));
                uart_puts("\r\n");
                jump_to_application(SLOT_ADDR(autre));
            }
            uart_puts("Aucune image de repli exploitable\r\n");
            break;
        }

        if (image_is_intact(&meta)) {
            /* Incrementer AVANT de sauter. Si l'application plante,
               le compteur aura deja progresse au prochain reset.
               L'incrementer apres n'aurait aucun effet, puisqu'on ne
               revient jamais du saut. */
            metadata_t essai = meta;
            essai.boot_fail_count = (uint8_t)(meta.boot_fail_count + 1U);
            metadata_write(&essai);

            uart_puts("Essai ");
            uart_dec(essai.boot_fail_count);
            uart_putc('/');
            uart_dec(MAX_BOOT_FAILURES);
            uart_puts("\r\n");

            update_mode(BOOT_WAIT_MS);
            jump_to_application(SLOT_ADDR(meta.active_slot));
        }
        uart_puts("Image en test invalide\r\n");
        break;

    case STATE_IN_PROGRESS:
        uart_puts("Transfert interrompu detecte\r\n");
        break;

    case STATE_EMPTY:
    default:
        uart_puts("Aucun firmware installe\r\n");
        break;
    }

    /* Tous les chemins qui n'ont pas saute aboutissent ici : il n'y a
       rien d'executable, on attend indefiniment une mise a jour. */
    update_mode(0);

    while (1) {
    }
}
