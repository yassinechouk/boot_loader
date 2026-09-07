#ifndef PROTOCOL_MGR_H
#define PROTOCOL_MGR_H

#include <stdint.h>
#include "protocol.h"
#include "metadata.h"

/*
 * Machine a etats du protocole de mise a jour.
 *
 * Ce module assemble les octets recus en trames, valide chacune,
 * execute la commande correspondante et emet la reponse. Il ne
 * connait pas le transport : il consomme ce que uart_getc() lui
 * donne et emet via uart_write(). Un portage sur CAN ne toucherait
 * que ces deux points.
 *
 * Le comportement reproduit celui de tools/bootloader_sim.py, qui
 * sert de specification executable et dont les 64 tests decrivent
 * les cas limites attendus.
 *
 * Effacement paresseux
 * --------------------
 * Le simulateur efface le slot entier au demarrage du transfert.
 * Sur silicium, 480 Ko representent 240 pages a une vingtaine de
 * millisecondes chacune, soit pres de cinq secondes d'indisponibilite
 * — pour un firmware qui n'en occupera peut-etre que 20 Ko.
 *
 * Ce module efface donc page par page, juste avant d'y ecrire. La
 * taille de bloc de 256 octets divisant les 2048 octets d'une page,
 * chaque frontiere de page coincide exactement avec une frontiere de
 * bloc : le test se reduit a offset % FLASH_PAGE_SIZE == 0.
 *
 * Base de temps injectee
 * ----------------------
 * protocol_poll() recoit l'heure courante en parametre plutot que de
 * dependre d'un timer. Le module reste ainsi testable sur PC, et le
 * choix de la source de temps — SysTick, TIM, autre — appartient a
 * l'appelant.
 */

typedef enum {
    PROTO_IDLE = 0,       /* aucun transfert en cours              */
    PROTO_RECEIVING,      /* transfert commence, blocs attendus    */
    PROTO_COMPLETE        /* firmware valide, redemarrage attendu  */
} protocol_state_t;

/* Prepare le module. A appeler apres uart_init() et crc32_init(). */
void protocol_init(void);

/*
 * Consomme les octets disponibles, traite les trames completes.
 * A appeler en boucle. now_ms est une base de temps monotone en
 * millisecondes ; sa precision n'a d'importance que pour les
 * timeouts.
 */
void protocol_poll(uint32_t now_ms);

/* Etat courant de la machine a etats. */
protocol_state_t protocol_get_state(void);

/* Vrai lorsqu'un firmware vient d'etre valide et que le bootloader
   doit redemarrer pour l'executer. */
int protocol_update_complete(void);

/* --- Diagnostic --- */
uint32_t protocol_frames_received(void);
uint32_t protocol_frames_rejected(void);
uint32_t protocol_bytes_written(void);

#endif /* PROTOCOL_MGR_H */
