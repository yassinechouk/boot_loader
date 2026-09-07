#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>

/*
 * Pilote du controleur de flash embarquee du STM32L476RG.
 *
 * Contraintes materielles (RM0351 section 3.3.7) :
 *
 *   - La flash est programmee par double-mot de 72 bits : 64 bits de
 *     donnees plus 8 bits d'ECC. Toute ecriture doit donc porter sur
 *     8 octets, a une adresse alignee sur 8. Un acces octet ou
 *     demi-mot leve SIZERR ; un desalignement leve PGAERR.
 *
 *   - Programmer une adresse deja programmee leve PROGERR, sauf si la
 *     valeur ecrite est entierement nulle. Les bits ECC ne peuvent pas
 *     etre recalcules sans effacement prealable.
 *
 *   - L'unite d'effacement est la page de 2 Ko. La flash de 1 Mo est
 *     organisee en deux banques de 256 pages, numerotees de 0 a 255
 *     dans chaque banque. Le bit BKER de FLASH_CR selectionne la
 *     banque.
 *
 * Choix de conception
 * -------------------
 * Les fonctions prennent des ADRESSES, jamais des numeros de page. La
 * conversion adresse -> (banque, page) est faite ici, au seul endroit
 * qui connait le dual-bank. Le reste du firmware raisonne en adresses,
 * comme metadata.h les definit.
 *
 * Le controleur est deverrouille puis reverrouille a l'interieur de
 * chaque operation. La flash n'est donc accessible en ecriture que
 * pendant les quelques microsecondes d'un effacement ou d'une
 * programmation, jamais pendant le parsing de trames ou l'attente.
 * Un pointeur qui derape a ce moment-la ne peut pas ecraser le
 * bootloader. flash_unlock() n'est volontairement pas exposee : une
 * API qui rend l'erreur impossible vaut mieux qu'une API qui la
 * deconseille.
 *
 * Le read-back est integre a flash_write() et non optionnel. Dans un
 * bootloader, aucune ecriture n'est dispensable de verification : tout
 * octet ecrit fait partie d'un firmware qui doit etre exact. Rendre la
 * verification separee creerait la possibilite de l'oublier sans
 * jamais apporter de benefice.
 *
 * Les longueurs et adresses non alignees sont REFUSEES plutot que
 * compensees. Le protocole utilise des blocs de 256 octets,
 * precisement choisis pour que la contrainte ne se pose jamais. Un
 * appel desaligne signale donc un bug de l'appelant, qu'il vaut mieux
 * voir immediatement que masquer.
 *
 * NON REENTRANT : le controleur est une ressource partagee a etat.
 */

typedef enum {
    FLASH_OK = 0,
    FLASH_ERR_ALIGN,      /* adresse ou longueur non multiple de 8    */
    FLASH_ERR_RANGE,      /* hors des limites de la flash             */
    FLASH_ERR_LOCKED,     /* echec de la sequence de deverrouillage   */
    FLASH_ERR_BUSY,       /* operation encore en cours au demarrage   */
    FLASH_ERR_PROG,       /* erreur signalee par le controleur        */
    FLASH_ERR_VERIFY      /* relecture differente de ce qui est ecrit */
} flash_status_t;

/*
 * Efface la page de 2 Ko contenant l'adresse fournie.
 *
 * L'adresse doit etre alignee sur une frontiere de page, sans quoi
 * FLASH_ERR_ALIGN est retourne : effacer 2 Ko a partir d'une adresse
 * quelconque detruirait des donnees que l'appelant ne croit pas viser.
 *
 * Une page effacee contient 0xFF sur toute son etendue.
 */
flash_status_t flash_erase_page(uint32_t address);

/*
 * Ecrit len octets a l'adresse fournie, puis relit pour verifier.
 *
 * address et len doivent tous deux etre multiples de 8. La zone visee
 * doit avoir ete effacee au prealable, sans quoi le controleur leve
 * PROGERR et FLASH_ERR_PROG est retourne.
 */
flash_status_t flash_write(uint32_t address, const uint8_t *data, uint32_t len);

/*
 * Verifie qu'une zone est entierement effacee (0xFF partout).
 * Utile avant d'ecrire, pour distinguer un slot vierge d'un slot
 * partiellement programme.
 */
int flash_is_erased(uint32_t address, uint32_t len);

/*
 * La flash est mappee en memoire : la lecture ne demande aucune
 * fonction particuliere, un dereferencement suffit. Ce helper existe
 * pour la lisibilite du code appelant.
 */
static inline const uint8_t *flash_ptr(uint32_t address)
{
    return (const uint8_t *)address;
}

#endif /* FLASH_H */
