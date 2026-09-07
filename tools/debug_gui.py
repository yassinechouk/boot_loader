"""
Interface de debug du protocole de mise a jour firmware.

Pilote le simulateur reel (bootloader_sim.BootloaderSim) : aucune
logique de protocole n'est reimplementee ici. L'interface ne fait
qu'observer et declencher. Si la machine a etats change, l'affichage
suit automatiquement.

Lancement :
    python3 debug_gui.py

Aucune dependance externe : tkinter est fourni avec Python.
"""

import os
import tkinter as tk
from tkinter import ttk, filedialog

import protocol as p
from crc32 import crc32_stm32
from bootloader_sim import BootloaderSim, SLOT_SIZE, PowerLoss

# ---------------------------------------------------------------
# Palette
# ---------------------------------------------------------------
BG        = "#1e1e2e"
PANEL     = "#282a36"
BORDER    = "#44475a"
FG        = "#f8f8f2"
DIM       = "#6272a4"
PC_COLOR  = "#8be9fd"
BL_COLOR  = "#50fa7b"
ACK       = "#50fa7b"
NACK      = "#ff5555"
FRAME_C   = "#f1fa8c"
ACCENT    = "#bd93f9"

MONO = ("DejaVu Sans Mono", 9)
MONO_B = ("DejaVu Sans Mono", 10, "bold")
TITLE = ("DejaVu Sans", 11, "bold")


class DebugGUI:
    def __init__(self, root):
        self.root = root
        root.title("Bootloader — visualisation du protocole")
        root.configure(bg=BG)
        root.geometry("1180x760")

        self.sim = BootloaderSim()
        self.firmware = os.urandom(1200)
        self.etapes = []
        self.index = 0
        self.auto = False

        self._build()
        self._preparer_etapes()
        self._refresh()

    # -----------------------------------------------------------
    # Construction de l'interface
    # -----------------------------------------------------------
    def _build(self):
        # --- barre de controle ---
        barre = tk.Frame(self.root, bg=BG)
        barre.pack(fill="x", padx=12, pady=(12, 6))

        self.btn_step = tk.Button(barre, text="Etape suivante",
                                  command=self.etape,
                                  bg=ACCENT, fg=BG, font=MONO_B,
                                  relief="flat", padx=14, pady=6)
        self.btn_step.pack(side="left")

        self.btn_auto = tk.Button(barre, text="Lecture auto",
                                  command=self.toggle_auto,
                                  bg=PANEL, fg=FG, font=MONO_B,
                                  relief="flat", padx=14, pady=6)
        self.btn_auto.pack(side="left", padx=6)

        tk.Button(barre, text="Reinitialiser", command=self.reset,
                  bg=PANEL, fg=FG, font=MONO_B,
                  relief="flat", padx=14, pady=6).pack(side="left")

        tk.Button(barre, text="Charger un .bin", command=self.charger,
                  bg=PANEL, fg=FG, font=MONO_B,
                  relief="flat", padx=14, pady=6).pack(side="left", padx=6)

        # injection de fautes
        tk.Label(barre, text="   Injecter :", bg=BG, fg=DIM,
                 font=MONO).pack(side="left")

        self.corrompre = tk.BooleanVar()
        tk.Checkbutton(barre, text="corruption CRC", variable=self.corrompre,
                       bg=BG, fg=NACK, selectcolor=PANEL, font=MONO,
                       activebackground=BG, activeforeground=NACK).pack(side="left")

        self.rejouer = tk.BooleanVar()
        tk.Checkbutton(barre, text="retransmission", variable=self.rejouer,
                       bg=BG, fg=FRAME_C, selectcolor=PANEL, font=MONO,
                       activebackground=BG, activeforeground=FRAME_C).pack(side="left")

        self.progress = tk.Label(barre, text="", bg=BG, fg=DIM, font=MONO)
        self.progress.pack(side="right")

        # --- zone principale : PC | flux | bootloader ---
        centre = tk.Frame(self.root, bg=BG)
        centre.pack(fill="both", expand=True, padx=12, pady=6)

        self.pc_panel = self._panneau(centre, "PC  (outil de flash)", PC_COLOR)
        self.pc_panel.pack(side="left", fill="both", expand=True)

        milieu = tk.Frame(centre, bg=BG, width=300)
        milieu.pack(side="left", fill="both", padx=10)
        milieu.pack_propagate(False)

        self.canvas = tk.Canvas(milieu, bg=BG, height=110,
                                highlightthickness=0)
        self.canvas.pack(fill="x", pady=(28, 6))

        self.hex_box = tk.Text(milieu, bg=PANEL, fg=FRAME_C, font=MONO,
                               height=14, relief="flat", wrap="word",
                               padx=8, pady=8)
        self.hex_box.pack(fill="both", expand=True)

        self.bl_panel = self._panneau(centre, "STM32  (bootloader)", BL_COLOR)
        self.bl_panel.pack(side="left", fill="both", expand=True)

        # --- flash ---
        flash_zone = tk.Frame(self.root, bg=BG)
        flash_zone.pack(fill="x", padx=12, pady=4)

        tk.Label(flash_zone, text="Flash", bg=BG, fg=DIM,
                 font=TITLE).pack(anchor="w")

        self.flash_canvas = tk.Canvas(flash_zone, bg=PANEL, height=76,
                                      highlightthickness=1,
                                      highlightbackground=BORDER)
        self.flash_canvas.pack(fill="x")

        # --- journal ---
        tk.Label(self.root, text="Journal", bg=BG, fg=DIM,
                 font=TITLE).pack(anchor="w", padx=12, pady=(6, 0))

        self.log = tk.Text(self.root, bg=PANEL, fg=FG, font=MONO,
                           height=7, relief="flat", padx=8, pady=6)
        self.log.pack(fill="x", padx=12, pady=(0, 12))
        self.log.tag_config("ack", foreground=ACK)
        self.log.tag_config("nack", foreground=NACK)
        self.log.tag_config("info", foreground=DIM)
        self.log.tag_config("pc", foreground=PC_COLOR)

    def _panneau(self, parent, titre, couleur):
        cadre = tk.Frame(parent, bg=PANEL, highlightthickness=1,
                         highlightbackground=BORDER)
        tk.Label(cadre, text=titre, bg=PANEL, fg=couleur,
                 font=TITLE).pack(anchor="w", padx=12, pady=(10, 4))
        cadre.corps = tk.Text(cadre, bg=PANEL, fg=FG, font=MONO,
                              relief="flat", padx=12, pady=4, wrap="word")
        cadre.corps.pack(fill="both", expand=True)
        cadre.corps.tag_config("cle", foreground=DIM)
        cadre.corps.tag_config("val", foreground=FG)
        cadre.corps.tag_config("hi", foreground=ACCENT)
        return cadre

    # -----------------------------------------------------------
    # Scenario
    # -----------------------------------------------------------
    def _preparer_etapes(self):
        """Construit la liste des trames que le PC va emettre."""
        self.etapes = [("GET_INFO", p.Frame(p.CMD_GET_INFO, 0))]
        self.fw_crc = crc32_stm32(self.firmware)
        self.blocs = list(p.split_firmware(self.firmware))
        # START_UPDATE et les DATA sont construits a la volee :
        # le slot cible depend de la reponse a GET_INFO.
        self.phase = "info"
        self.seq = 0
        self.bloc_index = 0
        self.derniere_trame = None
        self.derniere_reponse = None
        self.cible = None

    def _trame_suivante(self):
        """Retourne la prochaine trame a emettre, ou None si termine."""
        if self.phase == "info":
            return p.Frame(p.CMD_GET_INFO, 0)

        if self.phase == "start":
            su = p.StartUpdate(len(self.firmware), self.fw_crc,
                               0x00010000, self.cible)
            return p.Frame(p.CMD_START_UPDATE, 0, su.pack())

        if self.phase == "data":
            if self.bloc_index >= len(self.blocs):
                return None
            # Retransmission volontaire : on renvoie la trame precedente
            if self.rejouer.get() and self.bloc_index > 0:
                self.rejouer.set(False)
                return p.Frame(p.CMD_DATA, self.seq,
                               self.blocs[self.bloc_index - 1])
            return p.Frame(p.CMD_DATA, self.seq + 1,
                           self.blocs[self.bloc_index])

        if self.phase == "end":
            return p.Frame(p.CMD_END_UPDATE, self.seq + 1)

        return None

    # -----------------------------------------------------------
    # Execution d'une etape
    # -----------------------------------------------------------
    def etape(self):
        trame = self._trame_suivante()
        if trame is None:
            self._journal("Transfert termine.", "info")
            self._journal(f"Au redemarrage : {self.sim.boot()}", "info")
            self.auto = False
            self.btn_auto.config(text="Lecture auto")
            return

        octets = bytearray(p.encode(trame))

        # Injection de faute : on inverse un octet du payload
        corrompue = False
        if self.corrompre.get() and len(octets) > p.FRAME_HEADER_SIZE + 2:
            octets[p.FRAME_HEADER_SIZE + 1] ^= 0xFF
            corrompue = True
            self.corrompre.set(False)

        self.derniere_trame = trame
        self._journal(f"PC  -> {trame}"
                      + ("   [octet corrompu injecte]" if corrompue else ""),
                      "pc")

        try:
            brut = self.sim.handle(bytes(octets))
        except PowerLoss as e:
            self._journal(f"COUPURE : {e}", "nack")
            self._refresh()
            return

        if not brut:
            self._journal("BL  -> (aucune reponse, trame ignoree)", "info")
            self.derniere_reponse = None
            self._refresh()
            return

        reponse = p.decode(brut)
        self.derniere_reponse = reponse

        if reponse.cmd == p.RSP_NACK:
            err = p.ERROR_NAMES.get(reponse.data[0], reponse.data[0])
            self._journal(f"BL  -> NACK  {err}", "nack")
        else:
            nom = p.CMD_NAMES.get(reponse.cmd, hex(reponse.cmd))
            self._journal(f"BL  -> {nom}  seq={reponse.seq}", "ack")

        self._avancer(reponse, corrompue)
        self._refresh()

    def _avancer(self, reponse, corrompue):
        """Fait progresser la machine a etats du PC."""
        if reponse.cmd == p.RSP_NACK:
            return      # on reste sur place, le PC retransmettra

        if self.phase == "info":
            info = p.InfoResponse.unpack(reponse.data)
            self.cible = info.free_slot
            self._journal(
                f"    slot libre = {'AB'[self.cible]}, "
                f"envoi de app_slot{'AB'[self.cible]}.bin", "info")
            self.phase = "start"

        elif self.phase == "start":
            self.seq = 0
            self.phase = "data"

        elif self.phase == "data":
            if not corrompue:
                self.seq = reponse.seq
                # une retransmission ne fait pas avancer l'index
                if reponse.seq > self.bloc_index:
                    self.bloc_index += 1
            if self.bloc_index >= len(self.blocs):
                self.phase = "end"

        elif self.phase == "end":
            self.phase = "fini"

    # -----------------------------------------------------------
    # Rafraichissement de l'affichage
    # -----------------------------------------------------------
    def _refresh(self):
        self._maj_pc()
        self._maj_bl()
        self._maj_flux()
        self._maj_flash()

        total = len(self.blocs)
        self.progress.config(
            text=f"bloc {min(self.bloc_index, total)}/{total}   "
                 f"phase : {self.phase}")

    def _ligne(self, widget, cle, val, tag="val"):
        widget.insert("end", f"{cle:<18}", "cle")
        widget.insert("end", f"{val}\n", tag)

    def _maj_pc(self):
        t = self.pc_panel.corps
        t.config(state="normal")
        t.delete("1.0", "end")

        self._ligne(t, "firmware", f"{len(self.firmware)} octets")
        self._ligne(t, "CRC global", f"0x{self.fw_crc:08X}")
        self._ligne(t, "blocs", f"{len(self.blocs)} x {p.DATA_BLOCK_SIZE}")
        t.insert("end", "\n")
        self._ligne(t, "phase", self.phase, "hi")
        self._ligne(t, "prochain seq", self.seq + 1)
        self._ligne(t, "bloc courant", f"{self.bloc_index}/{len(self.blocs)}")
        if self.cible is not None:
            self._ligne(t, "slot cible", "AB"[self.cible], "hi")

        t.insert("end", "\n")
        restants = len(self.blocs) - self.bloc_index
        octets_restants = restants * p.DATA_BLOCK_SIZE
        duree = octets_restants * 10 / 115200
        self._ligne(t, "reste a envoyer", f"{octets_restants} octets")
        self._ligne(t, "duree estimee", f"{duree:.2f} s @115200")

        t.config(state="disabled")

    def _maj_bl(self):
        t = self.bl_panel.corps
        t.config(state="normal")
        t.delete("1.0", "end")

        self._ligne(t, "etat interne", self.sim.state, "hi")
        self._ligne(t, "seq attendu", self.sim.expected_seq)
        self._ligne(t, "dernier seq", self.sim.last_seq)
        if self.sim.target_slot is not None:
            self._ligne(t, "slot en ecriture", "AB"[self.sim.target_slot])
        self._ligne(t, "octets recus", self.sim.bytes_received)

        t.insert("end", "\nMetadonnees\n", "cle")
        meta = self.sim.read_metadata()
        if meta is None:
            t.insert("end", "  aucune copie valide\n", "val")
        else:
            self._ligne(t, "  compteur", meta.counter)
            self._ligne(t, "  slot actif", "AB"[meta.active_slot], "hi")
            self._ligne(t, "  etat",
                        p.STATE_NAMES.get(meta.state, meta.state), "hi")
            self._ligne(t, "  taille", meta.fw_size)
            self._ligne(t, "  echecs boot", meta.boot_fail_count)

        t.insert("end", "\nPages de metadonnees\n", "cle")
        for pg in (0, 1):
            from bootloader_sim import Metadata
            m = Metadata.unpack(self.sim.flash.read_meta_page(pg))
            if m is None:
                etat = "vierge ou invalide"
            else:
                etat = f"counter={m.counter}"
                if meta and m.counter == meta.counter:
                    etat += "   <- fait foi"
            self._ligne(t, f"  page {pg}", etat)

        t.config(state="disabled")

    def _maj_flux(self):
        c = self.canvas
        c.delete("all")
        w = c.winfo_width() or 280

        # fleche descendante PC -> BL
        c.create_line(20, 20, w - 20, 20, fill=DIM, width=2,
                      arrow="last", arrowshape=(12, 14, 5))
        if self.derniere_trame:
            nom = p.CMD_NAMES.get(self.derniere_trame.cmd, "?")
            c.create_text(w / 2, 8, text=nom, fill=PC_COLOR, font=MONO)

        # fleche remontante BL -> PC
        c.create_line(w - 20, 70, 20, 70, fill=DIM, width=2,
                      arrow="last", arrowshape=(12, 14, 5))
        if self.derniere_reponse:
            nom = p.CMD_NAMES.get(self.derniere_reponse.cmd, "?")
            coul = NACK if self.derniere_reponse.cmd == p.RSP_NACK else ACK
            c.create_text(w / 2, 58, text=nom, fill=coul, font=MONO)

        # dump hexadecimal de la derniere trame emise
        self.hex_box.config(state="normal")
        self.hex_box.delete("1.0", "end")
        if self.derniere_trame:
            brut = p.encode(self.derniere_trame)
            self.hex_box.insert("end", "Trame emise\n\n")
            self.hex_box.insert("end", f"MAGIC   {brut[0:2].hex(' ')}\n")
            self.hex_box.insert("end", f"CMD     {brut[2]:02x}\n")
            self.hex_box.insert("end", f"LENGTH  {brut[3:5].hex(' ')}"
                                       f"   ({len(self.derniere_trame.data)})\n")
            self.hex_box.insert("end", f"SEQ     {brut[5:7].hex(' ')}"
                                       f"   ({self.derniere_trame.seq})\n")
            corps = brut[7:len(brut) - 4]
            apercu = corps[:24].hex(' ')
            suite = " ..." if len(corps) > 24 else ""
            self.hex_box.insert("end", f"DATA    {apercu}{suite}\n")
            self.hex_box.insert("end", f"CRC32   {brut[-4:].hex(' ')}\n")
            self.hex_box.insert("end", f"\ntotal   {len(brut)} octets")
        self.hex_box.config(state="disabled")

    def _maj_flash(self):
        c = self.flash_canvas
        c.delete("all")
        w = c.winfo_width() or 1100
        if w < 100:
            self.root.after(50, self._maj_flash)
            return

        meta = self.sim.read_metadata()
        actif = meta.active_slot if meta else None

        for i, slot in enumerate((p.SLOT_A, p.SLOT_B)):
            y = 8 + i * 32
            c.create_text(24, y + 10, text=f"slot {'AB'[slot]}",
                          fill=DIM, font=MONO)

            x0, x1 = 70, w - 90
            largeur = x1 - x0
            c.create_rectangle(x0, y, x1, y + 20,
                               fill=BG, outline=BORDER)

            # proportion ecrite : on echantillonne pour rester rapide
            donnees = self.sim.flash.slots[slot]
            taille_utile = max(len(self.firmware), 1)
            ecrits = sum(1 for k in range(0, taille_utile, 64)
                         if donnees[k] != 0xFF)
            total_ech = max(1, len(range(0, taille_utile, 64)))
            frac = ecrits / total_ech

            if frac > 0:
                coul = BL_COLOR if slot == actif else ACCENT
                c.create_rectangle(x0, y, x0 + largeur * frac, y + 20,
                                   fill=coul, outline="")

            etiquette = f"{frac * 100:.0f}%"
            if slot == actif:
                etiquette += "  actif"
            c.create_text(x1 + 42, y + 10, text=etiquette,
                          fill=FG, font=MONO)

    # -----------------------------------------------------------
    # Commandes
    # -----------------------------------------------------------
    def toggle_auto(self):
        self.auto = not self.auto
        self.btn_auto.config(text="Pause" if self.auto else "Lecture auto")
        if self.auto:
            self._boucle()

    def _boucle(self):
        if not self.auto:
            return
        self.etape()
        if self.phase != "fini":
            self.root.after(220, self._boucle)
        else:
            self.auto = False
            self.btn_auto.config(text="Lecture auto")

    def reset(self):
        self.sim = BootloaderSim()
        self._preparer_etapes()
        self.log.delete("1.0", "end")
        self._journal("Simulateur reinitialise.", "info")
        self._refresh()

    def charger(self):
        chemin = filedialog.askopenfilename(
            title="Choisir un firmware",
            filetypes=[("Binaire", "*.bin"), ("Tous", "*.*")])
        if not chemin:
            return
        with open(chemin, "rb") as f:
            self.firmware = f.read()
        self.reset()
        self._journal(f"Charge : {os.path.basename(chemin)} "
                      f"({len(self.firmware)} octets)", "info")

    def _journal(self, texte, tag="info"):
        self.log.insert("end", texte + "\n", tag)
        self.log.see("end")


if __name__ == "__main__":
    root = tk.Tk()
    app = DebugGUI(root)
    root.after(120, app._refresh)     # une fois les tailles connues
    root.mainloop()
