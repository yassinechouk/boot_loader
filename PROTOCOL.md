# Firmware Update Protocol — STM32L476RG

**Protocol version: 1**
**Target: STM32L476RG (Nucleo-L476RG)**
**Transport: UART 115200 8N1 (CAN planned)**

---

## 1. Overview

This document specifies the binary protocol used between a host tool and the
embedded bootloader to transfer and install a new firmware image.

The protocol is **transport-agnostic**. The reference implementation runs over
UART; porting it to CAN or any other byte-oriented link requires no change to
the protocol layer itself.

### Design principles

- **The host is master.** It initiates every transaction; the bootloader only
  responds.
- **Every frame is acknowledged.** The host does not send the next frame until
  the previous one is acknowledged.
- **Nothing is destroyed before validation.** The bootloader checks everything
  it can — size, target slot, version — before erasing a single page.
- **Idempotence.** Reprocessing an already-received frame produces no side
  effect.
- **Defence in depth.** Three independent verification layers: per-frame CRC
  (transmission), read-back after write (storage), whole-image CRC computed
  from flash (consistency).

---

## 2. Frame format

Every frame, in both directions, shares the same structure.

```
Offset  Size  Field    Format
------  ----  -------  ------------------------------------
0       2     MAGIC    0xAA 0x55
2       1     CMD      message type
3       2     LENGTH   uint16, little-endian
5       2     SEQ      uint16, little-endian
7       N     DATA     payload, may be empty
7+N     4     CRC32    uint32, little-endian
```

**Fixed header: 7 bytes. Total overhead including CRC: 11 bytes per frame.**

### Byte order

All multi-byte fields are **little-endian**, the native order of the Cortex-M4.
This eliminates any conversion on the embedded side; the host absorbs the
difference, having the resources to do so.

Network protocols traditionally chose big-endian for interoperability between
heterogeneous machines. That constraint does not apply here — both endpoints
are under our control.

### MAGIC field

Fixed value `0xAA 0x55`. The two bytes are the binary inverse of each other,
producing eight consecutive transitions on the line — the pattern furthest
removed from a line stuck at a constant level, and therefore the easiest to
distinguish from noise or a failed transmitter.

`0x00` and `0xFF` are deliberately avoided: both occur abundantly in a compiled
binary as padding, erased flash, and zero-filled regions.

### CRC coverage

The CRC32 covers **CMD, LENGTH, SEQ and DATA** — the MAGIC is excluded.

Rationale: the magic validates itself. A frame whose preamble is corrupted is
never recognised as a frame, so its CRC is never evaluated. Including it would
add no detection capability.

Covering LENGTH, on the other hand, is **critical**. An unprotected length
field is a classic buffer-overflow vector.

### CRC parameters

CRC-32 with the standard Ethernet polynomial `0x04C11DB7`, computed by the
STM32L4 hardware CRC peripheral. Initial value `0xFFFFFFFF`, `REV_IN = 01`
(byte-wise bit reversal), `REV_OUT = 0`, no final XOR.

This configuration differs from `zlib.crc32`, which applies output reflection
and a final XOR. The host implementation matches the hardware rather than the
reverse: the peripheral computes in four AHB cycles with zero CPU cost, while
the host can absorb any algorithm without it being noticeable.

Verified against silicon:

| Input         | CRC32        |
|---------------|--------------|
| `"123456789"` | `0x9B63D02C` |
| four zeros    | `0xC704DD7B` |
| `"STM32"`     | `0xF4F0FF62` |

### Frame size

Maximum accepted `LENGTH`: **1024 bytes**.
Nominal data block size: **256 bytes**.

The block size is constrained by two properties of the STM32L4 flash
controller:

- **Multiple of 8** — the programming unit is a 64-bit double word. A size that
  is not a multiple of 8 would leave orphan bytes to be carried over between
  frames.
- **Divisor of 2048** — the erase unit is a 2 KB page. A non-divisor would make
  some blocks straddle two pages.

256 satisfies both and yields 4.3 % protocol overhead with a negligible RAM
buffer. This choice also makes lazy page erasure trivial to implement: page
boundaries always coincide with block boundaries.

---

## 3. Command codes

The most significant bit distinguishes requests from responses, which makes the
originator of any byte identifiable at a glance in a logic-analyser capture.

### Requests (host → bootloader)

| Value  | Name               | Description                                    |
|--------|--------------------|------------------------------------------------|
| `0x01` | `CMD_GET_INFO`     | Query board state                              |
| `0x02` | `CMD_START_UPDATE` | Announce a transfer: size, CRC, target slot    |
| `0x03` | `CMD_DATA`         | Carry a firmware block                         |
| `0x04` | `CMD_END_UPDATE`   | Signal completion, request whole-image check   |
| `0x05` | `CMD_ABORT`        | Cancel the transfer in progress                |

### Responses (bootloader → host)

| Value  | Name        | Description                       |
|--------|-------------|-----------------------------------|
| `0x81` | `RSP_INFO`  | Reply to `CMD_GET_INFO`           |
| `0x82` | `RSP_ACK`   | Frame accepted                    |
| `0x83` | `RSP_NACK`  | Frame rejected, with error code   |

---

## 4. Error codes

Carried in the DATA field of an `RSP_NACK`, one byte.

| Value  | Name              | Meaning                                    |
|--------|-------------------|--------------------------------------------|
| `0x01` | `ERR_CRC`         | Frame CRC mismatch                         |
| `0x02` | `ERR_SEQ`         | Unexpected sequence number                 |
| `0x03` | `ERR_LENGTH`      | LENGTH outside accepted bounds             |
| `0x04` | `ERR_FLASH`       | Erase, write or read-back failure          |
| `0x05` | `ERR_SIZE`        | Firmware too large for the slot            |
| `0x06` | `ERR_SLOT`        | Target slot differs from the announced one |
| `0x07` | `ERR_STATE`       | Command received in an incompatible state  |
| `0x08` | `ERR_GLOBAL_CRC`  | Whole-image CRC mismatch                   |
| `0x09` | `ERR_PROTO_VER`   | Unsupported protocol version               |

A bare NACK would only say "it failed". The error code turns a useless message
into an actionable diagnostic, at the cost of one byte.

---

## 5. Payloads

### 5.1 `CMD_GET_INFO` — request

Empty DATA, `LENGTH = 0`.

### 5.2 `RSP_INFO` — response (12 bytes)

```
Offset  Size  Field           Description
------  ----  --------------  ------------------------------------------
0       4     fw_version      Version of the active firmware (LE)
4       4     bl_version      Bootloader version (LE)
8       1     proto_version   Supported protocol version
9       1     active_slot     Currently running slot (0 = A, 1 = B)
10      1     free_slot       Slot the host must write to (0 = A, 1 = B)
11      1     state           Current state of the active slot
```

`active_slot` and `free_slot` are mutually deducible. The redundancy is
deliberate: it places the "where to write" decision on the side that holds the
real state, rather than leaving the host to infer it. The less the host infers,
the less it can get wrong.

### 5.3 `CMD_START_UPDATE` — payload (16 bytes)

```
Offset  Size  Field           Description
------  ----  --------------  ------------------------------------------
0       4     fw_size         Firmware size in bytes (LE)
4       4     fw_crc32        CRC32 of the whole image (LE)
8       4     fw_version      Version of the transferred firmware (LE)
12      1     target_slot     Destination slot (0 = A, 1 = B)
13      1     proto_version   Protocol version in use
14      2     reserved        Reserved, set to zero
```

The frame count is **not** transmitted: it follows from `fw_size` and the block
size. Sending a redundant value would open the possibility of an inconsistency
between two fields that would then have to be detected and arbitrated.

The `reserved` field brings the structure to 16 bytes and leaves room for a
future field without a size change, hence without breaking compatibility.

### 5.4 `CMD_DATA` — payload

Raw firmware block, `LENGTH` bytes, written at offset `SEQ × 256` from the
start of the target slot. `LENGTH` must be a non-zero multiple of 8, since the
flash controller only programs double words.

### 5.5 `CMD_END_UPDATE` — payload

Empty DATA. The bootloader re-reads the entire written region from flash,
recomputes the CRC32 and compares it against the `fw_crc32` received in
`CMD_START_UPDATE`.

### 5.6 `RSP_ACK` — payload

Empty DATA. The SEQ field echoes the acknowledged frame number.

---

## 6. Sequencing

### Numbering

`SEQ` starts at 0 for `CMD_START_UPDATE` and increments with every frame. The
bootloader remembers the last accepted `SEQ`.

### Handling on reception

| Condition            | Action                                          |
|----------------------|-------------------------------------------------|
| `SEQ == expected`    | Process, write, acknowledge                     |
| `SEQ == last`        | **Do not rewrite**, resend the acknowledgement  |
| Otherwise            | `RSP_NACK` with `ERR_SEQ`, abort the transfer   |

The second case corresponds to a retransmission following a lost ACK. The host
is not asking for the frame to be rewritten — it is waiting for the
acknowledgement it never received.

Rewriting would in fact be incorrect: flash cannot be reprogrammed without a
prior erase, and a second write to the same location yields an undefined
result.

This **idempotence** property is what makes retransmission harmless.

### Plausibility check

On reading `LENGTH`, before any buffering:

```
if LENGTH > 1024  ->  reject immediately, return to magic hunting
```

Waiting for the CRC before rejecting would require buffering a potentially
unmanageable amount of data. Every field is validated against known constraints
as soon as it is read.

### Timeouts

The bootloader restarts its inactivity timer after a frame has been **fully
processed**, not on the arrival of an individual byte. The counter therefore
does not run during flash operations, which may take tens of milliseconds.

| Phase                       | Timeout |
|-----------------------------|---------|
| Waiting for `CMD_START`     | 2 s     |
| Between `CMD_DATA` frames   | 5 s     |
| Host waiting for an ACK     | 1 s     |

The host retransmits up to three times before giving up.

---

## 7. Persistent metadata

### Firmware states

| Value  | State         | Meaning                                            |
|--------|---------------|----------------------------------------------------|
| `0x00` | `EMPTY`       | Slot empty or never programmed                     |
| `0x01` | `IN_PROGRESS` | Transfer started, not completed                    |
| `0x02` | `TESTING`     | Image installed, awaiting application confirmation |
| `0x03` | `VALID`       | Image validated, bootable without reservation      |

### Structure — partition table

```c
typedef struct {
    uint32_t size;              /* image size, 0 if absent */
    uint32_t crc32;             /* expected CRC32          */
    uint32_t version;           /* firmware version        */
    uint8_t  state;             /* fw_state_t              */
    uint8_t  reserved[3];
} slot_info_t;                  /* 16 bytes */

typedef struct {
    uint32_t    magic;          /* 0x424C4D44 = "BLMD"     */
    uint32_t    counter;        /* incremented per write   */
    slot_info_t slot[2];        /* A and B, independent    */
    uint8_t     active_slot;
    uint8_t     boot_fail_count;
    uint8_t     reserved[2];
    uint32_t    meta_crc32;     /* CRC32 of preceding 44 B */
} metadata_t;                   /* 48 bytes */
```

**Each slot carries its own size, CRC and version.** An earlier revision
described only the active firmware. The flaw surfaced only on rollback: when
switching to the other slot, the metadata retained the size and CRC of the
*rejected* image. The bootloader then read `VALID`, recomputed the fallback
slot's CRC against a wrong reference, found a mismatch and refused to boot —
on a board that carried a perfectly functional firmware.

Rollback had saved the device once, then bricked it on the next reset.
Describing both slots independently removes the problem: switching now amounts
to changing `active_slot` alone.

**Field distribution.** `size`, `crc32`, `version` and `state` are **per slot**:
each image carries its own at all times, including the inactive one.
`active_slot`, `boot_fail_count` and `counter` are **global** — the failure
counter in particular can only ever describe one slot, since only one can be in
`TESTING` at a time. This property would no longer hold with more than two
slots, or if several images could be evaluated concurrently.

### Dual-page mechanism

The structure is written to two distinct flash pages. On each update, **only
the inactive page is erased** and rewritten; the other stays intact and
readable throughout the operation.

Consequence: a power loss can never destroy both copies. On restart at least
one valid copy remains — the pre-update one if the interruption occurred during
the write.

A copy is accepted if its magic matches **and** its CRC is correct. The magic
alone would not suffice: it shares its 8-byte write unit with the counter, so
an interruption after the first write would leave a valid magic in front of
fields still at `0xFF`. `size` would then read `0xFFFFFFFF` — 4 GB — and the
bootloader would run past the end of flash while verifying the image.

Between two valid copies, the one with the higher counter prevails.

**Counter tie.** This can only result from corruption or a bug. The module then
deterministically selects page 0 rather than declaring the board blank.
Refusing to boot would be the wrong trade-off: two valid copies carry two
intact data sets, most likely describing the same state. There is no danger in
booting, whereas falling back to update mode would immobilise a working device.
One refuses to operate only when continuing would be dangerous. The anomaly
resolves itself: the next write carries a strictly higher counter.

The 32-bit counter overflow is ignored. Flash endurance — roughly 10 000 erase
cycles per page — is the effective limit, five orders of magnitude lower.

---

## 8. Fail-safe ordering

The order of writes follows one rule: **the invalidation marker is always
written before the destructive operation, never after.**

Starting a transfer:

1. Write `state = IN_PROGRESS` to metadata
2. Erase the target page
3. Write the blocks

The reverse order would leave a window during which the metadata claims a valid
firmware exists while it has just been erased — a *lying* state. With the
correct order, the worst case is a system that wrongly believes a transfer
failed while the previous image is intact: needless pessimism, but no data
loss.

Completing a transfer:

1. Verify the whole-image CRC by reading back from flash
2. Write `state = TESTING`, `active_slot = target`
3. Reset

### Lazy erasure

The target slot is **not** erased upfront. Each page is erased immediately
before being written.

Erasing 480 KB in advance means 240 pages at roughly 20 ms each — close to five
seconds of unresponsiveness, for a firmware that may occupy only 20 KB.
Measured on the host test harness: 3 page erases for a 1 KB image, 4 for 4 KB,
against 240 for a bulk erase.

This is only possible because 256 divides 2048: no block ever straddles two
pages, so the boundary test reduces to `offset % 2048 == 0`.

---

## 9. Test and rollback

A correct CRC proves an image's **integrity**, not its **correctness**. A
firmware transmitted without a single corrupted bit can still crash within its
first second.

`TESTING` addresses this:

- The bootloader increments the failure counter, then jumps to the image
- If the application starts correctly, **it writes `VALID` itself** and clears
  the counter
- If it crashes, the watchdog resets the board; the bootloader finds `TESTING`
  unconfirmed and, beyond the threshold, switches back to the previous slot

The counter is incremented **before** the jump. Incrementing it afterwards
would have no effect, since the jump never returns.

Before switching, the bootloader verifies that the fallback slot holds a valid
image. Rejecting the current one only to find nothing behind it would be worse
than continuing to try.

The application therefore carries a responsibility. Without its confirmation,
no rollback is possible — but without it, no firmware survives past three
boots either.

---

## 10. Nominal sequence

```
Host                                          Bootloader
│                                                      │
│──── CMD_GET_INFO (SEQ=0) ───────────────────────────▶│
│◀─── RSP_INFO {free_slot=B, fw_ver=1.0} ──────────────│
│                                                      │
│──── CMD_START_UPDATE (SEQ=0) ───────────────────────▶│ check size, slot
│     {size, crc32, ver, slot=B}                       │ write IN_PROGRESS
│◀─── RSP_ACK (SEQ=0) ─────────────────────────────────│
│                                                      │
│──── CMD_DATA (SEQ=1) [256 B] ───────────────────────▶│ erase page if needed
│◀─── RSP_ACK (SEQ=1) ─────────────────────────────────│ write + read-back
│                                                      │
│──── CMD_DATA (SEQ=2) [256 B] ───────────────────────▶│
│◀─── RSP_ACK (SEQ=2) ─────────────────────────────────│
│              ...                                     │
│──── CMD_DATA (SEQ=N) ───────────────────────────────▶│
│◀─── RSP_ACK (SEQ=N) ─────────────────────────────────│
│                                                      │
│──── CMD_END_UPDATE (SEQ=N+1) ───────────────────────▶│ read back, check CRC
│◀─── RSP_ACK (SEQ=N+1) ───────────────────────────────│ write TESTING, reset
│                                                      │
```

---

## 11. Bootloader state machine

```
        ┌──────────┐
        │   IDLE   │◀──────────────────────┐
        └────┬─────┘                       │
             │ valid CMD_START_UPDATE      │
             ▼                             │
      ┌─────────────┐   CMD_DATA           │
      │  RECEIVING  │◀────────┐            │
      └──────┬──────┘─────────┘            │
             │ CMD_END_UPDATE              │
             ▼                             │
      ┌─────────────┐                      │
      │  VERIFYING  │                      │
      └──────┬──────┘                      │
             │                             │
     ┌───────┴────────┐                    │
     │ CRC OK         │ CRC bad            │
     ▼                ▼                    │
┌──────────┐   ┌──────────────┐            │
│ COMPLETE │   │ NACK + ABORT │────────────┘
│ + reset  │   └──────────────┘
└──────────┘
```

`CMD_ABORT` or a timeout from any state returns to `IDLE`.

---

## 12. Known limitations

This protocol guarantees **integrity**, not **authenticity**. A CRC32 detects
accidental corruption; it offers no protection against a deliberately malicious
firmware, since an attacker can trivially recompute it.

A hardened version would replace the whole-image CRC with an asymmetric
signature — ECDSA P-256, for instance — with the public key held in a
write-protected flash region. The transport mechanism would be unchanged; only
the final validation step would differ.

Further limitations of the current design:

- The bootloader's listening window is two seconds after reset. A production
  device would need a way to force update mode — a button held at boot, or an
  application command that reboots into the bootloader.
- The dual-binary approach requires the host to hold two images per firmware
  version. A device with hardware bank remapping could use a single one.
- The failure counter is global, which relies on only one slot being in
  `TESTING` at a time.
- On rollback, the rejected slot keeps its `size` and `crc32` while its state
  becomes `EMPTY`. Harmless, since `state` is authoritative, but clearing them
  would be tidier.

These are accepted within the scope of this project, whose object is mastery of
the update mechanism rather than cryptographic hardening.

---

## 13. Revision history

| Version | Changes                                                        |
|---------|----------------------------------------------------------------|
| 1.0     | Initial specification                                           |
| 1.1     | Per-slot metadata (fixes rollback describing the wrong image)   |
