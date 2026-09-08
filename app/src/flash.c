#include <stdint.h>
#include "flash.h"
#include "metadata.h"

/* ----------------------------------------------------------------
 * Registres — RM0351 section 3.7
 * ---------------------------------------------------------------- */
#define FLASH_R_BASE        0x40022000UL

#define FLASH_KEYR          (*(volatile uint32_t *)(FLASH_R_BASE + 0x08))
#define FLASH_SR            (*(volatile uint32_t *)(FLASH_R_BASE + 0x10))
#define FLASH_CR            (*(volatile uint32_t *)(FLASH_R_BASE + 0x14))

/* Cles de deverrouillage — RM0351 section 3.7.3 */
#define FLASH_KEY1          0x45670123UL
#define FLASH_KEY2          0xCDEF89ABUL

/* FLASH_SR — RM0351 section 3.7.5 */
#define SR_BSY              (1U << 16)
#define SR_OPTVERR          (1U << 15)
#define SR_RDERR            (1U << 14)
#define SR_FASTERR          (1U <<  9)
#define SR_MISERR           (1U <<  8)
#define SR_PGSERR           (1U <<  7)
#define SR_SIZERR           (1U <<  6)
#define SR_PGAERR           (1U <<  5)
#define SR_WRPERR           (1U <<  4)
#define SR_PROGERR          (1U <<  3)
#define SR_OPERR            (1U <<  1)
#define SR_EOP              (1U <<  0)

/* Tous les drapeaux d'erreur, effacables en ecrivant 1 */
#define SR_ALL_ERRORS       (SR_OPTVERR | SR_RDERR  | SR_FASTERR | \
                             SR_MISERR  | SR_PGSERR | SR_SIZERR  | \
                             SR_PGAERR  | SR_WRPERR | SR_PROGERR | \
                             SR_OPERR)

/* FLASH_CR — RM0351 section 3.7.6 */
#define CR_LOCK             (1U << 31)
#define CR_STRT             (1U << 16)
#define CR_BKER             (1U << 11)
#define CR_PNB_SHIFT        3
#define CR_PNB_MASK         (0xFFU << CR_PNB_SHIFT)
#define CR_MER2             (1U << 15)
#define CR_MER1             (1U <<  2)
#define CR_PER              (1U <<  1)
#define CR_PG               (1U <<  0)

/* Organisation dual-bank : 256 pages de 2 Ko par banque */
#define PAGES_PER_BANK      256U
#define BANK2_START         (FLASH_BASE_ADDR + (PAGES_PER_BANK * FLASH_PAGE_SIZE))

#define FLASH_END_ADDR      (FLASH_BASE_ADDR + FLASH_TOTAL_SIZE)


/* ----------------------------------------------------------------
 * Utilitaires internes
 * ---------------------------------------------------------------- */

static void flash_wait_busy(void)
{
    while (FLASH_SR & SR_BSY) {
        /* attente active : une page s'efface en une vingtaine de ms,
           un double-mot s'ecrit en quelques dizaines de us */
    }
}

static void flash_clear_errors(void)
{
    /* Les drapeaux sont de type rc_w1 : on les efface en ecrivant 1.
       Cette etape est obligatoire avant toute programmation, faute de
       quoi le controleur leve PGSERR (RM0351 3.3.7, etape 2). */
    FLASH_SR = SR_ALL_ERRORS;
}

static flash_status_t flash_check_errors(void)
{
    uint32_t sr = FLASH_SR;

    if (sr & SR_ALL_ERRORS) {
        flash_clear_errors();
        return FLASH_ERR_PROG;
    }

    if (sr & SR_EOP) {
        FLASH_SR = SR_EOP;      /* acquitter la fin d'operation */
    }

    return FLASH_OK;
}

static flash_status_t flash_unlock(void)
{
    if ((FLASH_CR & CR_LOCK) == 0U) {
        return FLASH_OK;        /* deja deverrouille */
    }

    FLASH_KEYR = FLASH_KEY1;
    FLASH_KEYR = FLASH_KEY2;

    /* Une sequence incorrecte laisse LOCK arme jusqu'au prochain
       reset systeme : il n'est pas possible de reessayer. */
    return (FLASH_CR & CR_LOCK) ? FLASH_ERR_LOCKED : FLASH_OK;
}

static void flash_lock(void)
{
    FLASH_CR |= CR_LOCK;
}

static int address_in_flash(uint32_t address, uint32_t len)
{
    if (address < FLASH_BASE_ADDR) {
        return 0;
    }
    if (address + len > FLASH_END_ADDR) {
        return 0;
    }
    if (address + len < address) {
        return 0;               /* debordement arithmetique */
    }
    return 1;
}


/* ----------------------------------------------------------------
 * Effacement
 * ---------------------------------------------------------------- */

flash_status_t flash_erase_page(uint32_t address)
{
    if (!address_in_flash(address, FLASH_PAGE_SIZE)) {
        return FLASH_ERR_RANGE;
    }

    /* Une adresse non alignee sur une page effacerait 2 Ko a partir
       d'une frontiere que l'appelant ne vise pas. On refuse. */
    if (address % FLASH_PAGE_SIZE) {
        return FLASH_ERR_ALIGN;
    }

    /* Conversion adresse -> (banque, numero de page). La numerotation
       repart a zero dans chaque banque, d'ou le modulo. */
    uint32_t offset = address - FLASH_BASE_ADDR;
    uint32_t page   = offset / FLASH_PAGE_SIZE;
    uint32_t bank2  = (page >= PAGES_PER_BANK);
    uint32_t pnb    = page % PAGES_PER_BANK;

    if (FLASH_SR & SR_BSY) {
        return FLASH_ERR_BUSY;
    }

    flash_status_t st = flash_unlock();
    if (st != FLASH_OK) {
        return st;
    }

    flash_clear_errors();

    /* Construction du registre en une ecriture : PER seul, sans PG ni
       MER1/MER2, sans quoi le controleur leve PGSERR. */
    uint32_t cr = CR_PER | (pnb << CR_PNB_SHIFT);
    if (bank2) {
        cr |= CR_BKER;
    }
    FLASH_CR = cr;
    FLASH_CR = cr | CR_STRT;

    flash_wait_busy();

    st = flash_check_errors();

    FLASH_CR &= ~CR_PER;
    flash_lock();

    if (st != FLASH_OK) {
        return st;
    }

    /* Verification : une page effacee doit contenir 0xFF partout. */
    if (!flash_is_erased(address, FLASH_PAGE_SIZE)) {
        return FLASH_ERR_VERIFY;
    }

    return FLASH_OK;
}


/* ----------------------------------------------------------------
 * Ecriture
 * ---------------------------------------------------------------- */

flash_status_t flash_write(uint32_t address, const uint8_t *data, uint32_t len)
{
    if (data == 0 || len == 0U) {
        return FLASH_ERR_ALIGN;
    }

    /* Le controleur ne programme que des double-mots alignes.
       Un desalignement leve PGAERR, un acces plus etroit SIZERR.
       On refuse en amont plutot que de compenser : le protocole
       utilise des blocs de 256 octets, ces cas ne doivent pas
       survenir et signaleraient un bug de l'appelant. */
    if ((address % 8U) || (len % 8U)) {
        return FLASH_ERR_ALIGN;
    }

    if (!address_in_flash(address, len)) {
        return FLASH_ERR_RANGE;
    }

    if (FLASH_SR & SR_BSY) {
        return FLASH_ERR_BUSY;
    }

    flash_status_t st = flash_unlock();
    if (st != FLASH_OK) {
        return st;
    }

    flash_clear_errors();
    FLASH_CR |= CR_PG;

    for (uint32_t i = 0; i < len; i += 8U) {
        /* Les octets sources ne sont pas forcement alignes, on les
           reassemble mot par mot plutot que de dereferencer un
           uint32_t* potentiellement desaligne — indefini sur
           Cortex-M. */
        uint32_t low  = (uint32_t)data[i]
                      | ((uint32_t)data[i + 1] << 8)
                      | ((uint32_t)data[i + 2] << 16)
                      | ((uint32_t)data[i + 3] << 24);

        uint32_t high = (uint32_t)data[i + 4]
                      | ((uint32_t)data[i + 5] << 8)
                      | ((uint32_t)data[i + 6] << 16)
                      | ((uint32_t)data[i + 7] << 24);

        /* Les deux mots doivent etre ecrits successivement : la
           programmation se declenche automatiquement a reception du
           second (RM0351 3.3.7). */
        *(volatile uint32_t *)(address + i)       = low;
        *(volatile uint32_t *)(address + i + 4U)  = high;

        flash_wait_busy();

        st = flash_check_errors();
        if (st != FLASH_OK) {
            FLASH_CR &= ~CR_PG;
            flash_lock();
            return st;
        }
    }

    FLASH_CR &= ~CR_PG;
    flash_lock();

    /* Read-back. Verifie le stockage, la ou un CRC de transmission
       ne validerait que l'acheminement. Detecte une cellule
       defaillante ou une ecriture partielle. */
    const uint8_t *written = (const uint8_t *)address;
    for (uint32_t i = 0; i < len; i++) {
        if (written[i] != data[i]) {
            return FLASH_ERR_VERIFY;
        }
    }

    return FLASH_OK;
}


/* ----------------------------------------------------------------
 * Lecture
 * ---------------------------------------------------------------- */

int flash_is_erased(uint32_t address, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)address;

    for (uint32_t i = 0; i < len; i++) {
        if (p[i] != 0xFFU) {
            return 0;
        }
    }
    return 1;
}
