"""
Frame encoding and decoding for the firmware update protocol.

Transport-agnostic: this module knows nothing about serial ports or
CAN. It turns bytes into frames and back, nothing more.

The constants must stay in sync with shared/protocol.h. Field offsets
are a contract: swapping two fields of different widths shifts
everything after them, and neither side would fail to compile. See
PROTOCOL.md for the authoritative layout.
"""

import struct
from dataclasses import dataclass
from crc32 import crc32_stm32

# ---------------------------------------------------------------
# Constants — mirror of shared/protocol.h
# ---------------------------------------------------------------
PROTO_VERSION = 1

MAGIC = bytes([0xAA, 0x55])

FRAME_HEADER_SIZE = 7          # MAGIC(2) + CMD(1) + LENGTH(2) + SEQ(2)
FRAME_CRC_SIZE = 4
FRAME_OVERHEAD = FRAME_HEADER_SIZE + FRAME_CRC_SIZE

DATA_BLOCK_SIZE = 256          # multiple of 8, divisor of 2048
MAX_PAYLOAD_SIZE = 1024        # plausibility bound on LENGTH

# Requests (host -> bootloader)
CMD_GET_INFO = 0x01
CMD_START_UPDATE = 0x02
CMD_DATA = 0x03
CMD_END_UPDATE = 0x04
CMD_ABORT = 0x05

# Responses (bootloader -> host), bit 7 set
RSP_INFO = 0x81
RSP_ACK = 0x82
RSP_NACK = 0x83

# Error codes, carried in an RSP_NACK payload
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

# Firmware states
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
    """Generic protocol error."""


class BadMagic(ProtocolError):
    """The expected preamble is absent."""


class BadCRC(ProtocolError):
    """Computed CRC does not match the received one."""


class BadLength(ProtocolError):
    """LENGTH field outside accepted bounds."""


class Incomplete(ProtocolError):
    """Frame not fully received yet — not an error."""


# ---------------------------------------------------------------
# Frame
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
    """Serialise a frame into bytes ready for transmission."""
    if len(frame.data) > MAX_PAYLOAD_SIZE:
        raise BadLength(
            f"payload of {len(frame.data)} bytes, maximum {MAX_PAYLOAD_SIZE}"
        )

    # The CRC covers CMD + LENGTH + SEQ + DATA, not the MAGIC: the
    # preamble validates itself by being recognised at all.
    body = struct.pack("<BHH", frame.cmd, len(frame.data), frame.seq) + frame.data
    crc = crc32_stm32(body)

    return MAGIC + body + struct.pack("<I", crc)


def decode(raw: bytes) -> Frame:
    """
    Deserialise a complete frame.

    Raises Incomplete if the bytes do not suffice yet, or
    BadMagic / BadLength / BadCRC depending on what is wrong.
    """
    if len(raw) < FRAME_HEADER_SIZE:
        raise Incomplete("incomplete header")

    if raw[0:2] != MAGIC:
        raise BadMagic(f"expected {MAGIC.hex()}, got {raw[0:2].hex()}")

    cmd = raw[2]
    length = struct.unpack("<H", raw[3:5])[0]
    seq = struct.unpack("<H", raw[5:7])[0]

    # Plausibility check BEFORE any buffering. An unbounded length
    # field is a classic buffer-overflow vector.
    if length > MAX_PAYLOAD_SIZE:
        raise BadLength(f"LENGTH={length} exceeds {MAX_PAYLOAD_SIZE}")

    total = FRAME_OVERHEAD + length
    if len(raw) < total:
        raise Incomplete(f"{len(raw)}/{total} bytes received")

    data = raw[FRAME_HEADER_SIZE:FRAME_HEADER_SIZE + length]
    crc_recu = struct.unpack("<I", raw[FRAME_HEADER_SIZE + length:total])[0]

    body = raw[2:FRAME_HEADER_SIZE + length]
    crc_calcule = crc32_stm32(body)

    if crc_recu != crc_calcule:
        raise BadCRC(f"received 0x{crc_recu:08X}, computed 0x{crc_calcule:08X}")

    return Frame(cmd=cmd, seq=seq, data=data)


def frame_length(raw: bytes) -> int:
    """
    Total length of the frame whose header starts in raw.
    Lets a receiver know how many more bytes to wait for.
    """
    if len(raw) < FRAME_HEADER_SIZE:
        raise Incomplete("incomplete header")
    length = struct.unpack("<H", raw[3:5])[0]
    if length > MAX_PAYLOAD_SIZE:
        raise BadLength(f"LENGTH={length} exceeds {MAX_PAYLOAD_SIZE}")
    return FRAME_OVERHEAD + length


# ---------------------------------------------------------------
# Payloads structures
# ---------------------------------------------------------------
@dataclass
class StartUpdate:
    """
    Payload of CMD_START_UPDATE — 16 bytes.
    Exact mirror of start_update_t in shared/protocol.h.

    FIELD ORDER IS A CONTRACT. The two single-byte fields come before
    the reserved half-word:

        offset 12 : target_slot    (B)
        offset 13 : proto_version  (B)
        offset 14 : reserved       (H)

    Writing "<IIIBHB" instead of "<IIIBBH" moves proto_version from
    offset 13 to offset 15. Both sides still run; the bootloader reads
    a zero at offset 13 and answers ERR_PROTO_VER. Nothing in Python
    or C catches this — only the layout table in PROTOCOL.md is
    authoritative.

    The frame count is deliberately not transmitted: it follows from
    fw_size and DATA_BLOCK_SIZE. A redundant value could drift out of
    step with its source.
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
                f"START_UPDATE is {expected} bytes, got {len(data)}"
            )
        size, crc, ver, slot, proto, _res = struct.unpack(cls.FORMAT, data)
        if proto != PROTO_VERSION:
            raise ProtocolError(f"unsupported protocol version {proto}")
        return cls(fw_size=size, fw_crc32=crc, fw_version=ver, target_slot=slot)


@dataclass
class InfoResponse:
    """
    Payload of RSP_INFO — 12 bytes.
    Exact mirror of info_response_t in shared/protocol.h.

    active_slot and free_slot are mutually derivable. This redundancy
    is deliberate: the decision "where to write" belongs to the
    bootloader, which holds the real state.
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
            raise BadLength(f"RSP_INFO is {expected} bytes, got {len(data)}")
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
# Utilities
# ---------------------------------------------------------------
def split_firmware(data: bytes, block: int = DATA_BLOCK_SIZE):
    """Split a binary into blocks. The last one may be shorter."""
    for offset in range(0, len(data), block):
        yield data[offset:offset + block]


def frame_count(fw_size: int, block: int = DATA_BLOCK_SIZE) -> int:
    """Number of CMD_DATA frames needed for a given firmware size."""
    return (fw_size + block - 1) // block


if __name__ == "__main__":
    print("Structure sizes:")
    print(f"  StartUpdate  : {struct.calcsize(StartUpdate.FORMAT):2} bytes (expected 16)")
    print(f"  InfoResponse : {struct.calcsize(InfoResponse.FORMAT):2} bytes (expected 12)")

    f = Frame(cmd=CMD_DATA, seq=42, data=bytes(range(16)))
    raw = encode(f)
    print(f"\nEncoded frame ({len(raw)} bytes):")
    print(f"  {raw.hex(' ')}")
    print(f"\nDecoded back: {decode(raw)}")
