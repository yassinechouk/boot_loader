"""
Tests for the protocol and bootloader simulator.

Run without any hardware:
    python3 test_protocol.py

The main value is coverage of failure cases — corrupted frames,
disordered sequences, power cuts — which are difficult or impossible
to reproduce reliably on real hardware.
"""

import os
import struct
import sys

import protocol as p
from crc32 import crc32_stm32
from bootloader_sim import (
    BootloaderSim, Metadata, PowerLoss,
    METADATA_SIZE, SLOT_SIZE, MAX_BOOT_FAILURES,
)

_passed = 0
_failed = []


def check(name, condition, detail=""):
    global _passed
    if condition:
        _passed += 1
        print(f"  ok   {name}")
    else:
        _failed.append(name)
        print(f"  FAIL {name}   {detail}")


def section(title):
    print(f"\n--- {title} ---")


# ===============================================================
# Encoding / decoding
# ===============================================================
def test_encoding():
    section("Encoding and decoding")

    f = p.Frame(cmd=p.CMD_DATA, seq=7, data=b"hello")
    raw = p.encode(f)
    back = p.decode(raw)

    check("round-trip preserves cmd", back.cmd == f.cmd)
    check("round-trip preserves seq", back.seq == f.seq)
    check("round-trip preserves data", back.data == f.data)
    check("size = overhead + payload",
          len(raw) == p.FRAME_OVERHEAD + len(f.data),
          f"{len(raw)}")
    check("magic present", raw[0:2] == p.MAGIC)

    empty = p.decode(p.encode(p.Frame(p.CMD_GET_INFO, 0)))
    check("empty payload accepted", empty.data == b"")

    big = p.Frame(p.CMD_DATA, 1, bytes(p.MAX_PAYLOAD_SIZE))
    check("maximum payload accepted",
          len(p.decode(p.encode(big)).data) == p.MAX_PAYLOAD_SIZE)


def test_binary_fields():
    section("Binary layout")

    f = p.Frame(cmd=0x03, seq=0x1234, data=b"\xAA" * 0x0102)
    raw = p.encode(f)

    check("LENGTH little-endian", raw[3:5] == b"\x02\x01", raw[3:5].hex())
    check("SEQ little-endian", raw[5:7] == b"\x34\x12", raw[5:7].hex())
    check("CMD at offset 2", raw[2] == 0x03)

    # CRC covers CMD..DATA, not the MAGIC
    body = raw[2:len(raw) - 4]
    expected = crc32_stm32(body)
    received = struct.unpack("<I", raw[-4:])[0]
    check("CRC computed over CMD..DATA", expected == received)


def test_error_detection():
    section("Error detection")

    raw = bytearray(p.encode(p.Frame(p.CMD_DATA, 1, b"payload")))

    corrupted = bytearray(raw)
    corrupted[10] ^= 0xFF
    try:
        p.decode(bytes(corrupted))
        check("data corruption detected", False)
    except p.BadCRC:
        check("data corruption detected", True)

    bad_magic = bytearray(raw)
    bad_magic[1] = 0x00
    try:
        p.decode(bytes(bad_magic))
        check("invalid magic detected", False)
    except p.BadMagic:
        check("invalid magic detected", True)

    # Abnormal LENGTH: must be rejected before any buffering
    huge = bytearray(raw)
    huge[3:5] = struct.pack("<H", 60000)
    try:
        p.decode(bytes(huge))
        check("abnormal LENGTH rejected", False)
    except p.BadLength:
        check("abnormal LENGTH rejected", True)

    try:
        p.decode(raw[:5])
        check("truncated frame reported as Incomplete", False)
    except p.Incomplete:
        check("truncated frame reported as Incomplete", True)

    # SEQ corruption must also be caught: it is covered by the CRC
    seq_corrupted = bytearray(raw)
    seq_corrupted[5] ^= 0xFF
    try:
        p.decode(bytes(seq_corrupted))
        check("SEQ corruption detected", False)
    except p.BadCRC:
        check("SEQ corruption detected", True)


# ===============================================================
# Metadata
# ===============================================================
def test_metadata():
    section("Metadata")

    m = Metadata(counter=5, fw_size=1234, fw_crc32=0xDEADBEEF,
                 fw_version=0x00010203, active_slot=p.SLOT_B,
                 state=p.STATE_VALID, boot_fail_count=1)
    raw = m.pack()

    check("size is 28 bytes", len(raw) == METADATA_SIZE, str(len(raw)))

    reread = Metadata.unpack(raw)
    check("round-trip faithful",
          reread is not None
          and reread.counter == 5
          and reread.fw_size == 1234
          and reread.active_slot == p.SLOT_B
          and reread.state == p.STATE_VALID)

    # Erased page
    check("erased page rejected",
          Metadata.unpack(bytes([0xFF] * METADATA_SIZE)) is None)

    # Partial write: magic + counter present, rest at 0xFF.
    # Exactly the case meta_crc32 must catch.
    partial = raw[:8] + bytes([0xFF] * (METADATA_SIZE - 8))
    check("partial write rejected by CRC",
          Metadata.unpack(partial) is None)

    # Corrupted field
    corrupted = bytearray(raw)
    corrupted[10] ^= 0xFF
    check("corruption detected by CRC",
          Metadata.unpack(bytes(corrupted)) is None)


def test_page_selection():
    section("Metadata page selection")

    sim = BootloaderSim()
    check("blank board: no metadata", sim.read_metadata() is None)

    sim.write_metadata(Metadata(fw_size=100, state=p.STATE_VALID))
    m = sim.read_metadata()
    check("first write readable", m is not None and m.counter == 1)

    sim.write_metadata(Metadata(fw_size=200, state=p.STATE_VALID))
    m = sim.read_metadata()
    check("counter incremented", m.counter == 2)
    check("most recent takes precedence", m.fw_size == 200)

    sim.write_metadata(Metadata(fw_size=300, state=p.STATE_VALID))
    m = sim.read_metadata()
    check("third write", m.counter == 3 and m.fw_size == 300)

    check("pages alternate",
          sim.flash.erase_count[0] >= 1 and sim.flash.erase_count[1] >= 1,
          str(sim.flash.erase_count))


def test_metadata_power_cut():
    section("Power cut during metadata write")

    sim = BootloaderSim()
    sim.write_metadata(Metadata(fw_size=111, state=p.STATE_VALID))
    before = sim.read_metadata()

    # Power cut after a single 8-byte block
    try:
        sim.write_metadata(Metadata(fw_size=222, state=p.STATE_VALID),
                           stop_after_units=1)
        check("power cut simulated", False)
    except PowerLoss:
        check("power cut simulated", True)

    after = sim.read_metadata()
    check("old copy survives",
          after is not None and after.fw_size == before.fw_size,
          f"{after}")
    check("partial copy is ignored", after.counter == before.counter)

    # Power cut during erase
    sim2 = BootloaderSim()
    sim2.write_metadata(Metadata(fw_size=555, state=p.STATE_VALID))
    ref = sim2.read_metadata()
    try:
        sim2.write_metadata(Metadata(fw_size=666), interrupt_erase=True)
    except PowerLoss:
        pass
    survivor = sim2.read_metadata()
    check("survives a power cut during erase",
          survivor is not None and survivor.fw_size == ref.fw_size,
          f"{survivor}")


# ===============================================================
# State machine
# ===============================================================
def _transfer(sim, firmware, slot=None, version=0x00010000):
    """Runs a complete transfer. Returns the last response."""
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_GET_INFO, 0))))
    info = p.InfoResponse.unpack(rep.data)
    target = info.free_slot if slot is None else slot

    su = p.StartUpdate(len(firmware), crc32_stm32(firmware), version, target)
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su.pack()))))
    if rep.cmd != p.RSP_ACK:
        return rep

    seq = 1
    for block in p.split_firmware(firmware):
        rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, seq, block))))
        if rep.cmd != p.RSP_ACK:
            return rep
        seq += 1

    return p.decode(sim.handle(p.encode(p.Frame(p.CMD_END_UPDATE, seq))))


def test_nominal_transfer():
    section("Nominal transfer")

    sim = BootloaderSim()
    fw = os.urandom(1000)

    rep = _transfer(sim, fw)
    check("transfer acknowledged", rep.cmd == p.RSP_ACK, str(rep))

    meta = sim.read_metadata()
    check("state TESTING after transfer", meta.state == p.STATE_TESTING)
    check("slot switched to B", meta.active_slot == p.SLOT_B)
    check("size recorded", meta.fw_size == len(fw))

    written = sim.flash.read_slot(p.SLOT_B, 0, len(fw))
    check("flash content matches firmware", written == fw)


def test_unaligned_firmware():
    section("Firmware size not a multiple of block size")

    for size in (1, 255, 256, 257, 1000):
        sim = BootloaderSim()
        fw = os.urandom(size)
        rep = _transfer(sim, fw)
        written = sim.flash.read_slot(p.SLOT_B, 0, size)
        check(f"size {size} bytes",
              rep.cmd == p.RSP_ACK and written == fw)


def test_retransmission():
    section("Retransmission")

    sim = BootloaderSim()
    fw = os.urandom(600)

    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_GET_INFO, 0))))
    info = p.InfoResponse.unpack(rep.data)
    su = p.StartUpdate(len(fw), crc32_stm32(fw), 1, info.free_slot)
    sim.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su.pack())))

    blocks = list(p.split_firmware(fw))

    r1 = p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, 1, blocks[0]))))
    check("first frame acknowledged", r1.cmd == p.RSP_ACK)

    # Same frame resent: the ACK was lost
    r2 = p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, 1, blocks[0]))))
    check("retransmission acknowledged without re-write", r2.cmd == p.RSP_ACK)

    # Transfer must be able to continue normally
    r3 = p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, 2, blocks[1]))))
    check("transfer resumed after retransmission", r3.cmd == p.RSP_ACK)

    for i, block in enumerate(blocks[2:], start=3):
        p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, i, block))))

    end = p.decode(sim.handle(p.encode(p.Frame(p.CMD_END_UPDATE, len(blocks) + 1))))
    check("global CRC correct despite retransmission",
          end.cmd == p.RSP_ACK, str(end))


def test_out_of_order_sequence():
    section("Out-of-order sequence")

    sim = BootloaderSim()
    fw = os.urandom(600)

    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_GET_INFO, 0))))
    info = p.InfoResponse.unpack(rep.data)
    su = p.StartUpdate(len(fw), crc32_stm32(fw), 1, info.free_slot)
    sim.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su.pack())))

    blocks = list(p.split_firmware(fw))
    sim.handle(p.encode(p.Frame(p.CMD_DATA, 1, blocks[0])))

    # Skip frame 2
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, 3, blocks[2]))))
    check("out-of-sequence frame rejected", rep.cmd == p.RSP_NACK)
    check("ERR_SEQ error signalled",
          rep.data == bytes([p.ERR_SEQ]),
          p.ERROR_NAMES.get(rep.data[0]))


def test_pre_transfer_checks():
    section("Checks before erase")

    # Firmware too large
    sim = BootloaderSim()
    su = p.StartUpdate(SLOT_SIZE + 1, 0, 1, p.SLOT_B)
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su.pack()))))
    check("oversized firmware rejected", rep.data == bytes([p.ERR_SIZE]))
    check("slot not erased after rejection",
          all(b == 0xFF for b in sim.flash.slots[p.SLOT_B][:256]))

    # Wrong target slot: the wrong binary was sent scenario
    sim2 = BootloaderSim()
    su2 = p.StartUpdate(1000, 0, 1, p.SLOT_A)   # A is active, not free
    rep2 = p.decode(sim2.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su2.pack()))))
    check("wrong slot rejected", rep2.data == bytes([p.ERR_SLOT]))

    # Command in wrong state
    sim3 = BootloaderSim()
    rep3 = p.decode(sim3.handle(p.encode(p.Frame(p.CMD_DATA, 1, b"x" * 16))))
    check("DATA without START rejected", rep3.data == bytes([p.ERR_STATE]))


def test_invalid_global_crc():
    section("Invalid global CRC")

    sim = BootloaderSim()
    fw = os.urandom(600)

    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_GET_INFO, 0))))
    info = p.InfoResponse.unpack(rep.data)

    # Intentionally wrong CRC
    su = p.StartUpdate(len(fw), 0xDEADBEEF, 1, info.free_slot)
    sim.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su.pack())))

    seq = 1
    for block in p.split_firmware(fw):
        sim.handle(p.encode(p.Frame(p.CMD_DATA, seq, block)))
        seq += 1

    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_END_UPDATE, seq))))
    check("invalid global CRC detected", rep.cmd == p.RSP_NACK)
    check("ERR_GLOBAL_CRC error", rep.data == bytes([p.ERR_GLOBAL_CRC]))

    meta = sim.read_metadata()
    check("state remains IN_PROGRESS", meta.state == p.STATE_IN_PROGRESS)


def test_write_without_erase():
    section("Write on non-erased area")

    sim = BootloaderSim()
    ok1 = sim.flash.write_slot(p.SLOT_A, 0, b"\x01" * 8)
    ok2 = sim.flash.write_slot(p.SLOT_A, 0, b"\x02" * 8)

    check("first write accepted", ok1)
    check("re-write without erase rejected", not ok2)


# ===============================================================
# Boot and rollback
# ===============================================================
def test_boot_decision():
    section("Boot decision")

    sim = BootloaderSim()
    check("blank board -> receive mode", "receive mode" in sim.boot())

    sim.write_metadata(Metadata(fw_size=100, state=p.STATE_VALID,
                                active_slot=p.SLOT_A))
    check("VALID state -> jump", "jump" in sim.boot())

    sim2 = BootloaderSim()
    sim2.write_metadata(Metadata(fw_size=100, state=p.STATE_IN_PROGRESS))
    check("interrupted transfer detected", "interrupted" in sim2.boot())


def test_rollback():
    section("Rollback after repeated failures")

    sim = BootloaderSim()
    sim.write_metadata(Metadata(fw_size=100, state=p.STATE_TESTING,
                                active_slot=p.SLOT_B))

    for i in range(1, MAX_BOOT_FAILURES + 1):
        msg = sim.boot()
        check(f"attempt {i}: trial jump to slot B", "trial jump" in msg, msg)

    final = sim.boot()
    check("rollback triggered beyond threshold",
          "rollback" in final and "slot A" in final, final)


def test_application_confirmation():
    section("Application confirmation")

    sim = BootloaderSim()
    sim.write_metadata(Metadata(fw_size=100, state=p.STATE_TESTING,
                                active_slot=p.SLOT_B))
    sim.boot()

    # Application confirms successful boot
    meta = sim.read_metadata()
    meta.state = p.STATE_VALID
    meta.boot_fail_count = 0
    sim.write_metadata(meta)

    check("no rollback after confirmation",
          "slot B" in sim.boot())


# ===============================================================
def main():
    print("Firmware update protocol tests")
    print("=" * 50)

    test_encoding()
    test_binary_fields()
    test_error_detection()
    test_metadata()
    test_page_selection()
    test_metadata_power_cut()
    test_nominal_transfer()
    test_unaligned_firmware()
    test_retransmission()
    test_out_of_order_sequence()
    test_pre_transfer_checks()
    test_invalid_global_crc()
    test_write_without_erase()
    test_boot_decision()
    test_rollback()
    test_application_confirmation()

    print("\n" + "=" * 50)
    total = _passed + len(_failed)
    print(f"{_passed}/{total} tests passed")
    if _failed:
        print("\nFailures:")
        for name in _failed:
            print(f"  - {name}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
