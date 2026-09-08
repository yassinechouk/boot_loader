"""
STM32 bootloader simulator.

Reproduces the expected firmware behaviour without any hardware:
flash is a bytearray, metadata pages too. This allows testing the
state machine, sequencing, retransmissions and especially power cuts
— impossible to reproduce reliably on real hardware.

The fidelity targeted covers the properties that cause bugs:
  - an erased page is 0xFF throughout
  - writing requires a prior erase
  - writes are in 8-byte blocks (double-word)
  - a power cut leaves a partial, observable state
"""

import struct
from dataclasses import dataclass, field

import protocol as p
from crc32 import crc32_stm32

# ---------------------------------------------------------------
# Constants — mirror of shared/metadata.h
# ---------------------------------------------------------------
FLASH_PAGE_SIZE = 2048
SLOT_SIZE = 480 * 1024
METADATA_MAGIC = 0x424C4D44          # "BLMD"
METADATA_FORMAT = "<IIIIIBBBBI"      # 28 bytes
METADATA_SIZE = struct.calcsize(METADATA_FORMAT)
WRITE_UNIT = 8                       # 64-bit double-word
MAX_BOOT_FAILURES = 3
BOOTLOADER_VERSION = 0x00000100

ERASED = 0xFF


class PowerLoss(Exception):
    """Raised to simulate a power cut."""


# ---------------------------------------------------------------
# Metadata
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
        """Serialises, CRC included."""
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
        Deserialises from a page. Returns None if invalid.

        The magic alone is not sufficient: it shares its 8-byte block
        with the counter, so a power cut after the first write leaves
        a valid magic in front of fields still at 0xFF. The CRC decides.
        """
        if len(raw) < METADATA_SIZE:
            return None

        body = raw[:METADATA_SIZE - 4]
        stored_crc = struct.unpack("<I", raw[METADATA_SIZE - 4:METADATA_SIZE])[0]

        magic = struct.unpack("<I", body[0:4])[0]
        if magic != METADATA_MAGIC:
            return None

        if crc32_stm32(body) != stored_crc:
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
# Simulated flash
# ---------------------------------------------------------------
class SimulatedFlash:
    """
    Reproduces the constraints of the STM32L4 flash controller.

    write_after_n_units allows simulating a power cut: the write
    stops after N 8-byte blocks, leaving the rest erased.
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

    # -- application slots ------------------------------------------
    def erase_slot(self, slot: int):
        self.slots[slot] = bytearray([ERASED] * SLOT_SIZE)

    def write_slot(self, slot: int, offset: int, data: bytes) -> bool:
        """
        Writes then reads back to verify (read-back).
        Returns False if the area was not erased, which would yield
        an indeterminate result on real hardware.
        """
        if offset + len(data) > SLOT_SIZE:
            return False

        zone = self.slots[slot][offset:offset + len(data)]
        if any(b != ERASED for b in zone):
            return False        # write on non-erased area

        self.slots[slot][offset:offset + len(data)] = data
        return self.slots[slot][offset:offset + len(data)] == data

    def read_slot(self, slot: int, offset: int, length: int) -> bytes:
        return bytes(self.slots[slot][offset:offset + length])

    # -- metadata pages ---------------------------------------------
    def erase_meta_page(self, page: int, interrupt: bool = False):
        if interrupt:
            # Interrupted erase: indeterminate intermediate state.
            # Model the worst case: a half-erased page.
            half = FLASH_PAGE_SIZE // 2
            self.meta_pages[page][:half] = bytearray([ERASED] * half)
            raise PowerLoss(f"power cut during erase of page {page}")
        self.meta_pages[page] = bytearray([ERASED] * FLASH_PAGE_SIZE)
        self.erase_count[page] += 1

    def write_meta_page(self, page: int, data: bytes, stop_after_units=None):
        """
        Writes in 8-byte blocks. stop_after_units simulates a power cut
        after N blocks, leaving the rest of the page at 0xFF.
        """
        padded = data + bytes([ERASED] * ((-len(data)) % WRITE_UNIT))
        units = len(padded) // WRITE_UNIT

        for i in range(units):
            if stop_after_units is not None and i >= stop_after_units:
                raise PowerLoss(
                    f"power cut after {i} blocks out of {units} (page {page})"
                )
            start = i * WRITE_UNIT
            self.meta_pages[page][start:start + WRITE_UNIT] = \
                padded[start:start + WRITE_UNIT]

    def read_meta_page(self, page: int) -> bytes:
        return bytes(self.meta_pages[page][:METADATA_SIZE])


# ---------------------------------------------------------------
# Simulated bootloader
# ---------------------------------------------------------------
class BootloaderSim:
    """Bootloader state machine, without hardware."""

    # internal states
    IDLE = "IDLE"
    RECEIVING = "RECEIVING"

    def __init__(self, verbose: bool = False):
        self.flash = SimulatedFlash()
        self.verbose = verbose
        self.state = self.IDLE

        # transfer context
        self.expected_seq = 0
        self.last_seq = None
        self.target_slot = None
        self.fw_size = 0
        self.fw_crc32 = 0
        self.fw_version = 0
        self.bytes_received = 0

    # -- logging ----------------------------------------------------
    def _log(self, msg: str):
        if self.verbose:
            print(f"  [sim] {msg}")

    # -- metadata ---------------------------------------------------
    def read_metadata(self):
        """
        Reads both pages and returns the most recent valid one.
        Returns None if none is valid (blank board).
        """
        candidates = []
        for page in (0, 1):
            meta = Metadata.unpack(self.flash.read_meta_page(page))
            if meta is not None:
                candidates.append((meta.counter, page, meta))

        if not candidates:
            return None

        candidates.sort(key=lambda t: t[0])
        return candidates[-1][2]

    def write_metadata(self, meta: Metadata, stop_after_units=None,
                       interrupt_erase=False):
        """
        Writes to the inactive page only: the other remains intact
        and readable throughout the operation.
        """
        current = self.read_metadata()
        if current is None:
            page = 0
            meta.counter = 1
        else:
            # find which page holds the current version
            current_page = 0
            for pg in (0, 1):
                m = Metadata.unpack(self.flash.read_meta_page(pg))
                if m is not None and m.counter == current.counter:
                    current_page = pg
                    break
            page = 1 - current_page
            meta.counter = current.counter + 1

        self.flash.erase_meta_page(page, interrupt=interrupt_erase)
        self.flash.write_meta_page(page, meta.pack(),
                                   stop_after_units=stop_after_units)
        self._log(f"metadata written to page {page}: {meta}")

    # -- responses --------------------------------------------------
    def _ack(self, seq: int) -> p.Frame:
        return p.Frame(cmd=p.RSP_ACK, seq=seq)

    def _nack(self, seq: int, err: int) -> p.Frame:
        self._log(f"NACK {p.ERROR_NAMES.get(err, err)}")
        return p.Frame(cmd=p.RSP_NACK, seq=seq, data=bytes([err]))

    # -- frame handling ---------------------------------------------
    def handle(self, raw: bytes) -> bytes:
        """Receives raw bytes, returns the response as bytes."""
        try:
            frame = p.decode(raw)
        except p.BadCRC:
            return p.encode(self._nack(0, p.ERR_CRC))
        except p.BadLength:
            return p.encode(self._nack(0, p.ERR_LENGTH))
        except (p.BadMagic, p.Incomplete):
            return b""      # frame ignored, no response

        self._log(f"received {frame}")

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
        free = 1 - active

        # All checks BEFORE erasing anything.
        if su.fw_size > SLOT_SIZE:
            return self._nack(frame.seq, p.ERR_SIZE)
        if su.target_slot != free:
            return self._nack(frame.seq, p.ERR_SLOT)

        # Fail-safe ordering: mark IN_PROGRESS before erasing.
        # The reverse order would leave a window where metadata claims
        # a valid firmware exists while it has just been destroyed.
        updated = Metadata(
            fw_size=su.fw_size,
            fw_crc32=su.fw_crc32,
            fw_version=su.fw_version,
            active_slot=active,
            state=p.STATE_IN_PROGRESS,
        )
        self.write_metadata(updated)

        self.flash.erase_slot(su.target_slot)
        self._log(f"slot {'AB'[su.target_slot]} erased")

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

        # Retransmission: the PC did not receive the previous ACK.
        # It does not expect a re-write, only the missing acknowledgement.
        # Re-writing would be incorrect: flash cannot be reprogrammed
        # without erasing.
        if frame.seq == self.last_seq:
            self._log("retransmission detected, ACK resent without re-write")
            return self._ack(frame.seq)

        if frame.seq != self.expected_seq:
            return self._nack(frame.seq, p.ERR_SEQ)

        index = frame.seq - 1        # START_UPDATE occupied seq 0
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

        # Global verification by re-reading flash: validates storage,
        # where the per-frame CRC only validated delivery.
        content = self.flash.read_slot(self.target_slot, 0, self.fw_size)
        computed = crc32_stm32(content)

        if computed != self.fw_crc32:
            self._log(f"global CRC mismatch: 0x{computed:08X} != 0x{self.fw_crc32:08X}")
            self.state = self.IDLE
            return self._nack(frame.seq, p.ERR_GLOBAL_CRC)

        # TESTING, not VALID: a correct CRC proves integrity,
        # not correct operation. The application must confirm.
        meta = Metadata(
            fw_size=self.fw_size,
            fw_crc32=self.fw_crc32,
            fw_version=self.fw_version,
            active_slot=self.target_slot,
            state=p.STATE_TESTING,
        )
        self.write_metadata(meta)
        self.state = self.IDLE
        self._log("global CRC OK, transitioning to TESTING")

        return self._ack(frame.seq)

    def _on_abort(self, frame: p.Frame) -> p.Frame:
        self.state = self.IDLE
        self._log("transfer aborted")
        return self._ack(frame.seq)

    # -- boot simulation --------------------------------------------
    def boot(self) -> str:
        """
        Reproduces the boot decision.
        Returns a description of the chosen action.
        """
        meta = self.read_metadata()

        if meta is None:
            return "no valid metadata -> receive mode"

        if meta.state == p.STATE_VALID:
            return f"jump to slot {'AB'[meta.active_slot]}"

        if meta.state == p.STATE_TESTING:
            if meta.boot_fail_count >= MAX_BOOT_FAILURES:
                other = 1 - meta.active_slot
                return f"rollback to slot {'AB'[other]}"
            meta.boot_fail_count += 1
            self.write_metadata(meta)
            return (
                f"trial jump to slot {'AB'[meta.active_slot]} "
                f"(attempt {meta.boot_fail_count}/{MAX_BOOT_FAILURES})"
            )

        if meta.state == p.STATE_IN_PROGRESS:
            return "interrupted transfer detected -> receive mode"

        return "empty slot -> receive mode"


# ---------------------------------------------------------------
# Demo
# ---------------------------------------------------------------
if __name__ == "__main__":
    import os

    sim = BootloaderSim(verbose=True)
    print("Initial state:", sim.boot())

    firmware = os.urandom(1000)
    fw_crc = crc32_stm32(firmware)
    print(f"\nFirmware: {len(firmware)} bytes, CRC 0x{fw_crc:08X}")

    # GET_INFO
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_GET_INFO, 0))))
    info = p.InfoResponse.unpack(rep.data)
    print(f"\n{info}")

    # START_UPDATE
    su = p.StartUpdate(len(firmware), fw_crc, 0x00010000, info.free_slot)
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su.pack()))))
    print(f"response: {rep}")

    # DATA
    seq = 1
    for block in p.split_firmware(firmware):
        rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, seq, block))))
        assert rep.cmd == p.RSP_ACK, rep
        seq += 1

    # END_UPDATE
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_END_UPDATE, seq))))
    print(f"end: {rep}")

    print("\nAt next boot:", sim.boot())
