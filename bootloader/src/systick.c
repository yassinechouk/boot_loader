#include <stdint.h>
#include "systick.h"

/* ----------------------------------------------------------------
 * Registres SysTick — definis par ARM, identiques sur tout Cortex-M.
 * Adresses fixes dans l'espace du System Control Block.
 * ---------------------------------------------------------------- */
#define SYST_CSR    (*(volatile uint32_t *)0xE000E010UL)  /* controle  */
#define SYST_RVR    (*(volatile uint32_t *)0xE000E014UL)  /* recharge  */
#define SYST_CVR    (*(volatile uint32_t *)0xE000E018UL)  /* courant   */

#define CSR_ENABLE      (1U << 0)
#define CSR_TICKINT     (1U << 1)
#define CSR_CLKSOURCE   (1U << 2)   /* 1 = horloge du coeur */

/* Le compteur ne fait que 24 bits. */
#define SYST_MAX_RELOAD 0x00FFFFFFUL


static volatile uint32_t tick_count = 0;


void systick_init(uint32_t cpu_hz)
{
    uint32_t reload = (cpu_hz / 1000U) - 1U;

    if (reload > SYST_MAX_RELOAD) {
        /* Au-dela d'environ 16 MHz, une periode d'une milliseconde ne
           tient plus dans 24 bits. On sature plutot que de laisser la
           valeur se tronquer silencieusement : la base de temps sera
           trop lente, mais de facon visible. */
        reload = SYST_MAX_RELOAD;
    }

    tick_count = 0;

    SYST_RVR = reload;
    SYST_CVR = 0;               /* toute ecriture remet le compteur a zero */
    SYST_CSR = CSR_CLKSOURCE | CSR_TICKINT | CSR_ENABLE;
}


void SysTick_Handler(void)
{
    tick_count++;
}


uint32_t millis(void)
{
    return tick_count;
}


void delay_ms(uint32_t ms)
{
    uint32_t depart = tick_count;

    /* Forme resistante au debordement : la soustraction non signee
       donne la duree ecoulee meme a cheval sur un retour a zero. */
    while ((tick_count - depart) < ms) {
    }
}


void systick_deinit(void)
{
    SYST_CSR = 0;
    SYST_RVR = 0;
    SYST_CVR = 0;
}
