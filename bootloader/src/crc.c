#include <stdint.h>
#include "crc.h"

/* ----------------------------------------------------------------
 * Adresses — RM0351 section 2.2.2
 * ---------------------------------------------------------------- */
#define RCC_BASE            0x40021000UL
#define CRC_BASE            0x40023000UL

/* RM0351 section 6.4.16 : AHB1 peripheral clock enable register */
#define RCC_AHB1ENR         (*(volatile uint32_t *)(RCC_BASE + 0x48))
#define RCC_AHB1ENR_CRCEN   (1U << 12)

/* RM0351 section 15.4 : registres du peripherique CRC.
 *
 * CRC_DR est accessible par mot, demi-mot aligne a droite et octet
 * aligne a droite. On expose les deux largeurs utiles. */
#define CRC_DR_WORD         (*(volatile uint32_t *)(CRC_BASE + 0x00))
#define CRC_DR_BYTE         (*(volatile uint8_t  *)(CRC_BASE + 0x00))
#define CRC_CR              (*(volatile uint32_t *)(CRC_BASE + 0x08))
#define CRC_INIT_REG        (*(volatile uint32_t *)(CRC_BASE + 0x10))
#define CRC_POL_REG         (*(volatile uint32_t *)(CRC_BASE + 0x14))

/* CRC_CR — RM0351 section 15.4.3 */
#define CRC_CR_RESET        (1U << 0)   /* rs : s'efface en materiel */
#define CRC_CR_REV_IN_BYTE  (1U << 5)   /* REV_IN[1:0] = 01 */

#define CRC_POLYNOMIAL      0x04C11DB7UL
#define CRC_INIT_VALUE      0xFFFFFFFFUL


void crc32_init(void)
{
    RCC_AHB1ENR |= RCC_AHB1ENR_CRCEN;

    CRC_POL_REG  = CRC_POLYNOMIAL;
    CRC_INIT_REG = CRC_INIT_VALUE;

    /* REV_IN = 01, REV_OUT = 0, POLYSIZE = 00 (32 bits) */
    CRC_CR = CRC_CR_REV_IN_BYTE;
}


void crc32_reset(void)
{
    /* Ecriture directe plutot que |=.
     *
     * RESET est un bit de type "rs" : il est mis par logiciel et
     * efface automatiquement par le materiel. Un |= ferait une
     * lecture-modification-ecriture dont le resultat dependrait de
     * l'instant de la lecture. L'ecriture directe est deterministe.
     *
     * REV_IN doit etre reecrit explicitement, sans quoi il serait
     * perdu et tous les CRC suivants seraient faux. */
    CRC_CR = CRC_CR_REV_IN_BYTE | CRC_CR_RESET;
}


void crc32_update(const uint8_t *data, uint32_t len)
{
    /* Alimentation octet par octet.
     *
     * Le peripherique consomme un cycle AHB par octet quelle que soit
     * la largeur d'acces : 1 cycle pour un octet, 4 pour un mot. Le
     * debit est donc identique, et l'ecriture par mots n'apporterait
     * aucun gain — elle economiserait seulement des iterations de
     * boucle, negligeables face au temps du peripherique.
     *
     * Elle imposerait en revanche de gerer l'alignement du pointeur
     * source (un acces 32 bits non aligne est indefini sur Cortex-M)
     * et les octets residuels d'une longueur non multiple de 4. Deux
     * sources de bugs dans le module dont depend toute la validation
     * du protocole. */
    while (len--) {
        CRC_DR_BYTE = *data++;
    }
}


uint32_t crc32_get(void)
{
    return CRC_DR_WORD;
}


uint32_t crc32_compute(const uint8_t *data, uint32_t len)
{
    crc32_reset();
    crc32_update(data, len);
    return crc32_get();
}
