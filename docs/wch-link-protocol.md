# WCH-LinkE protocol

This document describes the USB protocol of the WCH-LinkE in RISC-V mode.
The `wch-link` personality (epic #63) must speak this protocol.
All facts come from USB captures of a real probe. No vendor code was copied.

Status: the USB protocol is captured on a CH32V003 (one wire) and on a CH32V208 (two wires).
The wire timing is open (see [Open questions](#open-questions)).

## Capture setup

| Item | Value |
|---|---|
| Probe | WCH-LinkE, USB ID `1a86:8010`, `bcdDevice` 2.19, product string `WCH-Link` |
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
  wlink retries four times (about 100 ms apart) and then stops (`v208-wlink-connect-fail.pcapng`).
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
The public notes of the epic also mention `0x0e` (disable debug). The captures did not use it.

## Command `0x0d`: info, connect, quit

| Sub-command | Meaning | Response payload |
|---|---|---|
| `01` | Probe information | `02 13 12 00` |
| `02` | Connect to the target (RISC-V) | `09 00 30 05 00` (CH32V003), `05 20 80 05 3c` (CH32V208) |
| `03` | Unknown. minichlink sends it first. | `03` |
| `04` | Unknown. wlink sends it on the CH32V208 after the first protection query. | `00` |
| `ff` | Quit. Release the target. | `ff` |

- **Probe information.** Byte 0 and byte 1 are the major and minor version of the firmware (2 and 19). This matches `bcdDevice` 2.19. Byte 2 (`0x12`) is the probe type. wlink maps it to `WCH-LinkE-CH32V305`. Byte 3 is `0x00`.
- **Connect.** The probe starts the debug interface and does the chip detection. This takes about 90 ms on the CH32V003 after a normal start and about 900 ms after an erase or a protect command. On the CH32V208 it takes about 60 ms. Byte 0 is a chip family code (`0x09` for the CH32V003, `0x05` for the CH32V208). Bytes 1 to 4 are the chip ID (u32 BE: `0x00300500`, `0x2080053c`).
- **Quit.** The host sends this at the end of every session. A session is connect to quit.
- **Sub-command `03`.** The probe needs about 74 ms to answer. The reason is not known.
- Any other sub-command value is echoed (`0d 01 05` gives `82 0d 01 05`). The probe does not report an error.

## Command `0x0c`: configure

Request payload: `chip-type speed`.

- wlink sends `01 speed` before the connect command and `family speed` after it. Here `family` is the first byte of the connect response (`09` or `05`).
  With the generic code `01`, the connect command still works on both chips. minichlink sends only the second form.
- The speed byte: `01` is high (the default), `02` is medium (wlink: 4000 kHz), `03` is low (wlink: 400 kHz). Captures of `wlink --speed low|medium|high` show this.
- The probe answers `01`.

## Command `0x11`: chip information

Request payload: `06` (wlink) or `05` (minichlink). The answer was the same for both on both chips.

| Bytes | Content (CH32V003 / CH32V208) |
|---|---|
| 0 to 3 | `ff ff 00 10` / `ff ff 00 a0`. Bytes 2 and 3 (u16 BE) give the flash size in KB (16 / 160). |
| 4 to 11 | Unique ID. Each 4-byte group is in reversed byte order, as in the memory read path (see [Byte order](#byte-order)). |
| 12 to 15 | `ff ff ff ff` / `e3 39 e3 39`. minichlink calls this field `PFlags`. |
| 16 to 19 | Chip ID (u32 BE, `0x00300500` / `0x2080053c`). |

The difference between sub-command `05` and `06` is open.

## Command `0x06`: read protection

| Sub-command | Meaning | Response payload |
|---|---|---|
| `01` | Query | `02` unprotected, `01` protected |
| `02` | Unprotect | `02` (CH32V003: about 1.1 s. CH32V208: about 2.1 s) |
| `03` | Protect | `03` (CH32V003: about 1.0 s. CH32V208: about 0.6 s) |
| `04` | Unknown. wlink sends it after a query that returned `02`. | `00` |

Facts about the protection:

- Protect takes effect only after a reset. A query directly after `wlink protect` still returns `02`. After `wlink reset`, it returns `01`.
- The unprotect command erases the flash of the chip. wlink `flash` sends it by itself when the query returns `01`.
- A memory read of a protected chip does not fail. It returns a fixed word, repeated. The word is `0x00000300` on the CH32V003 (`v003-wlink-dump-protected.pcapng`). On the CH32V208 the bytes arrive as `00 34 04 0a` (`v208-wlink-dump-protected.pcapng`).
- DMI access still works on a protected chip (`v003-wlink-status-protected.pcapng`).
- The CH32V208 behaves the same: protect, reset, query returns `01`, unprotect erases the flash.

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
The stub is specific to the chip family. The CH32V003 and the CH32V208 use two different stubs of 512 bytes. The probe runs it.

Sequence for a program of 16 KB on the CH32V003 (`v003-wlink-flash.pcapng`, 3.5 s).
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
- The block size depends on the chip: 1024 bytes on the CH32V003, 4096 bytes on the CH32V208 (see below).
- The image bytes go out in file order. They are not reversed (see [Byte order](#byte-order)).
- The host did not send a verify command. wlink checks the result with a separate memory read. The probe has a verify sub-command in the public notes, but no capture used it.

### CH32V208

The sequence is the same. The differences:

- Step 2 sends `01 08 08000000 00028000` (160 KB).
- The stub is a different 512-byte block.
- The data blocks have 4096 bytes (64 packets). The probe needs about 350 ms for each block.
- A program of 160 KB takes 15.2 s (about 10.8 KB/s).
- Erased flash reads as `0xe339e339`, not `0xff`.

## Erase (`wlink erase`)

`06 01 01`, `06 01 04`, then `02 01 01` (about 75 ms, response `02 01 01`), then a new connect (`0d 01 02`, about 900 ms), then quit.
The whole command took 0.19 s (CH32V003) and 0.26 s (CH32V208). The erase step `02 01 01` takes about 140 ms on the CH32V208.
The erase command needs no data transfer and no stub.

## Reset (`0x0b`)

`0b 01 01` resets the target and lets it run. The response comes after about 160 ms.
The `wlink reset` command waits 300 ms and then quits.

## Halt and resume

wlink and minichlink do both through DMI. No special command exists.

- **Halt.** The core is already halted when wlink reads `dmstatus` after the connect command. In `v003-wlink-halt.pcapng` the host writes only `dmactive` (`00000001`) and reads `dmstatus`. A halt of a running core (`haltreq`, bit 31) is open.
- **Resume.** wlink writes `dmcontrol` four times: `80000001` twice (`haltreq`), `00000001` (clear), `40000001` (`resumereq`, bit 30). Then it reads `dmstatus` twice and checks `allresumeack`.

## minichlink path

On the CH32V003, minichlink uses only commands `0x0d`, `0x0c`, `0x11`, `0x06` (query) and `0x08` (DMI).
It does not use `0x01`, `0x02` or `0x03` and does not use endpoint `0x02`.
It runs the flash write from the host with abstract commands and the program buffer.

On the CH32V208, minichlink uses the stub flow of wlink (it prints "binary blob write"):
`01` (area), `02 05`, the 512-byte stub on `0x02`, `02 07`, `02 02`, the data on `0x02`, then quit.
It differs from wlink in these points (`v208-mc-write.pcapng`):

- It sends the stub in transfers of 256 bytes and the data in transfers of 4096 bytes, not 64 bytes.
- It does not read the block acknowledge and does not send `02 08`. It sends quit, and the probe answers after 0.3 s.
- It writes `dmcontrol` with `ndmreset` (`80000003`) before the transfer. A protection query just after that returned `01` (protected) on an unprotected chip. The reason is open.
- A write of 4 KB takes 0.66 s.

Consequences for the personality:

- A probe with the commands `0x0d`, `0x0c`, `0x11`, `0x06 01` and `0x08` is enough for minichlink on a CH32V003.
- This is a good first target (work item #68). A CH32V20x needs the commands `0x01` and `0x02` and endpoint `0x02` for both tools.
- minichlink needs about 32 000 DMI round trips to write 16 KB on the CH32V003. This took 3.7 s, almost the same as wlink (3.5 s).

Observations:

- `minichlink -r file 0x08000000 16384` fails with "Exception executing abstract command". Reads of 4096, 8192 and 16000 bytes work. This is a minichlink or chip problem, not a probe problem. The capture `v003-mc-read.pcapng` uses 4096 bytes.
- minichlink sends `0d 03` before it does anything else.

## Byte order

Memory read data on endpoint `0x82` has the bytes of each 32-bit word in reversed order.
The host reverses each group of 4 bytes to get the memory content.
Example: the word at `0x08000000` is `6f 00 20 12` in memory and arrives as `12 20 00 6f`. On the CH32V208, `6f 20 e0 42` arrives as `42 e0 20 6f`.
The unique ID in the `0x11` answer uses the same order.
Flash write data does not use this reversal. The image goes out in file order.

## Timing seen on USB

| Operation | Time |
|---|---|
| Connect (`0d 01 02`) | CH32V003: about 90 ms (about 900 ms after erase or protect). CH32V208: about 60 ms |
| DMI round trip | about 0.1 ms |
| Read of 16 KB (wlink) | about 0.2 s |
| Program of 16 KB (wlink, CH32V003) | 3.5 s, of this 2.2 s for the data blocks |
| Program of 160 KB (wlink, CH32V208) | 15.2 s |
| Erase (wlink) | 0.19 s (CH32V003), 0.26 s (CH32V208) |
| Protect, unprotect | CH32V003: about 1.0 s, about 1.1 s. CH32V208: about 0.6 s, about 2.1 s |

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
| Idle time between frames | about 7 µs |
| SWDIO change after SWCLK falling edge | median 62 ns (5th percentile 4 ns, 95th percentile 728 ns) |

Observations about the frames:

- During the connect command (the first 60 ms), the probe sends about 200 identical frames of 85 clocks. They repeat every 227 µs.
  Each frame has a first part of about 40 clocks at the slow period, and a longer part at the fast period.
  The first part holds the pattern `0010001` followed by 33 zero bits. The value `0x11` is the address of `dmstatus`.
  The rest of the frame is high, which suggests that the target answers and the probe releases the line.
  The 85 clocks of a frame may hold a read of `dmstatus`. This is **not confirmed**. The bit framing is not decoded.
- Then the probe sends shorter frames of 53 and 54 clocks, spaced about 120 µs apart (chip detection).
- The connect command ends at about 59.7 ms. This matches the USB capture (`v208-wlink-status.pcapng`, 59.7 ms).
- After that, each DMI access from the host is one frame of 54 clocks. The spacing is 120 to 200 µs, so one access takes about 0.12 ms in total, with the USB round trip.

### Speed setting

`v208-wlink-status-low-timing.csv` and `v208-wlink-status-medium-timing.csv` are the same run with `wlink --speed low status` (speed byte `03`) and `--speed medium status` (`02`). Same channels and sample rate.

| Phase | Default speed (`01`) | Medium speed (`02`) | Low speed (`03`) |
|---|---|---|---|
| Connect (first 60 ms) | 85-clock frames, periods 3.08 µs and 2.09 µs | The same | The same |
| Host DMI frames of 54 clocks | Some with a period of about 1.10 µs (frame time 63 µs), some with about 2.09 µs (115 µs) | The same as default | All with about 2.09 µs (111 to 115 µs) |
| First accesses after connect | Three long bursts (648, 324, 162 clocks) with a period of about 120 ns (minimum 60 ns) | Seven bursts of 162 clocks (251 µs) with a period of about 1.12 µs | None |

Conclusions:

- The speed byte does not change the clock during the connect command. The probe runs the chip detection at a fixed rate.
- The speed byte changes the clock of the later accesses. Only the long bursts change between the default and the medium speed (about 120 ns against about 1.12 µs). At low speed, the frames take about 111 to 115 µs. The 400 kHz of the wlink help text is not the measured rate (the measured rate is about 478 kHz).
- At the default speed, the clock is faster in some frames. The long bursts with a period near 120 ns may hold several accesses with gaps shorter than 5 µs. This is **not confirmed**: the frames are not decoded.

## Wire timing (CH32V003, one wire)

Source: `wch-link-captures/v003-wlink-status-timing.csv`. The same Saleae Logic Pro recorded `wlink status` at 500 MS/s.
Channel 0 is SWIO (PD1). Channels 1 and 2 were not connected (constant 0 in the file).
The probe used its default speed. The run took about 95 ms.

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
- The later phase of the CH32V003 (after 45.8 ms) is slower than the later phase of the CH32V208. The connect command of the CH32V003 also takes longer (90 ms against 60 ms).

### Speed setting (CH32V003)

`v003-wlink-status-low-timing.csv` and `v003-wlink-status-medium-timing.csv` are the same run with `--speed low` (`03`) and `--speed medium` (`02`).

The speed byte has no visible effect on the one-wire line:

- The first frame has the same pulse widths in all three captures (low 8.2 µs, high 3.1 µs, low 9.2 µs, high 3.1 µs, low 101.5 µs, high 94.4 µs).
- The start of the later phase is the same (45.8 ms).
- The most common pulses of the later phase are the same (low 0.8 µs and 0.2 µs).
- The run time is the same (93 ms).

`v003-wlink-dump-timing.csv` and `v003-wlink-dump-low-timing.csv` hold `wlink dump 0x08000000 256` at the default speed and at `--speed low`.
They also match each other: the same first frame, the same pulse widths in the later phase, and the same long gaps (1.2 ms, 4.9 ms).
The run is 95 ms in both captures, and the dump adds about 3800 edges to the status run.

The probe may ignore the speed byte for the one-wire interface, or the speed applies to a part that this test does not use.
The cause is not known. A flash program at the three speeds could show a difference. This was not captured (a program of 16 KB needs 3.5 s).

Not measured on the CH32V003: the bit framing, and the wire timing of a flash program.

Not measured on the CH32V208:

- The 85-clock frames of the connect phase and the bits that the table above marks as open.

## Wire frame format

The captures show one frame format on the wire. A frame has 54 clocks. SWDIO is stable at the rising edge of SWCLK. Bit 0 is the first clock.
The decoder is `tools/wch-link/decode_frames.py`. It works on the CSV files of the CH32V208.
I checked the format against the 15 DMI accesses that the USB capture of the same run shows.
The address and the data of every access match. The order matches too.

| Bits | Content | Driven by |
|---|---|---|
| 0 | Start, always `1` | Probe |
| 1 to 7 | DM register address, MSB first | Probe |
| 8 to 9 | Operation field | Probe |
| 10 to 14 | Five response bits | Target |
| 15 to 46 | Data, MSB first | Probe (write), target (read) |
| 47 to 48 | Parity of the 32 data bits: `11` if the number of ones is odd, `00` if even | See below |
| 49 to 53 | `00110` after a write, `01110` after a read | Probe and target |

Observations:

- **Operation field.** `10` for a write of `dmcontrol`, `data0` and `abstractcs`. `11` for a write of `command` (`0x17`) and `data1` (`0x05`). `01` for most reads. `00` for reads of `dmstatus` and `hartinfo`. The reason for `11` and `00` is open.
- **Response bits.** `00000` after a write, `10000` after a write of `command`, `10100` after a read at the slow clock, `10000` after a read at the fast clock (about 120 ns). Their meaning is open (acknowledge or status).
- **Parity.** Both parity bits matched the data in all 60 frames of the CH32V208 capture. They may be one parity bit sent two times.
- **Who drives a bit.** The probe changes SWDIO about 70 to 90 ns after the falling edge of SWCLK. The target changes SWDIO about 12 ns after the falling edge. The read data and the response bits follow the target pattern. The write data follows the probe pattern.
- **Clock for each access.** At the default speed, a write uses 1.10 µs for each bit (about 907 kHz). A read uses 2.09 µs (about 478 kHz). At low speed, all accesses use 2.09 µs.
- **Fast triples.** The long bursts at about 120 ns for each bit (default speed) hold frames in the order `data1` write, `command` write, `data0` read. The probe sends them after the connect command. They look like abstract-command memory reads at the highest clock rate. The values were not matched to a USB access.
- **Connect frames.** The 85-clock frames of the connect phase do not follow this format. They are not decoded.

### One-wire frame format (CH32V003)

The one-wire line (SWIO) does not carry the 54-bit frame of the two-wire interface. It carries a shorter frame of 41 bits.
Each bit is one low pulse followed by a high level. The width of the low pulse gives the value:

| Bit | Low pulse | High level after it | Bit time |
|---|---|---|---|
| 1 | about 250 ns | about 370 ns | about 0.62 µs |
| 0 | about 860 ns | about 370 ns | about 1.23 µs |

Frames follow each other with a high level of 50 to 100 µs between them. The connect phase has other pulse patterns (see above).
The decoder is `tools/wch-link/decode_frames_1wire.py`. It works on the CH32V003 CSV files.

| Bits | Content |
|---|---|
| 0 | Start, always `1` |
| 1 to 7 | DM register address, MSB first |
| 8 | Operation: `1` is a write, `0` is a read |
| 9 to 40 | Data, MSB first |

The frame has no response bits, no parity and no tail. The two-wire frame has 13 more bits (a second operation bit, 5 response bits, 2 parity bits, 5 tail bits).
The one-wire line may carry them in another way, or the probe may not send them. This is open.

I checked the format against the USB capture of the same type of run. The 15 accesses of the host match in order and in address.
The values match, except for `dmstatus` (`0x004f0382` on the wire, `0x004c0382` in the USB capture, from an earlier run; the bits 16 to 19 change between runs).
The wire capture has three more accesses at the end (a write of `dmcontrol` with `0x40000001`, a read of `dmstatus`, a write of `dmcontrol` with `0x40000000`). They match a resume that the `status` command does at the end.
I did not check them against a USB capture.

Pulse widths are the same for reads and writes, and for the speeds `01`, `02` and `03`.
A frame lasts 50 to 150 µs, depending on the number of 0 bits (about 105 µs for typical data).

## Pins and USB ID

| Target | Pin on target | Probe signal |
|---|---|---|
| CH32V003 | PD1 (SWIO) | One wire. The probe signal is SWDIO (open drain, with pull-up). |
| CH32V20x | PA13 (SWDIO), PA14 (SWCLK) | SWDIO and SWCLK (confirmed on a CH32V208 with a WCH-LinkE) |

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
| Does the probe or the host hold the flash algorithm? | Both. The host sends a 512-byte stub per chip family. The probe runs it. minichlink uses the stub on the CH32V208. On the CH32V003 it writes by DMI and uses no stub. |
| Wire timing of the one-wire and the two-wire interface | **Mostly answered.** See [two-wire timing](#wire-timing-ch32v208-two-wires) and [one-wire timing](#wire-timing-ch32v003-one-wire), both at three speeds. Open: the response and parity bits on the one-wire line, and the timing of a flash program. |
| USB protocol on a CH32V20x | Answered with a CH32V208. It matches the CH32V003 protocol (see [CH32V208](#ch32v208)). |
| Probe variant check by wlink and minichlink | wlink prints the variant from byte 2 of the info response. It did not reject the probe. A check of other variants is open. |
| Error reason codes | **Open.** Seen: `0x55` (no target). The meaning of other codes is open. |
| Verify sub-command (`02 01 06`?) | **Open.** No capture used it. |
| Meaning of `0d 03`, `0d 04`, `06 01 04`, `11 01 05/06`, `02 01 05/07` | **Open.** The observed behavior is in this document. |
| `0x0e` (disable debug) | **Open.** No capture used it. |
