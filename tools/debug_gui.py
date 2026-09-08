"""
Debug interface for the firmware update protocol.

Drives the real simulator (bootloader_sim.BootloaderSim): no protocol
logic is reimplemented here. The interface only observes and triggers.
If the state machine changes, the display follows automatically.

Launch:
    python3 debug_gui.py

No external dependencies: tkinter is bundled with Python.
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

MONO   = ("DejaVu Sans Mono", 9)
MONO_B = ("DejaVu Sans Mono", 10, "bold")
TITLE  = ("DejaVu Sans", 11, "bold")


class DebugGUI:
    def __init__(self, root):
        self.root = root
        root.title("Bootloader — protocol visualisation")
        root.configure(bg=BG)
        root.geometry("1180x760")

        self.sim = BootloaderSim()
        self.firmware = os.urandom(1200)
        self.steps = []
        self.index = 0
        self.auto = False

        self._build()
        self._prepare_steps()
        self._refresh()

    # -----------------------------------------------------------
    # Interface construction
    # -----------------------------------------------------------
    def _build(self):
        # --- control bar ---
        bar = tk.Frame(self.root, bg=BG)
        bar.pack(fill="x", padx=12, pady=(12, 6))

        self.btn_step = tk.Button(bar, text="Next step",
                                  command=self.step,
                                  bg=ACCENT, fg=BG, font=MONO_B,
                                  relief="flat", padx=14, pady=6)
        self.btn_step.pack(side="left")

        self.btn_auto = tk.Button(bar, text="Auto play",
                                  command=self.toggle_auto,
                                  bg=PANEL, fg=FG, font=MONO_B,
                                  relief="flat", padx=14, pady=6)
        self.btn_auto.pack(side="left", padx=6)

        tk.Button(bar, text="Reset", command=self.reset,
                  bg=PANEL, fg=FG, font=MONO_B,
                  relief="flat", padx=14, pady=6).pack(side="left")

        tk.Button(bar, text="Load .bin", command=self.load_file,
                  bg=PANEL, fg=FG, font=MONO_B,
                  relief="flat", padx=14, pady=6).pack(side="left", padx=6)

        # fault injection
        tk.Label(bar, text="   Inject:", bg=BG, fg=DIM,
                 font=MONO).pack(side="left")

        self.corrupt = tk.BooleanVar()
        tk.Checkbutton(bar, text="CRC corruption", variable=self.corrupt,
                       bg=BG, fg=NACK, selectcolor=PANEL, font=MONO,
                       activebackground=BG, activeforeground=NACK).pack(side="left")

        self.replay = tk.BooleanVar()
        tk.Checkbutton(bar, text="retransmission", variable=self.replay,
                       bg=BG, fg=FRAME_C, selectcolor=PANEL, font=MONO,
                       activebackground=BG, activeforeground=FRAME_C).pack(side="left")

        self.progress = tk.Label(bar, text="", bg=BG, fg=DIM, font=MONO)
        self.progress.pack(side="right")

        # --- main area: PC | flow | bootloader ---
        centre = tk.Frame(self.root, bg=BG)
        centre.pack(fill="both", expand=True, padx=12, pady=6)

        self.pc_panel = self._panel(centre, "PC  (flash tool)", PC_COLOR)
        self.pc_panel.pack(side="left", fill="both", expand=True)

        middle = tk.Frame(centre, bg=BG, width=300)
        middle.pack(side="left", fill="both", padx=10)
        middle.pack_propagate(False)

        self.canvas = tk.Canvas(middle, bg=BG, height=110,
                                highlightthickness=0)
        self.canvas.pack(fill="x", pady=(28, 6))

        self.hex_box = tk.Text(middle, bg=PANEL, fg=FRAME_C, font=MONO,
                               height=14, relief="flat", wrap="word",
                               padx=8, pady=8)
        self.hex_box.pack(fill="both", expand=True)

        self.bl_panel = self._panel(centre, "STM32  (bootloader)", BL_COLOR)
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

        # --- log ---
        tk.Label(self.root, text="Log", bg=BG, fg=DIM,
                 font=TITLE).pack(anchor="w", padx=12, pady=(6, 0))

        self.log = tk.Text(self.root, bg=PANEL, fg=FG, font=MONO,
                           height=7, relief="flat", padx=8, pady=6)
        self.log.pack(fill="x", padx=12, pady=(0, 12))
        self.log.tag_config("ack", foreground=ACK)
        self.log.tag_config("nack", foreground=NACK)
        self.log.tag_config("info", foreground=DIM)
        self.log.tag_config("pc", foreground=PC_COLOR)

    def _panel(self, parent, title, color):
        frame = tk.Frame(parent, bg=PANEL, highlightthickness=1,
                         highlightbackground=BORDER)
        tk.Label(frame, text=title, bg=PANEL, fg=color,
                 font=TITLE).pack(anchor="w", padx=12, pady=(10, 4))
        frame.body = tk.Text(frame, bg=PANEL, fg=FG, font=MONO,
                             relief="flat", padx=12, pady=4, wrap="word")
        frame.body.pack(fill="both", expand=True)
        frame.body.tag_config("key", foreground=DIM)
        frame.body.tag_config("val", foreground=FG)
        frame.body.tag_config("hi", foreground=ACCENT)
        return frame

    # -----------------------------------------------------------
    # Scenario
    # -----------------------------------------------------------
    def _prepare_steps(self):
        """Builds the list of frames the PC will send."""
        self.steps = [("GET_INFO", p.Frame(p.CMD_GET_INFO, 0))]
        self.fw_crc = crc32_stm32(self.firmware)
        self.blocks = list(p.split_firmware(self.firmware))
        # START_UPDATE and DATA frames are built on the fly:
        # the target slot depends on the GET_INFO response.
        self.phase = "info"
        self.seq = 0
        self.block_index = 0
        self.last_frame = None
        self.last_response = None
        self.target = None

    def _next_frame(self):
        """Returns the next frame to send, or None if done."""
        if self.phase == "info":
            return p.Frame(p.CMD_GET_INFO, 0)

        if self.phase == "start":
            su = p.StartUpdate(len(self.firmware), self.fw_crc,
                               0x00010000, self.target)
            return p.Frame(p.CMD_START_UPDATE, 0, su.pack())

        if self.phase == "data":
            if self.block_index >= len(self.blocks):
                return None
            # Intentional retransmission: resend the previous frame
            if self.replay.get() and self.block_index > 0:
                self.replay.set(False)
                return p.Frame(p.CMD_DATA, self.seq,
                               self.blocks[self.block_index - 1])
            return p.Frame(p.CMD_DATA, self.seq + 1,
                           self.blocks[self.block_index])

        if self.phase == "end":
            return p.Frame(p.CMD_END_UPDATE, self.seq + 1)

        return None

    # -----------------------------------------------------------
    # Step execution
    # -----------------------------------------------------------
    def step(self):
        frame = self._next_frame()
        if frame is None:
            self._log("Transfer complete.", "info")
            self._log(f"At next boot: {self.sim.boot()}", "info")
            self.auto = False
            self.btn_auto.config(text="Auto play")
            return

        raw = bytearray(p.encode(frame))

        # Fault injection: flip one payload byte
        corrupted = False
        if self.corrupt.get() and len(raw) > p.FRAME_HEADER_SIZE + 2:
            raw[p.FRAME_HEADER_SIZE + 1] ^= 0xFF
            corrupted = True
            self.corrupt.set(False)

        self.last_frame = frame
        self._log(f"PC  -> {frame}"
                  + ("   [corrupted byte injected]" if corrupted else ""),
                  "pc")

        try:
            response_bytes = self.sim.handle(bytes(raw))
        except PowerLoss as e:
            self._log(f"POWER CUT: {e}", "nack")
            self._refresh()
            return

        if not response_bytes:
            self._log("BL  -> (no response, frame ignored)", "info")
            self.last_response = None
            self._refresh()
            return

        response = p.decode(response_bytes)
        self.last_response = response

        if response.cmd == p.RSP_NACK:
            err = p.ERROR_NAMES.get(response.data[0], response.data[0])
            self._log(f"BL  -> NACK  {err}", "nack")
        else:
            name = p.CMD_NAMES.get(response.cmd, hex(response.cmd))
            self._log(f"BL  -> {name}  seq={response.seq}", "ack")

        self._advance(response, corrupted)
        self._refresh()

    def _advance(self, response, corrupted):
        """Advances the PC state machine."""
        if response.cmd == p.RSP_NACK:
            return      # stay in place, PC will retransmit

        if self.phase == "info":
            info = p.InfoResponse.unpack(response.data)
            self.target = info.free_slot
            self._log(
                f"    free slot = {'AB'[self.target]}, "
                f"sending app_slot{'AB'[self.target]}.bin", "info")
            self.phase = "start"

        elif self.phase == "start":
            self.seq = 0
            self.phase = "data"

        elif self.phase == "data":
            if not corrupted:
                self.seq = response.seq
                # a retransmission does not advance the index
                if response.seq > self.block_index:
                    self.block_index += 1
            if self.block_index >= len(self.blocks):
                self.phase = "end"

        elif self.phase == "end":
            self.phase = "done"

    # -----------------------------------------------------------
    # Display refresh
    # -----------------------------------------------------------
    def _refresh(self):
        self._update_pc()
        self._update_bl()
        self._update_flow()
        self._update_flash()

        total = len(self.blocks)
        self.progress.config(
            text=f"block {min(self.block_index, total)}/{total}   "
                 f"phase: {self.phase}")

    def _line(self, widget, key, val, tag="val"):
        widget.insert("end", f"{key:<18}", "key")
        widget.insert("end", f"{val}\n", tag)

    def _update_pc(self):
        t = self.pc_panel.body
        t.config(state="normal")
        t.delete("1.0", "end")

        self._line(t, "firmware", f"{len(self.firmware)} bytes")
        self._line(t, "global CRC", f"0x{self.fw_crc:08X}")
        self._line(t, "blocks", f"{len(self.blocks)} x {p.DATA_BLOCK_SIZE}")
        t.insert("end", "\n")
        self._line(t, "phase", self.phase, "hi")
        self._line(t, "next seq", self.seq + 1)
        self._line(t, "current block", f"{self.block_index}/{len(self.blocks)}")
        if self.target is not None:
            self._line(t, "target slot", "AB"[self.target], "hi")

        t.insert("end", "\n")
        remaining = len(self.blocks) - self.block_index
        remaining_bytes = remaining * p.DATA_BLOCK_SIZE
        duration = remaining_bytes * 10 / 115200
        self._line(t, "remaining", f"{remaining_bytes} bytes")
        self._line(t, "estimated time", f"{duration:.2f} s @115200")

        t.config(state="disabled")

    def _update_bl(self):
        t = self.bl_panel.body
        t.config(state="normal")
        t.delete("1.0", "end")

        self._line(t, "internal state", self.sim.state, "hi")
        self._line(t, "expected seq", self.sim.expected_seq)
        self._line(t, "last seq", self.sim.last_seq)
        if self.sim.target_slot is not None:
            self._line(t, "slot being written", "AB"[self.sim.target_slot])
        self._line(t, "bytes received", self.sim.bytes_received)

        t.insert("end", "\nMetadata\n", "key")
        meta = self.sim.read_metadata()
        if meta is None:
            t.insert("end", "  no valid copy\n", "val")
        else:
            self._line(t, "  counter", meta.counter)
            self._line(t, "  active slot", "AB"[meta.active_slot], "hi")
            self._line(t, "  state",
                       p.STATE_NAMES.get(meta.state, meta.state), "hi")
            self._line(t, "  size", meta.fw_size)
            self._line(t, "  boot failures", meta.boot_fail_count)

        t.insert("end", "\nMetadata pages\n", "key")
        for pg in (0, 1):
            from bootloader_sim import Metadata
            m = Metadata.unpack(self.sim.flash.read_meta_page(pg))
            if m is None:
                status = "blank or invalid"
            else:
                status = f"counter={m.counter}"
                if meta and m.counter == meta.counter:
                    status += "   <- authoritative"
            self._line(t, f"  page {pg}", status)

        t.config(state="disabled")

    def _update_flow(self):
        c = self.canvas
        c.delete("all")
        w = c.winfo_width() or 280

        # downward arrow PC -> BL
        c.create_line(20, 20, w - 20, 20, fill=DIM, width=2,
                      arrow="last", arrowshape=(12, 14, 5))
        if self.last_frame:
            name = p.CMD_NAMES.get(self.last_frame.cmd, "?")
            c.create_text(w / 2, 8, text=name, fill=PC_COLOR, font=MONO)

        # upward arrow BL -> PC
        c.create_line(w - 20, 70, 20, 70, fill=DIM, width=2,
                      arrow="last", arrowshape=(12, 14, 5))
        if self.last_response:
            name = p.CMD_NAMES.get(self.last_response.cmd, "?")
            color = NACK if self.last_response.cmd == p.RSP_NACK else ACK
            c.create_text(w / 2, 58, text=name, fill=color, font=MONO)

        # hex dump of the last frame sent
        self.hex_box.config(state="normal")
        self.hex_box.delete("1.0", "end")
        if self.last_frame:
            raw = p.encode(self.last_frame)
            self.hex_box.insert("end", "Sent frame\n\n")
            self.hex_box.insert("end", f"MAGIC   {raw[0:2].hex(' ')}\n")
            self.hex_box.insert("end", f"CMD     {raw[2]:02x}\n")
            self.hex_box.insert("end", f"LENGTH  {raw[3:5].hex(' ')}"
                                       f"   ({len(self.last_frame.data)})\n")
            self.hex_box.insert("end", f"SEQ     {raw[5:7].hex(' ')}"
                                       f"   ({self.last_frame.seq})\n")
            body = raw[7:len(raw) - 4]
            preview = body[:24].hex(' ')
            suffix = " ..." if len(body) > 24 else ""
            self.hex_box.insert("end", f"DATA    {preview}{suffix}\n")
            self.hex_box.insert("end", f"CRC32   {raw[-4:].hex(' ')}\n")
            self.hex_box.insert("end", f"\ntotal   {len(raw)} bytes")
        self.hex_box.config(state="disabled")

    def _update_flash(self):
        c = self.flash_canvas
        c.delete("all")
        w = c.winfo_width() or 1100
        if w < 100:
            self.root.after(50, self._update_flash)
            return

        meta = self.sim.read_metadata()
        active = meta.active_slot if meta else None

        for i, slot in enumerate((p.SLOT_A, p.SLOT_B)):
            y = 8 + i * 32
            c.create_text(24, y + 10, text=f"slot {'AB'[slot]}",
                          fill=DIM, font=MONO)

            x0, x1 = 70, w - 90
            width = x1 - x0
            c.create_rectangle(x0, y, x1, y + 20,
                               fill=BG, outline=BORDER)

            # written proportion: sampled to stay fast
            data = self.sim.flash.slots[slot]
            useful = max(len(self.firmware), 1)
            written = sum(1 for k in range(0, useful, 64)
                          if data[k] != 0xFF)
            total_samples = max(1, len(range(0, useful, 64)))
            frac = written / total_samples

            if frac > 0:
                color = BL_COLOR if slot == active else ACCENT
                c.create_rectangle(x0, y, x0 + width * frac, y + 20,
                                   fill=color, outline="")

            label = f"{frac * 100:.0f}%"
            if slot == active:
                label += "  active"
            c.create_text(x1 + 42, y + 10, text=label,
                          fill=FG, font=MONO)

    # -----------------------------------------------------------
    # Commands
    # -----------------------------------------------------------
    def toggle_auto(self):
        self.auto = not self.auto
        self.btn_auto.config(text="Pause" if self.auto else "Auto play")
        if self.auto:
            self._loop()

    def _loop(self):
        if not self.auto:
            return
        self.step()
        if self.phase != "done":
            self.root.after(220, self._loop)
        else:
            self.auto = False
            self.btn_auto.config(text="Auto play")

    def reset(self):
        self.sim = BootloaderSim()
        self._prepare_steps()
        self.log.delete("1.0", "end")
        self._log("Simulator reset.", "info")
        self._refresh()

    def load_file(self):
        path = filedialog.askopenfilename(
            title="Choose a firmware",
            filetypes=[("Binary", "*.bin"), ("All", "*.*")])
        if not path:
            return
        with open(path, "rb") as f:
            self.firmware = f.read()
        self.reset()
        self._log(f"Loaded: {os.path.basename(path)} "
                  f"({len(self.firmware)} bytes)", "info")

    def _log(self, text, tag="info"):
        self.log.insert("end", text + "\n", tag)
        self.log.see("end")


if __name__ == "__main__":
    root = tk.Tk()
    app = DebugGUI(root)
    root.after(120, app._refresh)     # once widget sizes are known
    root.mainloop()
