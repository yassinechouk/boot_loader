#ifndef UART_H
#define UART_H

#include <stdint.h>

/*
 * Pilote USART2 — emission en polling, reception sur interruption.
 *
 * Sur la Nucleo-L476RG, USART2 est reliee au port COM virtuel du
 * ST-LINK via PA2 (TX) et PA3 (RX), ponts SB13/SB14 fermes en
 * configuration d'usine. Aucun cablage n'est necessaire.
 *
 * Pourquoi l'interruption en reception
 * ------------------------------------
 * A 115200 bauds, un octet arrive toutes les 87 us environ. Une
 * ecriture flash de 256 octets prend quelques centaines de us, un
 * effacement de page une vingtaine de ms — de quoi manquer plus de
 * deux cents octets si le CPU interrogeait le registre en boucle.
 *
 * Le protocole fonctionne certes en question-reponse stricte : le PC
 * n'emet pas pendant que le bootloader ecrit. Mais cette garantie
 * repose sur la discipline de l'emetteur. Un timeout qui expire au
 * mauvais moment, une retransmission mal synchronisee, et des octets
 * seraient perdus. Un bootloader ne doit pas dependre du bon
 * comportement de son interlocuteur.
 *
 * L'ISR se contente de ranger l'octet dans un tampon circulaire :
 * quelques microsecondes, largement compatibles avec l'intervalle
 * de 87 us.
 *
 * Tampon circulaire sans verrou
 * -----------------------------
 * Deux indices, head et tail. L'ISR ecrit head et lit tail ; le
 * contexte principal ecrit tail et lit head. Chaque indice a donc un
 * proprietaire unique en ecriture, ce qui elimine toute course sans
 * necessiter de section critique.
 *
 * Une case est sacrifiee pour distinguer le tampon plein du tampon
 * vide : head == tail signifie vide, (head + 1) % taille == tail
 * signifie plein. L'alternative — un compteur d'elements — serait
 * ecrite par les deux contextes et exigerait de desactiver les
 * interruptions a chaque acces.
 *
 * La taille est une puissance de deux : le modulo devient un
 * masquage de bits, une instruction au lieu d'une division d'une
 * dizaine de cycles, dans du code appele a chaque octet.
 *
 * Debordement
 * -----------
 * Un octet arrivant sur tampon plein est JETE, pas substitue au plus
 * ancien. Dans un protocole a trames verifiees par CRC, jeter le
 * nouvel octet corrompt la trame en cours : le CRC le detecte et le
 * PC retransmet. Ecraser le plus ancien corromprait en revanche une
 * trame deja recue et potentiellement en cours de traitement.
 *
 * L'evenement est compte, non signale : un compteur non nul en fin
 * de transfert indique un tampon sous-dimensionne ou un traitement
 * trop lent, information autrement invisible.
 *
 * NON REENTRANT en emission : uart_puts() ne doit pas etre appele
 * depuis une interruption pendant qu'il l'est en contexte principal.
 */

#define UART_RX_BUFFER_SIZE     512U    /* puissance de deux */

/* Initialise l'horloge, les broches PA2/PA3, le debit et l'interruption. */
void uart_init(uint32_t baudrate);

/* --- Emission (polling) --- */
void uart_putc(char c);
void uart_puts(const char *s);
void uart_write(const uint8_t *data, uint32_t len);

/* Attend la fin reelle de la transmission (drapeau TC).
   A appeler avant de couper l'horloge de l'USART ou de sauter vers
   l'application, sans quoi le dernier caractere serait tronque. */
void uart_flush(void);

/* --- Aides de formatage, utiles au diagnostic --- */
void uart_hex32(uint32_t v);
void uart_hex8(uint8_t v);
void uart_dec(uint32_t v);

/* --- Reception (interruption + tampon circulaire) --- */

/* Nombre d'octets disponibles. */
uint32_t uart_available(void);

/* Retire un octet du tampon. Retourne 1 si un octet etait present. */
int uart_getc(uint8_t *out);

/* Retire jusqu'a max octets. Retourne le nombre effectivement lu. */
uint32_t uart_read(uint8_t *dest, uint32_t max);

/* Vide le tampon. Utile pour se resynchroniser apres une erreur. */
void uart_rx_flush(void);

/* --- Diagnostic --- */
uint32_t uart_overrun_count(void);      /* tampon logiciel plein     */
uint32_t uart_hw_overrun_count(void);   /* drapeau ORE de l'USART    */
uint32_t uart_framing_error_count(void);/* drapeau FE                */
uint32_t uart_noise_error_count(void);  /* drapeau NE                */
void     uart_reset_counters(void);

#endif /* UART_H */
