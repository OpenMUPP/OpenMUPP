# WCH-LinkE protocol

This document describes the USB protocol of the WCH-LinkE in RISC-V mode.
The `wch-link` personality (epic #63) must speak this protocol.
The facts come from USB captures and logic analyzer captures of a real probe. A statement that has no capture behind it says so. No vendor code was copied.

Status: the USB protocol and the wire frames are captured on a CH32V003 (one wire) and on a CH32V208 (two wires).
A few fields are open (see [Open questions](#open-questions)).

## Capture setup

| Item | Value |
|---|---|
| Probe | WCH-LinkE, USB ID `1a86:8010`, `bcdDevice` 2.19, product string `WCH-Link` (from `lsusb`, not in the captures) |
| Probe type | wlink reports `WCH-Link v2.19(v39) (WCH-LinkE-CH32V305)`. The info response holds `02 13 12 00` (see [Command 0x0d](#command-0x0d-info-connect-quit)). The source of `v39` is open |
| Target 1 | CH32V003F4P6 (16 KB flash), chip ID `0x00300500`, one-wire interface on PD1 |
| Target 2 | CH32V208WBU6 (160 KB flash), chip ID `0x2080053c`, two-wire interface on PA13 (SWDIO) and PA14 (SWCLK), external power |
| Host tools | wlink 0.1.2 and minichlink (a local build, version not recorded) |
| Capture | Linux `usbmon` on the bus of the probe, filtered to the probe address |

The captures are in [`wch-link-captures/`](wch-link-captures/).
Each file has the traffic of one tool command.
The file name gives the target, the tool and the command, for example `v003-wlink-flash.pcapng`.
The prefix `v003-` marks the CH32V003. The prefix `v208-` marks the CH32V208.
`v003-mc-write-head.pcapng` holds only the first 4000 frames of the minichlink write.
The full capture had 13 MB.
On the CH32V208, the minichlink write has 4 KB only.

## USB interface

The probe has three interfaces. Only interface 0 carries the debug protocol.

| Interface | Endpoints | Use |
|---|---|---|
| 0 (vendor) | `0x01` OUT, `0x81` IN | Commands and responses (64-byte packets) |
| 0 (vendor) | `0x02` OUT, `0x82` IN | Bulk data: flash image, memory read, program acknowledge |
| 1 (CDC) | `0x84` IN | CDC control (interrupt) |
| 2 (CDC data) | `0x03` OUT, `0x83` IN | UART bridge |

All bulk endpoints have a maximum packet size of 64 bytes. The device is full speed (12 Mbit/s).
The `wch-link` personality needs interface 0 only.
The UART and the CDC interfaces are out of scope (see epic #63).

## Framing

Every command is one packet on endpoint `0x01`:

```
0x81  CMD  LEN  payload (LEN bytes)
```

The probe answers with one packet on endpoint `0x81`:

```
0x82  CMD  LEN  payload (LEN bytes)
```

The response repeats the command number.
A response is always shorter than 64 bytes in the captures.

Two cases do not follow this rule:

- **Error.** The probe sends an error frame `81 REASON LEN payload`. The first byte is `0x81`, not `0x82`.
  Seen: a connect command (`0d 01 02`) with no target gives `81 55 01 01` (reason `0x55`, payload `01`).
  wlink makes four connect attempts (one about every 180 ms) and then stops (`v208-wlink-connect-fail.pcapng`).
  An unknown command (`7f`) gives a normal-looking frame `82 81 01 ff`. The reason for the difference is open.
  No other reason code was seen.
- **Chip information.** Command `0x11` answers with 20 raw bytes. The packet has no `0x82` header and no length.

Multi-byte values in command payloads are big-endian.

## Command overview

| CMD | Name | Payload (request) | Response payload |
|---|---|---|---|
| `0x01` | Set area | address (u32 BE) + length (u32 BE), `LEN` = 8 | `01` |
| `0x02` | Program control | one sub-command byte | the sub-command byte |
| `0x03` | Memory read | address (u32 BE) + length (u32 BE), `LEN` = 8 | `01` |
| `0x06` | Read protection | one sub-command byte | status or the sub-command byte |
| `0x08` | DMI access | 6 bytes, see [DMI access](#command-0x08-dmi-access) | 6 bytes |
| `0x0b` | Reset | `01` | `01` |
| `0x0c` | Configure | chip type byte + speed byte | `01` |
| `0x0d` | Info, connect, quit | one sub-command byte | depends on the sub-command |
| `0x11` | Chip information | one sub-command byte | 20 raw bytes, no header |

The payload column does not show the `LEN` byte.
For example, `81 03 08 08000000 00004000` is command `0x03` with a payload of 8 bytes.

The captures use no other commands.
The notes of epic #63 also mention `0x0e` (disable debug). No capture used it.

## Command `0x0d`: info, connect, quit

| Sub-command | Meaning | Response payload |
|---|---|---|
| `01` | Probe information | `02 13 12 00` |
| `02` | Connect to the target (RISC-V) | `09 00 30 05 00` (CH32V003), `05 20 80 05 3c` (CH32V208) |
| `03` | Unknown. minichlink sends it first. wlink does not. | `03` |
| `04` | Unknown. wlink sends it on the CH32V208 after the first protection query. | `00` |
| `ff` | Quit. The probe resumes the core and releases the target (see [Probe actions](#probe-actions-at-connect-at-chip-information-and-at-quit)). | `ff` |

- **Probe information.** Byte 0 and byte 1 are the major and minor version of the firmware (2 and 19). This matches `bcdDevice` 2.19. Byte 2 (`0x12`) is the probe type. wlink maps it to `WCH-LinkE-CH32V305`. Byte 3 is `0x00`.
- **Connect.** The probe starts the debug interface and does the chip detection. This takes about 90 ms on the CH32V003 and about 59 ms on the CH32V208. A connect after an erase or a protect command takes the same time. Byte 0 is a chip family code (`0x09` for the CH32V003, `0x05` for the CH32V208). Bytes 1 to 4 are the chip ID (u32 BE: `0x00300500`, `0x2080053c`).
- **Quit.** The host sends this at the end of every session. A session is connect to quit.
- **Sub-command `03`.** The probe needed about 74 ms to answer in 5 of 7 CH32V003 runs. It needed 1 ms in the other 2. On the CH32V208 it needed 0.9 ms or less. The reason is not known.
- **Other values.** `0d 01 05` echoes `82 0d 01 05`, but it is not a no-op. The reply took 184 ms (CH32V003) and 97 ms (CH32V208). The next quit took 46 ms and 24 ms, instead of 0.2 ms (`v003-raw-errors.pcapng`, `v208-raw-errors.pcapng`). The effect is not known.
- **Length check.** A memory read with a wrong length (`81 03 01 00`) gets a normal reply `82 03 01 01`. The probe does not check the length.

## Command `0x0c`: configure

Request payload: `chip-type speed`.

- wlink sends `01 speed` before the connect command and `family speed` after it. Here `family` is the first byte of the connect response (`09` or `05`).
  With the generic code `01`, the connect command still works on both chips.
  minichlink sends only `09 speed`, before the connect command, also on the CH32V208 (family `05`). The connect command still works.
- The speed byte: `01` is high (the default), `02` is medium (wlink: 4000 kHz), `03` is low (wlink: 400 kHz). Captures of `wlink --speed low|medium|high` show this.
- The probe answers `01`.

## Command `0x11`: chip information

Request payload: `06` (wlink) or `05` (minichlink). The answer was the same for both on both chips.

| Bytes | Content (CH32V003 / CH32V208) |
|---|---|
| 0 to 3 | `ff ff 00 10` / `ff ff 00 a0`. Bytes 2 and 3 (u16 BE) give the flash size in KB (16 / 160). |
| 4 to 11 | Unique ID (two words) |
| 12 to 15 | `ff ff ff ff` / `e3 39 e3 39` |
| 16 to 19 | Chip ID (u32 BE, `0x00300500` / `0x2080053c`). |

Bytes 0 to 15 are four words of the target, in big-endian byte order: the words at `0x1ffff7e0`, `0x1ffff7e8`, `0x1ffff7ec` and `0x1ffff7f0`.
The probe reads them when the `0x11` command arrives (see [Probe actions](#probe-actions-at-connect-at-chip-information-and-at-quit)).
The low 16 bits of the first word give the flash size in KB (`0xffff00a0` on the CH32V208, `0xffff0010` on the CH32V003).
The difference between sub-command `05` and `06` is open.

## Command `0x06`: read protection

| Sub-command | Meaning | Response payload |
|---|---|---|
| `01` | Query | `02` unprotected, `01` protected |
| `02` | Unprotect | `02` (CH32V003: 108 ms. CH32V208: 210 ms) |
| `03` | Protect | `03` (CH32V003: 103 ms. CH32V208: 59 ms) |
| `04` | Unknown. wlink sends it after a query that returned `02`. | `00` |

Facts about the protection:

- Protect takes effect only after a reset. On the CH32V208, a query directly after `wlink protect` still returns `02` (`v208-wlink-protect.pcapng`). After `wlink reset`, it returns `01` (`v208-wlink-status-protected.pcapng`). On the CH32V003, the evidence is indirect: the query returns `01` only after a reset in `v003-mc-erase.pcapng`.
- wlink `flash` sends the unprotect command by itself when the query returns `01`. The captures do not show whether unprotect erases the flash (no capture reads the flash after it).
- A memory read of a protected chip does not fail. It returns a fixed word, repeated. The word is `0x00000300` on the CH32V003 (`v003-wlink-dump-protected.pcapng`). On the CH32V208 the bytes arrive as `00 34 04 0a` (`v208-wlink-dump-protected.pcapng`).
- DMI access still works on a protected chip (`v003-wlink-status-protected.pcapng`).
- The CH32V208 behaves the same: protect, reset, then the query returns `01`.

## Command `0x08`: DMI access

The probe gives access to the RISC-V debug module (DM) of the target.
One command does one register access.

Request payload (6 bytes): `address (u8)`, `data (u32 BE)`, `op (u8)`.

| `op` | Meaning |
|---|---|
| `01` | Read. The data field is `00 00 00 00`. |
| `02` | Write |

The response payload has the same layout. The `op` field now holds the status:

- `00`: success. For a read, the data field has the register value, in the same response. The read is not pipelined.
- `02`: failure (from the notes of epic #63). The captures show no failure.

Registers seen (the CH32V003 and the CH32V208 use the same set):

| Address | Name | Note |
|---|---|---|
| `0x04`, `0x05` | `data0`, `data1` | |
| `0x10` | `dmcontrol` | |
| `0x11` | `dmstatus` | |
| `0x12` | `hartinfo` | |
| `0x16` | `abstractcs` | |
| `0x17` | `command` | abstract command |
| `0x18` | `abstractauto` | |
| `0x20` to `0x22` | `progbuf0` to `progbuf2` | Seen over USB. minichlink writes them. |
| `0x40` | `haltsum0` | |
| `0x7c` | WCH-specific | The probe reads it at connect (CH32V003: `00010403`) |
| `0x7d`, `0x7e` | WCH-specific | minichlink writes `5aa50400` at start-up. The probe writes both itself at connect. |
| `0x7f` | WCH-specific | The probe reads the chip ID at connect (`0x2080053c`, `0x00300500`) |

The register layout follows the RISC-V debug specification.
The probe does not change the value of a register on the way.

A DMI round trip takes about 0.1 ms on the USB bus.
On the wire, one host access takes 120 to 200 µs, with the USB round trip (see [Wire timing](#wire-timing-ch32v208-two-wires)).

## Memory read (`0x03`)

wlink uses this path for `dump`. The sequence:

1. `03 08 addr len`: set the read range. Response `03 01 01`.
2. `02 01 0c`: start the read. Response `02 01 0c`.
3. The probe sends the data on endpoint `0x82` in 64-byte packets, without a header and without an acknowledge.

A read of the whole 16 KB flash needs 256 packets. It takes about 0.2 s.
The command ends after `len` bytes. No end marker follows.

## Flash program (wlink)

wlink does not use DMI to write the flash. The probe has a flash algorithm.
The host sends a stub of 512 bytes (RISC-V code) first. The probe loads it into the RAM of the target.
The stub is specific to the chip family. The CH32V003 and the CH32V208 use two different stubs of 512 bytes. The probe runs it.

Sequence for a program of 16 KB on the CH32V003 (`v003-wlink-flash.pcapng`, 3.5 s).
wlink connects twice: it reads the chip information and the protection state, quits, and connects again.
The table starts at the second connect.

| Step | Direction | Content |
|---|---|---|
| 1 | out | `0d 01 02` connect, then `06 01 01` (query) and `06 01 04` |
| 2 | out | `01 08 08000000 00004000`: flash address and length |
| 3 | out | `02 01 05`: start of the stub transfer. Response `02 01 05` after 6.7 ms (CH32V208: 0.4 to 1.6 ms). |
| 4 | out `0x02` | The stub: 512 bytes, 8 packets |
| 5 | out | `02 01 07`: the probe takes about 17 ms (CH32V208: 1.6 to 8.9 ms). Response `02 01 07`. (Likely: prepare the flash. This is not confirmed.) |
| 6 | out | `02 01 02`: start of the data phase. Immediate response `02 01 02`. |
| 7 | out `0x02` | The image in blocks of 1024 bytes (16 packets). After each block the probe sends `41 01 01 04` on `0x82`. |
| 8 | out | `02 01 08`: end. Response `02 01 08`. |
| 9 | out | `0b 01 01`: reset and run (omitted for `wlink flash -R`) |
| 10 | out | `0d 01 ff`: quit |

Facts about the data phase:

- The host sends the whole range, also the blocks that hold only `0xff`. The captured image had 16 blocks of 1024 bytes.
- The host waits for the block acknowledge before the next block. A block takes about 136 ms, so the whole flash needs about 2.2 s.
- The block size depends on the chip: 1024 bytes on the CH32V003, 4096 bytes on the CH32V208 (see below).
- The image bytes go out in file order. They are not reversed (see [Byte order](#byte-order)).
- The flash captures hold no verify command and no memory read. The notes of epic #63 list a verify sub-command. No capture used it.

### CH32V208

The sequence is the same. The differences:

- Step 2 sends `01 08 08000000 00028000` (160 KB).
- The stub is a different 512-byte block.
- The data blocks have 4096 bytes (64 packets). The probe needs about 350 ms for each block.
- A program of 160 KB takes 15.2 s (about 10.8 KB/s).
- Erased flash reads as `0xe339e339`, not `0xff`. A `wlink dump` after `wlink erase` showed this. That dump is not in the capture set.

## Erase (`wlink erase`)

`06 01 01`, `06 01 04`, then `02 01 01` (response `02 01 01` after 7.6 ms on the CH32V003 and 138 ms on the CH32V208), then a new connect (`0d 01 02`, 90 ms and 59 ms), then quit.
The whole command took 0.19 s (CH32V003) and 0.26 s (CH32V208).
The erase command needs no data transfer and no stub.

## Reset (`0x0b`)

`0b 01 01` resets the target and lets it run. The response comes after about 15 ms (CH32V003) and 10 ms (CH32V208).
The `wlink reset` command waits 300 ms and then quits.

## Halt and resume

wlink and minichlink do both through DMI. No special command exists.

- **Halt.** The core is already halted when the host reads `dmstatus` after the connect command, because the connect command halts the core (see [Probe actions](#probe-actions-at-connect-at-chip-information-and-at-quit)).
  In `v003-wlink-halt.pcapng` the host writes `dmcontrol` `00000000` and `00000001`. It reads `dmcontrol` and `dmstatus`, writes `00000001`, and reads `dmstatus`.
  A halt request for a running core is not in the captures.
- **Resume.** In `v003-wlink-resume.pcapng` the host reads `dmcontrol`, writes `a0000001` and reads `dmstatus`.
  Then it writes `80000001` twice (`haltreq`), `00000001` and `40000001` (`resumereq`, bit 30). Then it reads `dmstatus` twice.

## Probe actions at connect, at chip information and at quit

The probe does its own accesses to the target. The host does not send them. A personality must do the same, or the host tools get a different target state.
The accesses below come from the wire captures. The decoders are `tools/wch_link_frames_2wire.py` (CH32V208) and `tools/wch_link_frames_1wire.py` (CH32V003).

### Connect (`0d 01 02`)

CH32V208 (`v208-wlink-status-timing.csv`, from 46 ms):

1. Write `0x7e` and `0x7d` with `5aa50400`.
2. Read `dmstatus`: `0x00000c82` (running). Write `dmcontrol` `80000001` (`haltreq`) twice. Read `dmstatus`: `0x00000382` (halted). So the connect command halts the core.
3. Read `0x7f`: `0x2080053c`, the chip ID.
4. Read memory `0x1ffff704` (`0x2080053c`). Write CSR `0x7c0` with `0x00000300` (command `0x002307c0`).
5. Read and write the registers at `0x40021000`, `0x40021004`, `0x40021008`, `0x4002102c` and `0x40023800`. It reads `0x1ffff70c` in between (`c33f0055`).
   The probe uses the abstract command for memory access: `data1` holds the address, `command` `0x02200000` reads, `0x02210000` writes, and `data0` holds the data.
   The register at `0x40021004` gets `00350000`, `00000000`, `00000400` (two times), `00340400` (two times) and `00340402`. It reads `0x0034040a` at the end.
   The register at `0x40021000` goes from `03038083` through `02028083`, `00008083` and `01008083` to `03008083`. The probe also writes `00ff0000` to `0x40021008`, `0` to `0x4002102c`, and `00000a50` to `0x40023800`.
   The probe appears to change the clock setup of the target. The meaning of the registers is not checked against the CH32V20x manual.
6. The connect response goes to the host about 59 ms after the command.

CH32V003 (`v003-wlink-status-timing.csv`). The line is slow and not decoded from 46 to 77 ms. A break pulse of 2 ms follows. Then, from 78.9 ms:

1. Write `0x7e` and `0x7d` with `5aa50400`. Read `0x7c` (`00010403`) and `0x7d` (`5aa50401`).
2. Read `dmstatus` (`004f0c82`, running). Write `dmcontrol` `80000001` twice. Read `abstractcs`. Read `dmstatus` (`004f0382`, halted).
3. Read `0x7f`: `0x00300500`, the chip ID.
4. Write `data1` with `0x1ffff7c4`. Write the program buffer (`progbuf0` to `progbuf7`, 8 words). Run it with the command `0x00040000` (execute the program buffer). Read `data0` (`00300500`).
5. Write CSR `0x7c0` with `0x00000300` (command `0x002307c0`).
6. The connect response goes to the host about 90 ms after the command.
7. Write `abstractcs` with `ffffffff` (at 89.9 ms, just before the chip information reads). It may belong to the `0c` command that follows the connect command. The captures do not show which command causes it.

The CH32V003 reads memory with a program in the program buffer (command `0x00040000`). The CH32V208 uses the abstract memory access (command `0x02200000`).

### Chip information, protection query and `0d 04`

The probe reads the target when the command arrives, not at connect:

| Command | Target reads (CH32V208) | Used for |
|---|---|---|
| `0x11` | The words at `0x1ffff7e0`, `0x1ffff7e8`, `0x1ffff7ec` and `0x1ffff7f0` (`ffff00a0`, `6213f0eb`, `2f30dc32`, `e339e339`) | The first 16 bytes of the answer |
| `06 01` | `0x4002201c` (`000000fc`) and `0x40022020` (`ffffffff`) | The protection state in the answer |
| `0d 04` | `0x1ffff802` (`0000003f`), with the command `0x02000000` (a read of one byte) | Not known. The answer is `00`. |

The evidence is the reply time. It grows with the speed byte, because the target reads run at the clock that `0c` sets:

| Command | Reply time at high, medium and low speed |
|---|---|
| `0x11` | 0.24 ms, 1.10 ms, 1.58 ms |
| `06 01` | 0.16 ms, 0.57 ms, 0.83 ms |
| `0d 04` | 0.14 ms, 0.30 ms, 0.43 ms |

The wire time from the read of `0x1ffff7e0` to the read of `0x4002201c` shows the same growth (0.24 ms, 1.13 ms, 1.64 ms).
The CH32V003 does the same reads for `0x11` and `06 01` at 90.3 to 91.2 ms (`ffff0010`, `6784abcd`, `cf99bc48`, `ffffffff`, then `0x4002201c` and `0x40022020`). It has no `0d 04` access.
A personality must read the target when the command arrives. An answer from a value that was read at connect can be out of date, for example after a protect and a reset.

### Quit (`0d 01 ff`)

The probe writes `dmcontrol` `40000001` (`resumereq`), reads `dmstatus` (`0x00030c82`: running, resume acknowledged) and writes `dmcontrol` `40000000`.
The USB capture shows no host command for this. Both wire captures show the same three accesses at the end (CH32V208: 62.7 to 62.9 ms. CH32V003: 92.96 to 93.07 ms).
So quit resumes the core. This is why a halted core runs again after `wlink status`.

## minichlink path

On the CH32V003, minichlink uses only commands `0x0d`, `0x0c`, `0x11`, `0x06` (query) and `0x08` (DMI).
It does not use `0x01`, `0x02` or `0x03` and does not use endpoint `0x02`.
It runs the flash write from the host with abstract commands and the program buffer.

On the CH32V208, minichlink uses the stub flow of wlink (it prints "binary blob write"):
`01` (area), `02 05`, the 512-byte stub on `0x02`, `02 07`, `02 02`, the data on `0x02`, then quit.
It differs from wlink in these points (`v208-mc-write.pcapng`):

- It sends the stub and the data in transfers of 256 bytes (2 transfers for the stub, 16 for the 4 KB), not 64 bytes.
- Its stub is not the stub of wlink (the two share 85 percent of the bytes). The first bytes differ. The probe runs the stub that the host sends, so the stub is not fixed in the probe.
- It does not read the block acknowledge and does not send `02 08`. It sends quit, and the probe answers after 0.3 s.
- It writes `dmcontrol` with `ndmreset` (`80000003`) before the transfer. A protection query just after that returned `01` (protected) on an unprotected chip. The reason is open.
- A write of 4 KB takes 0.66 s.

Consequences for the personality:

- A probe with the commands `0x0d`, `0x0c`, `0x11`, `0x06 01` and `0x08` is enough for minichlink on a CH32V003.
- This is a good first target (work item #68). A CH32V20x needs the commands `0x01` and `0x02` and endpoint `0x02` for both tools.
- On the CH32V003, a minichlink write of 16 KB took about 32 000 DMI round trips and 3.7 s, almost the same as wlink (3.5 s). The committed `v003-mc-write-head.pcapng` holds only the first 4000 frames. The full capture is not in the repository.

Observations:

- `minichlink -r file 0x08000000 16384` fails with "Exception executing abstract command". Reads of 4096, 8192 and 16000 bytes work. This is a minichlink or chip problem, not a probe problem. The capture `v003-mc-read.pcapng` uses 4096 bytes.
- minichlink sends `0d 03` before it does anything else.

## Debug module layer

The firmware does the probe actions with `src/rvswd/DebugModule.h`. This layer uses the DMI accesses of the wire engine (`src/rvswd/RvSwdEngine.h`).
It gives DMI read and write, DM reset, halt, resume, abstract register access and memory access.
The host tests are in `tests/TestDebugModule.cpp`. They use a mock DM (`tests/MockDebugModule.h`) with a small RV32EC interpreter for the program buffer.

### Sequences

| Operation | DMI accesses | Source |
|---|---|---|
| DM reset | Write `dmcontrol` `00000000`, then `00000001`. Read `dmcontrol` until `dmactive` is 1. Read `dmstatus`. | `v003-wlink-halt.pcapng` |
| Halt | Write `dmcontrol` `80000001` two times. Read `dmstatus` until `allhalted` (bit 9) is 1. `haltreq` stays set. | Connect of the probe |
| Resume | Write `dmcontrol` `40000001`. Read `dmstatus` until `allresumeack` (bit 17) or `allrunning` (bit 11) is 1. | The first two accesses of the quit of the probe |
| Register read | Write `command` `0022xxxx` (`xxxx` is the register number). Read `abstractcs`. Read `data0`. | Chip information reads |
| Register write | Write `data0`. Write `command` `0023xxxx`. Read `abstractcs`. | Connect (CSR `0x7c0`) |
| Memory read, abstract | Write `data1` (address). Write `command` `02200000` (word) or `02000000` (byte). Read `abstractcs`. Read `data0`. | CH32V208 |
| Memory write, abstract | Write `data1` (address) and `data0` (value). Write `command` `02210000`. Read `abstractcs`. | CH32V208 |
| Memory read, program buffer | Write `data1` (address). Write `command` `00040000`. Read `abstractcs`. Read `data0`. | CH32V003 |
| Memory write, program buffer | Write `data1` (address) and `data0` (value). Write `command` `00040000`. Read `abstractcs`. | Not captured |

The probe does not read `abstractcs` after a memory access command. The layer reads it after each command, until `busy` (bit 12) is 0. It then checks `cmderr` (bits 8 to 10).
This costs one frame for each access. A `cmderr` that is not 0 stops all later commands, so the layer clears it after an error.
The CH32V003 shows `abstractcs` `08000602` (`cmderr` 6) after the halt of the connect command. Thus the layer also clears `cmderr` before its first command.
It does this again after a DM reset and after each DMI write of the host.

The layer selects the memory access method from the wire. One wire (CH32V00x) uses the program buffer. Two wires (CH32V20x, CH32V30x) use the abstract memory access.
The program buffer method has 32-bit accesses only.

### Program buffer programs

The probe loads the read program at connect: it writes `abstractauto` `0` and `progbuf0` to `progbuf7`.
The words come from `v003-wlink-status-timing.csv`. A disassembly gives this program:

```
progbuf0  7b251073  csrw   dscratch0, a0
progbuf1  7b359073  csrw   dscratch1, a1
progbuf2  e0000537  lui    a0, 0xe0000
progbuf3  0f852583  lw     a1, 0xf8(a0)    # a1 = data1 (the address)
progbuf4  2a23418c  c.lw   a1, 0(a1)       # (low half) a1 = the word
                    sw     a1, 0xf4(a0)    # (high half and progbuf5) data0 = a1
progbuf5  25730eb5  csrr   a0, dscratch0   # (high half and progbuf6)
progbuf6  25f37b20  csrr   a1, dscratch1   # (high half and progbuf7)
progbuf7  90027b30  c.ebreak               # (high half)
```

The CH32V003 maps `data0` to the address `0xe00000f4` and `data1` to `0xe00000f8`. `hartinfo` reads `002120f4`.
The program keeps `a0` and `a1` in `dscratch0` and `dscratch1`, so the hart registers do not change.
A 32-bit instruction can start in one word and end in the next word.

No capture shows a memory write by the program buffer: the probe writes the flash with its stub (see [Flash program](#flash-program-wlink)).
The layer uses its own write program with the same layout:

```
7b251073 7b359073 e0000537 0f452583 0f852503 2573c10c 25f37b20 90027b30
csrw dscratch0, a0;  csrw dscratch1, a1;  lui a0, 0xe0000
lw a1, 0xf4(a0);  lw a0, 0xf8(a0);  c.sw a1, 0(a0)
csrr a0, dscratch0;  csrr a1, dscratch1;  c.ebreak
```

An assembler (`clang --target=riscv32 -march=rv32ec`) gave the same words for both programs.
The layer loads a program one time. It loads it again after a DM reset, after a DMI write of the host, or for the other program.

### Errors

| Status | Cause |
|---|---|
| `Wire` | The engine returned a flag: a parity error (two wires) or a line that stays low (one wire). |
| `NoTarget` | Two wires: the response bits and the tail bits are `1111`. No target drives them. One wire: a read without a target gives `ffffffff`. Thus each `dmstatus` read checks the version field (2 for spec 0.13). |
| `Timeout` | A poll of `dmcontrol`, `dmstatus` or `abstractcs` did not end after 100 reads. |
| `Command` | `cmderr` was not 0 after a command (for example 4: the hart is not halted). |
| `Unsupported` | A byte or half-word access with the program buffer method. |

A real target is necessary to confirm the write program, the halt and resume times, and the no-target detection.

## Byte order

Memory read data on endpoint `0x82` holds each 32-bit word with the most significant byte first.
The host reverses each group of 4 bytes to get the little-endian memory content.
Example: the word at `0x08000000` is `6f 00 20 12` in memory and arrives as `12 20 00 6f`. On the CH32V208, `6f 20 e0 42` arrives as `42 e0 20 6f`.
The words in the `0x11` answer also have the most significant byte first.
Flash write data does not use this reversal. The image goes out in file order.

## Timing seen on USB

| Operation | Time |
|---|---|
| Connect (`0d 01 02`) | CH32V003: about 90 ms. CH32V208: about 59 ms |
| DMI round trip | about 0.1 ms |
| Read of 16 KB (wlink) | about 0.2 s |
| Program of 16 KB (wlink, CH32V003) | 3.5 s, of this 2.2 s for the data blocks |
| Program of 160 KB (wlink, CH32V208) | 15.2 s |
| Erase (wlink) | 0.19 s (CH32V003), 0.26 s (CH32V208) |
| Protect, unprotect | CH32V003: 103 ms, 108 ms. CH32V208: 59 ms, 210 ms |

## Wire timing (CH32V208, two wires)

Source: `wch-link-captures/v208-wlink-status-timing.csv`. A Saleae Logic Pro recorded `wlink status` at 500 MS/s (2 ns).
Channel 0 is SWDIO (PA13). Channel 1 is SWCLK (PA14). Channel 2 is NRST.
The probe used its default speed (`0c … 01`).
The file has one row for each state change, with the time in seconds. Time zero is the first falling edge of SWDIO.
The logic threshold was not recorded.

Measured facts:

| Item | Value |
|---|---|
| NRST | Stays high for the whole run. `wlink status` does not pulse it. |
| SWCLK idle level | Low |
| SWCLK high time | about 1.0 µs (median 1004 ns) |
| SWCLK period, first part of a frame | about 3.08 µs (325 kHz) |
| SWCLK period, rest of a frame | about 2.09 µs (478 kHz, range 1.98 to 2.30 µs) |
| Idle time between 85-clock frames | about 7 µs |
| SWDIO change after SWCLK falling edge | median 62 ns (5th percentile 4 ns, 95th percentile 728 ns) |

Observations about the frames:

- During the first 46 ms of the connect command, the probe sends about 200 identical frames of 85 clocks. They repeat every 227 µs and end at 45.6 ms.
  Each frame has a first part of about 40 clocks at the slow period, and a longer part at the fast period.
  The first part holds the pattern `0010001` followed by 33 zero bits. The rest of the frame is high.
  The meaning of these frames is **not known**. They are not decoded.
- From 46 ms the probe sends frames of 54 clocks, about 120 µs apart. These frames are decoded (see [Wire frame format](#wire-frame-format)). They hold the probe actions, the target reads and the host accesses (see [Probe actions](#probe-actions-at-connect-at-chip-information-and-at-quit)).
  Some frames have a pause of about 7 µs after the start clock. A decoder must join the start clock with the rest of the frame.
- The connect command ends at about 59 ms. On USB, the connect response comes 59.4 ms after the command (`v208-wlink-status.pcapng`).
- After that, each DMI access from the host is one frame of 54 clocks. The spacing is 120 to 200 µs, so one access takes about 0.12 ms in total, with the USB round trip.

### Speed setting

`v208-wlink-status-low-timing.csv` and `v208-wlink-status-medium-timing.csv` are the same run with `wlink --speed low status` (speed byte `03`) and `--speed medium status` (`02`). Same channels and sample rate.

| Phase | Default speed (`01`) | Medium speed (`02`) | Low speed (`03`) |
|---|---|---|---|
| Connect, 85-clock frames (first 46 ms) | periods 3.08 µs and 2.09 µs | The same | The same |
| Host DMI frames of 54 clocks | Some with a period of about 1.10 µs (frame time 63 µs), some with about 2.09 µs (115 µs) | The same as default | All with about 2.09 µs (111 to 115 µs) |
| Target reads for `0x11`, `06 01` and `0d 04` (after 59.6 ms) | Three long bursts (648, 324, 162 clocks) with a period of about 120 ns (minimum 60 ns) | Seven bursts of 162 clocks (251 µs) with a period of about 1.12 µs | Frames of 54 clocks with a period of about 2.09 µs, spread over 59.6 to 62.4 ms |

Conclusions:

- The speed byte does not change the clock during the connect command. The probe runs the chip detection at a fixed rate.
- The speed byte changes the clock of the later accesses. Only the long bursts change between the default and the medium speed (about 120 ns against about 1.12 µs). At low speed, the frames take about 111 to 115 µs. The 400 kHz of the wlink help text is not the measured rate (the measured rate is about 478 kHz).
- At the default speed, the clock is faster in some frames. The long bursts with a period near 120 ns hold 12, 6 and 3 frames of 54 clocks without a pause. They are the target reads for `0x11`, `06 01` and `0d 04` (see [Probe actions](#probe-actions-at-connect-at-chip-information-and-at-quit)).

## Wire timing (CH32V003, one wire)

Source: `wch-link-captures/v003-wlink-status-timing.csv`. The same Saleae Logic Pro recorded `wlink status` at 500 MS/s.
Channel 0 is SWIO (PD1). Channels 1 and 2 were not connected (constant 0 in the file).
The probe used its default speed. The run took 93 ms.

Measured facts:

| Item | Value |
|---|---|
| Idle level | High |
| First 45.8 ms (connect) | About 200 identical frames, one every 227 µs |
| One frame | low 8.2 µs, high 3.1 µs, low 9.2 µs, high 3.1 µs, low 101.6 µs, high 94.5 µs, low 4.0 µs, high 3.3 µs |
| After 45.8 ms | A different pattern with pulses of 0.2 to 4.2 µs (low 0.8 µs and high 0.4 µs are the most common). Frames repeat about every 1.56 ms. |
| End of activity | 93 ms |

Comparison with the two-wire capture of the CH32V208 (same probe, same speed):

- The frame time is the same: 227 µs.
- The first part of the frame has the same edge times as SWDIO of the CH32V208: low 8.2 µs, high 3.1 µs, low 9.2 µs, high 3.1 µs, low 101.5 µs.
  The low time of 101.5 µs is 33 bit times of 3.08 µs, the first-part period of the CH32V208 clock.
  So the one-wire line carries the same bit sequence as SWDIO. The bit period is 3.08 µs, and no clock line exists.
- The rest of the frame differs. The CH32V003 line stays high for 94.5 µs (45 bit times of 2.09 µs). The CH32V208 target pulls the line low in places.
  The two targets answer differently, or the CH32V003 does not answer this frame. This is **not confirmed**.
- The later phase of the CH32V003 (after 45.8 ms) is slower than the later phase of the CH32V208. The connect command of the CH32V003 also takes longer (90 ms against 59 ms).

### Speed setting (CH32V003)

`v003-wlink-status-low-timing.csv` and `v003-wlink-status-medium-timing.csv` are the same run with `--speed low` (`03`) and `--speed medium` (`02`).

The speed byte has no visible effect on the one-wire line:

- The first frame has the same pulse widths in all three captures (low 8.2 µs, high 3.1 µs, low 9.2 µs, high 3.1 µs, low 101.5 µs, high 94.4 µs).
- The start of the later phase is the same (45.8 ms).
- The most common pulses of the later phase are the same (low 0.8 µs and 0.2 µs).
- The run time is the same (93 ms).

`v003-wlink-dump-timing.csv` and `v003-wlink-dump-low-timing.csv` hold `wlink dump 0x08000000 256` at the default speed and at `--speed low`.
They also match each other. The first frame is the same. The pulse widths of the later phase are the same. The long gaps (1.2 ms and 4.9 ms) are the same.
The run is 95 ms in both captures, and the dump adds about 3800 edges to the status run.

The probe may ignore the speed byte for the one-wire interface, or the speed applies to a part that this test does not use.
The cause is not known. A flash program at the three speeds could show a difference. This was not captured (a program of 16 KB needs 3.5 s).

Not measured on the CH32V003: the wire timing of a flash program, and the slow pattern from 46 to 77 ms.

Not measured on the CH32V208: the wire timing of a flash program. The meaning of the 85-clock frames is open. The bits that the table below marks as open are open too.

## Wire frame format

The two-wire captures show one frame format on the wire, after the 85-clock frames. A frame has 54 clocks. SWDIO is stable at the rising edge of SWCLK. Bit 0 is the first clock.
The decoder is `tools/wch_link_frames_2wire.py`. It works on the CSV files of the CH32V208. It skips the 85-clock frames and counts them.
The format was checked against the 15 DMI accesses that the USB capture of the same run shows. The address and the data of every access match, and the order matches.
The decoder decodes 147 frames of the status run, and the parity is right in all of them.
It skips bursts that are not a multiple of 54 clocks (the 85-clock frames and a few short bursts) and counts them.

| Bits | Content | Driven by |
|---|---|---|
| 0 | The rising edge of SWCLK from the idle level, with SWDIO high. After this edge, SWDIO falls while SWCLK is high: the start condition. | Probe |
| 1 to 7 | DM register address, MSB first | Probe |
| 8 | Write bit: `1` for a write, `0` for a read | Probe |
| 9 | Parity of bits 1 to 8: `1` if the number of ones is odd | Probe |
| 10 | The level of bit 9. SWDIO changes direction after the rising edge. | Probe |
| 11 to 14 | Four response bits: `0000` or `0100` | Target |
| 15 to 46 | Data, MSB first | Probe (write), target (read) |
| 47 | Parity of the 32 data bits: `1` if the number of ones is odd | Probe (write), target (read) |
| 48 | The level of bit 47. SWDIO changes direction after the rising edge (write). | Probe (write), target (read) |
| 49 to 52 | Four tail bits: `0011` or `0111` | Target |
| 53 | Always `0`. After this edge, SWDIO rises while SWCLK is high: the stop condition. | Probe |

The frame is a start condition, 53 clocks and a stop condition. The start and the stop condition are the same as on an I2C bus.
The table numbers the bits as the decoder does: bit 0 is the clock before the start condition.
Bits 1 to 53 are the clocks between the start and the stop condition.
The rules for bits 9, 10, 47, 48 and 53 hold in all 440 frames of the three status captures (default, medium and low speed).

Observations:

- **Operation field (bits 8 and 9).** Bit 8 tells a write from a read. Bit 9 is a parity bit over the address and bit 8. This explains the values `11`, `10`, `01` and `00` of the earlier decoder output.
- **Response bits (11 to 14).** `0000` after all writes. `0100` after most reads, `0000` after the reads of `dmstatus` and `hartinfo`. Their meaning is open (acknowledge or status).
- **Parity (bits 47 and 48).** Bit 47 is the parity. Bit 48 keeps the level of bit 47 for one more clock, as bit 10 keeps the level of bit 9.
- **Tail bits (49 to 52).** `0011` follows all writes. After reads, `0111` or `0011` follows. The tail does not tell a read from a write. The meaning is open.
- **Who drives a bit.** The probe changes SWDIO about 70 to 90 ns after the falling edge of SWCLK. The target changes SWDIO about 4 to 14 ns after the falling edge.
  Bits 1 to 9 and 53 follow the probe pattern. Bits 11 to 14 and 49 to 52 follow the target pattern. Bits 15 to 47 follow the probe pattern in a write and the target pattern in a read.
  SWDIO changes while SWCLK is high in bits 10 and 14, and in bit 48 of a write: the line changes direction there.
- **Longer low phases.** The low phase before bits 1, 15 and 53 is about two half periods: 2.1 to 2.2 µs at the 2.09 µs clock. SWDIO changes direction before bits 15 and 53.
- **Idle.** Between frames, SWCLK is low and SWDIO is high. The probe keeps SWCLK high for about 1 to 5 µs after the stop condition.
  Before the start condition, SWCLK rises and stays high for about 1 to 8 µs.
- **Before the first frame.** After the 85-clock frames, the probe sends 9 clocks with SWDIO high, one clock with SWDIO low, and 90 clocks with SWDIO high. Then the first frame starts. This can be a line reset. This is **not confirmed**.
- **Clock for each access.** At the default speed, a write uses 1.10 µs for each bit (about 907 kHz). A read uses 2.09 µs (about 478 kHz). At low speed, all accesses use 2.09 µs.
- **Fast triples.** The long bursts at about 120 ns for each bit (default speed) hold frames in the order `data1` write, `command` write, `data0` read. These are the target reads for `0x11`, `06 01` and `0d 04`. The values of the first four reads match the `0x11` answer byte for byte.
- **Connect frames.** The 85-clock frames of the connect phase do not follow this format. They are not decoded.

### One-wire frame format (CH32V003)

The one-wire line (SWIO) does not carry the 54-bit frame of the two-wire interface. It carries a shorter frame of 41 bits.
Each bit is one low pulse followed by a high level. The width of the low pulse gives the value:

| Bit | Low pulse | High level after it | Bit time |
|---|---|---|---|
| 1 | about 250 ns | about 370 ns | about 0.62 µs |
| 0 | about 860 ns | about 370 ns | about 1.23 µs |

A frame lasts 27 to 48 µs (median 45 µs). A frame of only 1 bits is the shortest. Inside the burst of target reads, the next frame starts 4 to 12 µs after the end of a frame (median 7 µs).
Between host accesses, the gap is 51 to 106 µs (median 61 µs). The connect phase has other pulse patterns (see above).
The decoder is `tools/wch_link_frames_1wire.py`. It checks the start bit of each frame. A low pulse longer than 1.5 µs is a break, not a bit.
It decodes the host phase of the status runs. It does not decode all frames of the `dump` captures.
The first burst decodes 12 frames (`abstractauto`, `data1`, `progbuf0` to `progbuf7`, `command`, `data0`). Each later burst decodes 4 frames (`abstractauto`, `data1`, `command`, `data0`). Then the frames fall out of step. The decoder reports the pulses it could not decode.
The cause is open. The frame length may differ for some access types.

| Bits | Content |
|---|---|
| 0 | Start, always `1` |
| 1 to 7 | DM register address, MSB first |
| 8 | Operation: `1` is a write, `0` is a read |
| 9 to 40 | Data, MSB first |

The frame has no response bits, no parity and no tail. The two-wire frame has 13 more bits: the address parity, its repeat, 4 response bits, the data parity, its repeat, 4 tail bits and the stop bit.
The one-wire line may carry them in another way, or the probe may not send them. This is open.

The format was checked against the USB capture of the same type of run. The 15 accesses of the host match in order and in address.
The values match, except for `dmstatus`. It reads `0x004f0382` on the wire and `0x004c0382` in the USB capture of an earlier run. The bits 16 to 19 change between runs.
The wire capture has three more accesses at the end. They are the resume that the probe does at quit (see [Probe actions](#probe-actions-at-connect-at-chip-information-and-at-quit)).

Pulse widths are the same for the speeds `01`, `02` and `03`. The nominal widths are the same for reads and writes.

More pulse details (`v003-wlink-status-timing.csv`, from 78.8 ms):

- In a read, the high level after bit 8 is about 750 ns, not 370 ns. The line changes direction there.
- In the data bits of a read, the widths spread more: a low pulse of 246 to 374 ns for a 1, 824 to 868 ns for a 0, and a high level of 262 to 538 ns. In the data bits of a write, the widths are constant (248 ns, 864 ns and 368 ns).
  The spread shows that the target sets the length of the low pulses in a read. The logic analyzer cannot show which device pulls the line low at the start of a bit.
- The shortest gap between two frames is 3.7 µs.
- The line is low for 1.98 ms just before the first frame. Before that low level, the line is high for 0.99 ms. The `dump` capture shows the same. This can be a reset of the debug interface. This is **not confirmed**.

## Pins and USB ID

| Target | Pin on target | Probe signal |
|---|---|---|
| CH32V003 | PD1 (SWIO) | One wire, on the SWDIO signal. The drive type is not known (a logic analyzer cannot show it). |
| CH32V20x | PA13 (SWDIO), PA14 (SWCLK) | SWDIO and SWCLK (confirmed on a CH32V208 with a WCH-LinkE) |

The board definitions (`src/board/BoardDefinition.h`) say: SWCLK is the TCK pin, and SWDIO is the TMS pin. `src/board/BoardSwd.h` uses them for SWD.
The pins on the dongle:

| Board | SWCLK (TCK) | SWDIO output (TMS out) | SWDIO input (TMS in) | NRST (SRST) |
|---|---|---|---|---|
| Mini, OtherUse | PB13 | PB4 | PB6 | PB11 |
| ST-Link/V2 | PA5 | PB14 | PB14 | PB0 |

The one-wire interface of the CH32V003 uses the SWDIO signal. SWCLK stays unused. The pins are decided: no new pin is needed.
This was not tested on a dongle. The line of a pulse must go high in about 370 ns.
`board::SwdPins::swdioRelease()` releases the line to a pull-up input. A pull-up can be too slow for this time.
Thus the engine (`src/rvswd/RvSwdEngine.h`, pins in `src/board/BoardRvSwd.h`) drives the line high in the high level of each bit and when the line is idle.
In a read bit, the engine releases the line after its short low pulse. The target can then hold the line low for a 0.
The engine samples the line 600 ns after the falling edge. When the line is high again, the engine drives it high.
The engine does not drive the line high while the line is low. Thus it does not fight a target that holds the line low.

Risk for a read bit: only the internal pull-up raises the line for a 1. The STM32F1 pull-up is 30 to 50 kΩ.
With 20 to 50 pF of line and target pin, the time constant is 0.6 to 2.5 µs. At the sample point (600 ns), the line can still be below the input high level, and each 1 then reads as 0.
The capture shows a fast rise after a 1 of the target (low for 246 to 374 ns). Thus the WCH-LinkE or the target drives the line high.
A hardware test must measure the rise time on a dongle. If the rise is too slow, use one of these fixes:

- An external pull-up of about 1 kΩ on SWDIO.
- A later sample point, from the measured rise time. The shortest 0 of the target is 824 ns, so the sample point must stay below that time.

USB ID: the decision of the epic stays. The default is an own ID. The ID `1a86:8010` is a build option for personal use.
wlink and minichlink find the probe by this ID. Without it, they need a flag or a patch.

## Open questions

The table lists the questions of epic #63 and their state.

| Question | State |
|---|---|
| Payload of the program sub-commands | Answered for `02 01 05/07/02/08`. The meaning of `05` and `07` is a guess (see above). minichlink does not send `08`. |
| Order of the flash sequence | Answered (see [Flash program](#flash-program-wlink)). |
| Does the probe or the host hold the flash algorithm? | Both. The host sends a 512-byte stub per chip family. The probe runs it. minichlink uses the stub on the CH32V208. On the CH32V003 it writes by DMI and uses no stub. |
| Wire timing of the one-wire and the two-wire interface | **Mostly answered.** See [two-wire timing](#wire-timing-ch32v208-two-wires) and [one-wire timing](#wire-timing-ch32v003-one-wire), both at three speeds. Open: the response and parity bits on the one-wire line, and the timing of a flash program. |
| USB protocol on a CH32V20x | Answered with a CH32V208. It matches the CH32V003 protocol (see [CH32V208](#ch32v208)). |
| Probe variant check by wlink and minichlink | wlink prints the variant from byte 2 of the info response. It did not reject the probe. A check of other variants is open. |
| Error reason codes | **Open.** Seen: `0x55` (no target). The meaning of other codes is open. |
| Verify sub-command (`02 01 06`?) | **Open.** No capture used it. |
| Meaning of `0d 03`, `0d 04`, `0d 05`, `06 01 04`, `11 01 05/06`, `02 01 05/07` | **Open.** The observed behavior is in this document. |
| Does the probe do target accesses of its own? | Answered: the connect command halts the core. The commands `0x11`, `06 01` and `0d 04` read the target. The quit command resumes the core (see [Probe actions](#probe-actions-at-connect-at-chip-information-and-at-quit)). |
| Meaning of the 85-clock frames, the response bits, the tail bits, and the failure of one-wire frames in the `dump` captures | **Open.** The operation field values are answered: a write bit and a parity bit (see [Wire frame format](#wire-frame-format)). |
| `0x0e` (disable debug) | **Open.** No capture used it. |
