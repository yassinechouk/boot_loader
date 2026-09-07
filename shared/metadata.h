#ifndef METADATA_H
#define METADATA_H

#include <stdint.h>

/* =========================================================
 * Plan memoire flash — STM32L476RG
 *
 * 1 Mo de flash, organisee en deux banques de 512 Ko,
 * pages uniformes de 2 Ko.
 *
 *   0x08000000  +------------------+
 *               |   BOOTLOADER     |  32 Ko
 *   0x08008000  +------------------+
 *               |     SLOT A       |  480 Ko
 *   0x08080000  +------------------+
 *               |     SLOT B       |  480 Ko
 *   0x080FF000  +------------------+
 *               |  METADONNEES A   |  2 Ko
 *   0x080FF800  +------------------+
 *               |  METADONNEES B   |  2 Ko
 *   0x08100000  +------------------+
 * ========================================================= */
#define FLASH_BASE_ADDR         0x08000000UL
#define FLASH_TOTAL_SIZE        (1024U * 1024U)
#define FLASH_PAGE_SIZE         2048U

#define BOOTLOADER_ADDR         0x08000000UL
#define BOOTLOADER_SIZE         (32U * 1024U)

#define SLOT_A_ADDR             0x08008000UL
#define SLOT_B_ADDR             0x08080000UL
#define SLOT_SIZE               (480U * 1024U)

#define META_PAGE_A_ADDR        0x080FF000UL
#define META_PAGE_B_ADDR        0x080FF800UL

/* =========================================================
 * Identification des slots
 * ========================================================= */
#define SLOT_A                  0U
#define SLOT_B                  1U

#define SLOT_ADDR(s)            (((s) == SLOT_A) ? SLOT_A_ADDR : SLOT_B_ADDR)
#define OTHER_SLOT(s)           (((s) == SLOT_A) ? SLOT_B : SLOT_A)

/* =========================================================
 * Etats du firmware
 *
 * STATE_TESTING est l'etat intermediaire qui rend le rollback
 * possible : un CRC valide prouve l'integrite de l'image, pas
 * son bon fonctionnement. L'application doit confirmer
 * elle-meme son demarrage en passant a STATE_VALID.
 * ========================================================= */
typedef enum {
    STATE_EMPTY       = 0x00U,   /* slot vide ou jamais programme        */
    STATE_IN_PROGRESS = 0x01U,   /* transfert commence, non termine      */
    STATE_TESTING     = 0x02U,   /* installe, en attente de confirmation */
    STATE_VALID       = 0x03U    /* valide, bootable sans reserve        */
} fw_state_t;

/* =========================================================
 * Structure de metadonnees (24 octets, sans padding)
 *
 * Ecrite en double exemplaire dans deux pages distinctes.
 * A chaque mise a jour, seule la page inactive est effacee :
 * l'autre reste intacte et lisible, ce qui garantit qu'une
 * coupure d'alimentation ne detruit jamais les deux copies.
 *
 * La copie faisant foi est celle dont le compteur est le plus
 * eleve parmi celles dont le magic est valide.
 *
 * Le debordement du compteur 32 bits est ignore : l'endurance
 * de la flash (environ 10 000 cycles par page) constitue la
 * limite effective, cinq ordres de grandeur plus bas.
 * ========================================================= */
typedef struct {
    uint32_t magic;             /* METADATA_MAGIC si la copie est valide */
    uint32_t counter;           /* incremente a chaque ecriture          */
    uint32_t fw_size;           /* taille du firmware actif              */
    uint32_t fw_crc32;          /* CRC32 attendu de ce firmware          */
    uint32_t fw_version;        /* version du firmware actif             */
    uint8_t  active_slot;       /* SLOT_A ou SLOT_B                      */
    uint8_t  state;             /* fw_state_t                            */
    uint8_t  boot_fail_count;   /* demarrages rates consecutifs          */
    uint8_t  reserved;          /* extension future                      */
} metadata_t;

#define METADATA_MAGIC          0x424C4D44UL   /* "BLMD" */
#define METADATA_SIZE           sizeof(metadata_t)

/* =========================================================
 * Politique de rollback
 *
 * Au-dela de ce nombre de demarrages rates consecutifs sans
 * confirmation applicative, le bootloader bascule sur le slot
 * precedent.
 * ========================================================= */
#define MAX_BOOT_FAILURES       3U

/* =========================================================
 * Version du bootloader
 * Format : 0x00MMmmpp  (majeur, mineur, patch)
 * ========================================================= */
#define BOOTLOADER_VERSION      0x00000100UL   /* 0.1.0 */

#endif /* METADATA_H */
