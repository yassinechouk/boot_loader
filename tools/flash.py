#!/usr/bin/env python3
"""
Outil de mise a jour firmware par liaison serie.

    ./flash.py --port /dev/ttyACM0 --dir build/
    ./flash.py --port /dev/ttyACM0 --info
    ./flash.py --port /dev/ttyACM0 --file app_slotB.bin --slot B

Le binaire envoye depend du slot libre annonce par la carte : c'est
elle qui detient l'etat, pas l'outil. En mode --dir, le fichier est
choisi automatiquement parmi app_slotA.bin et app_slotB.bin.

Ce choix decoule de l'architecture dual-slot : l'application est liee
a une adresse fixe, donc compilee deux fois, une par emplacement. Voir
PROTOCOL.md pour les alternatives ecartees.
"""

import argparse
import os
import sys
import time

import protocol as p
from crc32 import crc32_stm32
from transport import SerialTransport, TransportError, Timeout


# ---------------------------------------------------------------
# Affichage
# ---------------------------------------------------------------
class Term:
    GRIS  = "\033[90m"
    VERT  = "\033[92m"
    ROUGE = "\033[91m"
    JAUNE = "\033[93m"
    BLEU  = "\033[94m"
    GRAS  = "\033[1m"
    FIN   = "\033[0m"

    actif = sys.stdout.isatty()

    @classmethod
    def c(cls, texte, couleur):
        return f"{couleur}{texte}{cls.FIN}" if cls.actif else texte


def info(msg):    print(f"  {msg}")
def succes(msg):  print(f"  {Term.c('OK', Term.VERT)}    {msg}")
def echec(msg):   print(f"  {Term.c('ECHEC', Term.ROUGE)} {msg}")
def etape(msg):   print(f"\n{Term.c(msg, Term.GRAS)}")


def barre(courant, total, largeur=40):
    frac = courant / total if total else 1.0
    plein = int(frac * largeur)
    trait = "#" * plein + "-" * (largeur - plein)
    pct = int(frac * 100)
    sys.stdout.write(f"\r  [{trait}] {pct:3}%  {courant}/{total} octets")
    sys.stdout.flush()


def duree_estimee(octets, baudrate=115200):
    """Chaque octet occupe 10 bits sur la ligne : start + 8 + stop."""
    trames = (octets + p.DATA_BLOCK_SIZE - 1) // p.DATA_BLOCK_SIZE
    total = octets + trames * p.FRAME_OVERHEAD
    return total * 10 / baudrate


# ---------------------------------------------------------------
# Operations
# ---------------------------------------------------------------
def lire_info(tr) -> p.InfoResponse:
    rep = tr.exchange(p.Frame(p.CMD_GET_INFO, 0))

    if rep.cmd == p.RSP_NACK:
        code = rep.data[0] if rep.data else 0
        raise TransportError(f"GET_INFO refuse : {p.ERROR_NAMES.get(code, code)}")

    if rep.cmd != p.RSP_INFO:
        raise TransportError(f"reponse inattendue : {rep}")

    return p.InfoResponse.unpack(rep.data)


def afficher_info(nfo: p.InfoResponse):
    def version(v):
        return f"{(v >> 16) & 0xFF}.{(v >> 8) & 0xFF}.{v & 0xFF}"

    info(f"protocole      : v{nfo.proto_version}")
    info(f"bootloader     : v{version(nfo.bl_version)}")
    info(f"firmware actif : v{version(nfo.fw_version)}")
    info(f"slot actif     : {'AB'[nfo.active_slot]}")
    info(f"slot libre     : {Term.c('AB'[nfo.free_slot], Term.BLEU)}")
    info(f"etat           : {p.STATE_NAMES.get(nfo.state, nfo.state)}")


def choisir_binaire(dossier: str, slot: int) -> str:
    """
    Selectionne le binaire correspondant au slot libre.

    Le firmware est lie a une adresse fixe : app_slotA.bin ne
    fonctionne qu'a l'adresse du slot A. Envoyer le mauvais produirait
    une image au CRC parfaitement valide mais inexecutable — cas que
    le bootloader rattrape via l'etat TESTING, mais qu'on evite ici.
    """
    nom = f"app_slot{'AB'[slot]}.bin"
    chemin = os.path.join(dossier, nom)

    if not os.path.isfile(chemin):
        raise SystemExit(
            f"{chemin} introuvable.\n"
            f"L'architecture dual-slot exige deux binaires, un par\n"
            f"emplacement. Verifiez que le Makefile de l'application\n"
            f"produit app_slotA.bin et app_slotB.bin."
        )
    return chemin


def envoyer(tr, firmware: bytes, slot: int, version: int,
            baudrate: int) -> bool:
    crc = crc32_stm32(firmware)
    blocs = list(p.split_firmware(firmware))

    etape("Transfert")
    info(f"taille   : {len(firmware)} octets")
    info(f"CRC32    : 0x{crc:08X}")
    info(f"blocs    : {len(blocs)} x {p.DATA_BLOCK_SIZE}")
    info(f"cible    : slot {'AB'[slot]}")
    info(f"estime   : {duree_estimee(len(firmware), baudrate):.1f} s")
    print()

    # --- annonce ---
    su = p.StartUpdate(len(firmware), crc, version, slot)
    rep = tr.exchange(p.Frame(p.CMD_START_UPDATE, 0, su.pack()))

    if rep.cmd == p.RSP_NACK:
        code = rep.data[0] if rep.data else 0
        echec(f"transfert refuse : {p.ERROR_NAMES.get(code, code)}")
        if code == p.ERR_SLOT:
            info("le binaire ne correspond pas au slot annonce par la carte")
        elif code == p.ERR_SIZE:
            info("firmware trop volumineux pour le slot")
        return False

    # --- blocs ---
    debut = time.time()
    envoyes = 0
    seq = 1

    for bloc in blocs:
        try:
            rep = tr.exchange(p.Frame(p.CMD_DATA, seq, bloc))
        except TransportError as e:
            print()
            echec(f"bloc {seq} : {e}")
            return False

        if rep.cmd == p.RSP_NACK:
            print()
            code = rep.data[0] if rep.data else 0
            echec(f"bloc {seq} refuse : {p.ERROR_NAMES.get(code, code)}")
            return False

        envoyes += len(bloc)
        seq += 1
        barre(envoyes, len(firmware))

    print()
    ecoule = time.time() - debut
    debit = len(firmware) / ecoule if ecoule > 0 else 0
    info(f"transmis en {ecoule:.1f} s ({debit:.0f} o/s)")

    # --- cloture ---
    etape("Verification")
    info("relecture de la flash et calcul du CRC global...")

    try:
        rep = tr.exchange(p.Frame(p.CMD_END_UPDATE, seq), retries=1)
    except Timeout:
        # La verification relit tout le slot ; sur un gros firmware
        # cela depasse le timeout ordinaire.
        rep = tr.receive(timeout=10.0)

    if rep.cmd == p.RSP_NACK:
        code = rep.data[0] if rep.data else 0
        echec(f"verification echouee : {p.ERROR_NAMES.get(code, code)}")
        if code == p.ERR_GLOBAL_CRC:
            info("le contenu relu differe du firmware envoye")
        return False

    succes("CRC global verifie")
    succes("image marquee TESTING")
    info("la carte redemarre ; l'application doit se confirmer")
    return True


# ---------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="Mise a jour firmware par liaison serie",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)

    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--dir", help="dossier contenant app_slotA.bin et app_slotB.bin")
    ap.add_argument("--file", help="binaire explicite")
    ap.add_argument("--slot", choices=["A", "B"],
                    help="forcer le slot cible (avec --file)")
    ap.add_argument("--version", default="0.1.0",
                    help="version du firmware, format M.m.p")
    ap.add_argument("--info", action="store_true",
                    help="interroger la carte sans rien envoyer")
    ap.add_argument("--verbose", action="store_true")

    args = ap.parse_args()

    if not args.info and not args.dir and not args.file:
        ap.error("indiquez --dir, --file ou --info")

    try:
        maj, mnr, pch = (int(x) for x in args.version.split("."))
        version = (maj << 16) | (mnr << 8) | pch
    except ValueError:
        ap.error("version attendue au format M.m.p")

    print(Term.c("\nMise a jour firmware", Term.GRAS))
    print(f"  port {args.port} @ {args.baud} bauds")

    try:
        with SerialTransport(args.port, args.baud, verbose=args.verbose) as tr:

            etape("Etat de la carte")
            nfo = lire_info(tr)
            afficher_info(nfo)

            if nfo.proto_version != p.PROTO_VERSION:
                echec(f"protocole v{nfo.proto_version} cote carte, "
                      f"v{p.PROTO_VERSION} cote outil")
                return 1

            if args.info:
                print()
                return 0

            # --- choix du binaire ---
            if args.file:
                chemin = args.file
                slot = nfo.free_slot
                if args.slot:
                    slot = 0 if args.slot == "A" else 1
                    if slot != nfo.free_slot:
                        echec(f"la carte attend le slot "
                              f"{'AB'[nfo.free_slot]}, pas {args.slot}")
                        return 1
            else:
                chemin = choisir_binaire(args.dir, nfo.free_slot)
                slot = nfo.free_slot

            with open(chemin, "rb") as f:
                firmware = f.read()

            if not firmware:
                echec(f"{chemin} est vide")
                return 1

            info(f"fichier        : {os.path.basename(chemin)}")

            # --- alignement ---
            reste = len(firmware) % 8
            if reste:
                # La flash ne se programme que par double-mot. Un
                # firmware non aligne est complete avec 0xFF, valeur
                # d'une cellule effacee, donc neutre.
                comble = 8 - reste
                firmware += b"\xFF" * comble
                info(f"complete de {comble} octets (alignement 64 bits)")

            if not envoyer(tr, firmware, slot, version, args.baud):
                print()
                return 1

            print()
            return 0

    except KeyboardInterrupt:
        print("\n\n  interrompu")
        return 130
    except TransportError as e:
        print()
        echec(str(e))
        info("verifiez que la carte est en mode reception "
             "et qu'aucun terminal ne retient le port")
        return 1


if __name__ == "__main__":
    sys.exit(main())
