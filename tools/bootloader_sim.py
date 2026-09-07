"""
Simulateur du bootloader STM32.

Reproduit le comportement attendu du firmware sans aucun materiel :
la flash est un bytearray, les pages de metadonnees aussi. Cela permet
de tester la machine a etats, le sequencement, les retransmissions et
surtout les coupures d'alimentation — impossibles a reproduire de
maniere fiable sur du vrai materiel.

La fidelite recherchee porte sur les proprietes qui causent des bugs :
  - une page effacee vaut 0xFF partout
  - on ne peut pas ecrire sans effacer au prealable
  - l'ecriture se fait par blocs de 8 octets (double-mot)
  - une coupure laisse un etat partiel observable
"""

import struct
from dataclasses import dataclass, field

import protocol as p
from crc32 import crc32_stm32

# ---------------------------------------------------------------
# Constantes — miroir de shared/metadata.h
# ---------------------------------------------------------------
FLASH_PAGE_SIZE = 2048
SLOT_SIZE = 480 * 1024
METADATA_MAGIC = 0x424C4D44          # "BLMD"
METADATA_FORMAT = "<IIIIIBBBBI"      # 28 octets
METADATA_SIZE = struct.calcsize(METADATA_FORMAT)
WRITE_UNIT = 8                       # double-mot 64 bits
MAX_BOOT_FAILURES = 3
BOOTLOADER_VERSION = 0x00000100

ERASED = 0xFF


class PowerLoss(Exception):
    """Levee pour simuler une coupure d'alimentation."""


# ---------------------------------------------------------------
# Metadonnees
# ---------------------------------------------------------------
@dataclass
class Metadata:
    counter: int = 0
    fw_size: int = 0
    fw_crc32: int = 0
    fw_version: int = 0
    active_slot: int = p.SLOT_A
    state: int = p.STATE_EMPTY
    boot_fail_count: int = 0

    def pack(self) -> bytes:
        """Serialise, CRC compris."""
        body = struct.pack(
            "<IIIII BBBB",
            METADATA_MAGIC,
            self.counter,
            self.fw_size,
            self.fw_crc32,
            self.fw_version,
            self.active_slot,
            self.state,
            self.boot_fail_count,
            0,  # reserved
        )
        return body + struct.pack("<I", crc32_stm32(body))

    @classmethod
    def unpack(cls, raw: bytes):
        """
        Deserialise depuis une page. Retourne None si invalide.

        Le magic seul ne suffit pas : il partage son bloc de 8 octets
        avec le compteur, donc une coupure apres la premiere ecriture
        laisse un magic valide devant des champs encore a 0xFF.
        Le CRC tranche.
        """
        if len(raw) < METADATA_SIZE:
            return None

        body = raw[:METADATA_SIZE - 4]
        crc_stocke = struct.unpack("<I", raw[METADATA_SIZE - 4:METADATA_SIZE])[0]

        magic = struct.unpack("<I", body[0:4])[0]
        if magic != METADATA_MAGIC:
            return None

        if crc32_stm32(body) != crc_stocke:
            return None

        (_, counter, fw_size, fw_crc, fw_ver,
         active, state, fails, _res) = struct.unpack("<IIIII BBBB", body)

        return cls(
            counter=counter,
            fw_size=fw_size,
            fw_crc32=fw_crc,
            fw_version=fw_ver,
            active_slot=active,
            state=state,
            boot_fail_count=fails,
        )

    def __repr__(self) -> str:
        return (
            f"Metadata(counter={self.counter}, "
            f"slot={'AB'[self.active_slot]}, "
            f"state={p.STATE_NAMES.get(self.state, self.state)}, "
            f"size={self.fw_size}, fails={self.boot_fail_count})"
        )


# ---------------------------------------------------------------
# Flash simulee
# ---------------------------------------------------------------
class SimulatedFlash:
    """
    Reproduit les contraintes du controleur flash du STM32L4.

    write_after_n_units permet de simuler une coupure : l'ecriture
    s'interrompt apres N blocs de 8 octets, laissant le reste efface.
    """

    def __init__(self):
        self.slots = {
            p.SLOT_A: bytearray([ERASED] * SLOT_SIZE),
            p.SLOT_B: bytearray([ERASED] * SLOT_SIZE),
        }
        self.meta_pages = [
            bytearray([ERASED] * FLASH_PAGE_SIZE),
            bytearray([ERASED] * FLASH_PAGE_SIZE),
        ]
        self.erase_count = [0, 0]

    # -- slots applicatifs ------------------------------------------
    def erase_slot(self, slot: int):
        self.slots[slot] = bytearray([ERASED] * SLOT_SIZE)

    def write_slot(self, slot: int, offset: int, data: bytes) -> bool:
        """
        Ecrit puis relit pour verifier (read-back).
        Retourne False si la zone n'etait pas effacee, ce qui rendrait
        le resultat indetermine sur du vrai materiel.
        """
        if offset + len(data) > SLOT_SIZE:
            return False

        zone = self.slots[slot][offset:offset + len(data)]
        if any(b != ERASED for b in zone):
            return False        # ecriture sur zone non effacee

        self.slots[slot][offset:offset + len(data)] = data
        return self.slots[slot][offset:offset + len(data)] == data

    def read_slot(self, slot: int, offset: int, length: int) -> bytes:
        return bytes(self.slots[slot][offset:offset + length])

    # -- pages de metadonnees ---------------------------------------
    def erase_meta_page(self, page: int, interrupt: bool = False):
        if interrupt:
            # Effacement interrompu : etat intermediaire indetermine.
            # On modelise le pire cas, une page a moitie effacee.
            half = FLASH_PAGE_SIZE // 2
            self.meta_pages[page][:half] = bytearray([ERASED] * half)
            raise PowerLoss(f"coupure pendant l'effacement de la page {page}")
        self.meta_pages[page] = bytearray([ERASED] * FLASH_PAGE_SIZE)
        self.erase_count[page] += 1

    def write_meta_page(self, page: int, data: bytes, stop_after_units=None):
        """
        Ecrit par blocs de 8 octets. stop_after_units simule une coupure
        apres N blocs, laissant le reste de la page a 0xFF.
        """
        padded = data + bytes([ERASED] * ((-len(data)) % WRITE_UNIT))
        units = len(padded) // WRITE_UNIT

        for i in range(units):
            if stop_after_units is not None and i >= stop_after_units:
                raise PowerLoss(
                    f"coupure apres {i} blocs sur {units} (page {page})"
                )
            start = i * WRITE_UNIT
            self.meta_pages[page][start:start + WRITE_UNIT] = \
                padded[start:start + WRITE_UNIT]

    def read_meta_page(self, page: int) -> bytes:
        return bytes(self.meta_pages[page][:METADATA_SIZE])


# ---------------------------------------------------------------
# Bootloader simule
# ---------------------------------------------------------------
class BootloaderSim:
    """Machine a etats du bootloader, sans materiel."""

    # etats internes
    IDLE = "IDLE"
    RECEIVING = "RECEIVING"

    def __init__(self, verbose: bool = False):
        self.flash = SimulatedFlash()
        self.verbose = verbose
        self.state = self.IDLE

        # contexte de transfert
        self.expected_seq = 0
        self.last_seq = None
        self.target_slot = None
        self.fw_size = 0
        self.fw_crc32 = 0
        self.fw_version = 0
        self.bytes_received = 0

    # -- journalisation ---------------------------------------------
    def _log(self, msg: str):
        if self.verbose:
            print(f"  [sim] {msg}")

    # -- metadonnees ------------------------------------------------
    def read_metadata(self):
        """
        Lit les deux pages et retourne la plus recente valide.
        Retourne None si aucune ne l'est (carte vierge).
        """
        candidats = []
        for page in (0, 1):
            meta = Metadata.unpack(self.flash.read_meta_page(page))
            if meta is not None:
                candidats.append((meta.counter, page, meta))

        if not candidats:
            return None

        candidats.sort(key=lambda t: t[0])
        return candidats[-1][2]

    def write_metadata(self, meta: Metadata, stop_after_units=None,
                       interrupt_erase=False):
        """
        Ecrit dans la page inactive uniquement : l'autre reste intacte
        et lisible pendant toute l'operation.
        """
        courant = self.read_metadata()
        if courant is None:
            page = 0
            meta.counter = 1
        else:
            # trouver quelle page porte la version courante
            page_courante = 0
            for pg in (0, 1):
                m = Metadata.unpack(self.flash.read_meta_page(pg))
                if m is not None and m.counter == courant.counter:
                    page_courante = pg
                    break
            page = 1 - page_courante
            meta.counter = courant.counter + 1

        self.flash.erase_meta_page(page, interrupt=interrupt_erase)
        self.flash.write_meta_page(page, meta.pack(),
                                   stop_after_units=stop_after_units)
        self._log(f"metadonnees ecrites page {page}: {meta}")

    # -- reponses ---------------------------------------------------
    def _ack(self, seq: int) -> p.Frame:
        return p.Frame(cmd=p.RSP_ACK, seq=seq)

    def _nack(self, seq: int, err: int) -> p.Frame:
        self._log(f"NACK {p.ERROR_NAMES.get(err, err)}")
        return p.Frame(cmd=p.RSP_NACK, seq=seq, data=bytes([err]))

    # -- traitement des trames --------------------------------------
    def handle(self, raw: bytes) -> bytes:
        """Recoit des octets bruts, retourne la reponse en octets."""
        try:
            frame = p.decode(raw)
        except p.BadCRC:
            return p.encode(self._nack(0, p.ERR_CRC))
        except p.BadLength:
            return p.encode(self._nack(0, p.ERR_LENGTH))
        except (p.BadMagic, p.Incomplete):
            return b""      # trame ignoree, pas de reponse

        self._log(f"recu {frame}")

        handlers = {
            p.CMD_GET_INFO: self._on_get_info,
            p.CMD_START_UPDATE: self._on_start_update,
            p.CMD_DATA: self._on_data,
            p.CMD_END_UPDATE: self._on_end_update,
            p.CMD_ABORT: self._on_abort,
        }

        handler = handlers.get(frame.cmd)
        if handler is None:
            return p.encode(self._nack(frame.seq, p.ERR_STATE))

        return p.encode(handler(frame))

    def _on_get_info(self, frame: p.Frame) -> p.Frame:
        meta = self.read_metadata()
        if meta is None:
            active, state, fw_ver = p.SLOT_A, p.STATE_EMPTY, 0
        else:
            active, state, fw_ver = meta.active_slot, meta.state, meta.fw_version

        info = p.InfoResponse(
            fw_version=fw_ver,
            bl_version=BOOTLOADER_VERSION,
            proto_version=p.PROTO_VERSION,
            active_slot=active,
            free_slot=1 - active,
            state=state,
        )
        return p.Frame(cmd=p.RSP_INFO, seq=frame.seq, data=info.pack())

    def _on_start_update(self, frame: p.Frame) -> p.Frame:
        try:
            su = p.StartUpdate.unpack(frame.data)
        except p.ProtocolError:
            return self._nack(frame.seq, p.ERR_PROTO_VER)

        meta = self.read_metadata()
        active = meta.active_slot if meta else p.SLOT_A
        libre = 1 - active

        # Toutes les verifications AVANT d'effacer quoi que ce soit.
        if su.fw_size > SLOT_SIZE:
            return self._nack(frame.seq, p.ERR_SIZE)
        if su.target_slot != libre:
            return self._nack(frame.seq, p.ERR_SLOT)

        # Fail-safe ordering : marquer IN_PROGRESS avant d'effacer.
        # L'ordre inverse laisserait une fenetre ou les metadonnees
        # affirment qu'un firmware valide existe alors qu'il vient
        # d'etre detruit.
        nouvelle = Metadata(
            fw_size=su.fw_size,
            fw_crc32=su.fw_crc32,
            fw_version=su.fw_version,
            active_slot=active,
            state=p.STATE_IN_PROGRESS,
        )
        self.write_metadata(nouvelle)

        self.flash.erase_slot(su.target_slot)
        self._log(f"slot {'AB'[su.target_slot]} efface")

        self.target_slot = su.target_slot
        self.fw_size = su.fw_size
        self.fw_crc32 = su.fw_crc32
        self.fw_version = su.fw_version
        self.bytes_received = 0
        self.expected_seq = frame.seq + 1
        self.last_seq = frame.seq
        self.state = self.RECEIVING

        return self._ack(frame.seq)

    def _on_data(self, frame: p.Frame) -> p.Frame:
        if self.state != self.RECEIVING:
            return self._nack(frame.seq, p.ERR_STATE)

        # Retransmission : le PC n'a pas recu l'ACK precedent.
        # Il n'attend pas une reecriture, seulement l'accuse manquant.
        # Reecrire serait d'ailleurs incorrect : la flash ne se
        # reprogramme pas sans effacement.
        if frame.seq == self.last_seq:
            self._log("retransmission detectee, ACK renvoye sans reecriture")
            return self._ack(frame.seq)

        if frame.seq != self.expected_seq:
            return self._nack(frame.seq, p.ERR_SEQ)

        index = frame.seq - 1        # START_UPDATE occupait seq 0
        offset = index * p.DATA_BLOCK_SIZE

        if not self.flash.write_slot(self.target_slot, offset, frame.data):
            return self._nack(frame.seq, p.ERR_FLASH)

        self.bytes_received += len(frame.data)
        self.last_seq = frame.seq
        self.expected_seq = frame.seq + 1

        return self._ack(frame.seq)

    def _on_end_update(self, frame: p.Frame) -> p.Frame:
        if self.state != self.RECEIVING:
            return self._nack(frame.seq, p.ERR_STATE)

        # Verification globale par relecture de la flash : elle
        # valide le stockage, la ou le CRC de trame ne validait
        # que la transmission.
        contenu = self.flash.read_slot(self.target_slot, 0, self.fw_size)
        calcule = crc32_stm32(contenu)

        if calcule != self.fw_crc32:
            self._log(f"CRC global KO : 0x{calcule:08X} != 0x{self.fw_crc32:08X}")
            self.state = self.IDLE
            return self._nack(frame.seq, p.ERR_GLOBAL_CRC)

        # TESTING, pas VALID : un CRC correct prouve l'integrite,
        # pas le bon fonctionnement. L'application devra confirmer.
        meta = Metadata(
            fw_size=self.fw_size,
            fw_crc32=self.fw_crc32,
            fw_version=self.fw_version,
            active_slot=self.target_slot,
            state=p.STATE_TESTING,
        )
        self.write_metadata(meta)
        self.state = self.IDLE
        self._log("CRC global OK, passage en TESTING")

        return self._ack(frame.seq)

    def _on_abort(self, frame: p.Frame) -> p.Frame:
        self.state = self.IDLE
        self._log("transfert annule")
        return self._ack(frame.seq)

    # -- simulation du demarrage ------------------------------------
    def boot(self) -> str:
        """
        Reproduit la decision prise au demarrage.
        Retourne une description de l'action choisie.
        """
        meta = self.read_metadata()

        if meta is None:
            return "aucune metadonnee valide -> mode reception"

        if meta.state == p.STATE_VALID:
            return f"saut vers le slot {'AB'[meta.active_slot]}"

        if meta.state == p.STATE_TESTING:
            if meta.boot_fail_count >= MAX_BOOT_FAILURES:
                autre = 1 - meta.active_slot
                return f"rollback vers le slot {'AB'[autre]}"
            meta.boot_fail_count += 1
            self.write_metadata(meta)
            return (
                f"saut d'essai vers le slot {'AB'[meta.active_slot]} "
                f"(tentative {meta.boot_fail_count}/{MAX_BOOT_FAILURES})"
            )

        if meta.state == p.STATE_IN_PROGRESS:
            return "transfert interrompu detecte -> mode reception"

        return "slot vide -> mode reception"


# ---------------------------------------------------------------
# Demonstration
# ---------------------------------------------------------------
if __name__ == "__main__":
    import os

    sim = BootloaderSim(verbose=True)
    print("Etat initial :", sim.boot())

    firmware = os.urandom(1000)
    fw_crc = crc32_stm32(firmware)
    print(f"\nFirmware de {len(firmware)} octets, CRC 0x{fw_crc:08X}")

    # GET_INFO
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_GET_INFO, 0))))
    info = p.InfoResponse.unpack(rep.data)
    print(f"\n{info}")

    # START_UPDATE
    su = p.StartUpdate(len(firmware), fw_crc, 0x00010000, info.free_slot)
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su.pack()))))
    print(f"reponse : {rep}")

    # DATA
    seq = 1
    for bloc in p.split_firmware(firmware):
        rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, seq, bloc))))
        assert rep.cmd == p.RSP_ACK, rep
        seq += 1

    # END_UPDATE
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_END_UPDATE, seq))))
    print(f"fin : {rep}")

    print("\nAu redemarrage :", sim.boot())
