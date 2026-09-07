#ifndef SYSTICK_H
#define SYSTICK_H

#include <stdint.h>

/*
 * Base de temps en millisecondes, batie sur le SysTick.
 *
 * Le SysTick est un compteur 24 bits integre au coeur Cortex-M, donc
 * present sur toute puce de cette famille quel que soit le fabricant.
 * Contrairement aux TIM du STM32, il fait partie de l'architecture
 * ARM : ce module se porte tel quel sur n'importe quel Cortex-M.
 *
 * Debordement
 * -----------
 * Le compteur revient a zero apres environ 49 jours. Ce n'est pas un
 * probleme tant que les mesures de duree s'ecrivent :
 *
 *     if ((millis() - depart) > duree)
 *
 * L'arithmetique non signee sur 32 bits etant modulaire, la
 * soustraction donne le temps reellement ecoule meme a cheval sur un
 * debordement : 0 - 4294967000 vaut 296.
 *
 * La forme suivante serait en revanche fausse, car depart + duree
 * peut deborder :
 *
 *     if (millis() > depart + duree)      // NE PAS ECRIRE CELA
 */

/* Configure une interruption toutes les millisecondes.
   cpu_hz doit correspondre a la frequence reelle du coeur ; au reset,
   le STM32L4 demarre sur MSI a 4 MHz. */
void systick_init(uint32_t cpu_hz);

/* Millisecondes ecoulees depuis systick_init(). */
uint32_t millis(void);

/* Attente bloquante. Precise, contrairement a une boucle de nop dont
   la duree depend du niveau d'optimisation et de l'horloge. */
void delay_ms(uint32_t ms);

/* Arrete le compteur et son interruption.
   A appeler avant de sauter vers l'application, qui reconfigurera le
   SysTick a sa facon : une interruption survenant pendant la
   transition trouverait une table des vecteurs incoherente. */
void systick_deinit(void);

#endif /* SYSTICK_H */
