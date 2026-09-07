"""
Encodage et decodage des trames du protocole de mise a jour firmware.

Independant du transport : ce module ne connait ni port serie, ni CAN.
Il traduit des octets en trames et inversement, rien de plus.

Les constantes doivent rester synchronisees avec shared/protocol.h.
Voir PROTOCOL.md pour la specification complete et les justifications
de conception.
"""

import struct
from dataclasses import dataclass
from crc32 import crc32_stm32

# ---------------------------------------------------------------
# Constantes — miroir de shared/protocol.h
# ---------------------------------------------------------------
PROTO_VERSION = 1

MAGIC = bytes([0xAA, 0x55])

FRAME_HEADER_SIZE = 7          # MAGIC(2) + CMD(1) + LENGTH(2) + SEQ(2)
FRAME_CRC_SIZE = 4
FRAME_OVERHEAD = FRAME_HEADER_SIZE + FRAME_CRC_SIZE

DATA_BLOCK_SIZE = 256          # multiple de 8, diviseur de 2048
MAX_PAYLOAD_SIZE = 1024        # borne de plausibilite sur LENGTH

# Requetes (PC -> bootloader)
CMD_GET_INFO = 0x01
CMD_START_UPDATE = 0x02
CMD_DATA = 0x03
CMD_END_UPDATE = 0x04
CMD_ABORT = 0x05

# Reponses (bootloader -> PC), bit 7 a 1
RSP_INFO = 0x81
RSP_ACK = 0x82
RSP_NACK = 0x83

# Codes d'erreur, payload d'un RSP_NACK
ERR_CRC = 0x01
ERR_SEQ = 0x02
ERR_LENGTH = 0x03
ERR_FLASH = 0x04
ERR_SIZE = 0x05
ERR_SLOT = 0x06
ERR_STATE = 0x07
ERR_GLOBAL_CRC = 0x08
ERR_PROTO_VER = 0x09

ERROR_NAMES = {
    ERR_CRC: "ERR_CRC",
    ERR_SEQ: "ERR_SEQ",
    ERR_LENGTH: "ERR_LENGTH",
    ERR_FLASH: "ERR_FLASH",
    ERR_SIZE: "ERR_SIZE",
    ERR_SLOT: "ERR_SLOT",
    ERR_STATE: "ERR_STATE",
    ERR_GLOBAL_CRC: "ERR_GLOBAL_CRC",
    ERR_PROTO_VER: "ERR_PROTO_VER",
}

CMD_NAMES = {
    CMD_GET_INFO: "CMD_GET_INFO",
    CMD_START_UPDATE: "CMD_START_UPDATE",
    CMD_DATA: "CMD_DATA",
    CMD_END_UPDATE: "CMD_END_UPDATE",
    CMD_ABORT: "CMD_ABORT",
    RSP_INFO: "RSP_INFO",
    RSP_ACK: "RSP_ACK",
    RSP_NACK: "RSP_NACK",
}

# Slots
SLOT_A = 0
SLOT_B = 1

# Etats du firmware
STATE_EMPTY = 0x00
STATE_IN_PROGRESS = 0x01
STATE_TESTING = 0x02
STATE_VALID = 0x03

STATE_NAMES = {
    STATE_EMPTY: "EMPTY",
    STATE_IN_PROGRESS: "IN_PROGRESS",
    STATE_TESTING: "TESTING",
    STATE_VALID: "VALID",
}


# ---------------------------------------------------------------
# Exceptions
# ---------------------------------------------------------------
class ProtocolError(Exception):
    """Erreur generique de protocole."""


class BadMagic(ProtocolError):
    """Le preambule attendu n'est pas present."""


class BadCRC(ProtocolError):
    """Le CRC calcule ne correspond pas au CRC recu."""


class BadLength(ProtocolError):
    """Le champ LENGTH est hors des bornes acceptees."""


class Incomplete(ProtocolError):
    """La trame n'est pas encore entierement recue — ce n'est pas une erreur."""


# ---------------------------------------------------------------
# Trame
# ---------------------------------------------------------------
@dataclass
class Frame:
    cmd: int
    seq: int
    data: bytes = b""

    def __repr__(self) -> str:
        name = CMD_NAMES.get(self.cmd, f"0x{self.cmd:02X}")
        return f"Frame({name}, seq={self.seq}, len={len(self.data)})"


def encode(frame: Frame) -> bytes:
    """Serialise une trame en octets prets a transmettre."""
    if len(frame.data) > MAX_PAYLOAD_SIZE:
        raise BadLength(
            f"payload de {len(frame.data)} octets, maximum {MAX_PAYLOAD_SIZE}"
        )

    # Le CRC couvre CMD + LENGTH + SEQ + DATA, pas le MAGIC :
    # celui-ci se valide de lui-meme par sa seule reconnaissance.
    body = struct.pack("<BHH", frame.cmd, len(frame.data), frame.seq) + frame.data
    crc = crc32_stm32(body)

    return MAGIC + body + struct.pack("<I", crc)


def decode(raw: bytes) -> Frame:
    """
    Deserialise une trame complete.

    Leve Incomplete si les octets ne suffisent pas encore, ou
    BadMagic / BadLength / BadCRC selon l'invalidite rencontree.
    """
    if len(raw) < FRAME_HEADER_SIZE:
        raise Incomplete("en-tete incomplet")

    if raw[0:2] != MAGIC:
        raise BadMagic(f"attendu {MAGIC.hex()}, recu {raw[0:2].hex()}")

    cmd = raw[2]
    length = struct.unpack("<H", raw[3:5])[0]
    seq = struct.unpack("<H", raw[5:7])[0]

    # Verification de plausibilite AVANT toute bufferisation.
    # Un champ de longueur non borne est un vecteur classique
    # de debordement de tampon.
    if length > MAX_PAYLOAD_SIZE:
        raise BadLength(f"LENGTH={length} depasse {MAX_PAYLOAD_SIZE}")

    total = FRAME_OVERHEAD + length
    if len(raw) < total:
        raise Incomplete(f"{len(raw)}/{total} octets recus")

    data = raw[FRAME_HEADER_SIZE:FRAME_HEADER_SIZE + length]
    crc_recu = struct.unpack("<I", raw[FRAME_HEADER_SIZE + length:total])[0]

    body = raw[2:FRAME_HEADER_SIZE + length]
    crc_calcule = crc32_stm32(body)

    if crc_recu != crc_calcule:
        raise BadCRC(f"recu 0x{crc_recu:08X}, calcule 0x{crc_calcule:08X}")

    return Frame(cmd=cmd, seq=seq, data=data)


def frame_length(raw: bytes) -> int:
    """
    Longueur totale de la trame dont l'en-tete commence dans raw.
    Permet a un recepteur de savoir combien d'octets attendre encore.
    """
    if len(raw) < FRAME_HEADER_SIZE:
        raise Incomplete("en-tete incomplet")
    length = struct.unpack("<H", raw[3:5])[0]
    if length > MAX_PAYLOAD_SIZE:
        raise BadLength(f"LENGTH={length} depasse {MAX_PAYLOAD_SIZE}")
    return FRAME_OVERHEAD + length


# ---------------------------------------------------------------
# Payloads structures
# ---------------------------------------------------------------
@dataclass
class StartUpdate:
    """
    Payload de CMD_START_UPDATE — 16 octets.
    Miroir exact de start_update_t dans shared/protocol.h.

    Le nombre de trames n'est volontairement pas transmis : il se
    deduit de fw_size et de DATA_BLOCK_SIZE. Une donnee redondante
    pourrait devenir incoherente avec sa source.
    """
    fw_size: int
    fw_crc32: int
    fw_version: int
    target_slot: int

    FORMAT = "<IIIBBH"

    def pack(self) -> bytes:
        return struct.pack(
            self.FORMAT,
            self.fw_size,
            self.fw_crc32,
            self.fw_version,
            self.target_slot,
            PROTO_VERSION,
            0,  # reserved
        )

    @classmethod
    def unpack(cls, data: bytes) -> "StartUpdate":
        expected = struct.calcsize(cls.FORMAT)
        if len(data) != expected:
            raise BadLength(
                f"START_UPDATE fait {expected} octets, recu {len(data)}"
            )
        size, crc, ver, slot, proto, _ = struct.unpack(cls.FORMAT, data)
        if proto != PROTO_VERSION:
            raise ProtocolError(f"version de protocole {proto} non supportee")
        return cls(fw_size=size, fw_crc32=crc, fw_version=ver, target_slot=slot)


@dataclass
class InfoResponse:
    """
    Payload de RSP_INFO — 12 octets.
    Miroir exact de info_response_t dans shared/protocol.h.

    active_slot et free_slot sont mutuellement deductibles. Cette
    redondance est volontaire : la decision "ou ecrire" revient au
    bootloader, qui detient l'etat reel.
    """
    fw_version: int
    bl_version: int
    proto_version: int
    active_slot: int
    free_slot: int
    state: int

    FORMAT = "<IIBBBB"

    def pack(self) -> bytes:
        return struct.pack(
            self.FORMAT,
            self.fw_version,
            self.bl_version,
            self.proto_version,
            self.active_slot,
            self.free_slot,
            self.state,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "InfoResponse":
        expected = struct.calcsize(cls.FORMAT)
        if len(data) != expected:
            raise BadLength(f"RSP_INFO fait {expected} octets, recu {len(data)}")
        return cls(*struct.unpack(cls.FORMAT, data))

    def __repr__(self) -> str:
        return (
            f"InfoResponse(fw=0x{self.fw_version:08X}, "
            f"bl=0x{self.bl_version:08X}, "
            f"active={'AB'[self.active_slot]}, "
            f"free={'AB'[self.free_slot]}, "
            f"state={STATE_NAMES.get(self.state, self.state)})"
        )


# ---------------------------------------------------------------
# Utilitaires
# ---------------------------------------------------------------
def split_firmware(data: bytes, block: int = DATA_BLOCK_SIZE):
    """Decoupe un binaire en blocs. Le dernier peut etre plus court."""
    for offset in range(0, len(data), block):
        yield data[offset:offset + block]


def frame_count(fw_size: int, block: int = DATA_BLOCK_SIZE) -> int:
    """Nombre de trames CMD_DATA necessaires pour un firmware donne."""
    return (fw_size + block - 1) // block


if __name__ == "__main__":
    print("Tailles des structures :")
    print(f"  StartUpdate  : {struct.calcsize(StartUpdate.FORMAT):2} octets (attendu 16)")
    print(f"  InfoResponse : {struct.calcsize(InfoResponse.FORMAT):2} octets (attendu 12)")

    f = Frame(cmd=CMD_DATA, seq=42, data=bytes(range(16)))
    raw = encode(f)
    print(f"\nTrame encodee ({len(raw)} octets) :")
    print(f"  {raw.hex(' ')}")
    print(f"\nRelue : {decode(raw)}")
