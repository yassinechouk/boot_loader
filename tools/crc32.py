"""
CRC-32 reproducing the STM32L4 hardware peripheral.

Target peripheral configuration:
  - polynomial : 0x04C11DB7 (CRC-32 Ethernet)
  - init       : 0xFFFFFFFF
  - REV_IN     : 01 (bit reversal per byte)
  - REV_OUT    : 0
  - final XOR  : none

This variant differs from zlib.crc32, which applies output
reflection and a final XOR. The PC aligns itself to the hardware
rather than the reverse: the STM32 peripheral computes in 4 AHB
cycles without consuming CPU, whereas the PC can absorb any
algorithm without it being noticeable.

VALIDATED on STM32L476RG
--------------------------
The CRC peripheral configured with POL=0x04C11DB7, INIT=0xFFFFFFFF,
REV_IN=01, REV_OUT=0, fed byte by byte, produces:

    "123456789"  ->  0x9B63D02C
    4 zeros      ->  0xC704DD7B
    "STM32"      ->  0xF4F0FF62

Identical to this implementation.
"""

POLY = 0x04C11DB7
INIT = 0xFFFFFFFF


def _reverse_byte(b: int) -> int:
    """Reverses the bit order of a byte — equivalent to REV_IN=01."""
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4)
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2)
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1)
    return b


def crc32_stm32(data: bytes) -> int:
    """Computes CRC32 as produced by the STM32L4 peripheral."""
    crc = INIT

    for byte in data:
        crc ^= _reverse_byte(byte) << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ POLY) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF

    return crc


if __name__ == "__main__":
    # Reference test vectors to compare against hardware
    for sample in (b"123456789", b"\x00\x00\x00\x00", b"STM32"):
        print(f"{sample!r:24} -> 0x{crc32_stm32(sample):08X}")
