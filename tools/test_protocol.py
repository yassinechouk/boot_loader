"""
Tests du protocole et du simulateur de bootloader.

Executables sans aucun materiel :
    python3 test_protocol.py

L'interet principal est de couvrir les cas d'echec — trames corrompues,
sequences desordonnees, coupures d'alimentation — qui sont difficiles
ou impossibles a reproduire de maniere fiable sur du vrai materiel.
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

_reussis = 0
_echoues = []


def check(nom, condition, detail=""):
    global _reussis
    if condition:
        _reussis += 1
        print(f"  ok   {nom}")
    else:
        _echoues.append(nom)
        print(f"  ECHEC {nom}   {detail}")


def section(titre):
    print(f"\n--- {titre} ---")


# ===============================================================
# Encodage / decodage
# ===============================================================
def test_encodage():
    section("Encodage et decodage")

    f = p.Frame(cmd=p.CMD_DATA, seq=7, data=b"hello")
    raw = p.encode(f)
    back = p.decode(raw)

    check("aller-retour preserve cmd", back.cmd == f.cmd)
    check("aller-retour preserve seq", back.seq == f.seq)
    check("aller-retour preserve data", back.data == f.data)
    check("taille = overhead + payload",
          len(raw) == p.FRAME_OVERHEAD + len(f.data),
          f"{len(raw)}")
    check("magic present", raw[0:2] == p.MAGIC)

    vide = p.decode(p.encode(p.Frame(p.CMD_GET_INFO, 0)))
    check("payload vide accepte", vide.data == b"")

    gros = p.Frame(p.CMD_DATA, 1, bytes(p.MAX_PAYLOAD_SIZE))
    check("payload maximal accepte",
          len(p.decode(p.encode(gros)).data) == p.MAX_PAYLOAD_SIZE)


def test_champs_binaires():
    section("Disposition binaire")

    f = p.Frame(cmd=0x03, seq=0x1234, data=b"\xAA" * 0x0102)
    raw = p.encode(f)

    check("LENGTH en little-endian", raw[3:5] == b"\x02\x01", raw[3:5].hex())
    check("SEQ en little-endian", raw[5:7] == b"\x34\x12", raw[5:7].hex())
    check("CMD a l'offset 2", raw[2] == 0x03)

    # Le CRC couvre CMD..DATA, pas le MAGIC
    body = raw[2:len(raw) - 4]
    attendu = crc32_stm32(body)
    recu = struct.unpack("<I", raw[-4:])[0]
    check("CRC calcule sur CMD..DATA", attendu == recu)


def test_detection_erreurs():
    section("Detection d'erreurs")

    raw = bytearray(p.encode(p.Frame(p.CMD_DATA, 1, b"payload")))

    corrompu = bytearray(raw)
    corrompu[10] ^= 0xFF
    try:
        p.decode(bytes(corrompu))
        check("corruption des donnees detectee", False)
    except p.BadCRC:
        check("corruption des donnees detectee", True)

    mauvais_magic = bytearray(raw)
    mauvais_magic[1] = 0x00
    try:
        p.decode(bytes(mauvais_magic))
        check("magic invalide detecte", False)
    except p.BadMagic:
        check("magic invalide detecte", True)

    # LENGTH aberrant : doit etre rejete avant toute bufferisation
    enorme = bytearray(raw)
    enorme[3:5] = struct.pack("<H", 60000)
    try:
        p.decode(bytes(enorme))
        check("LENGTH aberrant rejete", False)
    except p.BadLength:
        check("LENGTH aberrant rejete", True)

    try:
        p.decode(raw[:5])
        check("trame tronquee signalee Incomplete", False)
    except p.Incomplete:
        check("trame tronquee signalee Incomplete", True)

    # Une corruption du SEQ doit aussi etre vue : il est couvert
    seq_corrompu = bytearray(raw)
    seq_corrompu[5] ^= 0xFF
    try:
        p.decode(bytes(seq_corrompu))
        check("corruption du SEQ detectee", False)
    except p.BadCRC:
        check("corruption du SEQ detectee", True)


# ===============================================================
# Metadonnees
# ===============================================================
def test_metadonnees():
    section("Metadonnees")

    m = Metadata(counter=5, fw_size=1234, fw_crc32=0xDEADBEEF,
                 fw_version=0x00010203, active_slot=p.SLOT_B,
                 state=p.STATE_VALID, boot_fail_count=1)
    raw = m.pack()

    check("taille de 28 octets", len(raw) == METADATA_SIZE, str(len(raw)))

    relu = Metadata.unpack(raw)
    check("aller-retour fidele",
          relu is not None
          and relu.counter == 5
          and relu.fw_size == 1234
          and relu.active_slot == p.SLOT_B
          and relu.state == p.STATE_VALID)

    # Page effacee
    check("page effacee rejetee",
          Metadata.unpack(bytes([0xFF] * METADATA_SIZE)) is None)

    # Ecriture partielle : magic + counter presents, reste a 0xFF.
    # C'est precisement le cas que meta_crc32 doit attraper.
    partielle = raw[:8] + bytes([0xFF] * (METADATA_SIZE - 8))
    check("ecriture partielle rejetee par le CRC",
          Metadata.unpack(partielle) is None)

    # Corruption d'un champ
    corrompue = bytearray(raw)
    corrompue[10] ^= 0xFF
    check("corruption detectee par le CRC",
          Metadata.unpack(bytes(corrompue)) is None)


def test_selection_page():
    section("Selection de la page de metadonnees")

    sim = BootloaderSim()
    check("carte vierge : aucune metadonnee", sim.read_metadata() is None)

    sim.write_metadata(Metadata(fw_size=100, state=p.STATE_VALID))
    m = sim.read_metadata()
    check("premiere ecriture lisible", m is not None and m.counter == 1)

    sim.write_metadata(Metadata(fw_size=200, state=p.STATE_VALID))
    m = sim.read_metadata()
    check("compteur incremente", m.counter == 2)
    check("la plus recente fait foi", m.fw_size == 200)

    sim.write_metadata(Metadata(fw_size=300, state=p.STATE_VALID))
    m = sim.read_metadata()
    check("troisieme ecriture", m.counter == 3 and m.fw_size == 300)

    check("les pages alternent",
          sim.flash.erase_count[0] >= 1 and sim.flash.erase_count[1] >= 1,
          str(sim.flash.erase_count))


def test_coupure_metadonnees():
    section("Coupure pendant l'ecriture des metadonnees")

    sim = BootloaderSim()
    sim.write_metadata(Metadata(fw_size=111, state=p.STATE_VALID))
    avant = sim.read_metadata()

    # Coupure apres un seul bloc de 8 octets
    try:
        sim.write_metadata(Metadata(fw_size=222, state=p.STATE_VALID),
                           stop_after_units=1)
        check("coupure simulee", False)
    except PowerLoss:
        check("coupure simulee", True)

    apres = sim.read_metadata()
    check("l'ancienne copie survit",
          apres is not None and apres.fw_size == avant.fw_size,
          f"{apres}")
    check("la copie partielle est ignoree", apres.counter == avant.counter)

    # Coupure pendant l'effacement
    sim2 = BootloaderSim()
    sim2.write_metadata(Metadata(fw_size=555, state=p.STATE_VALID))
    ref = sim2.read_metadata()
    try:
        sim2.write_metadata(Metadata(fw_size=666), interrupt_erase=True)
    except PowerLoss:
        pass
    survivant = sim2.read_metadata()
    check("survie a une coupure pendant l'effacement",
          survivant is not None and survivant.fw_size == ref.fw_size,
          f"{survivant}")


# ===============================================================
# Machine a etats
# ===============================================================
def _transfert(sim, firmware, slot=None, version=0x00010000):
    """Deroule un transfert complet. Retourne la derniere reponse."""
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_GET_INFO, 0))))
    info = p.InfoResponse.unpack(rep.data)
    cible = info.free_slot if slot is None else slot

    su = p.StartUpdate(len(firmware), crc32_stm32(firmware), version, cible)
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su.pack()))))
    if rep.cmd != p.RSP_ACK:
        return rep

    seq = 1
    for bloc in p.split_firmware(firmware):
        rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, seq, bloc))))
        if rep.cmd != p.RSP_ACK:
            return rep
        seq += 1

    return p.decode(sim.handle(p.encode(p.Frame(p.CMD_END_UPDATE, seq))))


def test_transfert_nominal():
    section("Transfert nominal")

    sim = BootloaderSim()
    fw = os.urandom(1000)

    rep = _transfert(sim, fw)
    check("transfert acquitte", rep.cmd == p.RSP_ACK, str(rep))

    meta = sim.read_metadata()
    check("etat TESTING apres transfert", meta.state == p.STATE_TESTING)
    check("slot bascule vers B", meta.active_slot == p.SLOT_B)
    check("taille memorisee", meta.fw_size == len(fw))

    ecrit = sim.flash.read_slot(p.SLOT_B, 0, len(fw))
    check("contenu flash identique au firmware", ecrit == fw)


def test_firmware_non_aligne():
    section("Firmware de taille non multiple du bloc")

    for taille in (1, 255, 256, 257, 1000):
        sim = BootloaderSim()
        fw = os.urandom(taille)
        rep = _transfert(sim, fw)
        ecrit = sim.flash.read_slot(p.SLOT_B, 0, taille)
        check(f"taille {taille} octets",
              rep.cmd == p.RSP_ACK and ecrit == fw)


def test_retransmission():
    section("Retransmission")

    sim = BootloaderSim()
    fw = os.urandom(600)

    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_GET_INFO, 0))))
    info = p.InfoResponse.unpack(rep.data)
    su = p.StartUpdate(len(fw), crc32_stm32(fw), 1, info.free_slot)
    sim.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su.pack())))

    blocs = list(p.split_firmware(fw))

    r1 = p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, 1, blocs[0]))))
    check("premiere trame acquittee", r1.cmd == p.RSP_ACK)

    # Meme trame renvoyee : l'ACK avait ete perdu
    r2 = p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, 1, blocs[0]))))
    check("retransmission acquittee sans reecriture", r2.cmd == p.RSP_ACK)

    # Le transfert doit pouvoir continuer normalement
    r3 = p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, 2, blocs[1]))))
    check("transfert poursuivi apres retransmission", r3.cmd == p.RSP_ACK)

    for i, bloc in enumerate(blocs[2:], start=3):
        p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, i, bloc))))

    fin = p.decode(sim.handle(p.encode(p.Frame(p.CMD_END_UPDATE, len(blocs) + 1))))
    check("CRC global correct malgre la retransmission",
          fin.cmd == p.RSP_ACK, str(fin))


def test_sequence_desordonnee():
    section("Sequence desordonnee")

    sim = BootloaderSim()
    fw = os.urandom(600)

    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_GET_INFO, 0))))
    info = p.InfoResponse.unpack(rep.data)
    su = p.StartUpdate(len(fw), crc32_stm32(fw), 1, info.free_slot)
    sim.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su.pack())))

    blocs = list(p.split_firmware(fw))
    sim.handle(p.encode(p.Frame(p.CMD_DATA, 1, blocs[0])))

    # On saute la trame 2
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_DATA, 3, blocs[2]))))
    check("trame hors sequence rejetee", rep.cmd == p.RSP_NACK)
    check("erreur ERR_SEQ signalee",
          rep.data == bytes([p.ERR_SEQ]),
          p.ERROR_NAMES.get(rep.data[0]))


def test_verifications_prealables():
    section("Verifications avant effacement")

    # Firmware trop volumineux
    sim = BootloaderSim()
    su = p.StartUpdate(SLOT_SIZE + 1, 0, 1, p.SLOT_B)
    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su.pack()))))
    check("firmware trop gros refuse", rep.data == bytes([p.ERR_SIZE]))
    check("slot non efface apres refus",
          all(b == 0xFF for b in sim.flash.slots[p.SLOT_B][:256]))

    # Mauvais slot cible : le scenario du mauvais binaire envoye
    sim2 = BootloaderSim()
    su2 = p.StartUpdate(1000, 0, 1, p.SLOT_A)   # A est actif, pas libre
    rep2 = p.decode(sim2.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su2.pack()))))
    check("mauvais slot refuse", rep2.data == bytes([p.ERR_SLOT]))

    # Commande hors etat
    sim3 = BootloaderSim()
    rep3 = p.decode(sim3.handle(p.encode(p.Frame(p.CMD_DATA, 1, b"x" * 16))))
    check("DATA sans START refuse", rep3.data == bytes([p.ERR_STATE]))


def test_crc_global_faux():
    section("CRC global invalide")

    sim = BootloaderSim()
    fw = os.urandom(600)

    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_GET_INFO, 0))))
    info = p.InfoResponse.unpack(rep.data)

    # CRC annonce volontairement faux
    su = p.StartUpdate(len(fw), 0xDEADBEEF, 1, info.free_slot)
    sim.handle(p.encode(p.Frame(p.CMD_START_UPDATE, 0, su.pack())))

    seq = 1
    for bloc in p.split_firmware(fw):
        sim.handle(p.encode(p.Frame(p.CMD_DATA, seq, bloc)))
        seq += 1

    rep = p.decode(sim.handle(p.encode(p.Frame(p.CMD_END_UPDATE, seq))))
    check("CRC global faux detecte", rep.cmd == p.RSP_NACK)
    check("erreur ERR_GLOBAL_CRC", rep.data == bytes([p.ERR_GLOBAL_CRC]))

    meta = sim.read_metadata()
    check("etat reste IN_PROGRESS", meta.state == p.STATE_IN_PROGRESS)


def test_ecriture_non_effacee():
    section("Ecriture sur zone non effacee")

    sim = BootloaderSim()
    ok1 = sim.flash.write_slot(p.SLOT_A, 0, b"\x01" * 8)
    ok2 = sim.flash.write_slot(p.SLOT_A, 0, b"\x02" * 8)

    check("premiere ecriture acceptee", ok1)
    check("reecriture sans effacement refusee", not ok2)


# ===============================================================
# Demarrage et rollback
# ===============================================================
def test_decision_demarrage():
    section("Decision au demarrage")

    sim = BootloaderSim()
    check("carte vierge -> reception", "reception" in sim.boot())

    sim.write_metadata(Metadata(fw_size=100, state=p.STATE_VALID,
                                active_slot=p.SLOT_A))
    check("etat VALID -> saut", "saut vers le slot A" in sim.boot())

    sim2 = BootloaderSim()
    sim2.write_metadata(Metadata(fw_size=100, state=p.STATE_IN_PROGRESS))
    check("transfert interrompu detecte", "interrompu" in sim2.boot())


def test_rollback():
    section("Rollback apres echecs repetes")

    sim = BootloaderSim()
    sim.write_metadata(Metadata(fw_size=100, state=p.STATE_TESTING,
                                active_slot=p.SLOT_B))

    for i in range(1, MAX_BOOT_FAILURES + 1):
        msg = sim.boot()
        check(f"tentative {i} : essai du slot B", "essai" in msg, msg)

    final = sim.boot()
    check("rollback declenche au-dela du seuil",
          "rollback" in final and "slot A" in final, final)


def test_confirmation_application():
    section("Confirmation applicative")

    sim = BootloaderSim()
    sim.write_metadata(Metadata(fw_size=100, state=p.STATE_TESTING,
                                active_slot=p.SLOT_B))
    sim.boot()

    # L'application confirme son bon demarrage
    meta = sim.read_metadata()
    meta.state = p.STATE_VALID
    meta.boot_fail_count = 0
    sim.write_metadata(meta)

    check("aucun rollback apres confirmation",
          "saut vers le slot B" in sim.boot())


# ===============================================================
def main():
    print("Tests du protocole de mise a jour firmware")
    print("=" * 50)

    test_encodage()
    test_champs_binaires()
    test_detection_erreurs()
    test_metadonnees()
    test_selection_page()
    test_coupure_metadonnees()
    test_transfert_nominal()
    test_firmware_non_aligne()
    test_retransmission()
    test_sequence_desordonnee()
    test_verifications_prealables()
    test_crc_global_faux()
    test_ecriture_non_effacee()
    test_decision_demarrage()
    test_rollback()
    test_confirmation_application()

    print("\n" + "=" * 50)
    total = _reussis + len(_echoues)
    print(f"{_reussis}/{total} tests reussis")
    if _echoues:
        print("\nEchecs :")
        for nom in _echoues:
            print(f"  - {nom}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
