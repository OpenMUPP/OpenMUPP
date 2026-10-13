# WCH-LinkE protocol

This document describes the USB protocol of the WCH-LinkE in RISC-V mode.
The `wch-link` personality (epic #63) must speak this protocol.
All facts come from USB captures of a real probe. No vendor code was copied.

Status: the CH32V003 part is complete. The CH32V20x part and the wire timing are open (see [Open questions](#open-questions)).

## Capture setup

| Item | Value |
|---|---|
| Probe | WCH-LinkE, USB ID `1a86:8010`, `bcdDevice` 2.19, product string `WCH-Link` |
| Probe type | wlink reports `WCH-Link v2.19(v39) (WCH-LinkE-CH32V305)`. The info response holds `02 13 12 00` (see [Command 0x0d](#command-0x0d-info-connect-quit)). The source of `v39` is open |
| Target | CH32V003F4P6 (16 KB flash), chip ID `0x00300500`, connected through the one-wire interface |
| Host tools | wlink 0.1.2 and minichlink (a local build, version not recorded) |
| Capture | Linux `usbmon` on the bus of the probe, filtered to the probe address |

The captures are in [`wch-link-captures/`](wch-link-captures/).
Each file has the traffic of one tool command.
The file name gives the tool and the command, for example `wlink-flash.pcapng`.
`mc-write-head.pcapng` holds only the first 4000 frames of the minichlink write.
The full capture had 13 MB.

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

- **Error.** An unknown command gives `82 81 01 ff`. The second byte is `0x81`, not the command. The payload is one reason byte (`0xff` here).
  The captures contain no other error. The reason codes are open.
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
The public notes of the epic also mention `0x0e` (disable debug). The captures did not use it.

## Command `0x0d`: info, connect, quit

| Sub-command | Meaning | Response payload |
|---|---|---|
| `01` | Probe information | `02 13 12 00` |
| `02` | Connect to the target (RISC-V) | `09 00 30 05 00` |
| `03` | Unknown. minichlink sends it first. | `03` |
| `ff` | Quit. Release the target. | `ff` |

- **Probe information.** Byte 0 and byte 1 are the major and minor version of the firmware (2 and 19). This matches `bcdDevice` 2.19. Byte 2 (`0x12`) is the probe type. wlink maps it to `WCH-LinkE-CH32V305`. Byte 3 is `0x00`.
- **Connect.** The probe starts the debug interface and does the chip detection. This takes about 90 ms after a normal start. After an erase or a protect command it takes about 900 ms. Byte 0 is a chip family code (`0x09` for the CH32V003). Bytes 1 to 4 are the chip ID (u32 BE, `0x00300500`).
- **Quit.** The host sends this at the end of every session. A session is connect to quit.
- **Sub-command `03`.** The probe needs about 74 ms to answer. The reason is not known.
- Any other sub-command value is echoed (`0d 01 05` gives `82 0d 01 05`). The probe does not report an error.

## Command `0x0c`: configure

Request payload: `chip-type speed`.

- wlink sends `01 speed` before the connect command and `family speed` after it. Here `family` is the first byte of the connect response (`09`). minichlink sends only the second form.
- The speed byte: `01` is high (the default), `02` is medium (wlink: 4000 kHz), `03` is low (wlink: 400 kHz). Captures of `wlink --speed low|medium|high` show this.
- The probe answers `01`.

## Command `0x11`: chip information

Request payload: `06` (wlink) or `05` (minichlink). The answer was the same for both on the CH32V003.

| Bytes | Content on the CH32V003 |
|---|---|
| 0 to 3 | `ff ff 00 10`. Bytes 2 and 3 (u16 BE) give the flash size in KB (16). |
| 4 to 11 | Unique ID. Each 4-byte group is in reversed byte order, as in the memory read path (see [Byte order](#byte-order)). |
| 12 to 15 | `ff ff ff ff`. minichlink calls this field `PFlags`. |
| 16 to 19 | Chip ID (u32 BE, `0x00300500`). |

The difference between sub-command `05` and `06` is open.

## Command `0x06`: read protection

| Sub-command | Meaning | Response payload |
|---|---|---|
| `01` | Query | `02` unprotected, `01` protected |
| `02` | Unprotect | `02` (about 1.1 s) |
| `03` | Protect | `03` (about 1.0 s) |
| `04` | Unknown. wlink sends it after a query that returned `02`. | `00` |

Facts about the protection:

- Protect takes effect only after a reset. A query directly after `wlink protect` still returns `02`. After `wlink reset`, it returns `01`.
- The unprotect command erases the flash of the chip. wlink `flash` sends it by itself when the query returns `01`.
- A memory read of a protected chip does not fail. It returns the word `0x00000300` repeated (see `wlink-dump-protected.pcapng`).
- DMI access still works on a protected chip (`wlink-status-protected.pcapng`).

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
- `02`: failure (public notes). The captures show no failure.

Registers seen on the CH32V003:

| Address | Name | Note |
|---|---|---|
| `0x04`, `0x05` | `data0`, `data1` | |
| `0x10` | `dmcontrol` | |
| `0x11` | `dmstatus` | |
| `0x12` | `hartinfo` | |
| `0x16` | `abstractcs` | |
| `0x17` | `command` | abstract command |
| `0x18` | `abstractauto` | |
| `0x20` to `0x27` | `progbuf0` to `progbuf7` | minichlink uses `progbuf0` to `progbuf2` |
| `0x40` | `haltsum0` | |
| `0x7d`, `0x7e` | WCH-specific | minichlink writes `5aa50400` at start-up |

The register layout follows the RISC-V debug specification.
The probe does not change the value of a register on the way.

A DMI round trip takes about 0.1 ms on the USB bus.
A real DMI access on the wire may take longer (see [Open questions](#open-questions)).

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
The stub is specific to the chip family. The probe runs it.

Sequence for a program of 16 KB (`wlink-flash.pcapng`, 3.5 s).
wlink connects twice: it reads the chip information and the protection state, quits, and connects again.
The table starts at the second connect.

| Step | Direction | Content |
|---|---|---|
| 1 | out | `0d 01 02` connect, then `06 01 01` (query) and `06 01 04` |
| 2 | out | `01 08 08000000 00004000`: flash address and length |
| 3 | out | `02 01 05`: start of the stub transfer. Response `02 01 05` after 67 ms. |
| 4 | out `0x02` | The stub: 512 bytes, 8 packets |
| 5 | out | `02 01 07`: the probe takes about 170 ms. Response `02 01 07`. (Likely: erase and prepare. This is not confirmed.) |
| 6 | out | `02 01 02`: start of the data phase. Immediate response `02 01 02`. |
| 7 | out `0x02` | The image in blocks of 1024 bytes (16 packets). After each block the probe sends `41 01 01 04` on `0x82`. |
| 8 | out | `02 01 08`: end. Response `02 01 08`. |
| 9 | out | `0b 01 01`: reset and run (omitted for `wlink flash -R`) |
| 10 | out | `0d 01 ff`: quit |

Facts about the data phase:

- The host sends the whole range, also the blocks that hold only `0xff`. The captured image had 16 blocks of 1024 bytes.
- The host waits for the block acknowledge before the next block. A block takes about 136 ms, so the whole flash needs about 2.2 s.
- The image bytes go out in file order. They are not reversed (see [Byte order](#byte-order)).
- The host did not send a verify command. wlink checks the result with a separate memory read. The probe has a verify sub-command in the public notes, but no capture used it.

## Erase (`wlink erase`)

`06 01 01`, `06 01 04`, then `02 01 01` (about 75 ms, response `02 01 01`), then a new connect (`0d 01 02`, about 900 ms), then quit.
The whole command took 0.19 s.
The erase command needs no data transfer and no stub.

## Reset (`0x0b`)

`0b 01 01` resets the target and lets it run. The response comes after about 160 ms.
The `wlink reset` command waits 300 ms and then quits.

## Halt and resume

wlink and minichlink do both through DMI. No special command exists.

- **Halt.** The core is already halted when wlink reads `dmstatus` after the connect command. In `wlink-halt.pcapng` the host writes only `dmactive` (`00000001`) and reads `dmstatus`. A halt of a running core (`haltreq`, bit 31) is open.
- **Resume.** wlink writes `dmcontrol` four times: `80000001` twice (`haltreq`), `00000001` (clear), `40000001` (`resumereq`, bit 30). Then it reads `dmstatus` twice and checks `allresumeack`.

## minichlink path

minichlink uses only commands `0x0d`, `0x0c`, `0x11`, `0x06` (query) and `0x08` (DMI).
It does not use `0x01`, `0x02` or `0x03` and does not use endpoint `0x02`.
It runs the flash write from the host with abstract commands and the program buffer.

Consequences for the personality:

- A probe with the commands `0x0d`, `0x0c`, `0x11`, `0x06 01` and `0x08` is enough for minichlink.
- This is a good first target (work item #68). The commands `0x01`, `0x02` and `0x03` are only needed for wlink.
- minichlink needs about 32 000 DMI round trips to write 16 KB. This took 3.7 s, almost the same as wlink (3.5 s).

Observations:

- `minichlink -r file 0x08000000 16384` fails with "Exception executing abstract command". Reads of 4096, 8192 and 16000 bytes work. This is a minichlink or chip problem, not a probe problem. The capture `mc-read.pcapng` uses 4096 bytes.
- minichlink sends `0d 03` before it does anything else.

## Byte order

Memory read data on endpoint `0x82` has the bytes of each 32-bit word in reversed order.
The host reverses each group of 4 bytes to get the memory content.
Example: the word at `0x08000000` is `6f 00 20 12` in memory and arrives as `12 20 00 6f`.
The unique ID in the `0x11` answer uses the same order.
Flash write data does not use this reversal. The image goes out in file order.

## Timing seen on USB

| Operation | Time |
|---|---|
| Connect (`0d 01 02`) | about 90 ms (about 900 ms after erase or protect) |
| DMI round trip | about 0.1 ms |
| Read of 16 KB (wlink) | about 0.2 s |
| Program of 16 KB (wlink) | 3.5 s, of this 2.2 s for the data blocks |
| Erase (wlink) | 0.19 s |
| Protect, unprotect | about 1.0 s, about 1.1 s |

## Pins and USB ID

| Target | Pin on target | Probe signal |
|---|---|---|
| CH32V003 | PD1 (SWIO) | One wire. The probe signal is SWDIO (open drain, with pull-up). |
| CH32V20x | PA13 (SWDIO), PA14 (SWCLK) | SWDIO and SWCLK |

The Versaloon board definitions (`src/board/boards/`) have no SWD pin names. They have JTAG pins.
On the Mini, this mapping is proposed (derived from the JTAG pins; not tested):

| Signal | Pin | Source |
|---|---|---|
| SWDIO (output) | PB4 (TMS out) | `Mini.h` |
| SWDIO (input) | PB6 (TMS in) | `Mini.h` |
| SWCLK | PB13 (TCK) | `Mini.h` |

The single wire of the CH32V003 would use the SWDIO pair.
The wire level of the single-wire interface is open (needs a logic analyzer). The pin choice is not confirmed on hardware.

USB ID: the decision of the epic stays. The default is an own ID. The ID `1a86:8010` is a build option for personal use.
wlink and minichlink find the probe by this ID. Without it, they need a flag or a patch.

## Open questions

The table lists the questions of epic #63 and their state.

| Question | State |
|---|---|
| Payload of the program sub-commands | Answered for `02 01 05/07/02/08`. The meaning of `05` and `07` is a guess (see above). |
| Order of the flash sequence | Answered (see [Flash program](#flash-program-wlink)). |
| Does the probe or the host hold the flash algorithm? | Both. The host sends a 512-byte stub per chip family. The probe runs it. minichlink uses no stub. |
| Wire timing of the one-wire interface | **Open.** Needs a logic analyzer. |
| Two-wire (RVSWD) behavior, CH32V20x | **Open.** Needs a V207 board. |
| Probe variant check by wlink and minichlink | wlink prints the variant from byte 2 of the info response. It did not reject the probe. A check of other variants is open. |
| Error reason codes | **Open.** Only one error seen (`ff` for an unknown command). |
| Verify sub-command (`02 01 06`?) | **Open.** No capture used it. |
| Meaning of `0d 03`, `06 01 04`, `11 01 05/06` | **Open.** The observed behavior is in this document. |
| `0x0e` (disable debug) | **Open.** No capture used it. |
