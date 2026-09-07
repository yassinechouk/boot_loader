#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* =========================================================
 * Version du protocole
 * ========================================================= */
#define PROTO_VERSION           1U

/* =========================================================
 * Format de trame
 *
 *   Offset  Taille  Champ
 *   0       2       MAGIC   (0xAA 0x55)
 *   2       1       CMD
 *   3       2       LENGTH  (little-endian)
 *   5       2       SEQ     (little-endian)
 *   7       N       DATA
 *   7+N     4       CRC32   (little-endian)
 *
 * Le CRC couvre CMD, LENGTH, SEQ et DATA — pas le MAGIC,
 * qui se valide lui-meme par sa seule reconnaissance.
 * ========================================================= */
#define FRAME_MAGIC_0           0xAAU
#define FRAME_MAGIC_1           0x55U

#define FRAME_HEADER_SIZE       7U      /* MAGIC + CMD + LENGTH + SEQ */
#define FRAME_CRC_SIZE          4U
#define FRAME_OVERHEAD          (FRAME_HEADER_SIZE + FRAME_CRC_SIZE)

/* Offsets des champs dans la trame */
#define FRAME_OFF_MAGIC         0U
#define FRAME_OFF_CMD           2U
#define FRAME_OFF_LENGTH        3U
#define FRAME_OFF_SEQ           5U
#define FRAME_OFF_DATA          7U

/* =========================================================
 * Tailles
 *
 * DATA_BLOCK_SIZE est contraint par le controleur flash :
 *   - multiple de 8    (unite de programmation, double-mot 64 bits)
 *   - diviseur de 2048 (unite d'effacement, page)
 * ========================================================= */
#define DATA_BLOCK_SIZE         256U
#define MAX_PAYLOAD_SIZE        1024U   /* borne de plausibilite sur LENGTH */
#define MAX_FRAME_SIZE          (MAX_PAYLOAD_SIZE + FRAME_OVERHEAD)

/* =========================================================
 * Commandes (PC -> bootloader)
 * ========================================================= */
#define CMD_GET_INFO            0x01U
#define CMD_START_UPDATE        0x02U
#define CMD_DATA                0x03U
#define CMD_END_UPDATE          0x04U
#define CMD_ABORT               0x05U

/* =========================================================
 * Reponses (bootloader -> PC)
 * Le bit 7 marque une reponse : lisible a l'oeil dans un dump.
 * ========================================================= */
#define RSP_INFO                0x81U
#define RSP_ACK                 0x82U
#define RSP_NACK                0x83U

#define IS_RESPONSE(cmd)        (((cmd) & 0x80U) != 0U)

/* =========================================================
 * Codes d'erreur (payload d'un RSP_NACK, 1 octet)
 * ========================================================= */
#define ERR_CRC                 0x01U   /* CRC de trame invalide            */
#define ERR_SEQ                 0x02U   /* numero de sequence inattendu     */
#define ERR_LENGTH              0x03U   /* LENGTH hors bornes               */
#define ERR_FLASH               0x04U   /* echec d'ecriture ou de relecture */
#define ERR_SIZE                0x05U   /* firmware trop volumineux         */
#define ERR_SLOT                0x06U   /* mauvais slot cible               */
#define ERR_STATE               0x07U   /* commande dans un etat invalide   */
#define ERR_GLOBAL_CRC          0x08U   /* CRC du firmware complet invalide */
#define ERR_PROTO_VER           0x09U   /* version de protocole non geree   */

/* =========================================================
 * Payload de CMD_START_UPDATE (16 octets)
 *
 * Champs ordonnes par taille decroissante : aucun padding
 * insere par le compilateur, disposition identique quelle
 * que soit la chaine de compilation.
 *
 * Le nombre de trames n'est pas transmis : il se deduit de
 * fw_size et de DATA_BLOCK_SIZE. Une donnee redondante
 * pourrait devenir incoherente.
 * ========================================================= */
typedef struct {
    uint32_t fw_size;
    uint32_t fw_crc32;
    uint32_t fw_version;
    uint8_t  target_slot;
    uint8_t  proto_version;
    uint8_t  reserved[2];
} start_update_t;

/* =========================================================
 * Payload de RSP_INFO (12 octets)
 *
 * active_slot et free_slot sont mutuellement deductibles.
 * Cette redondance est volontaire : la decision "ou ecrire"
 * revient au bootloader, qui detient l'etat reel, plutot
 * qu'a l'outil PC qui devrait l'inferer.
 * ========================================================= */
typedef struct {
    uint32_t fw_version;
    uint32_t bl_version;
    uint8_t  proto_version;
    uint8_t  active_slot;
    uint8_t  free_slot;
    uint8_t  state;
} info_response_t;

/* =========================================================
 * Timeouts (millisecondes)
 *
 * Le timer est arme apres traitement complet d'une trame,
 * jamais a la reception d'un octet : l'ecriture flash ne
 * doit pas declencher un faux positif.
 * ========================================================= */
#define TIMEOUT_START_MS        2000U
#define TIMEOUT_DATA_MS         5000U

#endif /* PROTOCOL_H */
