# STM32 Dual-Slot OTA Bootloader

A bare-metal firmware update system for the STM32L476RG, written from scratch
without HAL, CubeMX or any vendor abstraction layer.

The board receives a new firmware over UART, verifies it at three independent
levels, installs it into a spare flash slot, and boots it on trial. If the new
image fails to confirm that it started correctly, an independent watchdog
resets the board and the bootloader rolls back to the previous image —
automatically, with no host involvement, no button press, and no physical
access to the board.

```
========================================
  BOOTLOADER v0.2.0
========================================
Reset caused by the watchdog
Active slot : B
State       : TESTING
Size        : 4472 bytes
Version     : 0x00040000
Boot fails  : 3
Fallback    : slot A, VALID, 4344 bytes

Failure threshold reached, rolling back
Falling back to slot A

Jumping to 0x08008000
```

---

## Why this project

Development boards are flashed over SWD. Production hardware usually is not:
the debug connector is omitted to save cost and board space, and read-out
protection is enabled so the firmware cannot be extracted or replaced by
whoever holds the device.

What remains is the interface the product already uses to do its job — a serial
link, a CAN bus, a network connection. A bootloader is what turns that
interface into an update path.

Building one from scratch also forces contact with parts of the architecture
that application development hides completely: linker scripts, the vector
table, runtime flash programming, and designing for interrupted operations.

---

## What it does

- Receives firmware over UART using a custom binary protocol
- Verifies transmission (per-frame CRC32), storage (read-back after write) and
  consistency (whole-image CRC recomputed from flash)
- Maintains two application slots, so a failed update never takes the device
  down
- Persists its state across power loss in duplicated, CRC-protected flash pages
- Boots a new image on trial and rolls back if the application does not confirm
- Uses an independent watchdog so a hung application is detected without human
  intervention
- Survives power loss at any point in the transfer
- Triggers an update remotely — no reset button, no physical access required

Everything is written directly against the hardware registers. No HAL, no
CubeMX, no RTOS.

---

## Hardware

| Item | Notes |
|---|---|
| NUCLEO-L476RG | Cortex-M4F, 1 MB flash (2 × 512 KB banks), 128 KB RAM |
| Mini-USB cable | Powers the board, carries SWD and the virtual COM port |

Nothing else. UART reaches the host through the on-board ST-LINK, so no wiring
is required.

---

## Memory layout

```
0x08000000  ┌──────────────────┐
            │    BOOTLOADER    │   32 KB   (9.1 KB used)
0x08008000  ├──────────────────┤
            │      SLOT A      │  480 KB
0x08080000  ├──────────────────┤
            │      SLOT B      │  480 KB
0x080FF000  ├──────────────────┤
            │   METADATA A     │    2 KB
0x080FF800  ├──────────────────┤
            │   METADATA B     │    2 KB
0x08100000  └──────────────────┘
```

---

## Getting started

### Toolchain

```bash
sudo apt install gcc-arm-none-eabi gdb-multiarch openocd
pip install pyserial --break-system-packages
```

### Build and flash the bootloader

```bash
cd bootloader
make
make flash          # over SWD — the only way to install the bootloader
```

### Build the application

```bash
cd app
make                # produces app_slotA.bin and app_slotB.bin
make verify         # confirms each is linked to its own address
```

### Update over UART

No reset button needed. A single command triggers the update from end to end:

```bash
python3 tools/ota_flash.py
```

What this does:

1. Reads the `_boot_request` symbol from every built ELF and verifies they
   agree on its address.
2. Opens the serial port **before** triggering, so the bootloader's
   confirmation line is never missed.
3. Uses OpenOCD (SWD) to write a magic word into RAM and reset the board.
4. Waits for the bootloader banner `OTA request accepted` — proof the
   mechanism worked — then hands off to `flash.py`.

The bootloader finds the magic word on the next boot, clears it immediately,
and stretches its listen window from 2 s to 30 s. The normal boot sequence
(VALID / TESTING / rollback) runs unchanged — if no transfer arrives in time
the board simply boots the existing application.

```
Board status
  protocol       : v1
  bootloader     : v0.2.0
  active slot    : A
  free slot      : B
  state          : VALID
  file           : app_slotB.bin

Transfer
  size     : 4296 bytes
  CRC32    : 0xA7B5340A
  blocks   : 17 x 256
  target   : slot B
  [########################################] 100%  4296/4296 bytes
  transmitted in 0.7 s (6579 B/s)

Verification
  OK    global CRC verified
  OK    image marked TESTING
```

The host never chooses the target slot. It asks the board which slot is free
and sends the matching binary.

#### Trigger over UART (no debugger)

If the application is running and no ST-Link is available:

```bash
python3 tools/ota_flash.py --via-uart
```

`ota_flash.py` holds the trigger character `'U'` on the serial line. The
application requires **four consecutive polls** to see it exclusively before
resetting. A stray byte resets the streak, so an idle terminal cannot trigger
it accidentally.

#### Inspect without transferring

```bash
python3 tools/flash.py --port /dev/ttyACM0 --info
```

---

## Design decisions

The reasoning behind each choice matters more than the choice itself. These are
the ones that shaped the system.

### Why two application slots

A single-slot bootloader must erase the running firmware before writing the new
one. Any interruption during that window — power loss, cable disconnect, a
corrupted transfer — leaves the device with no bootable image and no way to
recover except physical access.

Two slots remove the window entirely. The incoming image is written to the
inactive slot; the running one is never touched until the new image has proven
itself.

### Why two binaries per firmware version

A compiled binary is bound to a link address. Function calls and global
variable references are resolved at link time, so an image built for
`0x08008000` will not run at `0x08080000`.

The effect is visible in the first sixteen bytes of each image, dumped straight
from flash:

```
slot A:  0080 0120  2186 0008  6986 0008  6986 0008
slot B:  0080 0120  2106 0808  6906 0808  6906 0808
         └────────┘ └────────┘
         stack ptr  reset handler
         identical  0x08008621 vs 0x08080621
```

Same source, same RAM, but every flash address shifted by 0x78000 — exactly the
480 KB between the two slots. The trailing `1` on each handler address is the
Thumb bit; without it the jump raises an immediate HardFault.

Three alternatives were considered:

| Approach | Verdict |
|---|---|
| Position-independent code (`-fPIC`) | Rejected. The vector table cannot be position-independent — it holds absolute addresses the hardware reads directly. Bare-metal PIC on ARM is poorly documented and full of edge cases. |
| Copy to a fixed execution slot | Rejected. The copy is itself a destructive, non-atomic operation: a power loss mid-copy destroys the old image without completing the new one, recreating the exact problem dual-slot was meant to eliminate. |
| Hardware bank swap (`BFB2`) | Rejected for this target. On STM32L4 the swap is performed by the ST ROM bootloader, which would bypass this bootloader entirely unless it were duplicated in both banks. |
| **Two binaries** | **Chosen.** One flash write per byte, no scratch region, instant rollback. |

The cost is two build artefacts instead of one — a build-system concern, moved
to the host, where resources are abundant.

### Why 256-byte data blocks

Two hardware constraints intersect:

- The flash programming unit is a 64-bit double word, so block size must be a
  **multiple of 8**
- The erase unit is a 2 KB page, so block size should be a **divisor of 2048**
  to prevent blocks straddling page boundaries

256 satisfies both. It also turned out to enable **lazy page erasure**: because
page boundaries always coincide with block boundaries, the bootloader can erase
each page immediately before writing it, testing only `offset % 2048 == 0`.

Erasing the full 480 KB slot upfront would take about five seconds. Lazy
erasure costs 3 page erases for a 1 KB image instead of 240.

### Why a `TESTING` state

A correct CRC proves an image's *integrity*, not its *correctness*. A firmware
transmitted without a single corrupted bit can still crash immediately —
because of a bug, a hardware mismatch, or simply because the wrong binary was
sent to the wrong slot.

So a freshly installed image is marked `TESTING`, not `VALID`. The bootloader
increments a failure counter, then jumps to it. The application must write
`VALID` itself once it has run long enough to be considered healthy. If it
never does, the counter reaches its threshold and the bootloader falls back.

The counter is incremented **before** the jump. Incrementing it afterwards
would have no effect, since the jump never returns.

### Why the watchdog starts in the bootloader

Rollback depends on a failing application causing a reset. Without a watchdog,
an application that hangs simply hangs — the board freezes, the failure counter
never advances, and the fallback never happens. Early testing required pressing
the reset button by hand three times, which is not a mechanism.

The IWDG closes that gap. It is clocked by the LSI, an oscillator independent
of the system clock, so it keeps counting even if a broken firmware destroys
the clock configuration.

It is started on the **first line of the bootloader's main()**, not by the
application. Starting it from the application would leave a blind spot: a crash
inside `startup.s`, or a HardFault on the very first instruction, would never
be detected. Starting it just before the jump would leave the bootloader itself
unwatched, and it contains several blocking waits — USART TXE, flash BSY, the
receive loop — any of which can hang if a peripheral stops responding.

Surveillance must begin before the thing it watches.

Since the IWDG cannot be stopped once running, the bootloader refreshes it
during update mode. The cost is nil: the receive loop runs thousands of times
per second, and the longest blocking operation — a page erase at roughly 20 ms
— stays a hundred and fifty times below the three-second timeout.

The application refreshes it **conditionally**, on the application cycle having
advanced. An unconditional refresh would only catch a complete hang: a program
looping over a section that happens to contain the refresh call would keep the
watchdog quiet while doing nothing useful.

The three-second timeout is deliberately generous. The two failure modes are
not symmetric: too short causes a false positive — a healthy firmware reset in
a loop and rolled back for no reason — while too long only delays detection.
This system drives no actuator, so a few seconds of undefined behaviour during
an update carries no risk. With the LSI specified at ±5 %, the real timeout
lies between 2.86 s and 3.16 s.

### Why duplicated metadata pages

The metadata records which slot is active and in what state. It has to survive
power loss, so it lives in flash — but updating flash requires erasing a page
first, and a power loss during that erase would destroy the very information
needed to recover.

Two pages solve this. Only the inactive one is ever erased; the other stays
readable throughout. Between two valid copies, the higher counter wins.

Each copy carries a CRC over its own contents. The magic number alone is
insufficient: it shares its 8-byte write unit with the counter, so an
interruption after the first write would leave a valid magic in front of fields
still at `0xFF`. `size` would read as 4 GB and the bootloader would run past
the end of flash.

### Fail-safe ordering

The invalidation marker is always written **before** the destructive operation:

```
1. write state = IN_PROGRESS
2. erase the target page
3. write the blocks
```

The reverse order leaves a window where the metadata claims a valid firmware
exists while it has just been erased — a state that *lies*. With the correct
order, the worst case is a system that wrongly believes a transfer failed while
the old image is intact: needless pessimism, not data loss.

The same principle appears in journalling filesystems and database commit
protocols.

### Why interrupt-driven UART reception

At 115200 baud a byte arrives every 87 µs. A flash page erase takes around
20 ms — long enough to miss over two hundred bytes if the CPU were polling.

The protocol is strictly request-response, so in principle the host never
transmits while the board is writing. But that guarantee depends on the
*sender's* discipline. A bootloader should not depend on its counterpart
behaving correctly.

Reception uses a lock-free single-producer, single-consumer ring buffer. The
ISR writes `head` and reads `tail`; the main context writes `tail` and reads
`head`. No variable is written by both, so no critical section is needed. One
slot is sacrificed to distinguish full from empty — the alternative, an element
counter, would be written by both contexts and would require disabling
interrupts on every access.

The hardware `ORE` flag is explicitly cleared. Left set, the USART **stops
receiving entirely** — a failure mode that produces no visible symptom beyond
frames going unanswered.

---

## A design flaw found by testing

The nominal update path worked on the first attempt. The rollback path did not
— and the way it failed is worth recording.

The original metadata structure described only the *active* firmware: one
`fw_size`, one `fw_crc32`, one `fw_version`. Rollback copied that structure,
changed `active_slot`, and wrote it back.

The result:

```
VTOR         : 0x08080000
Image verification:
  computed CRC : 0x74A4F8EC
  expected CRC : 0x980C80AA   MISMATCH
```

The rollback itself succeeded — the board booted the fallback image. But the
metadata now described the *rejected* image while pointing at the fallback
slot. On the next reset the bootloader would read `VALID`, recompute the CRC
against the wrong reference, find a mismatch, and refuse to boot.

Rollback had saved the device once, then bricked it on the following restart.

The fix was to describe each slot independently — a partition table rather than
a description of the active image. Rollback then reduces to changing
`active_slot`; both images keep their own size, CRC and version at all times.

This flaw was invisible on the nominal path and would only have surfaced in the
field, after the first genuine rollback.

---

## Testing

### Host-side test suites

The protocol, metadata manager and CRC are implemented in Python as well,
allowing the full state machine to be exercised on a PC — including cases that
are impractical to reproduce reliably on hardware, such as power loss after
exactly one 8-byte write.

```bash
cd tools
python3 test_protocol.py     # 64 tests: framing, sequencing, error paths
python3 bootloader_sim.py    # full transfer against the simulator
python3 debug_gui.py         # step-through visualisation with fault injection
```

| Suite | Tests | Covers |
|---|---|---|
| Protocol state machine | 47 | framing, resync, retransmission, timeouts, all error codes |
| Metadata manager | 32 | dual-page selection, power loss during erase and write, counter ties |
| Ring buffer | 22 | FIFO order, wraparound, saturation accounting |
| Rollback scenario | 15 | two-image lifecycle across rollback and reinstall |

### On-silicon verification

| Module | Result |
|---|---|
| CRC32 | 3 vectors matching the Python implementation exactly |
| Flash driver | 10 tests including all five rejection paths |
| Metadata manager | 25 tests, persistence verified across bootloader reflash |
| UART | sustained burst at 115200 baud, zero bytes lost, buffer reaching 511/511 |
| IWDG | timeout measured, debug freeze verified, reset cause reported |

### Robustness scenarios

Each was performed on hardware and produced the expected behaviour.

**Autonomous rollback.** An application built to hang after two cycles was
installed. With no human intervention, the watchdog reset the board three
times, the failure counter advanced on each attempt, and the bootloader
switched back to the previous slot — the whole sequence taking about twenty
seconds.

```
cycle 1
cycle 2

*** DELIBERATE HANG ***
The watchdog will no longer be refreshed.
Reset expected in ~3 s.

[silence, then]

========================================
  BOOTLOADER v0.2.0
========================================
Reset caused by the watchdog
State       : TESTING
Boot fails  : 3
Failure threshold reached, rolling back
Falling back to slot A
```

On the following reset the fallback image booted with a matching CRC — the
specific case that exposed the design flaw described above.

**Power loss mid-transfer.** The cable was pulled at 51 % of a 100 KB transfer.
The active slot remained `VALID` and the board booted normally; the target slot
was left `IN_PROGRESS` and correctly excluded from consideration.

```
Active slot : B
State       : VALID
Fallback    : slot A, IN_PROGRESS, 102400 bytes
```

**Wrong binary.** Sending the slot-B image while slot A is free is rejected
before a single byte is transmitted, by the host tool and again by the
bootloader's `ERR_SLOT`.

**Corrupted frame.** A single flipped bit fails the frame CRC; the bootloader
NACKs and the host retransmits. Reprocessing is harmless because frame handling
is idempotent.

**Slot alternation.** Two consecutive updates were sent without specifying a
target. The board directed the first to slot B and the second to slot A,
choosing from its own metadata rather than any host-side counter. Dumping both
slots afterwards confirmed two distinct images in flash.

---

## Repository layout

```
├── bootloader/
│   ├── main.c              boot decision, rollback, jump
│   ├── startup.s           vector table, .data/.bss init
│   ├── linker.ld           32 KB at 0x08000000
│   └── src/
│       ├── crc.c           hardware CRC peripheral driver
│       ├── flash.c         erase, write with read-back, dual-bank
│       ├── metadata_mgr.c  dual-page persistent state
│       ├── protocol_mgr.c  frame assembly and command dispatch
│       ├── uart.c          interrupt-driven RX, ring buffer
│       ├── systick.c       millisecond time base
│       └── iwdg.c          independent watchdog
│
├── app/
│   ├── main.c              demo application with self-confirmation
│   ├── linker_slotA.ld     0x08008000
│   └── linker_slotB.ld     0x08080000
│
├── shared/                 headers used by both images
│
├── tools/
│   ├── flash.py            update tool
│   ├── transport.py        serial transport layer
│   ├── protocol.py         frame encode/decode
│   ├── crc32.py            CRC matching the STM32 peripheral
│   ├── bootloader_sim.py   executable specification
│   ├── test_protocol.py    host test suite
│   └── debug_gui.py        protocol visualiser
│
└── PROTOCOL.md             full specification
```

---

## Limitations

**Integrity, not authenticity.** The CRC32 detects accidental corruption. It
offers no protection against a deliberately crafted firmware, since an attacker
can recompute it trivially. Hardening would mean replacing the whole-image CRC
with an ECDSA signature verified against a public key in a write-protected
region. The transport would be unchanged; only the final validation step would
differ.

**Fixed update window.** The bootloader listens for two seconds after reset.
The boot request flag extends this to thirty seconds on demand, via SWD or
the UART trigger in the application. Physical button access is not required.

**Dual binaries.** The host must hold two images per firmware version. Hardware
with true bank remapping could use one.

**The bootloader cannot update itself.** Fixing a bug in it requires SWD access
or the ST ROM bootloader via the BOOT0 pin. A two-stage design — a small
immutable first stage able to replace the second — would lift this, at a
complexity cost not justified here.

---

## Planned work

- FreeRTOS in the application layer, with a supervisor task driving the
  conditional watchdog refresh
- CAN as a second transport, exercising the protocol's transport independence
- Firmware signature verification

---

## References

- **RM0351** — STM32L4x5/L4x6 reference manual (flash, CRC, USART, IWDG, boot)
- **UM1724** — STM32 Nucleo-64 boards user manual (pinout, ST-LINK, solder
  bridges)
- **DS10198** — STM32L476xx datasheet (alternate function mapping, LSI accuracy)
- **MCUboot** — reference implementation studied for comparison

---

## License

MIT
