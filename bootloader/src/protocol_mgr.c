#include <stdint.h>
#include "protocol_mgr.h"
#include "protocol.h"
#include "metadata.h"
#include "metadata_mgr.h"
#include "flash.h"
#include "crc.h"
#include "uart.h"

/* ----------------------------------------------------------------
 * Assemblage des trames
 *
 * Les octets arrivent un par un, sans garantie de decoupage. Un
 * automate les accumule jusqu'a disposer d'une trame complete.
 * ---------------------------------------------------------------- */
typedef enum {
    RX_MAGIC0 = 0,
    RX_MAGIC1,
    RX_HEADER,      /* CMD, LENGTH, SEQ : 5 octets */
    RX_DATA,
    RX_CRC          /* 4 octets */
} rx_state_t;

static rx_state_t rx_state;
static uint8_t    rx_frame[FRAME_HEADER_SIZE + MAX_PAYLOAD_SIZE + FRAME_CRC_SIZE];
static uint32_t   rx_index;      /* position dans rx_frame        */
static uint32_t   rx_expected;   /* octets restants pour l'etat   */

/* ----------------------------------------------------------------
 * Contexte de transfert
 * ---------------------------------------------------------------- */
static protocol_state_t state;

static uint16_t expected_seq;
static uint16_t last_seq;
static uint8_t  target_slot;
static uint32_t fw_size;
static uint32_t fw_crc32;
static uint32_t fw_version;
static uint32_t bytes_written;

static uint32_t last_activity_ms;
static uint32_t timeout_ms;

static uint32_t cnt_frames_ok;
static uint32_t cnt_frames_ko;


/* ----------------------------------------------------------------
 * Emission d'une trame
 * ---------------------------------------------------------------- */

static void send_frame(uint8_t cmd, uint16_t seq,
                       const uint8_t *data, uint16_t len)
{
    uint8_t header[FRAME_HEADER_SIZE];

    header[0] = FRAME_MAGIC_0;
    header[1] = FRAME_MAGIC_1;
    header[2] = cmd;
    header[3] = (uint8_t)(len & 0xFFU);
    header[4] = (uint8_t)((len >> 8) & 0xFFU);
    header[5] = (uint8_t)(seq & 0xFFU);
    header[6] = (uint8_t)((seq >> 8) & 0xFFU);

    /* Le CRC couvre CMD, LENGTH, SEQ et DATA — pas le MAGIC, qui se
       valide de lui-meme par sa seule reconnaissance. */
    crc32_reset();
    crc32_update(&header[FRAME_OFF_CMD], FRAME_HEADER_SIZE - FRAME_OFF_CMD);
    if (len > 0U && data != 0) {
        crc32_update(data, len);
    }
    uint32_t crc = crc32_get();

    uint8_t trailer[FRAME_CRC_SIZE];
    trailer[0] = (uint8_t)(crc & 0xFFU);
    trailer[1] = (uint8_t)((crc >> 8) & 0xFFU);
    trailer[2] = (uint8_t)((crc >> 16) & 0xFFU);
    trailer[3] = (uint8_t)((crc >> 24) & 0xFFU);

    uart_write(header, FRAME_HEADER_SIZE);
    if (len > 0U && data != 0) {
        uart_write(data, len);
    }
    uart_write(trailer, FRAME_CRC_SIZE);
}


static void send_ack(uint16_t seq)
{
    send_frame(RSP_ACK, seq, 0, 0);
}


static void send_nack(uint16_t seq, uint8_t err)
{
    send_frame(RSP_NACK, seq, &err, 1);
}


/* ----------------------------------------------------------------
 * Abandon du transfert
 * ---------------------------------------------------------------- */

static void abort_transfer(void)
{
    state = PROTO_IDLE;
    bytes_written = 0;
    expected_seq = 0;
    last_seq = 0xFFFFU;
}


/* ----------------------------------------------------------------
 * Traitement des commandes
 * ---------------------------------------------------------------- */

static void on_get_info(uint16_t seq)
{
    metadata_t meta;
    uint8_t  active = SLOT_A;
    uint8_t  st     = STATE_EMPTY;
    uint32_t ver    = 0;

    if (metadata_read(&meta)) {
        active = meta.active_slot;
        st     = meta.slot[active].state;
        ver    = meta.slot[active].version;
    }

    /* La reponse est construite octet par octet plutot que par un
       cast de struct : elle traverse une liaison serie, donc son
       agencement doit etre explicite et independant du compilateur. */
    uint8_t payload[12];

    payload[0]  = (uint8_t)(ver & 0xFFU);
    payload[1]  = (uint8_t)((ver >> 8) & 0xFFU);
    payload[2]  = (uint8_t)((ver >> 16) & 0xFFU);
    payload[3]  = (uint8_t)((ver >> 24) & 0xFFU);

    payload[4]  = (uint8_t)(BOOTLOADER_VERSION & 0xFFU);
    payload[5]  = (uint8_t)((BOOTLOADER_VERSION >> 8) & 0xFFU);
    payload[6]  = (uint8_t)((BOOTLOADER_VERSION >> 16) & 0xFFU);
    payload[7]  = (uint8_t)((BOOTLOADER_VERSION >> 24) & 0xFFU);

    payload[8]  = (uint8_t)PROTO_VERSION;
    payload[9]  = active;
    payload[10] = (uint8_t)(1U - active);    /* slot libre */
    payload[11] = st;

    send_frame(RSP_INFO, seq, payload, 12);
}


static void on_start_update(uint16_t seq, const uint8_t *data, uint16_t len)
{
    if (len != 16U) {
        send_nack(seq, ERR_LENGTH);
        return;
    }

    uint32_t size = (uint32_t)data[0]
                  | ((uint32_t)data[1] << 8)
                  | ((uint32_t)data[2] << 16)
                  | ((uint32_t)data[3] << 24);

    uint32_t crc  = (uint32_t)data[4]
                  | ((uint32_t)data[5] << 8)
                  | ((uint32_t)data[6] << 16)
                  | ((uint32_t)data[7] << 24);

    uint32_t ver  = (uint32_t)data[8]
                  | ((uint32_t)data[9] << 8)
                  | ((uint32_t)data[10] << 16)
                  | ((uint32_t)data[11] << 24);

    uint8_t slot  = data[12];
    uint8_t proto = data[13];

    if (proto != PROTO_VERSION) {
        send_nack(seq, ERR_PROTO_VER);
        return;
    }

    metadata_t meta;
    uint8_t active = metadata_read(&meta) ? meta.active_slot : SLOT_A;
    uint8_t libre  = (uint8_t)(1U - active);

    /* Toutes les verifications possibles ont lieu AVANT le moindre
       effacement. Un refus ne doit rien detruire. */
    if (size == 0U || size > SLOT_SIZE) {
        send_nack(seq, ERR_SIZE);
        return;
    }
    if (slot != libre) {
        send_nack(seq, ERR_SLOT);
        return;
    }

    /* Ordonnancement fail-safe : marquer IN_PROGRESS avant toute
       operation destructive. L'ordre inverse laisserait une fenetre
       pendant laquelle les metadonnees affirmeraient qu'un firmware
       valide existe alors qu'il vient d'etre efface. */
    metadata_t nouvelle;
    uint8_t *raw = (uint8_t *)&nouvelle;
    for (unsigned i = 0; i < sizeof(nouvelle); i++) {
        raw[i] = 0;
    }
    /* On repart de l'etat courant : les informations de l'AUTRE slot
       doivent survivre, faute de quoi un rollback ulterieur ne
       saurait plus decrire l'image de repli. */
    metadata_t courant;
    if (metadata_read(&courant)) {
        nouvelle = courant;
    }
    nouvelle.active_slot        = active;   /* inchange tant que non valide */
    nouvelle.boot_fail_count    = 0;
    nouvelle.slot[slot].size    = size;
    nouvelle.slot[slot].crc32   = crc;
    nouvelle.slot[slot].version = ver;
    nouvelle.slot[slot].state   = STATE_IN_PROGRESS;

    if (metadata_write(&nouvelle) != META_OK) {
        send_nack(seq, ERR_FLASH);
        return;
    }

    /* Le slot n'est pas efface ici : chaque page le sera juste avant
       d'etre ecrite. Effacer 480 Ko d'avance couterait pres de cinq
       secondes pour un firmware qui n'en occupera peut-etre que 20. */

    target_slot   = slot;
    fw_size       = size;
    fw_crc32      = crc;
    fw_version    = ver;
    bytes_written = 0;
    expected_seq  = (uint16_t)(seq + 1U);
    last_seq      = seq;
    state         = PROTO_RECEIVING;
    timeout_ms    = TIMEOUT_DATA_MS;

    send_ack(seq);
}


static void on_data(uint16_t seq, const uint8_t *data, uint16_t len)
{
    if (state != PROTO_RECEIVING) {
        send_nack(seq, ERR_STATE);
        return;
    }

    /* Retransmission : le PC n'a pas recu l'accuse precedent. Il
       n'attend pas une reecriture — la flash ne se reprogramme pas
       sans effacement — mais l'accuse manquant. Retraiter la trame
       doit rester sans effet de bord. */
    if (seq == last_seq) {
        send_ack(seq);
        return;
    }

    if (seq != expected_seq) {
        send_nack(seq, ERR_SEQ);
        abort_transfer();
        return;
    }

    if (len == 0U || (len % 8U) != 0U) {
        /* La flash ne programme que par double-mot. Une longueur non
           multiple de 8 ne peut pas etre ecrite telle quelle. */
        send_nack(seq, ERR_LENGTH);
        return;
    }

    uint32_t offset = bytes_written;

    if (offset + len > fw_size) {
        send_nack(seq, ERR_SIZE);
        abort_transfer();
        return;
    }

    uint32_t addr = SLOT_ADDR(target_slot) + offset;

    /* Effacement paresseux. La taille de bloc divisant celle d'une
       page, une frontiere de page tombe toujours sur une frontiere
       de bloc : aucun bloc n'est jamais a cheval sur deux pages. */
    if ((offset % FLASH_PAGE_SIZE) == 0U) {
        if (flash_erase_page(addr) != FLASH_OK) {
            send_nack(seq, ERR_FLASH);
            abort_transfer();
            return;
        }
    }

    if (flash_write(addr, data, len) != FLASH_OK) {
        send_nack(seq, ERR_FLASH);
        abort_transfer();
        return;
    }

    bytes_written += len;
    last_seq      = seq;
    expected_seq  = (uint16_t)(seq + 1U);

    send_ack(seq);
}


static void on_end_update(uint16_t seq)
{
    if (state != PROTO_RECEIVING) {
        send_nack(seq, ERR_STATE);
        return;
    }

    if (bytes_written != fw_size) {
        send_nack(seq, ERR_SIZE);
        abort_transfer();
        return;
    }

    /* Verification globale par RELECTURE de la flash.
     *
     * Le CRC de trame validait l'acheminement ; celui-ci valide le
     * stockage. Il detecte ce que le premier ne peut pas voir : une
     * cellule defaillante, une ecriture partielle, une erreur
     * d'adressage. */
    const uint8_t *stored = (const uint8_t *)SLOT_ADDR(target_slot);
    uint32_t calcule = crc32_compute(stored, fw_size);

    if (calcule != fw_crc32) {
        send_nack(seq, ERR_GLOBAL_CRC);
        abort_transfer();
        return;
    }

    /* STATE_TESTING et non STATE_VALID : un CRC correct prouve
       l'integrite de l'image, pas son bon fonctionnement. Un firmware
       transmis sans la moindre corruption peut planter des sa
       premiere seconde. L'application devra se confirmer elle-meme. */
    metadata_t meta;
    uint8_t *raw = (uint8_t *)&meta;
    for (unsigned i = 0; i < sizeof(meta); i++) {
        raw[i] = 0;
    }
    metadata_t courant2;
    if (metadata_read(&courant2)) {
        meta = courant2;
    }
    meta.active_slot                  = target_slot;
    meta.boot_fail_count              = 0;
    meta.slot[target_slot].size       = fw_size;
    meta.slot[target_slot].crc32      = fw_crc32;
    meta.slot[target_slot].version    = fw_version;
    meta.slot[target_slot].state      = STATE_TESTING;

    if (metadata_write(&meta) != META_OK) {
        send_nack(seq, ERR_FLASH);
        abort_transfer();
        return;
    }

    send_ack(seq);
    uart_flush();

    state = PROTO_COMPLETE;
}


static void dispatch(uint8_t cmd, uint16_t seq,
                     const uint8_t *data, uint16_t len)
{
    cnt_frames_ok++;

    switch (cmd) {
    case CMD_GET_INFO:
        on_get_info(seq);
        break;
    case CMD_START_UPDATE:
        on_start_update(seq, data, len);
        break;
    case CMD_DATA:
        on_data(seq, data, len);
        break;
    case CMD_END_UPDATE:
        on_end_update(seq);
        break;
    case CMD_ABORT:
        abort_transfer();
        send_ack(seq);
        break;
    default:
        send_nack(seq, ERR_STATE);
        break;
    }
}


/* ----------------------------------------------------------------
 * Assemblage des octets recus
 * ---------------------------------------------------------------- */

static void rx_reset(void)
{
    rx_state    = RX_MAGIC0;
    rx_index    = 0;
    rx_expected = 0;
}


static void feed(uint8_t byte)
{
    switch (rx_state) {

    case RX_MAGIC0:
        if (byte == FRAME_MAGIC_0) {
            rx_frame[0] = byte;
            rx_state = RX_MAGIC1;
        }
        /* Tout autre octet est ignore : on cherche un debut de trame
           dans un flux potentiellement bruite. */
        break;

    case RX_MAGIC1:
        if (byte == FRAME_MAGIC_1) {
            rx_frame[1] = byte;
            rx_index    = 2;
            rx_expected = FRAME_HEADER_SIZE - 2U;
            rx_state    = RX_HEADER;
        } else if (byte == FRAME_MAGIC_0) {
            /* Sequence 0xAA 0xAA 0x55 : le second 0xAA peut etre le
               vrai debut. On reste en attente du 0x55. */
            rx_frame[0] = byte;
        } else {
            rx_state = RX_MAGIC0;
        }
        break;

    case RX_HEADER:
        rx_frame[rx_index++] = byte;
        if (--rx_expected == 0U) {
            uint16_t len = (uint16_t)rx_frame[FRAME_OFF_LENGTH]
                         | (uint16_t)((uint16_t)rx_frame[FRAME_OFF_LENGTH + 1] << 8);

            /* Verification de plausibilite AVANT toute bufferisation.
               Un champ de longueur non borne est un vecteur classique
               de debordement de tampon : attendre 60000 octets
               depasserait la capacite du tampon de trame. */
            if (len > MAX_PAYLOAD_SIZE) {
                cnt_frames_ko++;
                rx_reset();
                break;
            }

            if (len == 0U) {
                rx_expected = FRAME_CRC_SIZE;
                rx_state    = RX_CRC;
            } else {
                rx_expected = len;
                rx_state    = RX_DATA;
            }
        }
        break;

    case RX_DATA:
        rx_frame[rx_index++] = byte;
        if (--rx_expected == 0U) {
            rx_expected = FRAME_CRC_SIZE;
            rx_state    = RX_CRC;
        }
        break;

    case RX_CRC:
        rx_frame[rx_index++] = byte;
        if (--rx_expected == 0U) {
            uint16_t len = (uint16_t)rx_frame[FRAME_OFF_LENGTH]
                         | (uint16_t)((uint16_t)rx_frame[FRAME_OFF_LENGTH + 1] << 8);

            uint32_t off = FRAME_HEADER_SIZE + len;

            uint32_t recu = (uint32_t)rx_frame[off]
                          | ((uint32_t)rx_frame[off + 1] << 8)
                          | ((uint32_t)rx_frame[off + 2] << 16)
                          | ((uint32_t)rx_frame[off + 3] << 24);

            uint32_t calcule = crc32_compute(&rx_frame[FRAME_OFF_CMD],
                                             (FRAME_HEADER_SIZE - FRAME_OFF_CMD) + len);

            if (calcule == recu) {
                uint8_t  cmd = rx_frame[FRAME_OFF_CMD];
                uint16_t seq = (uint16_t)rx_frame[FRAME_OFF_SEQ]
                             | (uint16_t)((uint16_t)rx_frame[FRAME_OFF_SEQ + 1] << 8);

                dispatch(cmd, seq, &rx_frame[FRAME_OFF_DATA], len);
            } else {
                cnt_frames_ko++;
                uint16_t seq = (uint16_t)rx_frame[FRAME_OFF_SEQ]
                             | (uint16_t)((uint16_t)rx_frame[FRAME_OFF_SEQ + 1] << 8);
                send_nack(seq, ERR_CRC);
            }

            rx_reset();
        }
        break;

    default:
        rx_reset();
        break;
    }
}


/* ----------------------------------------------------------------
 * Interface publique
 * ---------------------------------------------------------------- */

void protocol_init(void)
{
    rx_reset();
    state = PROTO_IDLE;

    expected_seq  = 0;
    last_seq      = 0xFFFFU;
    target_slot   = SLOT_A;
    fw_size       = 0;
    fw_crc32      = 0;
    fw_version    = 0;
    bytes_written = 0;

    last_activity_ms = 0;
    timeout_ms       = TIMEOUT_START_MS;

    cnt_frames_ok = 0;
    cnt_frames_ko = 0;
}


void protocol_poll(uint32_t now_ms)
{
    uint8_t byte;
    int recu = 0;

    while (uart_getc(&byte)) {
        feed(byte);
        recu = 1;
    }

    /* Le compteur d'inactivite repart apres traitement complet des
       octets disponibles, jamais a la reception d'un seul. Une
       ecriture flash de plusieurs dizaines de millisecondes ne doit
       pas etre comptee comme une periode de silence. */
    if (recu) {
        last_activity_ms = now_ms;
        return;
    }

    if (state == PROTO_RECEIVING) {
        if ((now_ms - last_activity_ms) > timeout_ms) {
            /* L'emetteur ne repond plus. Le slot cible contient un
               firmware partiel ; les metadonnees restent en
               IN_PROGRESS, ce que le prochain demarrage saura
               interpreter. */
            abort_transfer();
            last_activity_ms = now_ms;
        }
    }
}


protocol_state_t protocol_get_state(void)
{
    return state;
}


int protocol_update_complete(void)
{
    return (state == PROTO_COMPLETE);
}


uint32_t protocol_frames_received(void) { return cnt_frames_ok; }
uint32_t protocol_frames_rejected(void) { return cnt_frames_ko; }
uint32_t protocol_bytes_written(void)   { return bytes_written; }
