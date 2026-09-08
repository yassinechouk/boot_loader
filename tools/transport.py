"""
Serial transport for the update protocol.

This layer is the only point in the PC code that knows about the
serial port. protocol.py encodes and decodes frames without knowing
how they are carried; porting to CAN or TCP socket would only touch
this file.

Requires pyserial:
    pip install pyserial --break-system-packages
"""

import time

try:
    import serial
except ImportError:
    raise SystemExit(
        "pyserial is required:\n"
        "    pip install pyserial --break-system-packages"
    )

import protocol as p


class TransportError(Exception):
    pass


class Timeout(TransportError):
    pass


class Disconnected(TransportError):
    """The serial port has disappeared: board unplugged or ST-LINK reset."""
    pass


class SerialTransport:
    """
    Sends a frame and waits for the response, then decodes it.

    Reading is done in two passes: first the 7-byte header, from
    which the payload length is derived, then the rest. Same
    reasoning as on the firmware side — it is impossible to know
    how many bytes to wait for before reading LENGTH.
    """

    def __init__(self, port: str, baudrate: int = 115200,
                 timeout: float = 1.0, verbose: bool = False):
        self.verbose = verbose
        self.timeout = timeout
        try:
            self.ser = serial.Serial(port, baudrate, timeout=timeout)
        except serial.SerialException as e:
            raise TransportError(f"cannot open {port}: {e}")

        # The ST-LINK may have bytes pending from a previous session.
        # Start clean.
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
        self._log(f"-> {frame}  ({len(raw)} bytes)")
        try:
            self.ser.write(raw)
            self.ser.flush()
        except serial.SerialException as e:
            raise Disconnected("cannot write to port") from e

    def receive(self, timeout: float = None) -> p.Frame:
        """
        Reads a complete frame. Raises Timeout if nothing arrives.

        Resynchronises on the magic: spurious bytes or a truncated
        response do not block indefinitely.
        """
        deadline = time.time() + (timeout if timeout is not None else self.timeout)

        # --- search for the magic ---
        window = b""
        while time.time() < deadline:
            try:
                byte = self.ser.read(1)
            except serial.SerialException as e:
                raise Disconnected("cannot read from port") from e
            if not byte:
                continue
            window = (window + byte)[-2:]
            if window == p.MAGIC:
                break
        else:
            raise Timeout("no preamble received")

        # --- read the rest of the header ---
        rest = self._read_exact(p.FRAME_HEADER_SIZE - 2, deadline)
        header = p.MAGIC + rest

        length = int.from_bytes(header[3:5], "little")
        if length > p.MAX_PAYLOAD_SIZE:
            raise TransportError(f"abnormal LENGTH: {length}")

        # --- payload and CRC ---
        tail = self._read_exact(length + p.FRAME_CRC_SIZE, deadline)

        frame = p.decode(header + tail)
        self._log(f"<- {frame}")
        return frame

    def _read_exact(self, n: int, deadline: float) -> bytes:
        buf = b""
        while len(buf) < n:
            if time.time() > deadline:
                raise Timeout(f"{len(buf)}/{n} bytes received")
            try:
                chunk = self.ser.read(n - len(buf))
            except serial.SerialException as e:
                raise Disconnected("read interrupted") from e
            if chunk:
                buf += chunk
        return buf

    # ------------------------------------------------------------
    def exchange(self, frame: p.Frame, retries: int = 3) -> p.Frame:
        """
        Sends and waits for the response, retransmitting if necessary.

        A retransmission is harmless on the bootloader side: receiving
        the same frame twice produces the same result as receiving it
        once. This property — idempotence — is what makes this loop safe.
        """
        last_error = None

        for attempt in range(retries):
            try:
                self.send(frame)
                return self.receive()

            except serial.SerialException as e:
                # The port has disappeared: board unplugged, or ST-LINK reset.
                # Retrying makes no sense, and letting the raw exception
                # propagate would give an unreadable Python traceback
                # instead of a diagnostic.
                raise Disconnected(
                    "board disconnected during transfer"
                ) from e

            except (Timeout, p.BadCRC, p.BadMagic) as e:
                # These three cases are recoverable: lost frame,
                # corrupted frame, or desynchronisation. Retransmission
                # is harmless on the bootloader side — reprocessing an
                # already-received frame produces the same result.
                last_error = e
                if attempt < retries - 1:
                    self._log(f"failure ({e}), retrying")
                    try:
                        self.ser.reset_input_buffer()
                    except serial.SerialException as e2:
                        raise Disconnected(
                            "board disconnected"
                        ) from e2
                    time.sleep(0.05)

        raise TransportError(
            f"no response after {retries} attempts: {last_error}"
        )
