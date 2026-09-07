#ifndef METADATA_MGR_H
#define METADATA_MGR_H

#include <stdint.h>
#include "metadata.h"

/*
 * Gestion des metadonnees persistantes du bootloader.
 *
 * Mecanisme a double page
 * -----------------------
 * La structure est ecrite en deux exemplaires, dans deux pages flash
 * distinctes. A chaque mise a jour, seule la page inactive est
 * effacee puis reecrite : l'autre reste intacte et lisible pendant
 * toute l'operation.
 *
 * Consequence : une coupure d'alimentation ne peut jamais detruire
 * les deux copies simultanement. Au redemarrage, il reste toujours
 * au moins une copie valide — celle d'avant la mise a jour si la
 * coupure est survenue pendant l'ecriture.
 *
 * Une copie est retenue si son magic correspond ET si son CRC est
 * correct. Le magic seul ne suffirait pas : il partage son bloc de
 * 8 octets avec le compteur, donc une coupure apres la premiere
 * ecriture laisserait un magic valide devant des champs encore a
 * 0xFF. Le CRC couvrant les 28 premiers octets, toute alteration
 * partielle est rejetee.
 *
 * Entre deux copies valides, celle dont le compteur est le plus
 * eleve fait foi.
 *
 * Cas d'egalite des compteurs
 * ---------------------------
 * Cette situation ne peut resulter que d'une corruption ou d'un bug.
 * Le module choisit alors la page 0, de facon deterministe, plutot
 * que de declarer la carte vierge.
 *
 * Refuser de demarrer serait le mauvais choix : deux copies valides
 * portent deux jeux de donnees intacts, decrivant vraisemblablement
 * le meme etat. Il n'y a aucun danger a booter, alors que basculer
 * en mode reception immobiliserait un appareil qui fonctionnait.
 * On ne refuse de fonctionner que si continuer serait dangereux.
 *
 * L'anomalie se resout d'elle-meme : la prochaine ecriture porte un
 * compteur strictement superieur.
 *
 * Copie plutot que pointeur
 * -------------------------
 * metadata_read() recopie les donnees en RAM au lieu de retourner un
 * pointeur vers la flash. Un tel pointeur deviendrait silencieusement
 * invalide des que metadata_write() effacerait la page visee : les
 * champs se mettraient a lire 0xFF sans aucun avertissement.
 *
 * Le cout est de 32 octets sur 96 Ko. Le benefice est que l'appelant
 * possede ses donnees et peut les modifier avant de les reecrire.
 *
 * NON REENTRANT : s'appuie sur flash.c et crc.c, qui ne le sont pas.
 */

typedef enum {
    META_OK = 0,
    META_ERR_ERASE,       /* echec de l'effacement de la page inactive */
    META_ERR_WRITE,       /* echec de la programmation                 */
    META_ERR_VERIFY,      /* relecture incoherente apres ecriture      */
    META_ERR_ARG          /* argument nul                              */
} metadata_status_t;

/*
 * Lit la copie la plus recente et la recopie dans dest.
 *
 * Retourne 1 si une copie valide a ete trouvee, 0 sinon — auquel cas
 * dest n'est pas modifie. L'absence de copie valide signifie une
 * carte vierge ou des metadonnees detruites : dans les deux cas, le
 * bootloader doit passer en mode reception.
 */
int metadata_read(metadata_t *dest);

/*
 * Ecrit une nouvelle version dans la page inactive.
 *
 * Les champs magic, counter, reserved et meta_crc32 de src sont
 * ignores : le module les calcule lui-meme. L'appelant ne peut donc
 * pas produire une structure au CRC faux ou au compteur incoherent,
 * meme en essayant.
 *
 * Seuls sont repris : fw_size, fw_crc32, fw_version, active_slot,
 * state, boot_fail_count.
 */
metadata_status_t metadata_write(const metadata_t *src);

/*
 * Efface les deux pages. Ramene la carte a l'etat vierge.
 * Destine aux tests et a une remise a zero volontaire.
 */
metadata_status_t metadata_erase_all(void);

/*
 * Indique si une copie donnee est structurellement valide.
 * Exposee pour les tests ; le module l'utilise en interne.
 */
int metadata_is_valid(const metadata_t *meta);

#endif /* METADATA_MGR_H */
