# CZINPUT v1-v4 binary format

All movie-container integers are unsigned little endian, independent of C struct
packing. New files use `.cic` (CUzeboxex Input Capture). Existing CZINPUT files
named `.cap` are also accepted; renaming them does not change the format. Raw legacy captures
have no CZINPUT signature and are rejected.

The 16-byte file header is eight bytes `CZINPUT\0`, a u32 version (`1`/`3` = Full Debug, `2`/`4` = Compact), and a
u32 header size (`16`). Versions 1 and 3 store logical chunks directly. Versions 2 and 4
store them inside the compressed blocks described below. New recordings use
versions 3/4 with behavior identity and subsystem fingerprints. The reader also
accepts versions 1/2 with their original strict checks; these older files do not
acquire the additional diagnostics retroactively.
Each logical chunk has a four-byte ASCII tag, u32 payload length,
u32 IEEE CRC32 of its payload, then the payload. CRC32 uses polynomial
`0xEDB88320`, initial value `0xFFFFFFFF`, and final inversion. Payload lengths
are bounded at 32 MiB. Unknown required tags are rejected.

The first chunks must be META and STAT, followed by timed chunks and one END!.
EOF without END!, any trailing data, CRC failure or malformed length is invalid.
A reader scans/validates the complete file before modifying the live machine.

## Versions 2 and 4 Compact transport

After the 16-byte file header, each independent block has a 16-byte header:
`CMP2` (four bytes), expanded length (u32), stored length (u32), and IEEE CRC32
of the expanded bytes (u32). Both lengths must be nonzero and at most 65,536;
stored length must not exceed expanded length. Equal lengths mean raw bytes.
Otherwise the payload uses this bounded LZ token format:

- Control byte 0–127: copy the next `control + 1` literal bytes.
- Control byte 128–255: copy `(control & 127) + 3` bytes from a backward
  distance encoded in the next two bytes (u16 little endian). Distance must
  be nonzero and no greater than bytes already expanded in this block.
  Overlapping copies are allowed; no references cross block boundaries.

The decoder consumes the entire stored payload and produces exactly the declared
expanded length, then checks CRC before exposing any bytes. Logical chunk headers
and payloads may span blocks. The snapshot is compressed using the same transport.
No codec dictionary or external compression library is required. Unknown versions,
block tags, invalid references, truncated blocks, and expansion overruns fail.

The logical END! must be the last bytes of the last block, followed by physical
EOF. Empty blocks, trailing compressed tokens/bytes, and missing END! are rejected.
Logical chunk CRCs remain present, as do every FRAM CPU/SD digest and timestamp.
Older readers reject newer version numbers. Both new recording modes require
a v3/v4-capable reader; choosing Full Debug does not downgrade the format.

## META (104 bytes)

| Offset | Type | Meaning |
|---|---|---|
| 0 | u32 | Savestate ABI signature |
| 4 | u32 | CRC32 of the complete loaded ROM file |
| 8 | u64 | ROM file byte length |
| 16 | u32 | CRC32 of initial 64 KiB emulated flash, including bootloader |
| 20 | u32 | Start mode: 0=current state, 1=reset then snapshot |
| 24 | 8 × u32 | SD model: preset, init ms, command wait bytes, read wait bytes, write busy ms, CS-high ms, min/max initialization byte cycles |
| 56 | u32 | Reserved, zero |
| 60 | 2 × u8 | Multitap presence for P1 and P2 |
| 62 | 2 bytes | Reserved, zero |
| 64 | 8 × u8 | Slot types, P1 slots 0–3 then P2 slots 0–3: 0=none, 1=pad, 2=Super Mouse, 3=keyboard |
| 72 | u32 | v3/v4: emulator behavior identifier (`0x20261015` in the default build); v1/v2: zero |
| 76 | u32 | v3/v4: fingerprint schema 1; v1/v2: zero |
| 80 | 24 bytes | Reserved, zero |

When no ROM path exists, the ROM identity is the CRC32 of current 64 KiB flash,
with length zero. Slot topology and starting EEPROM are also stored in STAT.
The ABI signature covers the relevant runtime struct sizes, integer and pointer
sizes, and native byte order. It is a compatibility check, not a core-behavior
version guarantee; changes to core semantics may require a future format version.

The 2026-10-15 interrupt/EEPROM corrections leave this v1 container and native
state layout unchanged, but can desynchronize older recordings. The v1/v2 ABI
signature does not identify those semantic changes; affected recordings need to
be remade. v3/v4 additionally require an exact behavior identifier match before
restoring state. The optional [AVR hardware models](avr-hardware-options.md)
use distinct identifiers for each compiled flag combination. Maintainers must
bump the base `CAPTURE_BEHAVIOR_ID` in `capture.h` when
core semantics change; it is independent of native layout and is not a Git SHA.
The CRC implementation now uses a 16-entry nibble table with identical results.

## STAT

An uncompressed snapshot produced by `savestate_capture_to_mem`. Its internal
layout is native to the savestate ABI, unlike the portable container. The reader
requires compatible sizes and validates nested block lengths before restoring.
This captures CPU/flash/EEPROM, SPI RAM, SD/VFAT, ESP and input state. Live host
handles are sanitized on restore. External networking/serial activity is not a
supported movie source.

## Timed records

Every timed payload begins with a u64 elapsed CPU-cycle count. It starts at zero
at the snapshot and extends the core's wrapping 32-bit cycle counter to 64 bits.
Events at the same cycle retain file order. Frame numbers are relative to the
movie, independent of the GUI's frame counter.

| Tag | Payload bytes | Fields after timestamp |
|---|---|---|
| LACH | 28 | u8 port (0–1), u8 slot (0–3), u8 bit count (1–64), u8 probe flag, u64 low-active serial shift packet, u64 current frame index |
| TOPO | 196 | u64 current frame index, 180-byte topology/protocol description |
| KEYB | 16 | u8 port, u8 slot, u8 keyboard command, u8 response, u32 current frame index |
| SECT | 524 | u32 SD sector number, 512 initial data bytes |
| FRAM | 24 (v1/v2), 44 (v3/v4) | u64 completed-frame count, u32 CPU-state digest, u32 running SD-write digest |
| END! | 16 | u64 completed-frame count |

LACH captures the packet at the actual latch, including a pending multitap probe
prefix. It bypasses host packet construction during replay. KEYB captures each
keyboard protocol response, including zero/empty responses and device queries;
legacy dongles and tap-routed keyboards share this boundary.

TOPO is emitted when the emulated device configuration changes, before the
next affected packet, keyboard response, SD operation or frame boundary. Its
180-byte description contains two u8 tap-presence flags, u8 legacy-keyboard
enable, u8 legacy-mouse enable (must be zero), eight u8 virtual-device IDs
(255=unassigned), and sixteen 8-byte device entries. Each entry contains u8 used,
u8 type, u32 options, u8 haptic enable and u8 haptic bus ID. IDs are internal;
input addresses remain the fixed physical port/tap slot. The last 40 bytes
contain two sets of three u32 values (selected slot, probe pending, last activity
cycle), followed by four u32 tap-controller values (state, arm pulses, command
pulses, target mask). These also preserve GUI preset resets which leave the same
device types connected but reset protocol progress. Host bindings are not
needed to replay replacement devices. Guest-controlled slot selection and probe
state continue to run through the normal emulated bus protocol.

SECT appears on first read or write of a sector. Subsequent reads/writes use the
private cache. It embeds only sectors actually used and has no dependency on
host file ordering at playback. The current implementation limits the cache to
16384 sectors. Sector writes never call the backing VFAT mutation path.

FRAM is emitted after each `frame_run`, before audio/video submission. The CPU
digest starts with CRC32(SRAM), then runs the same CRC update (without another
final inversion) over I/O registers, EEPROM and the little-endian u32 PC. The SD
digest starts at `0xFFFFFFFF` and updates over each write's little-endian u32 byte
address and u8 value, without final inversion. It detects different writes even
if the final sector contents happen to agree.

END! is emitted when recording is stopped cleanly. Playback checks it before
executing another frame, so input replay can produce video with an exact ending.


## v3/v4 frame fingerprints

The first 24 bytes of FRAM retain the v1/v2 timestamp, frame count, combined CPU
checksum and running SD-write digest. The additional little-endian u32 fields are:

| Offset | Field |
| --- | --- |
| 24 | Independent IEEE CRC32 of SRAM |
| 28 | Independent IEEE CRC32 of I/O/register space |
| 32 | Independent IEEE CRC32 of EEPROM |
| 36 | IEEE CRC32 over the little-endian u32 SPI RAM size followed by all visible SPI RAM bytes |
| 40 | AVR PC (word address) |

All checks remain strict. On mismatch the status includes the recorded frame,
first differing named subsystem, and expected/actual value. Fingerprinting the
whole visible SPI RAM increases recording/replay work, especially for multi-MiB
configurations; ordinary gameplay and logical traces do not compute these hashes
per video frame. The behavior identifier is a maintained compatibility contract,
not proof that two independently modified executables behave identically.

## Replay checkpoints and seek

`INPUT_MOVIE CHECKPOINT TAKE|RESTORE <0..3>` and `INPUT_MOVIE SEEK <frame>` operate
on in-memory checkpoints; no new checkpoint chunks are written into the movie.
Slot 0 holds replay's initial state. Checkpoints restore both native machine state
and event/transport/SD bookkeeping. Seek replays strict events from the nearest
saved checkpoint and pauses at a recorded frame boundary. Completed or failed
replays can restore retained checkpoints; a new movie session invalidates them.
The movie must remain byte-identical and its ROM identity must still match.
See [logical development tests](logical-development-tests.md) for commands,
limits, and the distinction between movie frames and logical-update observations.
