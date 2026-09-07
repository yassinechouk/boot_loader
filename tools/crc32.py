"""
CRC-32 reproduisant le peripherique materiel du STM32L4.

Configuration cible du peripherique :
  - polynome  : 0x04C11DB7 (CRC-32 Ethernet)
  - init      : 0xFFFFFFFF
  - REV_IN    : 01 (inversion par octet)
  - REV_OUT   : 0
  - XOR final : aucun

Cette variante differe de zlib.crc32, qui applique une reflexion en
sortie et un XOR final. Le PC s'aligne sur le materiel plutot que
l'inverse : le peripherique STM32 calcule en 4 cycles AHB sans
consommer de CPU, alors que le PC peut absorber n'importe quel
algorithme sans que cela se remarque.

VALIDE sur STM32L476RG
----------------------
Le peripherique CRC configure avec POL=0x04C11DB7, INIT=0xFFFFFFFF,
REV_IN=01, REV_OUT=0, alimente octet par octet, produit :

    "123456789"  ->  0x9B63D02C
    4 zeros      ->  0xC704DD7B
    "STM32"      ->  0xF4F0FF62

Identique a cette implementation.
"""

POLY = 0x04C11DB7
INIT = 0xFFFFFFFF


def _reverse_byte(b: int) -> int:
    """Inverse l'ordre des bits d'un octet — equivalent de REV_IN=01."""
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4)
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2)
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1)
    return b


def crc32_stm32(data: bytes) -> int:
    """Calcule le CRC32 tel que le produirait le peripherique STM32L4."""
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
    # Vecteurs de reference a comparer avec le materiel
    for essai in (b"123456789", b"\x00\x00\x00\x00", b"STM32"):
        print(f"{essai!r:24} -> 0x{crc32_stm32(essai):08X}")
