#ifndef CRC_H
#define CRC_H

#include <stdint.h>

/*
 * Pilote du peripherique CRC materiel du STM32L4.
 *
 * Configuration retenue :
 *   POL      = 0x04C11DB7  (CRC-32 Ethernet, valeur de reset)
 *   INIT     = 0xFFFFFFFF  (valeur de reset)
 *   REV_IN   = 01          (inversion des bits par octet)
 *   REV_OUT  = 0
 *   XOR final: aucun
 *
 * REV_IN corrige l'ordre little-endian de la memoire : le peripherique
 * interprete un mot de 32 bits en big-endian interne, alors que le
 * Cortex-M range les octets en little-endian. L'inversion est faite en
 * materiel, sans cout logiciel.
 *
 * Cette configuration produit les memes valeurs que tools/crc32.py,
 * verifiees sur STM32L476RG :
 *     "123456789"  ->  0x9B63D02C
 *     4 zeros      ->  0xC704DD7B
 *     "STM32"      ->  0xF4F0FF62
 *
 * NON REENTRANT
 * -------------
 * Le peripherique CRC est une ressource partagee qui porte un etat
 * entre les appels. Un calcul lance depuis un contexte d'interruption
 * pendant qu'un autre est en cours en contexte principal corromprait
 * les deux resultats.
 *
 * Aucune protection n'est mise en place : dans ce bootloader, seul le
 * contexte principal calcule des CRC. La reception UART en interruption
 * se contente de remplir un tampon. Ajouter une section critique dans
 * crc32_update() augmenterait la latence d'interruption sur une boucle
 * appelee des centaines de fois par transfert, au risque de perdre des
 * octets — un vrai probleme cree pour s'en eviter un qui n'existe pas.
 *
 * Si un appel depuis une interruption devenait necessaire, il faudrait
 * revoir cette decision explicitement.
 */

/* Active l'horloge du peripherique et applique la configuration.
   A appeler une fois au demarrage, avant tout autre appel. */
void crc32_init(void);

/* ----------------------------------------------------------------
 * API incrementale
 *
 * Permet d'alimenter le calcul par morceaux, sans jamais detenir
 * l'integralite des donnees en memoire.
 * ---------------------------------------------------------------- */

/* Recharge la valeur initiale. A appeler avant chaque nouveau calcul. */
void crc32_reset(void);

/* Ajoute len octets au calcul en cours. */
void crc32_update(const uint8_t *data, uint32_t len);

/* Retourne le CRC accumule depuis le dernier crc32_reset(). */
uint32_t crc32_get(void);

/* ----------------------------------------------------------------
 * Raccourci pour un calcul en une passe
 * ---------------------------------------------------------------- */
uint32_t crc32_compute(const uint8_t *data, uint32_t len);

#endif /* CRC_H */
