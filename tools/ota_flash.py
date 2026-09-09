#!/usr/bin/env python3
"""
ota_flash.py -- One-command OTA update without pressing the reset button.

Strategy
--------
OpenOCD is already connected via ST-Link.  Instead of asking the running
application to write the boot request flag (which requires the UART RX
path to be clean), we write it *directly into RAM* with a single OpenOCD
command, then immediately reset the target.

The bootloader reads the flag on the very first instruction of main(),
clears it, and enters update_mode(0) -- waiting indefinitely for a
firmware upload.  flash.py then connects and sends the binary.

This avoids any dependency on the application's UART receive path.

Usage
-----
    python3 tools/ota_flash.py [extra flash.py args]
    python3 tools/ota_flash.py --version 2.0.0
"""

import os
import sys
import time
import subprocess

# ---------------------------------------------------------------
# Paths / constants
# ---------------------------------------------------------------
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT  = os.path.dirname(TOOLS_DIR)
APP_DIR    = os.path.join(REPO_ROOT, 'app')
FLASH_PY   = os.path.join(TOOLS_DIR, 'flash.py')

PORT  = '/dev/ttyACM0'
BAUD  = 115200

# Must match shared/boot_request.h
BOOT_REQUEST_ADDR  = 0x20017FFC
BOOT_REQUEST_MAGIC = 0xDEADBEEF

OPENOCD_IFACE  = 'interface/stlink.cfg'
OPENOCD_TARGET = 'target/stm32l4x.cfg'

# How long to wait for the bootloader to finish its startup before
# flash.py tries to connect.
SETTLE_DELAY = 0.8   # seconds

# ---------------------------------------------------------------

try:
    import serial
except ImportError:
    print("pyserial is required:  pip install pyserial --break-system-packages")
    sys.exit(1)

# ---------------------------------------------------------------
# Terminal colours
# ---------------------------------------------------------------
GREEN  = "\033[92m"
RED    = "\033[91m"
YELLOW = "\033[93m"
BOLD   = "\033[1m"
END    = "\033[0m"

def ok(msg):   print(f"  {GREEN}OK{END}    {msg}")
def err(msg):  print(f"  {RED}FAILED{END} {msg}")
def info(msg): print(f"  {msg}")
def step(msg): print(f"\n{BOLD}{msg}{END}")


# ---------------------------------------------------------------
# OpenOCD trigger
# ---------------------------------------------------------------

def trigger_via_openocd() -> bool:
    """
    Use OpenOCD to:
      1. Halt the target.
      2. Write BOOT_REQUEST_MAGIC directly into RAM at BOOT_REQUEST_ADDR.
      3. Resume execution (soft reset so the bootloader re-reads the flag).

    Returns True on success.
    """
    step("OTA trigger (via OpenOCD)")
    info(f"writing 0x{BOOT_REQUEST_MAGIC:08X} → 0x{BOOT_REQUEST_ADDR:08X}")

    openocd_cmds = (
        f"init; "
        f"halt; "
        f"mww 0x{BOOT_REQUEST_ADDR:08X} 0x{BOOT_REQUEST_MAGIC:08X}; "
        f"reset run; "
        f"exit"
    )

    cmd = [
        'openocd',
        '-f', OPENOCD_IFACE,
        '-f', OPENOCD_TARGET,
        '-c', openocd_cmds,
    ]

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=15,
        )
    except FileNotFoundError:
        err("openocd not found -- is it installed?")
        return False
    except subprocess.TimeoutExpired:
        err("openocd timed out")
        return False

    if result.returncode != 0:
        err("openocd failed")
        # Show only the useful part of stderr
        for line in result.stderr.splitlines():
            if any(k in line for k in ('Error', 'failed', 'FAILED')):
                info(f"  {line}")
        return False

    ok("magic word written, target reset")
    return True


# ---------------------------------------------------------------
# Wait for bootloader
# ---------------------------------------------------------------

def wait_for_bootloader(timeout: float = 4.0) -> bool:
    """
    Open the serial port and wait until the bootloader prints its
    update-mode banner ("Waiting for firmware").

    If the banner is not seen within `timeout` seconds we still return
    True, because the bootloader may have already printed it before we
    opened the port.  flash.py will give a clearer error if it cannot
    connect.
    """
    step("Waiting for bootloader")

    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.2, dsrdtr=False, rtscts=False)
    except serial.SerialException as exc:
        err(f"cannot open {PORT}: {exc}")
        return False

    buf   = b""
    found = False
    start = time.time()

    while (time.time() - start) < timeout:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            try:
                text = chunk.decode("ascii", errors="replace")
                for line in text.splitlines():
                    line = line.strip()
                    if line:
                        info(f"  board > {line}")
            except Exception:
                pass

            decoded = buf.decode("ascii", errors="replace")
            if "Waiting for firmware" in decoded or "OTA request" in decoded:
                found = True
                break

        time.sleep(0.05)

    ser.close()

    if found:
        ok("bootloader is in update mode")
    else:
        # Not necessarily a fatal error -- the bootloader may have printed
        # the message before we could open the port.
        info("banner not captured (port may have been busy); proceeding anyway")

    return True


# ---------------------------------------------------------------
# Flash
# ---------------------------------------------------------------

def run_flash(extra_args: list) -> int:
    step("Firmware transfer")
    cmd = [sys.executable, FLASH_PY,
           '--port', PORT,
           '--dir',  APP_DIR] + extra_args
    return subprocess.call(cmd)


# ---------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------

def main() -> int:
    extra_args = sys.argv[1:]

    if not trigger_via_openocd():
        err("Could not set boot request flag via OpenOCD.")
        info("Make sure the ST-Link is connected and the board is powered.")
        return 1

    # Give the bootloader time to start and reach update_mode().
    time.sleep(SETTLE_DELAY)

    wait_for_bootloader(timeout=3.0)

    # Small extra pause after closing the monitor port so flash.py can
    # open it cleanly.
    time.sleep(0.2)

    return run_flash(extra_args)


if __name__ == "__main__":
    sys.exit(main())
