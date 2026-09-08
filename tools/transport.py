"""
Transport serie pour le protocole de mise a jour.

Cette couche est le seul point du code PC qui connait le port serie.
protocol.py encode et decode des trames sans savoir par ou elles
passent ; un portage sur CAN ou sur socket TCP ne toucherait que ce
fichier.

Necessite pyserial :
    pip install pyserial --break-system-packages
"""

import time

try:
    import serial
except ImportError:
    raise SystemExit(
        "pyserial est requis :\n"
        "    pip install pyserial --break-system-packages"
    )

import protocol as p


class TransportError(Exception):
    pass


class Timeout(TransportError):
    pass


class SerialTransport:
    """
    Envoie une trame, attend la reponse, la decode.

    La lecture se fait en deux temps : l'en-tete de 7 octets d'abord,
    dont on tire la longueur du payload, puis le reste. C'est le meme
    raisonnement que cote firmware — on ne peut pas savoir combien
    d'octets attendre avant d'avoir lu LENGTH.
    """

    def __init__(self, port: str, baudrate: int = 115200,
                 timeout: float = 1.0, verbose: bool = False):
        self.verbose = verbose
        self.timeout = timeout
        try:
            self.ser = serial.Serial(port, baudrate, timeout=timeout)
        except serial.SerialException as e:
            raise TransportError(f"ouverture de {port} impossible : {e}")

        # Le ST-LINK peut avoir des octets en attente d'une session
        # precedente. On repart propre.
        time.sleep(0.05)
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    # ------------------------------------------------------------
    def _log(self, msg: str):
        if self.verbose:
            print(f"    [ser] {msg}")

    def send(self, frame: p.Frame):
        raw = p.encode(frame)
        self._log(f"-> {frame}  ({len(raw)} octets)")
        self.ser.write(raw)
        self.ser.flush()

    def receive(self, timeout: float = None) -> p.Frame:
        """
        Lit une trame complete. Leve Timeout si rien n'arrive.

        Se resynchronise sur le magic : des octets parasites ou une
        reponse tronquee ne bloquent pas definitivement.
        """
        limite = time.time() + (timeout if timeout is not None else self.timeout)

        # --- chercher le magic ---
        fenetre = b""
        while time.time() < limite:
            octet = self.ser.read(1)
            if not octet:
                continue
            fenetre = (fenetre + octet)[-2:]
            if fenetre == p.MAGIC:
                break
        else:
            raise Timeout("aucun preambule recu")

        # --- lire le reste de l'en-tete ---
        reste = self._read_exact(p.FRAME_HEADER_SIZE - 2, limite)
        entete = p.MAGIC + reste

        length = int.from_bytes(entete[3:5], "little")
        if length > p.MAX_PAYLOAD_SIZE:
            raise TransportError(f"LENGTH aberrant : {length}")

        # --- payload et CRC ---
        suite = self._read_exact(length + p.FRAME_CRC_SIZE, limite)

        frame = p.decode(entete + suite)
        self._log(f"<- {frame}")
        return frame

    def _read_exact(self, n: int, limite: float) -> bytes:
        buf = b""
        while len(buf) < n:
            if time.time() > limite:
                raise Timeout(f"{len(buf)}/{n} octets recus")
            morceau = self.ser.read(n - len(buf))
            if morceau:
                buf += morceau
        return buf

    # ------------------------------------------------------------
    def exchange(self, frame: p.Frame, retries: int = 3) -> p.Frame:
        """
        Envoie et attend la reponse, en retransmettant si necessaire.

        Une retransmission est inoffensive cote bootloader : recevoir
        deux fois la meme trame produit le meme resultat que la
        recevoir une fois. Cette propriete — l'idempotence — est ce
        qui rend cette boucle sure.
        """
        derniere = None

        for essai in range(retries):
            try:
                self.send(frame)
                return self.receive()
            except (Timeout, p.BadCRC, p.BadMagic) as e:
                derniere = e
                if essai < retries - 1:
                    self._log(f"echec ({e}), nouvel essai")
                    self.ser.reset_input_buffer()
                    time.sleep(0.05)

        raise TransportError(
            f"aucune reponse apres {retries} tentatives : {derniere}"
        )
