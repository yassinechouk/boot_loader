# OTA Updates Without the Reset Button

How a firmware update is triggered on the **STM32L476RG** without pressing the
Nucleo's reset button, and what it required from the bootloader, the
application and the host scripts.

---

## Contents

1. [The problem](#1-the-problem)
2. [The mechanism](#2-the-mechanism)
3. [Memory layout](#3-memory-layout)
4. [Files added](#4-files-added)
5. [Files changed](#5-files-changed)
6. [Sequence](#6-sequence)
7. [Usage](#7-usage)
8. [Failure modes](#8-failure-modes)

---

## 1. The problem

The bootloader listens on the serial link for **two seconds** after every
reset, then starts the application. The window is short on purpose: it costs
two seconds on every boot. But flashing an image meant pressing **Reset** and
then launching the host script inside that window — a race you often lose, and
one that assumes physical access to the board.

The goal is therefore not to remove the listen window, but to be able to
**lengthen it on demand**, without touching the board.

---

## 2. The mechanism

A **RAM flag that survives reset**.

1. The host (over SWD) or the application itself writes the magic word
   `0xDEADBEEF` to the last word of RAM.
2. The target is reset — AIRCR, or OpenOCD's `reset run`.
3. SRAM1 is **not** cleared by a system reset, so the bootloader finds the
   magic word on the next boot.
4. It **clears the flag immediately**, then stretches its listen window from
   2 s to **30 s**.
5. The host transfers the binary into the free slot.

### The central decision: lengthen, don't divert

The first version entered an **infinite** update mode and never started the
application. That was the wrong instinct. If the transfer never begins — wrong
file, busy port, host interrupted — the board sits there holding a perfectly
good application in flash, and the only way out is the reset button. Which is
exactly the button this mechanism exists to avoid needing.

So the flag changes **only how long the bootloader waits**, never the boot
decision. The usual sequence (VALID / TESTING / IN_PROGRESS / EMPTY, rollback
included) runs unchanged. A failed attempt costs thirty seconds, after which
the board boots exactly as it would have.

`update_mode()` only honours its timeout while the protocol is idle
(`PROTO_IDLE`), so a transfer already under way is never cut short by the
window expiring.

---

## 3. Memory layout

* **SRAM1** on the STM32L476: 96 KB, `0x20000000` to `0x20018000`.
* **Flag**: `0x20017FFC` — the last word.
* **Stack top `_estack`**: `0x20017FF8`.

```
0x20018000 +-------------------------------------------+
0x20017FFC | _boot_request  (4 bytes)                  | <-- flag
0x20017FF8 +-------------------------------------------+ <-- _estack
           |                                           |  ^
           |                  Stack                    |  |  grows
           |                                           |  |  downward
           +-------------------------------------------+
           |                   Heap                    |
           +-------------------------------------------+
           |           .data and .bss                  |
0x20000000 +-------------------------------------------+
```

### Why eight bytes come off the stack, not four

Reserving four would leave `_estack` at `0x20017FFC` — aligned to 4, but
**not to 8**. AAPCS requires an 8-byte aligned stack pointer at every public
interface, and GCC generates code assuming it. On ARMv7-M that does not fault
immediately, which is precisely the problem: it is a violated contract waiting
for a `double`, a `-O2`, or an FPU-using function to surface it. Four more
bytes out of 96 KB costs nothing.

### Why SRAM1 and not SRAM2

SRAM2 has an option byte (`SRAM2_RST`) that erases it on every reset, for
security. A flag placed there could vanish depending on how the board is
configured. SRAM1 is only disturbed by a power cycle.

### Why the address is not hard-coded

It is defined in the **linker scripts** — the only place that knows where RAM
ends — and exported as a symbol:

```ld
_boot_request = ORIGIN(RAM) + LENGTH(RAM) - 4;
_estack       = ORIGIN(RAM) + LENGTH(RAM) - 8;
```

A `0x20017FFC` constant copied into C and into Python would have kept matching
right up until someone changed `LENGTH(RAM)`. On that day the flag lands
**inside the live stack**, and the file becomes a source of random corruption
instead of a build error. Same reasoning as the field offsets in `protocol.h`:
a memory layout is a contract, so it is stated once, in the place that owns it.

---

## 4. Files added

### 4.1. `shared/boot_request.h`

Shared between the bootloader and the application.

```c
extern uint32_t _boot_request;                    /* defined by the linker */
#define BOOT_REQUEST_ADDR   ((volatile uint32_t *)&_boot_request)
#define BOOT_REQUEST_MAGIC  0xDEADBEEFUL
```

Three `static inline` functions: `boot_request_set()`,
`boot_request_pending()`, `boot_request_clear()`.

### 4.2. `tools/ota_flash.py`

One-command host orchestration.

* **`resolve_boot_request_addr()`** — reads the `_boot_request` symbol from all
  three ELFs (`bootloader.elf`, `app_slotA.elf`, `app_slotB.elf`) with
  `arm-none-eabi-nm` and **requires them to agree**. A disagreement, or a
  missing symbol, describes a build that would fail silently on the board, so
  the script refuses to run.
* **`trigger_via_swd()`** *(default)* — OpenOCD writes the magic word and
  resets:
  ```bash
  openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
          -c "init; halt; mww <addr> 0xDEADBEEF; reset run; exit"
  ```
* **`trigger_via_uart()`** — holds the trigger character on the serial line
  until the running application confirms. No debugger needed, but it assumes a
  live application.
* **Banner wait** — the serial port is opened **before** the reset. The
  bootloader prints `OTA request accepted` within milliseconds of restarting;
  a script that opened the port afterwards would routinely miss the one line
  proving the mechanism worked. If the banner never arrives the script fails
  with the likely cause, instead of letting `flash.py` time out with nothing to
  say.

---

## 5. Files changed

### 5.1. Linker scripts

`bootloader/linker.ld`, `app/linker_slotA.ld`, `app/linker_slotB.ld`:

```diff
- _estack = ORIGIN(RAM) + LENGTH(RAM);
+ _boot_request = ORIGIN(RAM) + LENGTH(RAM) - 4;
+ _estack       = ORIGIN(RAM) + LENGTH(RAM) - 8;
```

### 5.2. `bootloader/main.c`

The flag is read **after** the watchdog starts (still the first line of
`main()`) and after peripheral initialisation — not "on the first instruction",
as an earlier draft of this document claimed.

```c
uint32_t listen_ms   = BOOT_WAIT_MS;    /* 2 s  */
int      ota_request = 0;

if (boot_request_pending()) {
    boot_request_clear();               /* BEFORE acting on it */
    ota_request = 1;
    listen_ms   = OTA_WAIT_MS;          /* 30 s */
}
```

`listen_ms` is then passed to the two `update_mode()` calls in the boot
sequence (`VALID` and `TESTING`). No other branch changes.

Clearing precedes acting: clearing afterwards would mean a reset during the
transfer left the request armed, and the board would re-enter update mode on
every boot from then on.

### 5.3. `app/main.c`

**UART receive.** `uart_init()` now clears the inherited error flags and drains
`RDR`:

```c
USART2_ICR = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3);
(void)USART2_RDR;
```

The bootloader gates the USART2 clock off before jumping, but **gating a clock
does not reset a peripheral**: its status flags cross the handover intact. And
the bootloader has just been receiving protocol frames at full rate, so `ORE`
is more likely set than not. Left alone it would make the receive path deaf
from the application's first instant. `uart_getc()` also clears `ORE` on every
call — the point `bootloader/src/uart.c` already makes at length in its
interrupt handler.

**OTA trigger.** One byte is not enough:

```c
#define OTA_TRIGGER_BYTE    'U'
#define OTA_CONFIRM_POLLS   4
```

`ota_trigger_poll()` drains whatever is available each pass and tracks a
streak; **four consecutive polls** must see the trigger character and nothing
else. A stray byte scores one poll and the next one erases it.

Why not a multi-byte magic word, which would match the rest of the project?
Because this receive path has no interrupt and no ring buffer: each pass reads
at most one byte, `RDR` holds exactly one, and a magic word sent in a single
write arrives in forty microseconds — everything past its first byte is lost to
the overrun. So the confirmation is built along the axis that is actually
available: time. Losing bytes to overrun becomes harmless, because the host
keeps sending.

`software_reset()` now waits for `TC` (shift register empty) before writing to
AIRCR. `TXE` only reports that the holding register is free, and resetting then
cuts the last character in half.

### 5.4. `bootloader/src/protocol_mgr.c`

`on_start_update()` no longer zeroes `boot_fail_count`. That counter describes
the slot **on trial**, which is not the one being written, and a transfer that
merely *starts* proves nothing. Zeroing it there let a host that begins an
update and then fails push rollback further away every time, for a firmware
that was busy failing. `on_end_update()` clears it once the new image is really
in flash — the point at which it genuinely no longer describes anything.

### 5.5. `shared/metadata.h`

`BOOTLOADER_VERSION` goes from `0x00000100` (0.1.0) to `0x00000200` (0.2.0).
The bump is not cosmetic: a 0.1.0 bootloader has `_estack` at the very top of
RAM, so **its own first stack push overwrites the flag word** before it can be
read. The trigger then does nothing, silently. `ota_flash.py` diagnoses that
case through the missing banner.

### 5.6. `tools/flash.py`

`--version` no longer defaults to a fixed `0.1.0`. Left alone, every image
flashed during development stamped itself with the same number, so the field
said nothing — and both slots ended up claiming v0.1.0, which is exactly when
you want to tell them apart. It now defaults to one patch above whatever
`GET_INFO` reports, which keeps versions distinct and ordered with nothing to
remember, and puts the question on the side of the link that knows the answer.

The transfer estimate also accounts for the ACK frames coming back and for
flash programming time. Counting only the outgoing payload under-predicted by
roughly half.

---

## 6. Sequence

```mermaid
sequenceDiagram
    autonumber
    participant Host as Host PC (ota_flash.py)
    participant OCD as OpenOCD (SWD)
    participant RAM as RAM (_boot_request)
    participant BL as Bootloader
    participant Flash as Flash (free slot)

    Host->>Host: read _boot_request from all 3 ELFs, require agreement
    Host->>Host: open the serial port BEFORE triggering
    Host->>OCD: init; halt; mww <addr> 0xDEADBEEF; reset run
    OCD->>RAM: write the magic word
    OCD->>BL: reset run
    Note over BL: iwdg_start() -- first line of main()
    BL->>RAM: boot_request_pending() ?
    RAM-->>BL: yes
    BL->>RAM: boot_request_clear() -> 0
    BL-->>Host: "OTA request accepted, listening for 30 s"
    Note over BL: normal boot sequence, window stretched to 30 s
    Host->>BL: GET_INFO
    BL-->>Host: RSP_INFO (free slot, bl_version, fw_version)
    Host->>BL: START_UPDATE
    loop 256-byte blocks
        Host->>BL: DATA + CRC
        BL->>Flash: 64-bit programming
        BL-->>Host: ACK
    end
    Host->>BL: END_UPDATE
    BL->>BL: global CRC32 re-read from flash -> TESTING
    BL->>BL: software reset, boots the new image
```

If the transfer never starts, the branch after "listening for 30 s" expires and
the board boots the existing application.

---

## 7. Usage

### Update over SWD (default)

```bash
python3 tools/ota_flash.py
```

### Update over the serial link, no debugger

```bash
python3 tools/ota_flash.py --via-uart
```

Assumes a live application running its main loop. Measured on hardware: about
two seconds of sustained sending before confirmation.

### Explicit firmware version

```bash
python3 tools/ota_flash.py --version 2.0.0
```

Without it, the version is derived as one patch above what the board reports.
Any argument this script does not recognise is passed through to `flash.py`.

### Serial port

Do not leave a terminal (`minicom`, `screen`, `picocom`) open on
`/dev/ttyACM0` during the operation: Linux hands received bytes to the first
process that reads them.

### First install of bootloader 0.2.0

The bootloader cannot update itself. On a board carrying an older version it
has to be flashed once over SWD:

```bash
make -C bootloader flash
```

---

## 8. Failure modes

| Situation | Behaviour |
|---|---|
| Flag set, no host ever connects | 30 s of listening, then the application boots normally |
| Transfer interrupted midway | `active_slot` unchanged, target slot left `IN_PROGRESS`; the board boots the existing application |
| Reset during the transfer | Flag already cleared: no update loop |
| Stray byte on the serial line | A streak of 4 polls is required; an isolated byte is erased on the next pass |
| Receive overrun (`ORE`) | Cleared on every call; the trigger tolerates lost bytes by construction |
| Bootloader older than 0.2.0 | Flag overwritten by its own stack; `ota_flash.py` diagnoses it through the missing banner |
| Power cycle | RAM indeterminate; false-positive probability 1 in 2³² |
| Application crashed | `--via-uart` fails cleanly; `--via-swd` still works |

### What is not covered

* The serial trigger is still a **repeated single character**, not an
  authenticated frame. It guards against accidents, not against an adversary
  with access to the link. A hostile environment would need a real signed
  frame, handled by the parser in `protocol_mgr.c`.
* A power cut during a metadata write is handled by the existing dual-page
  scheme, independently of this mechanism.
