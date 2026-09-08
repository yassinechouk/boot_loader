#include <stdint.h>
#include "metadata_mgr.h"
#include "flash.h"
#include "crc.h"

/* Le CRC couvre tout sauf lui-meme : les 28 premiers octets. */
#define META_CRC_COVERAGE   (sizeof(metadata_t) - sizeof(uint32_t))

static const uint32_t meta_pages[2] = {
    META_PAGE_A_ADDR,
    META_PAGE_B_ADDR
};


/* ----------------------------------------------------------------
 * Validation
 * ---------------------------------------------------------------- */

int metadata_is_valid(const metadata_t *meta)
{
    if (meta == 0) {
        return 0;
    }

    if (meta->magic != METADATA_MAGIC) {
        return 0;
    }

    /* Le CRC tranche les cas que le magic ne peut pas detecter :
       ecriture interrompue, cellule degradee, effacement partiel. */
    uint32_t calcule = crc32_compute((const uint8_t *)meta,
                                     META_CRC_COVERAGE);

    return (calcule == meta->meta_crc32);
}


/* ----------------------------------------------------------------
 * Lecture
 * ---------------------------------------------------------------- */

/*
 * Charge une page et indique si elle est exploitable.
 * La copie est faite avant validation : lire depuis la flash puis
 * valider la copie evite de relire deux fois la meme zone.
 */
static int load_page(unsigned index, metadata_t *dest)
{
    const metadata_t *src = (const metadata_t *)meta_pages[index];

    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dest;
    for (unsigned i = 0; i < sizeof(metadata_t); i++) {
        d[i] = s[i];
    }

    return metadata_is_valid(dest);
}


int metadata_read(metadata_t *dest)
{
    if (dest == 0) {
        return 0;
    }

    metadata_t a;
    metadata_t b;

    int a_ok = load_page(0, &a);
    int b_ok = load_page(1, &b);

    if (!a_ok && !b_ok) {
        return 0;               /* carte vierge ou metadonnees detruites */
    }

    const metadata_t *retenu;

    if (a_ok && b_ok) {
        /* Comparaison stricte : a compteurs egaux, la page 0 gagne.
           Voir la note sur le cas d'egalite dans metadata_mgr.h. */
        retenu = (b.counter > a.counter) ? &b : &a;
    } else {
        retenu = a_ok ? &a : &b;
    }

    const uint8_t *s = (const uint8_t *)retenu;
    uint8_t *d = (uint8_t *)dest;
    for (unsigned i = 0; i < sizeof(metadata_t); i++) {
        d[i] = s[i];
    }

    return 1;
}


/* ----------------------------------------------------------------
 * Ecriture
 * ---------------------------------------------------------------- */

/*
 * Determine dans quelle page ecrire et quel compteur utiliser.
 *
 * On cible systematiquement la page qui ne porte PAS la version
 * courante : elle peut etre effacee sans risque, puisque l'autre
 * reste lisible pendant toute l'operation.
 */
static void select_target(unsigned *page, uint32_t *counter)
{
    metadata_t a;
    metadata_t b;

    int a_ok = load_page(0, &a);
    int b_ok = load_page(1, &b);

    if (!a_ok && !b_ok) {
        *page = 0;
        *counter = 1;
        return;
    }

    if (a_ok && b_ok) {
        if (b.counter > a.counter) {
            *page = 0;                  /* B est courante -> ecrire en A */
            *counter = b.counter + 1U;
        } else {
            *page = 1;                  /* A est courante -> ecrire en B */
            *counter = a.counter + 1U;
        }
        return;
    }

    if (a_ok) {
        *page = 1;
        *counter = a.counter + 1U;
    } else {
        *page = 0;
        *counter = b.counter + 1U;
    }
}


metadata_status_t metadata_write(const metadata_t *src)
{
    if (src == 0) {
        return META_ERR_ARG;
    }

    unsigned page;
    uint32_t counter;
    select_target(&page, &counter);

    /* Construction integrale a partir de zero.
     *
     * L'initialisation a {0} garantit que reserved et tout eventuel
     * remplissage valent zero, ce qui garde les dumps memoire
     * lisibles et permettra de distinguer, le jour ou un octet
     * reserve sera utilise, une valeur ecrite d'un residu.
     *
     * magic, counter et meta_crc32 sont calcules ici et non repris
     * de src : l'appelant ne doit pas pouvoir produire une structure
     * au CRC faux ou au compteur incoherent.
     *
     * La mise a zero est ecrite en boucle explicite plutot qu'avec
     * une initialisation { 0 }. Sur une structure de cette taille,
     * GCC remplacerait cette derniere par un appel a memset(), qui
     * n'existe pas en compilation -nostdlib. */
    metadata_t out;
    {
        uint8_t *raw = (uint8_t *)&out;
        for (unsigned i = 0; i < sizeof(out); i++) {
            raw[i] = 0;
        }
    }

    out.magic           = METADATA_MAGIC;
    out.counter         = counter;
    out.fw_size         = src->fw_size;
    out.fw_crc32        = src->fw_crc32;
    out.fw_version      = src->fw_version;
    out.active_slot     = src->active_slot;
    out.state           = src->state;
    out.boot_fail_count = src->boot_fail_count;

    out.meta_crc32 = crc32_compute((const uint8_t *)&out,
                                   META_CRC_COVERAGE);

    uint32_t addr = meta_pages[page];

    if (flash_erase_page(addr) != FLASH_OK) {
        return META_ERR_ERASE;
    }

    /* sizeof(metadata_t) vaut 32, multiple de l'unite d'ecriture. */
    if (flash_write(addr, (const uint8_t *)&out,
                    sizeof(metadata_t)) != FLASH_OK) {
        return META_ERR_WRITE;
    }

    /* Relecture complete : flash_write() a deja fait son read-back,
       mais on verifie ici que la structure relue est reconnue valide
       par les memes criteres que ceux du demarrage. */
    metadata_t relu;
    if (!load_page(page, &relu)) {
        return META_ERR_VERIFY;
    }
    if (relu.counter != counter) {
        return META_ERR_VERIFY;
    }

    return META_OK;
}


/* ----------------------------------------------------------------
 * Effacement complet
 * ---------------------------------------------------------------- */

metadata_status_t metadata_erase_all(void)
{
    for (unsigned i = 0; i < 2; i++) {
        if (flash_erase_page(meta_pages[i]) != FLASH_OK) {
            return META_ERR_ERASE;
        }
    }
    return META_OK;
}
