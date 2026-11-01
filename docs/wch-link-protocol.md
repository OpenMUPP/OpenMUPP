# WCH-LinkE protocol

This document describes the USB protocol of the WCH-LinkE in RISC-V mode, and the wire protocol between the probe and the target.
The `wch-link` personality speaks this protocol. [WCH-Link personality](wch-link.md) describes its design.
The facts come from USB captures and logic analyzer captures of a real probe. A statement that has no capture behind it says so. No vendor code was copied.

Status: the USB protocol and the wire frames are captured on a CH32V003 (one wire), on a CH32V208 (two wires) and on a CH32V006 (one wire).
A few fields are open (see [Open questions](#open-questions)).

## Notation

- All bytes are hexadecimal, without `0x`. Example: `0d`.
- A packet in the text has the form `CMD LEN payload`, without the header byte (`81` for a request, `82` for a response).
  Example: `0d 01 02` is command `0d`, length 1, payload `02`. The full request packet is `81 0d 01 02`.
- A packet in the text that starts with `81` or `82` shows the header byte too. The document uses this form for the error frame and the reply to an unknown command, because the short form is not clear there (see [Framing](#framing)).
- The words `addr` and `len` in a packet are u32 big-endian fields. Example: `03 08 addr len`.
- A column with the title "payload" shows the payload only, without `CMD` and `LEN`.
- A number with `0x`, for example `0x2080053c`, is a value: an address, a register value or a chip ID.
- Bytes on the data endpoints (`0x02`, `0x82`) have no header and no length.

## Captures

The captures are in [`wch-link-captures/`](wch-link-captures/README.md), with the setup, the file names and the decoders.
The targets are a CH32V003 (one wire), a CH32V208 (two wires) and a CH32V006 (one wire). The host tools are wlink 0.1.2 and 0.1.3, and minichlink.
A file name in parentheses, for example (`v208-wlink-protect.pcapng`), names the capture that shows a fact.
The `wlink flash` captures and `v208-mc-write.pcapng` are not in this repository (see [the notes](wch-link-captures/README.md#usb-captures-pcapng)).

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
The UART and the CDC interfaces are out of scope.

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

The table has all commands and sub-commands of the captures.
It also has the requests that the source code of wlink 0.1.2 and minichlink (`pgm-wch-linke.c`) can send.
The column "Seen" tells the source of a row:

- "captured": a USB capture.
- "tools": the source code of wlink or minichlink. No capture has the request, so the probe response is not known.

A name in quotes is the name in the wlink source code.

| Request | Name | Response | Seen | Section |
|---|---|---|---|---|
| `01 08 addr len` | Set the flash area | `01 01 01` | captured | [Flash program](#flash-program-wlink) |
| `02 01 01` | Mass erase | `02 01 01` | captured | [Erase](#erase-wlink-erase) |
| `02 01 02` | Start the data phase of a program | `02 01 02` | captured | [Flash program](#flash-program-wlink) |
| `02 01 04` | "WriteFlashAndVerify". wlink sends it in place of `02 01 02` | not known | tools | — |
| `02 01 05` | Start the stub transfer | `02 01 05` | captured | [Flash program](#flash-program-wlink) |
| `02 01 06` | "Prepare" in wlink. Not confirmed | not known | tools | [Flash program](#flash-program-wlink) |
| `02 01 07` | After the stub transfer. Likely: prepare the flash | `02 01 07` | captured | [Flash program](#flash-program-wlink) |
| `02 01 08` | End of the program | `02 01 08` | captured | [Flash program](#flash-program-wlink) |
| `02 01 0b` | wlink sends it in place of `02 01 07` for one chip type (`riscvchip` 1) | not known | tools | — |
| `02 01 0c` | Start the memory read | `02 01 0c`, then the data on `0x82` | captured | [Memory read](#memory-read-0x03) |
| `03 08 addr len` | Set the memory read range | `03 01 01` | captured | [Memory read](#memory-read-0x03) |
| `06 01 01` | Protection query | `06 01 01` (protected) or `06 01 02` (unprotected) | captured | [Read protection](#command-0x06-read-protection) |
| `06 01 02` | Unprotect | `06 01 02` | captured | [Read protection](#command-0x06-read-protection) |
| `06 01 03` | Protect | `06 01 03` | captured | [Read protection](#command-0x06-read-protection) |
| `06 01 04` | Unknown. wlink: "CheckReadProtectEx" | `06 01 00` | captured | [Read protection](#command-0x06-read-protection) |
| `06 08 02 b ff ff ff ff ff ff` | "UnprotectEx". minichlink sends `b` = `ff` or `f7` | not known | tools | — |
| `06 08 03 b ff ff ff ff ff ff` | "ProtectEx". minichlink sends `b` = `f7` | not known | tools | — |
| `06 07 02 …`, `06 07 03 …` | Unprotect and protect for the CH570 (minichlink) | not known | tools | — |
| `08 06 reg data op` | DMI access. `reg` and `op` are u8 fields | `08 06 reg data status` | captured | [DMI access](#command-0x08-dmi-access) |
| `0b 01 01` | Reset and run the target | `0b 01 01` | captured | [Reset](#reset-0x0b) |
| `0b 01 02` | "Chip" reset. The wlink source says that the memory is not reset | not known | tools | — |
| `0b 01 03` | "Normal" reset | not known | tools | — |
| `0c 02 type speed` | Configure the chip type and the speed | `0c 01 01` | captured | [Configure](#command-0x0c-configure) |
| `0d 01 01` | Probe information | `0d 04 02 13 12 00` | captured | [Info, connect, quit](#command-0x0d-info-connect-quit) |
| `0d 01 02` | Connect | `0d 05 family chip-id` | captured | [Info, connect, quit](#command-0x0d-info-connect-quit) |
| `0d 01 03` | Unknown (minichlink sends it first) | `0d 01 03` | captured | [Info, connect, quit](#command-0x0d-info-connect-quit) |
| `0d 01 04` | Likely: read the split of RAM and flash ("GetChipRomRamSplit", only for the CH32V20x, CH32V30x, CH32V317 and CH32V4x7). Not confirmed | `0d 01 00` | captured | [Info, connect, quit](#command-0x0d-info-connect-quit) |
| `0d 01 05` | Unknown. The tools send the split with a parameter (next row) | `0d 01 05` | captured | [Info, connect, quit](#command-0x0d-info-connect-quit) |
| `0d 02 05 n` | Set the split of RAM and flash | not known | tools | — |
| `0d 02 08 chip` | Erase the code flash by the reset pin | not known | tools | — |
| `0d 02 0f chip` | Erase the code flash by the power. minichlink sends `0d 01 0f 09`: its `LEN` is 1, but the payload has 2 bytes | not known | tools | — |
| `0d 01 09`, `0d 01 0a` | Target power 3.3 V on, off | not known | tools | — |
| `0d 01 0b`, `0d 01 0c` | Target power 5 V on, off | not known | tools | — |
| `0d 02 0e 13`, `0d 02 0e 14`, `0d 02 0e 15` | Reset pin low, high, floating (wlink). minichlink sends `0d 01 13` for low | not known | tools | — |
| `0d 02 ee 00`, `0d 02 ee 01` | SDI print on, off (firmware 2.10 or later) | not known | tools | — |
| `0d 01 ff` | Quit | `0d 01 ff` | captured | [Info, connect, quit](#command-0x0d-info-connect-quit) |
| `0e 01 01` | Disable debug ("DisableDebug") | not known | tools | — |
| `11 01 05`, `11 01 06` | Chip information | 20 raw bytes, no header | captured | [Chip information](#command-0x11-chip-information) |
| `11 01 09` | Chip information (wlink "V1", minichlink) | not known | tools | — |
| `7f 01 00` (unknown command) | Unknown command. No other unknown command was tested | `82 81 01 ff` | captured | [Framing](#framing) |
| `0d 01 02` without a target | Error frame | `81 55 01 01` | captured | [Framing](#framing) |

## Command `0x0d`: info, connect, quit

| Sub-command | Meaning | Response payload |
|---|---|---|
| `01` | Probe information | `02 13 12 00` |
| `02` | Connect to the target (RISC-V) | `09 00 30 05 00` (CH32V003), `05 20 80 05 3c` (CH32V208), `4e 00 60 06 20` (CH32V006) |
| `03` | Unknown. minichlink sends it first. wlink does not. | `03` |
| `04` | Likely: read the split of RAM and flash. wlink names it "GetChipRomRamSplit" and sends it only for the CH32V20x, CH32V30x, CH32V317 and CH32V4x7. Not confirmed. In the captures, wlink sends it on the CH32V208 after the first protection query. | `00` |
| `ff` | Quit. The probe resumes the core and releases the target (see [Probe actions](#probe-actions-at-connect-at-chip-information-and-at-quit)). | `ff` |

- **Probe information.** Byte 0 and byte 1 are the major and minor version of the firmware (2 and 19). This matches `bcdDevice` 2.19. Byte 2 (`0x12`) is the probe type. wlink maps it to `WCH-LinkE-CH32V305`. Byte 3 is `0x00`.
- **Connect.** The probe starts the debug interface and does the chip detection. This takes about 90 ms on the CH32V003 and about 59 ms on the CH32V208. A connect after an erase or a protect command takes the same time. Byte 0 is a chip family code (`0x09` for the CH32V003, `0x05` for the CH32V208). Bytes 1 to 4 are the chip ID (u32 BE: `0x00300500`, `0x2080053c`).
- **Quit.** The host sends this at the end of every session. A session is connect to quit.
- **Sub-command `03`.** The probe needed about 74 ms to answer in 5 of 7 CH32V003 runs. It needed 1 ms in the other 2. On the CH32V208 it needed 0.9 ms or less. The reason is not known.
- **Other values.** `0d 01 05` gets the echo `0d 01 05`, but it is not a no-op. The tools send `05` with a parameter, as `0d 02 05 n` (set the split of RAM and flash). The reply took 184 ms (CH32V003) and 97 ms (CH32V208). The next quit took 46 ms and 24 ms, instead of 0.2 ms (`v003-raw-errors.pcapng`, `v208-raw-errors.pcapng`). The effect is not known.
- **Length check.** A memory read with a wrong length (`03 01 00`) gets a normal reply `03 01 01`. The probe does not check the length.

## Command `0x0c`: configure

Request payload: `type speed`. `type` is the chip type.

- wlink sends `01 speed` before the connect command and `family speed` after it. Here `family` is the first byte of the connect response (`09` or `05`).
  With the generic code `01`, the connect command still works on both chips.
  minichlink sends only `09 speed`, before the connect command, also on the CH32V208 (family `05`). The connect command still works.
- The speed byte: `01` is high (the default), `02` is medium, `03` is low. Captures of `wlink --speed low|medium|high` show this.
  The wlink help text gives 6000 kHz for high, 4000 kHz for medium and 400 kHz for low. The measured two-wire clock does not agree.
  The connect frames use the same clock at all speeds. After the connect, at low speed, all frames use about 478 kHz. At high and medium speed, a DMI write of the host uses about 907 kHz and a DMI read about 478 kHz (see [Speed setting](#speed-setting)).
  On one wire, the speed byte has no visible effect (see [Speed setting (CH32V003)](#speed-setting-ch32v003)).
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
| `04` | Unknown. wlink names it "CheckReadProtectEx" and sends it after a query that returned `02`. | `00` |

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
- `02`: failure. No capture shows a failure, so the value is not confirmed. The personality uses it.

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

## Reset (`0x0b`)

`0b 01 01` resets the target and lets it run. The response comes after about 15 ms (CH32V003) and 10 ms (CH32V208).
The `wlink reset` command waits 300 ms and then quits.

## Memory read (`0x03`)

wlink uses this path for `dump`. The sequence:

1. `03 08 addr len`: set the read range. Response `03 01 01`.
2. `02 01 0c`: start the read. Response `02 01 0c`.
3. The probe sends the data on endpoint `0x82` in 64-byte packets, without a header and without an acknowledge.

A read of the whole 16 KB flash needs 256 packets. It takes about 0.2 s.
The command ends after `len` bytes. No end marker follows.

### Byte order

Memory read data on endpoint `0x82` holds each 32-bit word with the most significant byte first.
The host reverses each group of 4 bytes to get the little-endian memory content.
Example: the word at `0x08000000` is `6f 00 20 12` in memory and arrives as `12 20 00 6f`. On the CH32V208, `6f 20 e0 42` arrives as `42 e0 20 6f`.
The words in the `0x11` answer also have the most significant byte first.
Flash write data does not use this reversal. The image goes out in file order.

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
- The flash captures hold no verify command and no memory read. No capture used a verify sub-command.

### CH32V208

The sequence is the same. The differences:

- Step 2 sends `01 08 08000000 00028000` (160 KB).
- The stub is a different 512-byte block.
- The data blocks have 4096 bytes (64 packets). The probe needs about 350 ms for each block.
- A program of 160 KB takes 15.2 s (about 10.8 KB/s).
- Erased flash reads as `0xe339e339`, not `0xff`. A `wlink dump` after `wlink erase` showed this. That dump is not in the capture set.

### The stub

A disassembly of the captured stubs shows their interface.
The three stubs (wlink CH32V003, wlink CH32V208, minichlink CH32V208) use the same interface. `llvm-objdump` disassembles them as RV32 code with the C extension.
No stub code is copied into this repository.

- The stub is position-independent code, and it is a function without a return: it ends with `ebreak`. The probe thus sets `dcsr.ebreakm`, so that the `ebreak` halts the hart.
- `a0` holds operation bits. The stub does the operations in this order:

  | Bit | Operation |
  |---|---|
  | `0x01` | Unlock: `KEY1` and `KEY2` to `FLASH_KEYR` and to `FLASH_MODEKEYR` |
  | `0x02` | Mass erase: `MER` and `STRT` in `FLASH_CTLR` |
  | `0x04` | Fast page erase (`FTER`) of the pages from `a1`, for `a2` bytes |
  | `0x08` | Fast page program (`FTPG`) of the pages from `a1`, for `a2` bytes, from a RAM buffer. Then a check of `WRPRTERR`. |
  | `0x10` | Verify: compares the flash words with the buffer, and the sum of the flash words with a checksum word in RAM |

- `a1` is the flash address, `a2` the length in bytes. The page size is in the stub code: 64 bytes (CH32V003) and 256 bytes (CH32V208).
- The result is in `a0`: `0` for success, `0x08` for a write protection error, `0x10` for a verify error.
- The stub writes `0xaaaa` to `0x40003000` (`IWDG_CTLR`) in each wait loop: it feeds the watchdog.
- The stub uses the stack (`sp`): the probe must set it.
- The addresses of the buffer and the checksum word are fixed in the stub code:

  | Stub | Buffer | Block size | Checksum word |
  |---|---|---|---|
  | CH32V003 | `0x20000200` | 1024 bytes | `0x20000610` |
  | CH32V208 | `0x20001000` | 4096 bytes | `0x20002010` |

  The buffer of the CH32V003 starts directly after the 512 bytes of the stub. Thus the stub is in RAM at `0x20000000`.
  The checksum word is 16 bytes after the end of the buffer.

The captures do not show which operation bits the probe uses for each command. The wire timing of a flash program is not captured.

### Erase (`wlink erase`)

`06 01 01`, `06 01 04`, then `02 01 01` (response `02 01 01` after 7.6 ms on the CH32V003 and 138 ms on the CH32V208), then a new connect (`0d 01 02`, 90 ms and 59 ms), then quit.
The whole command took 0.19 s (CH32V003) and 0.26 s (CH32V208).
The erase command needs no data transfer and no stub.

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

### Chip information, protection query and `0d 01 04`

The probe reads the target when the command arrives, not at connect:

| Command | Target reads (CH32V208) | Used for |
|---|---|---|
| `0x11` | The words at `0x1ffff7e0`, `0x1ffff7e8`, `0x1ffff7ec` and `0x1ffff7f0` (`ffff00a0`, `6213f0eb`, `2f30dc32`, `e339e339`) | The first 16 bytes of the answer |
| `06 01 01` | `0x4002201c` (`000000fc`) and `0x40022020` (`ffffffff`) | The protection state in the answer |
| `0d 01 04` | `0x1ffff802` (`0000003f`), with the command `0x02000000` (a read of one byte) | Likely the split of RAM and flash: `0x1ffff802` is the `USER` option byte, which holds `SRAM_CODE_MODE` on the CH32V20x. Not confirmed. The answer is `00`. |

The evidence is the reply time. It grows with the speed byte, because the target reads run at the clock that `0c` sets:

| Command | Reply time at high, medium and low speed |
|---|---|
| `0x11` | 0.24 ms, 1.10 ms, 1.58 ms |
| `06 01 01` | 0.16 ms, 0.57 ms, 0.83 ms |
| `0d 01 04` | 0.14 ms, 0.30 ms, 0.43 ms |

The wire time from the read of `0x1ffff7e0` to the read of `0x4002201c` shows the same growth (0.24 ms, 1.13 ms, 1.64 ms).
The CH32V003 does the same reads for `0x11` and `06 01 01` at 90.3 to 91.2 ms (`ffff0010`, `6784abcd`, `cf99bc48`, `ffffffff`, then `0x4002201c` and `0x40022020`). It has no `0d 01 04` access.
A personality must read the target when the command arrives. An answer from a value that was read at connect can be out of date, for example after a protect and a reset.

### Quit (`0d 01 ff`)

The probe writes `dmcontrol` `40000001` (`resumereq`), reads `dmstatus` (`0x00030c82`: running, resume acknowledged) and writes `dmcontrol` `40000000`.
The USB capture shows no host command for this. Both wire captures show the same three accesses at the end (CH32V208: 62.7 to 62.9 ms. CH32V003: 92.96 to 93.07 ms).
So quit resumes the core. This is why a halted core runs again after `wlink status`.

## Halt and resume

wlink and minichlink do both through DMI. No special command exists.

- **Halt.** The core is already halted when the host reads `dmstatus` after the connect command, because the connect command halts the core (see [Probe actions](#probe-actions-at-connect-at-chip-information-and-at-quit)).
  In `v003-wlink-halt.pcapng` the host writes `dmcontrol` `00000000` and `00000001`. It reads `dmcontrol` and `dmstatus`, writes `00000001`, and reads `dmstatus`.
  A halt request for a running core is not in the captures.
- **Resume.** In `v003-wlink-resume.pcapng` the host reads `dmcontrol`, writes `a0000001` and reads `dmstatus`.
  Then it writes `80000001` twice (`haltreq`), `00000001` and `40000001` (`resumereq`, bit 30). Then it reads `dmstatus` twice.

## minichlink path

On the CH32V003, minichlink uses only commands `0x0d`, `0x0c`, `0x11`, `0x06` (query) and `0x08` (DMI).
It does not use `0x01`, `0x02` or `0x03` and does not use endpoint `0x02`.
It runs the flash write from the host with abstract commands and the program buffer.

On the CH32V208, minichlink uses the stub flow of wlink (it prints "binary blob write"):
`01 08 addr len` (area), `02 01 05`, the 512-byte stub on `0x02`, `02 01 07`, `02 01 02`, the data on `0x02`, then quit.
It differs from wlink in these points (`v208-mc-write.pcapng`):

- It sends the stub and the data in transfers of 256 bytes (2 transfers for the stub, 16 for the 4 KB), not 64 bytes.
- Its stub is not the stub of wlink (the two share 85 percent of the bytes). The first bytes differ. The probe runs the stub that the host sends, so the stub is not fixed in the probe.
- It does not read the block acknowledge and does not send `02 01 08`. It sends quit, and the probe answers after 0.3 s.
- It writes `dmcontrol` with `ndmreset` (`80000003`) before the transfer. A protection query just after that returned `01` (protected) on an unprotected chip. The reason is open.
- A write of 4 KB takes 0.66 s.

Consequences for the personality:

- A probe with the commands `0x0d`, `0x0c`, `0x11`, `0x06 01` and `0x08` is enough for minichlink on a CH32V003.
- This is a good first target. A CH32V20x needs the commands `0x01` and `0x02` and endpoint `0x02` for both tools.
- On the CH32V003, a minichlink write of 16 KB took about 32 000 DMI round trips and 3.7 s, almost the same as wlink (3.5 s). The committed `v003-mc-write-head.pcapng` holds only the first 4000 frames. The full capture is not in the repository.

Observations:

- `minichlink -r file 0x08000000 16384` fails with "Exception executing abstract command". Reads of 4096, 8192 and 16000 bytes work. This is a minichlink or chip problem, not a probe problem. The capture `v003-mc-read.pcapng` uses 4096 bytes.
- minichlink sends `0d 01 03` before it does anything else.

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
| 11 to 14 | Handshake: bit 11 low, bit 12 released, bit 13 low, bit 14 released | Bit 11: target. Bit 13: probe. Bits 12 and 14: nobody. |
| 15 to 46 | Data, MSB first | Probe (write), target (read) |
| 47 | Parity of the 32 data bits: `1` if the number of ones is odd | Probe (write), target (read) |
| 48 | The level of bit 47. SWDIO changes direction after the rising edge (write). | Probe (write), target (read) |
| 49 to 52 | Handshake: bit 49 low, bit 50 released, bit 51 high, bit 52 released | Bit 49: target. Bit 51: probe. Bits 50 and 52: nobody. |
| 53 | Always `0`. After this edge, SWDIO rises while SWCLK is high: the stop condition. | Probe |

The frame is a start condition, 53 clocks and a stop condition. The start and the stop condition are the same as on an I2C bus.
The table numbers the bits as the decoder does: bit 0 is the clock before the start condition.
Bits 1 to 53 are the clocks between the start and the stop condition.
The rules for bits 9, 10, 47, 48 and 53 hold in all 440 frames of the three status captures (default, medium and low speed).

Observations:

- **Operation field (bits 8 and 9).** Bit 8 tells a write from a read. Bit 9 is a parity bit over the address and bit 8. This explains the values `11`, `10`, `01` and `00` of the earlier decoder output.
- **Handshake bits (11 to 14 and 49 to 52).** The analog capture of the WCH-LinkE shows the driver of each bit (see [Who drives a bit](#who-drives-a-bit-analog-capture)).
  The target pulls bit 11 and bit 49 low. The probe drives bit 13 low and bit 51 high. Nobody drives bits 12, 14, 50 and 52.
  In the released bits, SWDIO rises slowly from the low level. Thus the logic analyzer shows `0` or `1` there, by chance.
  This explains the values `0000`, `0100`, `0011` and `0111` of the decoder. A probe must drive bit 13 and bit 51.
  When the probe released bit 13, the CH32V208 drove bit 11 only, and then no more bits of the frame. It did not do the write.
- **Parity (bits 47 and 48).** Bit 47 is the parity. Bit 48 keeps the level of bit 47 for one more clock, as bit 10 keeps the level of bit 9.
- **Who drives a bit.** The probe changes SWDIO about 70 to 90 ns after the falling edge of SWCLK. The target changes SWDIO about 4 to 14 ns after the falling edge.
  Bits 1 to 9, 13, 51 and 53 follow the probe pattern. Bits 11 and 49 follow the target pattern. Bits 15 to 47 follow the probe pattern in a write and the target pattern in a read.
  SWDIO changes while SWCLK is high in bits 10 and 14, and in bit 48 of a write: the line changes direction there.
- **Longer low phases.** The low phase before bits 1, 15 and 53 is about two half periods: 2.1 to 2.2 µs at the 2.09 µs clock. SWDIO changes direction before bits 15 and 53.
- **Idle.** Between frames, SWCLK is low and SWDIO is high. The probe keeps SWCLK high for about 1 to 5 µs after the stop condition.
  Before the start condition, SWCLK rises and stays high for about 1 to 8 µs.
- **Before the first frame.** After the 85-clock frames, the probe sends 9 clocks with SWDIO high, one clock with SWDIO low, and 90 clocks with SWDIO high. Then the first frame starts. This can be a line reset. This is **not confirmed**.
- **Clock for each access.** At the default speed, a write uses 1.10 µs for each bit (about 907 kHz). A read uses 2.09 µs (about 478 kHz). At low speed, all accesses use 2.09 µs.
- **Fast triples.** The long bursts at about 120 ns for each bit (default speed) hold frames in the order `data1` write, `command` write, `data0` read. These are the target reads for `0x11`, `06 01 01` and `0d 01 04`. The values of the first four reads match the `0x11` answer byte for byte.

### Who drives a bit (analog capture)

Source: an analog capture of `wlink status` on the CH32V208, at 12.5 MS/s.
Two parts of it are in the repository: `v208-wlink-status-analog-connect.csv` (the first five connect frames) and `v208-wlink-status-analog-frames.csv` (the first frames of 54 clocks).

The voltage of SWDIO in the high phase of SWCLK tells the driver:

| Level | Driver |
|---|---|
| 0.00 to 0.02 V | Probe, low |
| 3.29 to 3.31 V | Probe, high |
| -0.07 to -0.14 V | Target, low |
| 1.5 to 1.9 V, rising | Nobody: the line rises from low through a pull-up |
| 3.35 to 3.55 V | Nobody: the line stays high through a pull-up |

Seven frames of 54 clocks (writes and reads) show the same handshake bits, as the frame table gives.

### Connect frames

The 85-clock frames of the connect phase are `dmstatus` reads (address `0x11`), with 32 more clocks:

1. A start condition and bits 1 to 52 of a read of `dmstatus`. The handshake bits are not the same as in the frame format above (see below).
2. 32 clocks with SWDIO released (high).
3. SWDIO falls while SWCLK is high: a start condition.
4. One clock with SWDIO low, then the stop condition.

In the first frame, the target answers `0x00000c82` (version 2, authenticated, hart running). The parity is right.
In this frame, the probe drives bits 12, 13 and 14 low, and nobody drives bit 51.

In the next frames, the target does not answer with data (`v208-wlink-status-analog-connect.csv`):

- The target pulls bit 10 and bit 48 low: one bit earlier than bits 11 and 49 of the frame format.
- The probe drives bits 11 to 40 low and bit 41 high. Nobody drives bits 42 to 47.

The cause of the shift by one bit is not known.
The probe uses these frames to wait for a target. A probe without them connects too: the personality sends no connect frames (see [Hardware test](wch-link.md#hardware-test)).

## One-wire frame format (CH32V003)

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

The frame has no handshake bits and no parity. The two-wire frame has 13 more bits: the address parity, its repeat, 4 handshake bits, the data parity, its repeat, 4 handshake bits and the stop bit.
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

## Wire timing (CH32V208, two wires)

Source: `v208-wlink-status-timing.csv`, a logic analyzer capture of `wlink status` at 500 MS/s (2 ns), with SWDIO, SWCLK and NRST.
The probe used its default speed (`0c … 01`).

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
  The frames are decoded (see [Connect frames](#connect-frames)).
- From 46 ms the probe sends frames of 54 clocks, about 120 µs apart. These frames are decoded (see [Wire frame format](#wire-frame-format)). They hold the probe actions, the target reads and the host accesses (see [Probe actions](#probe-actions-at-connect-at-chip-information-and-at-quit)).
  Some frames have a pause of about 7 µs after the start clock. A decoder must join the start clock with the rest of the frame.
- The connect command ends at about 59 ms. On USB, the connect response comes 59.4 ms after the command (`v208-wlink-status.pcapng`).
- After that, each DMI access from the host is one frame of 54 clocks. The spacing is 120 to 200 µs, so one access takes about 0.12 ms in total, with the USB round trip.

### Speed setting

`v208-wlink-status-low-timing.csv` and `v208-wlink-status-medium-timing.csv` are the same run with `wlink --speed low status` (speed byte `03`) and `--speed medium status` (`02`).

| Phase | Default speed (`01`) | Medium speed (`02`) | Low speed (`03`) |
|---|---|---|---|
| Connect, 85-clock frames (first 46 ms) | periods 3.08 µs and 2.09 µs | The same | The same |
| Host DMI frames of 54 clocks | Some with a period of about 1.10 µs (frame time 63 µs), some with about 2.09 µs (115 µs) | The same as default | All with about 2.09 µs (111 to 115 µs) |
| Target reads for `0x11`, `06 01 01` and `0d 01 04` (after 59.6 ms) | Three long bursts (648, 324, 162 clocks) with a period of about 120 ns (minimum 60 ns) | Seven bursts of 162 clocks (251 µs) with a period of about 1.12 µs | Frames of 54 clocks with a period of about 2.09 µs, spread over 59.6 to 62.4 ms |

Conclusions:

- The speed byte does not change the clock during the connect command. The probe runs the chip detection at a fixed rate.
- The speed byte changes the clock of the later accesses. Only the long bursts change between the default and the medium speed (about 120 ns against about 1.12 µs). At low speed, the frames take about 111 to 115 µs. The 400 kHz of the wlink help text is not the measured rate (the measured rate is about 478 kHz).
- At the default speed, the clock is faster in some frames. The long bursts with a period near 120 ns hold 12, 6 and 3 frames of 54 clocks without a pause. They are the target reads for `0x11`, `06 01 01` and `0d 01 04` (see [Probe actions](#probe-actions-at-connect-at-chip-information-and-at-quit)).

## Wire timing (CH32V003, one wire)

Source: `v003-wlink-status-timing.csv`, a logic analyzer capture of `wlink status` at 500 MS/s, with SWIO only.
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

Not measured on the CH32V208: the wire timing of a flash program.

## CH32V006 (one wire, QingKe V2C)

Source: the `v006-` captures. The target is a CH32V006K8U6 (chip ID `0x00600620`, 62 KB flash). The probe is the same WCH-LinkE (firmware 2.19).
The USB commands are the same as for the CH32V003 (see the sections above). This section lists the differences.
The one-wire decoder `tools/wch_link_frames_1wire.py` decodes the wire frames without change.
The USB captures hold 15 DMI accesses of the host. The one-wire decode of `v006-wlink-status-timing.csv` shows all 15, in the same order, with the same address and data.

| Item | CH32V003 | CH32V006 |
|---|---|---|
| Chip family code in the connect response and in `0c 02` | `09` | `4e` |
| Connect time | 90 ms | 94 ms |
| Register `0x7c` at connect | `00010403` | `11038403` |
| Register `0x16` at connect | `08000602` | `08000702` |
| Memory access of the probe | Program buffer (command `0x00040000`) | Abstract command (`0x02200000` read, `0x02210000` write), as on the CH32V208. A program in the program buffer ends with `cmderr` 2 on the CH32V006 (tested with the firmware of this project). |
| Flash page | 64 bytes | 256 bytes |
| Data block of a flash program | 1024 bytes | 1024 bytes |
| Mass erase (`02 01 01`) | 7.6 ms | 113 ms |
| Protect (`06 01 03`) | 103 ms | 55 ms |
| Unprotect (`06 01 02`) | 108 ms | 123 ms |
| Protection query after a protect and a new connect | `02` (not protected) | `01` (protected) |
| Protected read of flash | `00000300` | `20 b0 00 00`, repeated |
| Reset (`0b 01 01`) | 15 ms | 6 ms |

Observations:

- **Two-wire attempts.** From 0 to 76 ms, the probe sends two-wire frames on SWCLK and SWDIO. It sends 202 frames of 85 clocks and 20 times the three frames `7e` write, `7d` write and `dmstatus` read. The V006 does not answer: every `dmstatus` read returns `0`.
  After 76 ms, SWCLK stays high and the probe talks on one wire. The V006 answers at once. The datasheet says that the chip supports one-wire (default) and two-wire SDI, on PD1 (SWDIO) and PB3 (SWCLK). The captures do not show how to select the two-wire mode.
  MounRiver OpenOCD with the `two_wires` option sends the same USB commands as without it.
- **Probe actions at connect.** After the chip ID read, the probe writes `abstractcs` with `ffffffff` and does about 30 accesses to `0x40022000` (`FLASH_ACTLR`, written with `00000002`) and to `0x40021000`, `0x40021004` and `0x40021008` (RCC).
  The last RCC accesses set the PLL on (`0x40021000` goes from `00509c83` to `01509c83` and `03509c83`). The probe also writes `0x00008080` to `0x40022010` and `0x0000b020` to `0x4002200c` (the reset values of `FLASH_CTLR` and `FLASH_STATR`).
  The CH32V003 shows none of these accesses. The meaning of the RCC values is not checked against the manual.
- **Chip information.** The words at `0x1ffff7e0`, `0x1ffff7e8`, `0x1ffff7ec` and `0x1ffff7f0` give `ffff003e`, `5f8dabcd`, `c7b7bc5d` and `ffffffff`. The `0x11` answer holds them and the chip ID. The protection query reads `0x4002201c` (`03fffffc`) and `0x40022020` (`ffffffff`).
- **Flash program (wlink).** The sequence is the same as for the CH32V003. The step 2 packet is `01 08 08000000 00001000` for 4 KB. The stub has 512 bytes and differs from the CH32V003 stub and the CH32V208 stub (`v006-wlink-flash.pcapng`).
  A disassembly shows the interface of [the stub](#the-stub): fast page erase and fast page program in steps of 256 bytes (64 words), the buffer at `0x20000200`, and the checksum word at `0x20000610`. A block of 1024 bytes needs about 59 ms.
  The program of 4 KB takes 0.29 s from `02 01 05` to `02 01 08`.
- **Quit.** The quit writes `dmcontrol` `40000001`. The following `dmstatus` read gives `004f0382`: the hart is still halted (`v006-wlink-status-timing.csv`). A single write that clears `haltreq` and sets `resumereq` does not resume a CH32V006.
  `wlink resume` clears `haltreq` in a write of its own (`00000001`), then writes `40000001`, and the hart runs (`dmstatus` `004f0c82`). The project firmware sends both writes in a resume. The test used the firmware of this project on the CH32V006 and the host tool `dmi` accesses.
- **Option bytes.** A write of `FLASH_CTLR` with `FTPG` does not set the bit on a CH32V006 (the register reads back without it), also after the unlock of `FLOCK`. Fast page programming of the option bytes (reference manual 18.5.2) does not work through the debug module.
  `OBPG` programming works (tested with `minichlink` accesses and with the firmware of this project): `OBER` and `STRT` erase the option bytes, then `FLASH_CTLR` `OBPG | STRT | OBWRE` and a 16-bit write for each option half word. The hardware adds the inverse byte of a byte like `a5`. A write of the byte `ff` alone leaves the half word unprogrammed (`ffff`). Thus the firmware writes the byte and its inverse.
  The write of `RDPR` `0xa5` takes about 110 ms on a protected chip, because the chip erases the main flash first. An erase of the option bytes without a new `RDPR` leaves the chip protected after a reset.
- **Protect.** After `06 01 03`, wlink quits and connects again. The query then returns `01`. A reset is not needed on the CH32V006.
- **minichlink.** It uses no stub. A write of 4 KB sends 3591 DMI commands, an erase 1081. Its connect (`0d 01 02`) is often answered by the error frame `81 55 01 02` after about 0.5 s. minichlink then sends `0d 01 13`, quit, a reset and a new connect. The write still works.
  There is no halt capture, because `minichlink -T` waits for input.

## Open questions

The table lists the open questions about the protocol and their state.

| Question | State |
|---|---|
| Payload of the program sub-commands | Answered for `02 01 05`, `02 01 07`, `02 01 02` and `02 01 08`. The meaning of `05` and `07` is a guess (see above). minichlink does not send `08`. |
| Order of the flash sequence | Answered (see [Flash program](#flash-program-wlink)). |
| Does the probe or the host hold the flash algorithm? | Both. The host sends a 512-byte stub per chip family. The probe runs it. minichlink uses the stub on the CH32V208. On the CH32V003 it writes by DMI and uses no stub. The interface of the stub comes from a disassembly (see [The stub](#the-stub)). Erase, protect and unprotect use no stub: the probe holds that algorithm. |
| Operation bits of the probe for each stub call | **Open.** The personality uses its own sequence (see [Flash](wch-link.md#flash)). |
| Wire timing of the one-wire and the two-wire interface | **Mostly answered.** See [two-wire timing](#wire-timing-ch32v208-two-wires) and [one-wire timing](#wire-timing-ch32v003-one-wire), both at three speeds. The personality uses the one-wire times with a timer (see [One-wire timer](wch-link.md#one-wire-timer)). Open: the response and parity bits on the one-wire line, and the timing of a flash program. |
| USB protocol on a CH32V20x | Answered with a CH32V208. It matches the CH32V003 protocol (see [CH32V208](#ch32v208)). |
| Probe variant check by wlink and minichlink | wlink prints the variant from byte 2 of the info response. It did not reject the probe. A check of other variants is open. |
| Error reason codes | **Open.** Seen: `0x55` (no target). The meaning of other codes is open. |
| Meaning of `02 01 06` | **Open.** wlink calls it "Prepare". No capture used it. |
| Meaning of `0d 01 03`, `0d 01 04`, `0d 01 05`, `06 01 04`, `11 01 05`, `11 01 06`, `02 01 05` and `02 01 07` | **Open.** The observed behavior is in this document. |
| Does the probe do target accesses of its own? | Answered: the connect command halts the core. The commands `0x11`, `06 01 01` and `0d 01 04` read the target. The quit command resumes the core (see [Probe actions](#probe-actions-at-connect-at-chip-information-and-at-quit)). |
| Meaning of the 85-clock frames, the response bits, the tail bits, and the failure of one-wire frames in the `dump` captures | **Partly answered.** The 85-clock frames are `dmstatus` reads (see [Connect frames](#connect-frames)). The response bits and the tail bits are a handshake (see [Wire frame format](#wire-frame-format)). The one-wire `dump` frames are open. The operation field values are answered: a write bit and a parity bit (see [Wire frame format](#wire-frame-format)). |
| `0x0e` (disable debug) | **Open.** No capture used it. The personality does the quit (see [Commands](wch-link.md#commands)). |
