# WCH-Link personality

The `wch-link` personality makes the dongle a WCH-LinkE in RISC-V mode.
The dongle then debugs and programs WCH RISC-V chips (CH32V) with the host tools `wlink` and `minichlink`.
The personality speaks the USB protocol of the WCH-LinkE.
[wch-link-protocol.md](wch-link-protocol.md) describes the protocol and the wire frames. [Implementation](#implementation) describes the command processor.

## Build

Use the preset of the board with the `-wch-link` suffix, for example `mini-wch-link`:

```sh
cmake --preset mini-wch-link
cmake --build --preset mini-wch-link -j
```

Or set the CMake option `VSL_PERSONALITIES` to `wch-link`.
All boards build with this value.
The build writes `versaloon-ng-<board>-wch-link-<image>-<type>-<version>.hex` and `.bin`.
Write the image as in [Firmware update](firmware-update.md).

The `wch-link` image has these properties:

- USB interface 0 is the WCH-Link interface: bulk OUT `0x01` and IN `0x81` for commands, bulk OUT `0x02` and IN `0x82` for data.
- The image has no CDC-ACM serial port. The packet memory of the USB peripheral (512 bytes) has no room for it.
  Thus the UART bridge and the debug log are off.
  A `Debug` build of this image needs `-DVSL_DEBUG_CDC=OFF`, and it has no debug log. CMake stops with an error when `VSL_DEBUG_CDC` is on.
- The image does not link the Versaloon and CMSIS-DAP code. The SWD and JTAG engines of these personalities are not used.
- The LEDs show the target power, the command activity and the USB state. They show no probe error pattern.
- The image has no `VERSALOON_FW_UPDATE` command. Use the key to start the bootloader (see [Firmware update](firmware-update.md)).
- A multi image can have this personality with others, for example `-DVSL_PERSONALITIES=versaloon,cmsis-dap,wch-link` (see [Multi image](usb-device.md#multi-image)).
  The switch commands of the other personalities (code `2`), the switch request on EP0 and the key switch to it.
  The WCH-Link protocol has no command that switches the personality.
  To leave the WCH-Link personality, use the switch request on EP0 or the key (see [Switch request on EP0](usb-device.md#switch-request-on-ep0)).
  `tools/vsl_personality.py` sends the switch request on EP0, for example `python3 tools/vsl_personality.py switch versaloon`.
  The tool knows the WCH-Link personality by its USB ID (`1209:0001` or `1a86:8010`) and the product string `Versaloon WCH-Link`.
  The `wch-link` image has only one personality, so it stalls the switch request on EP0.
  While the WCH-Link personality is active, the image has no serial port, and the debug log of a `Debug` build is silent.

## USB ID

The default USB ID is `1209:0001`, the test ID of pid.codes.
It stays until the project has its own product ID.
The product string is `Versaloon WCH-Link`.

`wlink` and `minichlink` look for a probe with the ID `1a86:8010` of WCH.
Thus they do not find a probe with the default ID.
The CMake option `VSL_WCH_LINK_WCH_USB_ID` builds the image with the ID `1a86:8010`:

```sh
cmake --preset mini-wch-link-wch-id
cmake --build --preset mini-wch-link-wch-id -j
```

The vendor ID `1a86` belongs to WCH.
Use this option only for personal use.
Do not publish an image with this ID.
The names of these image files have `-wch-link-wch-id` after the board.
In a multi image, the tag ends with `-wch-id`, for example `-vsl+dap+wch-wch-id`.
CI does not build this image.
The option is only valid when `VSL_PERSONALITIES` has `wch-link`.

## Connect a target

The dongle uses the SWD pins of the board (see [Pins](#pins) for the pin of each board):

| Target | Wires | Dongle signals |
|---|---|---|
| CH32V003, CH32V002/4/5/6/7, CH32M007, CH641 | One wire: SWIO on PD1 | SWDIO |
| CH32V20x, CH32V30x, CH32V317, CH32X035, CH32L103, CH643, CH645 | Two wires: SWDIO, SWCLK | SWDIO and SWCLK |

Connect the ground of the dongle to the ground of the target.
The dongle does not supply the target. The WCH-Link commands for the target supply are not supported.
Power the target from its own supply.

A connect needs a known chip ID (see [Connect](#connect) for the full table of chip ID prefixes and family codes).
For another chip, the connect fails.

## Use the probe

On Linux, a udev rule gives a normal user access to the probe.
Copy `etc/70-versaloon-wch-link.rules` to `/etc/udev/rules.d/`.
Then reload the rules with `udevadm control --reload` and plug in the probe again.
The file has a rule for the default ID and a rule for the ID of WCH.
The file number must stay below 73, because systemd applies the `uaccess` tag in `73-seat-late.rules`.
The rule for the default ID also checks the product string, because other devices use the test ID too.

```text
SUBSYSTEM=="usb", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", ATTRS{product}=="Versaloon WCH-Link", MODE="0660", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="8010", MODE="0660", TAG+="uaccess"
```

The host tools then find the probe if it has the ID that they look for (see [USB ID](#usb-id)).
Examples with `wlink`:

```sh
wlink status
wlink flash firmware.bin
wlink erase
wlink dump 0x08000000 256
```

`minichlink` writes and reads the flash of the target too.
The commands that the personality supports are in [Commands](#commands).

## Size

The size of the `standalone` image (Release build):

| Board | Size | Limit |
|---|---|---|
| Mini | 22,156 bytes | 65,536 bytes |
| OtherUse | 22,152 bytes | 65,536 bytes |
| ST-Link/V2 | 22,124 bytes | 65,536 bytes |

The `app-8k` image has the limit 57,344 bytes, and the `app-10k` image has the limit 55,296 bytes (see [Flash layout](firmware-update.md#flash-layout-and-size-limit)).
Each image uses less than half of its limit.
CI checks each image against its limit (`ci/check-size.sh`).

For comparison, the Versaloon image of the Mini has 27,168 bytes, the CMSIS-DAP image 31,480 bytes, and the dual image 37,660 bytes.

### Combined image

The question was whether an image with all three personalities fits the Mini.
The Mini has an STM32F103CB with 128 KB flash.
The firmware and the linker script use only the first 64 KB, so the limits above apply.

A build measured the size. The build is a test build only. It links the dual image of the Mini (Versaloon and CMSIS-DAP) and the objects of the WCH-Link personality.
The measurement is from before the one-wire timer. The timer adds about 2.2 KB, so each margin below is about 2.2 KB smaller.
It has no third descriptor set and no switch code.

| Image (Mini) | Size | Limit |
|---|---|---|
| Dual image | 41,488 bytes | 64,512 bytes |
| Dual image with the WCH-Link objects | 51,312 bytes | 64,512 bytes |
| Same, `app-8k` | 51,304 bytes | 56,320 bytes |
| Same, `app-10k` | 51,312 bytes | 54,272 bytes |

The WCH-Link code adds 9.8 KB.
The wire engine and the debug module layer are templates, and they are a large part of this size.
The `app-10k` image has a margin of 2,960 bytes only.
A third descriptor set and the switch code use some of this margin.
The data and BSS grow from 15.9 KB to 16.4 KB. The RAM has 19.75 KB.

Thus a combined image fits in the first 64 KB, but the margin for the legacy bootloader is small.
It does not need the other 64 KB of flash.

The firmware now builds the combined image: `-DVSL_PERSONALITIES=versaloon,cmsis-dap,wch-link` (see [Multi image](usb-device.md#multi-image)).
A Release build of the Mini measured these sizes (`.bin` files, with the one-wire timer and the switch request on EP0).
Without link-time optimisation (LTO), the `app-10k` image did not fit: it was 1,740 bytes too large.
Issue #75 added LTO (option `VSL_LTO`), removed the heap code of newlib and removed the 64-bit division:

| Image (Mini, Release) | Before #75 | Now | Limit |
|---|---|---|---|
| `standalone` | 56,012 bytes | 48,876 bytes | 64,512 bytes |
| `app-8k` | 56,012 bytes | 48,884 bytes | 56,320 bytes |
| `app-10k` | Does not fit | 48,876 bytes | 54,272 bytes |

The `app-10k` image now has a margin of 5,396 bytes.
The data and BSS use 16.4 KB.
A `Debug` build has no LTO. A `Debug` build of the combined image fits no image: the build skips all images (see [Images that do not fit](development.md#images-that-do-not-fit)).

## Implementation

The personality speaks the protocol with these parts:

| Part | File | Task |
|---|---|---|
| Descriptors | `src/usb/WchLinkDescriptors.h` | USB ID, interface 0 with the endpoints `0x01`, `0x81`, `0x02` and `0x82` |
| USB function | `src/usb/WchLinkFunction.h` | Moves the packets between the endpoints and the processor |
| Processor | `src/protocol/wchlink/WchLinkProcessor.h` | Runs the commands |
| Target | `src/protocol/wchlink/WchLinkTarget.h` | Connects the processor to the debug module layer and the wire engine |

The host tests are in `tests/TestWchLink.cpp`. They send the packets through the USB function, with the wire engine on mock pins and a mock DM.
The build option `VSL_PERSONALITIES` with `wch-link` puts the personality into a firmware image (see [Build](#build)).

### USB

- The interface has the endpoint numbers of the WCH-LinkE, with 64 bytes each. The interface has no CDC interfaces.
  The endpoints and EP0 use 408 bytes of the 512 bytes of packet memory.
- The default USB ID is `1209:0001`, the test ID of pid.codes. It stays until the project has its own product ID.
  The CMake option `VSL_WCH_LINK_WCH_USB_ID` (a build define of the same name) selects `1a86:8010`. Use this ID only for personal use, because the vendor ID belongs to WCH.
- The function takes one request at a time. The command OUT endpoint gets a NAK until the host reads the response.
- The function arms the data OUT endpoint `0x02` only while the processor takes data: in the stub phase and in the data phase of a program sequence (see [Flash](#flash)).
  A packet is written to the target before the next request runs. Thus a quit directly after the data (minichlink) finds the last block programmed.
  The endpoint cannot be disarmed. When a sequence ends while it is armed, the function drops the next packet.
- A host can leave a packet on the data IN endpoint `0x82` unread. minichlink does not read the block acknowledge of its last block.
  A connect, a quit, a suspend or a new read range (`03`) then marks that packet as stale. The function takes it back before it sends the response (`art::usb::Device::cancelInTransfer()` of libArT).
  Thus the next memory read gives its own data first. The WCH-LinkE gives the stale packet to the next read (see [Hardware test](#hardware-test)).

### Commands

| Command | Behavior |
|---|---|
| `0d 01` | Replies `02 13 12 00`, the values of the captured WCH-LinkE |
| `0d 02` | Connects (see below) |
| `0d 03`, `0d 04` | Reply `03` and `00`. No target access. |
| `0d ff` | Quit: `resumereq`, the `dmstatus` poll, `dmcontrol` `40000000`. Then the pins are released. |
| Other `0d` values | The unknown-command reply (see below) |
| `0c` | Ignores the chip type, and sets the speed. Replies `01`. |
| `08` | DMI access. `op` `00` does no access. A failure, or an access before a connect, gives the status `02`. |
| `03`, then `02 01 0c` | Memory read on endpoint `0x82` |
| `11` | Reads the four words at the command, and adds the chip ID of the connect |
| `06 01` | Protection query. Reads `0x4002201c` (`FLASH_OBR`) and `0x40022020` at the command. Replies `01` when `RDPRT` (bit 1 of `FLASH_OBR`) is 1, else `02`. |
| `0b 01` | Writes `dmcontrol` `00000003` (`ndmreset`), waits 1 ms, writes `00000001`, then `10000001` (`ackhavereset`). The hart runs after the reset. |
| `01` | Keeps the flash area (address and length) for the program sequence. Replies `01`. |
| `02 01` | Mass erase with the own algorithm (see [Flash](#flash)) |
| `02 05`, `02 07`, `02 02`, `02 08` | The program sequence with the host stub (see [Flash](#flash)) |
| `06 02`, `06 03` | Unprotect and protect with the own algorithm (see [Flash](#flash)). Reply `02` and `03`. |
| `06 04` | Replies `00`. No target access. |
| `0e` | Not captured. The processor does the quit and echoes the sub-command. Before a connect, it only echoes. |

All other commands get the reply of the WCH-LinkE to an unknown command: `82 81 01 ff`.
The processor does nothing for them. This includes the other sub-commands of `02` and `06`, for example a verify sub-command.
It also includes the firmware-update and the mode-switch commands of the WCH-LinkE. The captures do not show these commands.

The `0d` sub-commands other than `01`, `02`, `03`, `04` and `ff` also get this reply.
wlink and minichlink use them for actions of the probe: target power (`09` to `0c`), erase by the reset pin or by the power (`08`, `0f`), the reset pin (`13` to `15`), the split of RAM and flash (`05`), and SDI print (`ee`).
An echo tells the host that the action occurred. The processor does not do these actions, so it refuses them.
The WCH-LinkE echoes `0d 05` (see [Command `0x0d`](wch-link-protocol.md#command-0x0d-info-connect-quit)). The processor does not.
A packet without the header `81` gets the same reply.

A command that needs a target before a connect, or a failed target access, gives the error frame `81 55 01 01`.

A suspend of the USB bus, and a bus reset or a deconfigure, release the pins without target accesses.
Thus the hart stays halted after the connect, until the next session.

### Connect

The chip type of the `0c` command does not select the wire. The processor tries one wire first, then two wires, for all chip types.
wlink sends the type `01` also for the CH32V003. minichlink sends the type `09` (CH32V00x) also for the CH32V208, and the WCH-LinkE then connects on two wires (`v208-mc-info.pcapng`).
One wire comes first, because its frames change SWDIO only while SWCLK is low. A two-wire target sees no start condition and no stop condition in them.
The frames of two wires are noise on the line of a one-wire target.
On each wire, the processor does these probe actions:

1. Write `0x7e` and `0x7d` with `5aa50400`.
2. Read `dmstatus`. A wrong version field tells that no target is present.
3. Halt the hart: `dmcontrol` `80000001` two times, then the `dmstatus` poll.
4. Read the chip ID at `0x7f`. The values `00000000` and `ffffffff` tell that no target is present.
5. Write CSR `0x7c0` with `0x00000300`.

The family code of the response comes from a table of chip ID prefixes (the top 12 bits). wlink selects the chip settings from this code.

| Prefix | Chip series | Family code | Source |
|---|---|---|---|
| `003` | CH32V003 | `09` | Captured |
| `641` | CH641 | `49` | Reference manual and wlink |
| `002`, `004`, `005`, `006`, `007` | CH32V002/4/5/6/7, CH32M007 | `4e` | CH32V00X reference manual and wlink |
| `208` | CH32V208 | `05` | Captured |
| `203`, `207` | CH32V203, CH32V207 | `05` | Same series as the CH32V208. Not captured. |
| `303`, `305`, `307` | CH32V30x | `06` | Reference manual and wlink |
| `317` | CH32V317 | `86` | Reference manual and wlink |
| `033`, `035` | CH32X035 | `0d` | Reference manual and wlink |
| `103` | CH32L103 | `0e` | Reference manual and wlink |
| `643` | CH643 | `0c` | Reference manual and wlink |
| `645`, `653` | CH645 | `46` | Reference manual and wlink |

A chip ID that is not in the table gives the error frame. A wrong family code would make the host use wrong chip settings without a warning.
The processor then resumes the hart and ends `dmactive`, as the quit does. It tries no other wire.

The processor does not do these probe actions:

- The 85-clock connect frames (see [Connect frames](wch-link-protocol.md#connect-frames)). The CH32V208 connects without them.
- The writes to the clock registers of the CH32V208 at connect.
- The read of `0x1ffff7c4` of the CH32V003 at connect.
- The read of `0x1ffff802` for `0d 04`.

Their meaning is not known. The flash operations do not need the clock setup: each flash operation starts with a reset (see [Flash](#flash)).

### Memory read

The processor reads one word for each 4 bytes of the range, and sends each word with the most significant byte first.
It reads the next packet only when the host took the packet before. After a failed word, the processor stops the target accesses, and the rest of the data is `0xff`.
A new `03` command, a connect or a quit stops a read that the host did not take completely.

The data has no header, so the host cannot tell an old packet from a new packet.
Thus a new `03` command, a connect, a quit or a suspend also takes back a packet that is on its way on endpoint `0x82`.
`02 01 0c` gives the error frame while a packet of the read before still waits for the host.
This occurs when no `03`, connect or quit came before it, or when the host reads that packet at the same time.

### Flash

The flash module (`src/protocol/wchlink/WchLinkFlash.h`) does the flash commands.
The supported series have the same flash controller registers for these commands (reference manuals in the datasheet store, chapter "Flash Memory and User Option Bytes").

Each flash command starts with a reset that halts the hart:

1. Write `dmcontrol` `80000003` (`haltreq`, `ndmreset`). Wait 1 ms.
2. Write `dmcontrol` `80000001`, then halt (`haltreq` two times, the `dmstatus` poll).
3. Write `dmcontrol` `90000001` (`ackhavereset`).
4. Write CSR `0x7c0` with `0x00000300`, as the connect does.

After the reset, the flash controller is locked, the clock is the HSI, and no code of the target runs.
The WCH-LinkE sets a clock of its own at the connect of a CH32V208 (see [Probe actions](wch-link-protocol.md#probe-actions-at-connect-at-chip-information-and-at-quit)). The reset makes this clock setup unnecessary.
The CH32V20x manual asks for a system clock of 120 MHz or less for flash operations. A target program can set 144 MHz.
minichlink also resets with `haltreq` before its program sequence.

The processor selects the RAM layout from the family code of the connect: `09` and `49` give the CH32V003 layout, `4e` the CH32V00X (CH32V002/4/5/6/7) layout, and the remaining two-wire V4 family codes give the CH32V20x layout.

| Item | CH32V003 (`09`, `49`) | CH32V00X (`4e`) | CH32V20x (`05`, `06`, `86`, `0c`, `0d`, `0e`, `46`) |
|---|---|---|---|
| Stub address | `0x20000000` | `0x20000000` | `0x20000000` |
| Buffer, block size | `0x20000200`, 1024 bytes | `0x20000200`, 1024 bytes | `0x20001000`, 4096 bytes |
| Checksum word | `0x20000610` | `0x20000610` | `0x20002010` |
| Page size | 64 bytes | 256 bytes | 256 bytes |
| Stack pointer | `0x20000800` (end of 2 KB RAM) | `0x20000800` | `0x20002800` (end of 10 KB RAM, the smallest CH32V20x) |
| Option bytes (half words) | 6 | 8 | 8 |

#### Own algorithm

The commands without a stub use the flash controller registers directly, with DMI memory accesses:

- **Unlock.** Read `FLASH_CTLR`. Write `KEY1` and `KEY2` to `FLASH_KEYR` only if `LOCK` is 1: a key write to an unlocked controller locks it until the next reset.
- **Wait.** Read `FLASH_STATR` until `BSY` is 0, one read each 1 ms, for 3 s at most. Then write 1 to `EOP` and `WRPRTERR`. A `WRPRTERR` is a failure.
- **Mass erase (`02 01`).** Reset, unlock, `FLASH_CTLR` `MER`, then `MER` and `STRT`, wait, `FLASH_CTLR` 0, lock (`LOCK` and `FLOCK`).
- **Protect (`06 03`) and unprotect (`06 02`).** Reset. Read the old option bytes (see below). Unlock, then write `KEY1` and `KEY2` to `FLASH_OBKEYR`. Erase the option bytes (`OBER`, `STRT`).
  - On CH32V003 (`09`, `49`) and the two-wire V4 series: program each option byte with `OBPG` and a 16-bit write, and wait after each byte.
  - On CH32V00X (`4e`), `FLASH_CTLR` has no `OBPG` bit (CH32V00X reference manual 18.5.2): unlock `FLOCK` via `FLASH_MODEKEYR`, clear the internal page buffer (`FTPG | BUFRST`), load 4 32-bit words (each holding two option bytes and their inverses) into `0x1ffff800`–`0x1ffff80c` with `FTPG | BUFLOAD`, write `0x1ffff800` to `FLASH_ADDR`, and program the page (`FTPG | STRT`).
  Then `FLASH_CTLR` 0 and lock.
  Each `FLASH_CTLR` write keeps `OBWRE` 1: a 0 locks the option bytes again.
  - `RDPR` gets `0xa5` (unprotect) or `0x00` (protect). The value `0xcc` is not used, because it is a permanent protection on other chips.
  - `USER`, `DATA0` and `DATA1` keep their values. The CH32V20x `USER` byte holds the split of RAM and flash.
  - The old values come from `FLASH_OBR` and `FLASH_WPR` first. These registers hold the loaded option bytes, also on a protected chip.
    No capture shows that a debugger can read the option byte area of a protected chip: a protected flash read gives a fixed word (`v003-wlink-dump-protected.pcapng`).
    - CH32V003 and CH32V00X: `USER` is `FLASH_OBR` bits 9 to 2, `DATA0` bits 17 to 10, `DATA1` bits 25 to 18 (CH32V003 RM 16.3.7, CH32V00X RM 18.3.7).
    - CH32V20x and the other V4 series: `USER` bits 2 to 0 are `FLASH_OBR` bits 4 to 2, and `USER` bits 7 to 5 (`SRAM_CODE_MODE`) are `FLASH_OBR` bits 10 to 8. `USER` bits 4 and 3 are reserved (1). The manual gives two different places for `SRAM_CODE_MODE` (32.4.6 and 32.6). `FLASH_OBR` has no `DATA` bytes, so they get `0xff`.
    - `WRPR0` to `WRPR3` are the bytes of `FLASH_WPR`.
  - Then the module reads the option byte area. A byte that has its correct inverse replaces the value from the registers. A failed read is no error.
  - Unprotect writes `0xff` to the write protection bytes (`WRPR`). Protect keeps them.
  - When `RDPR` becomes `0xa5` on a protected chip, the chip erases the main flash itself.
  - After an unprotect, the module resets the target again with `ndmreset`. The WCH-LinkE answers a query after an unprotect with `02` (`v003-wlink-flash-protected.pcapng`), so it also reloads `FLASH_OBR` in some way.
    The reference manuals do not agree with themselves here. The unprotect procedure asks for a power-on reset to load the new `RDPR` (CH32V003 16.5.4, CH32V20x 32.6.4, step 3). The notes of `FLASH_OBR` say that a system reset loads it.
    If `ndmreset` does not load `RDPR`, the query after `06 02` still gives `01`, and `wlink flash` sends the unprotect again in a loop. The hardware test must check this.
    After a protect, a query still gives `02` until the next reset (`v208-wlink-protect.pcapng`). The module does not reset.

On one wire, a 16-bit write uses the program of [Program buffer programs](#program-buffer-programs).

#### Program sequence

The host sends the stub of its chip (see [The stub](wch-link-protocol.md#the-stub)). The processor runs it:

| Request | Action |
|---|---|
| `01 addr len` | Keeps the area. |
| `02 05` | Reset. Reads `dpc`, `dcsr` and `mstatus`. Sets `dcsr.ebreakm` and clears `mstatus.MIE`. Arms the data OUT endpoint for the stub. |
| Stub, 512 bytes | Written to RAM at the stub address, one word for each 4 bytes, in file order (little-endian). |
| `02 07` | Calls the stub with `a0` = `01` (unlock). Only one time in each sequence: a second unlock would lock the controller. |
| `02 02` | Starts the data phase. The area address must be a multiple of the page size. |
| Image | Written to the buffer. When a block is full, or at the end of the area, the processor writes the checksum word and calls the stub with `a0` = `1c` (page erase, program, verify), `a1` = the block address, `a2` = the block length. Then it sends `41 01 01 04` on the data IN endpoint. |
| `02 08` | Locks the controller (`LOCK` and `FLOCK` in `FLASH_CTLR`). Writes `dpc`, `dcsr` and `mstatus` back. Replies `08` if the image was complete. |

A stub call writes `a0`, `a1`, `a2`, `sp` and `dpc` with abstract register writes and resumes the hart.
The processor then reads `dmstatus` each 1 ms until the hart halts, for 3 s at most. Then it reads the result in `a0`.
If the hart does not halt, the processor halts it, and the call fails.

- A short last block is filled with `0xff` to the end of its page, because the stub programs whole pages. The stub call gets the length to the end of the page.
  A length that is not a multiple of 4 fills the last word with `0xff` too.
- The image goes to the stub in the bytes of the area only. Bytes after the end of the area are ignored.
  The data OUT endpoint stays armed after the end of the area, until `02 08`, a quit or a connect. Thus a host that sends more bytes than the area (for example padding to a block) gets no NAK. The captures use areas of whole blocks only.
- A failed block gives `41 01 01 ff`. The failure value is not captured. The processor then takes no more data, and `02 08` gives the error frame.
- A request out of order gives the error frame. Before a connect, all flash commands give the error frame.
- A quit, a connect, an erase or a protection command ends a sequence as `02 08` does. A quit then resumes the hart at the reset vector, because `dpc` is the value after the reset.
- minichlink does not read the block acknowledge. The acknowledge waits on the data IN endpoint until the host reads it, as the packets of a memory read do.
  A quit or a connect drops an acknowledge that is not on its way yet, and takes back the acknowledge on its way. Thus a memory read (`02 0c`) of a later tool gets its own data (issue #77).

### Speed

On two wires, the speed `01` (high) sets SWCLK to 907 kHz. The speeds `02` and `03` set 478 kHz.
The WCH-LinkE uses faster clocks in some frames (see [Speed setting](wch-link-protocol.md#speed-setting)). The processor does not.
On one wire, the speed has no effect.

### Debug module layer


The firmware does the probe actions with `src/rvswd/DebugModule.h`. This layer uses the DMI accesses of the wire engine (`src/rvswd/RvSwdEngine.h`).
It gives DMI read and write, DM reset, halt, resume, abstract register access and memory access.
The host tests are in `tests/TestDebugModule.cpp`. They use a mock DM (`tests/MockDebugModule.h`) with a small RV32EC interpreter for the program buffer.

#### Sequences

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

Resume and quit:

- The quit command of the probe also writes `dmcontrol` `40000000` (`dmactive` 0) after the `dmstatus` read. The resume of the layer does not do this write. The quit command of the personality must add it, else the DM stays active.
- A `resumereq` write to a running hart clears `allresumeack` and does not set it again (debug spec 0.13). Thus `allrunning` also ends the poll. For a halted hart, the access sequence stays the sequence of the probe.

The layer refuses a misaligned address before it sends a frame (status `Misaligned`). A word address must be a multiple of 4, and a half-word address a multiple of 2.
The CH32V003 gets an exception on a misaligned word access.

The layer selects the memory access method from the wire. One wire (CH32V00x) uses the program buffer. Two wires (CH32V20x, CH32V30x) use the abstract memory access.
The program buffer method has 32-bit reads, and 32-bit and 16-bit writes.

#### Program buffer programs

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

No capture shows a memory write by the program buffer: the probe writes the flash with its stub (see [Flash program](wch-link-protocol.md#flash-program-wlink)).
The layer uses its own write program with the same layout:

```
7b251073 7b359073 e0000537 0f452583 0f852503 2573c10c 25f37b20 90027b30
csrw dscratch0, a0;  csrw dscratch1, a1;  lui a0, 0xe0000
lw a1, 0xf4(a0);  lw a0, 0xf8(a0);  c.sw a1, 0(a0)
csrr a0, dscratch0;  csrr a1, dscratch1;  c.ebreak
```

A load or a store in the program can get an exception, for example at an address without memory. The hart then stops the program with `cmderr` 3.
At that time, `a0` and `a1` hold values of the program, and `dscratch0` and `dscratch1` hold the values of the hart.
Thus the layer then loads and runs a restore program. It writes `progbuf0` to `progbuf2`:

```
7b202573 7b3025f3 00009002
csrr a0, dscratch0;  csrr a1, dscratch1;  c.ebreak
```

The layer uses the restore program, not an abstract register write of `a0` and `a1`. The restore program needs no extra reads before each access.

The flash controller needs 16-bit writes for the option bytes. A 16-bit store does not fit in the layout with `dscratch0` and `dscratch1`: the program needs 34 bytes, and the program buffer has 32 bytes.
Thus the layer uses a shorter program for a 16-bit write. It changes `a0` and `a1`:

```
e0000537 0f452583 0f852503 00b51023 00009002
lui a0, 0xe0000;  lw a1, 0xf4(a0);  lw a0, 0xf8(a0);  sh a1, 0(a0);  c.ebreak
```

Before the program, the layer reads `a0` and `a1` with abstract register reads. After the program, it writes them back with abstract register writes, also after an exception.
A 16-bit write thus costs 16 frames. Only the option bytes use it.
A real CH32V003 must confirm that `dscratch0` and `dscratch1` keep their values after the exception.

An assembler (`clang --target=riscv32 -march=rv32ec`) gave the same words for the four programs.
The layer loads a program one time. It loads it again after a DM reset, after a DMI write of the host, after the restore program, or for another program.

#### Errors

| Status | Cause |
|---|---|
| `Wire` | The engine returned a flag: a parity error (two wires), a line that stays low (one wire), or a frame that the timer did not end correctly or did not start because the core clock is below 72 MHz (one wire, see [One-wire timer](#one-wire-timer)). |
| `NoTarget` | Two wires: bit 11 and bit 49 are `1`. The target drives these two bits low (see [Wire frame format](wch-link-protocol.md#wire-frame-format)). One wire: a read without a target gives `ffffffff` on the Mini and the OtherUse, because the pull-up of the SWDIO input pin holds the line high. On the ST-Link/V2, nothing holds the line high in a read bit (see [One-wire timer](#one-wire-timer)), so a read without a target can give other values. Thus each `dmstatus` read checks the version field (2 for spec 0.13). On one wire, only a `dmstatus` read detects a missing target. `dmiRead` gives `ffffffff` with `Ok`. |
| `Timeout` | A poll of `dmcontrol`, `dmstatus` or `abstractcs` did not end after 100 reads. On one wire, a register access or a memory access without a target also gives `Timeout` on the Mini and the OtherUse: `abstractcs` reads `ffffffff` (`busy` 1). On the ST-Link/V2, it can end with another status. After a `Timeout`, read `dmstatus` to find a missing target. |
| `Command` | `cmderr` was not 0 after a command (for example 4: the hart is not halted, 3: an exception in the program buffer, 5: a bus error of the abstract memory access). |
| `Unsupported` | A byte access, or a half-word read, with the program buffer method. |
| `Misaligned` | The address is not a multiple of the access width. The layer sends no frame. |

A real target is necessary to confirm the write program, the restore program, the halt and resume times, and the no-target detection.

### Pins

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

#### One-wire timer

The width of a low pulse is the value of the bit. Thus code cannot make the bits.
On the OtherUse dongle, waits on the cycle counter gave a 1 of 120 to 530 ns, and the CH32V003 did not answer.
Constants for the time of the code worked, but only for one board, one clock and one build.

A hardware timer makes each edge (`src/board/BoardRvSwd.cpp`). The engine (`src/rvswd/RvSwdEngine.h`) gives it the bits of a frame.

| Board | SWDIO output | Timer channel | Update DMA | Sample channel and DMA |
|---|---|---|---|---|
| Mini, OtherUse | PB4 | TIM3_CH1, partial remap | TIM3_UP, DMA1 channel 3 | TIM3_CH3, DMA1 channel 2 |
| ST-Link/V2 (not tested on hardware) | PB14 | TIM1_CH2N | TIM1_UP, DMA1 channel 5 | TIM1_CH1, DMA1 channel 2 |

`board::oneWireTimerOf()` (`src/board/BoardDefinition.h`) holds this table. The timers run at the core clock (72 MHz).

- **Edges.** The timer runs in PWM mode 1 with an inverted output: low for CCR ticks, then high until ARR.
  On each update event, a DMA burst writes ARR and the CCRs of the next bit. No code runs between two bits.
- **Times.** The bits have the WCH-LinkE times: a 1 is low for 250 ns, a 0 is low for 860 ns, and the high level is 370 ns.
  The high level after bit 8 of a read is 750 ns. A read bit lasts 1.3 µs.
- **Read bits.** In the turnaround after bit 8, the code switches SWDIO to open drain. The STM32F1 has no internal pull-up on an output pin.
  On the Mini and the OtherUse, the pull-up of the separate SWDIO input pin (PB6) holds the line high. On the ST-Link/V2 (one SWDIO pin), nothing on the probe holds the line high in a read bit.
  The timer then drives the low pulse of 250 ns, and the target can hold the line low for a 0.
  The sample channel compares at 450 ns. Its DMA copies the input port (`IDR`) to RAM.
  The DMA reads 80 to 210 ns after the compare (measured), thus at 530 to 660 ns.
  That is between the longest 1 (374 ns) and the shortest 0 (824 ns) of the target.
- **DMA budget.** One DMA transfer takes 80 to 210 ns (measured on TIM3). The update burst must end within the shortest bit (620 ns).
  The burst has 3 words on TIM3 (ARR, a reserved word, CCR1) and 4 words on TIM1 (ARR, RCR, CCR1, CCR2). The 4-word burst is not tested.
  A burst of 5 words on TIM3 did not end in time.
  A sample transfer in each write bit also delayed the burst after a row of 1 bits. Bit 8 of a chip ID read then kept the short low time.
  Thus the sample request is on only for the read bits. The code switches it on in the turnaround and off after the last read bit. The cycle counter times both switches, and the code reads no timer or DMA register during the frame.
  A burst that ends late shifts the times of each later bit, and no check finds that. Thus the frames fail with `frameFlag` when the core clock is below 72 MHz (for example on the HSI after a crystal failure). Such a frame sends no bits.
- **Pending requests.** A timer DMA request stays pending until a channel serves it. A request at the end of a frame then made the next frame lose bit 0.
  Thus the last update event of a frame has its own table entry, and the end period (0.9 ms) is long enough to stop the counter.

The line after a release: a capture with analog input (target side) shows the rise after a 1 of the target. The line goes from 0.3 V to 3.07 V in about 120 ns (10 to 90 %).
Thus the target, or a strong pull-up on the target side, drives the line high. The STM32F1 pull-up (30 to 50 kΩ) cannot give that rise.
At the sample point, the line is at 3.06 V. While the probe drives the line low alone, the line is at 0.3 V. This shows a series resistance between the probe and the target.

## Hardware test

Work item #71 tests the personality on real hardware.
A real target must confirm these items:

- The connect on both wires, and the try of one wire on a CH32V20x.
- The reset sequence. The WCH-LinkE reset is not captured on the wire.
- The DMI reads at 907 kHz on two wires.
- wlink and minichlink with the WCH ID build.
- The flash commands on a CH32V003 and a CH32V208: `wlink erase`, `wlink flash`, `wlink protect` and `wlink unprotect`, and the minichlink write.
  - The stub interface: the stub address `0x20000000`, the stack pointers, the operation bits `1c` for each block, and the checksum word.
  - The verify of `0xff` words on the CH32V208, where erased flash reads `0xe339e339`.
  - The reset with `haltreq` before each flash command, on one wire and on two wires.
  - The reload of `FLASH_OBR` by the `ndmreset` after an unprotect (the manuals ask for a power-on reset), and the read of the option bytes on a protected chip.
  - The place of `SRAM_CODE_MODE` in the CH32V20x `FLASH_OBR`, and the `USER` byte after an unprotect.
  - The 16-bit write of the option bytes on one wire.
  - The time of a program of the full flash, against the WCH-LinkE (3.5 s and 15.2 s).
  - A minichlink write, then `wlink dump` without a new USB enumeration: how the WCH-LinkE handles the acknowledge that minichlink did not read.

Setup:

| Item | Value |
|---|---|
| Probe | OtherUse board, `wch-link` image with `VSL_WCH_LINK_WCH_USB_ID` (USB ID `1a86:8010`) |
| Target | CH32V208WBU6 board, external power. SWDIO on PA13, SWCLK on PA14. |
| Host | Linux, wlink 0.1.2 |
| Logic analyzer | Saleae Logic Pro 8: SWDIO on channel 0, SWCLK on channel 1 |

Results:

| Item | Result |
|---|---|
| USB ID | wlink and minichlink find the probe only with the WCH USB ID. The default ID `1209:0001` hides it (issue #76). |
| Connect on two wires (CH32V208), `wlink status` | Passed, 6 of 6 runs. The connect tries one wire first, then two wires. wlink shows the chip ID `0x2080053c`, 160 KB flash, `misa` `RV32ACIMUX` and `dmstatus` `0x382`. |
| `wlink dump`, `wlink regs` | Passed. A dump of the full flash (160 KB) takes 11.4 s. Two dumps are the same. |
| `wlink halt`, `resume`, `reset` | Passed. |
| `wlink erase` | Passed, 0.16 s. The flash then reads `0xe339e339`. |
| `wlink flash` | Passed. 147 KB take 23 s. A dump after the program is the same as the image. |
| `wlink protect` | Passed. The protection starts at the next reset, as on the WCH-LinkE. A dump then reads `0xffffffff`. |
| `wlink unprotect` | Passed. The protection ends at once, without a power cycle: the `ndmreset` loads `RDPR`. The chip erases the flash. The option bytes then read `RDPR` `0xa5`, `USER` `0x3f`, `DATA0` and `DATA1` `0x00`, `WRPR` `0xff`. |
| minichlink | Failed first: minichlink sends the chip type `09` also for a CH32V208, and the processor tried only one wire for this type. The processor now tries both wires for all types. |
| `minichlink -i`, `-r`, `-w` | Passed after the fix. A read of 160 KB takes 6.9 s, and it is the same as the wlink dump. A write of 147 KB takes 22 s. A wlink dump after the write is the same as the image. |
| wlink after `minichlink -w` | Failed first: each memory read gave the error frame `81 55 01 01` until a USB replug. minichlink does not read the block acknowledge of the last block, and the acknowledge stayed on the data IN endpoint (issue #77). The WCH-LinkE has the same problem in part: the first wlink dump fails with `Invalid payload length`, and the next one works. The personality now takes the packet back. Passed after the fix, 3 of 3 runs: the first wlink dump gives the right data. |

The first connect failed: wlink got the error frame `81 55 01 01`.
The cause was the handshake bits of the two-wire frame: the probe must drive bit 13 low and bit 51 high (see [Wire frame format](wch-link-protocol.md#wire-frame-format)).
The engine released these bits, and the CH32V208 then dropped each frame after bit 11.

### CH32V003 (one wire)

Setup: the same probe and host. The target is a CH32V003F4P6 board with its own supply, SWIO (PD1) on SWDIO. The logic analyzer has SWDIO on channel 1 and SWCLK on channel 3.

The first connect failed. The probe sent the right bits, but with wrong low pulses: a 1 was 120 to 530 ns, a 0 was 460 to 1060 ns (WCH-LinkE: 250 and 860 ns).
The engine timed each pulse with waits on the cycle counter. The time of the code between the waits changed the pulses, and the CH32V003 did not answer.
The engine now makes the bits with a timer and DMA (see [One-wire timer](#one-wire-timer)).
On the wire, the low pulses are now 250 to 260 ns and 860 to 870 ns, and the high level is 370 to 380 ns.

Results with the timer:

| Item | Result |
|---|---|
| `wlink status` | Passed, 6 of 6 runs. wlink shows the chip ID `0x00300500`, 16 KB flash, `misa` `RV32CEX`, `marchid` `WCH-V2A` and `dmstatus` `0x4c0382`. |
| `wlink dump`, `wlink regs` | Passed. A dump of the full flash (16 KB) takes 1.7 s. Two dumps are the same. |
| `wlink halt`, `resume`, `reset` | Passed. |
| `wlink erase` | Passed. The flash then reads `0xff`. |
| `wlink flash` | Passed. 16 KB take 4.4 s (the full wlink run). A dump after the program is the same as the image. The WCH-LinkE takes 3.5 s. A read bit of the personality has a fixed time of 1.3 µs, so the personality is slower. |
| `wlink protect` | Passed. The protection starts at the next reset, as on the WCH-LinkE. |
| `wlink unprotect` | Passed. The protection ends at once, and the chip erases the flash. The 16-bit write of the option bytes works: they then read `RDPR` `0xa5`, `USER` `0x17`, `DATA0` `0x00`, `DATA1` `0xff`, `WRPR0` and `WRPR1` `0xff`. |
| `minichlink -i`, `-w` | Passed. A write of 16 KB takes 5.2 s. Three wlink dumps after the write are the same as the image (issue #77 does not occur). |
| `minichlink -r` | Passed for a read of 16,380 bytes (0.9 s), and it is the same as the wlink dump. A read that ends at `0x08004000` (the end of the flash) fails with `cmderr` 3 (exception). The code before the timer gave the same result, so the probe is not the cause. minichlink seems to read one word after the end of the read. A check with a WCH-LinkE is open. |

### CH32V208 with the timer image

The two-wire engine did not change, but the probe now sets up TIM3 and DMA at each connect, and the connect tries one wire first with the timer.
A second OtherUse probe (legacy bootloader, `app-10k` image) ran the CH32V208 items again:

| Item | Result |
|---|---|
| `wlink status` | Passed, 20 of 20 runs. |
| `wlink dump` | Passed. Three dumps of the full flash take 11.4 s each, and they are the same as a dump with the image before the timer. |
| `wlink halt`, `regs`, `resume`, `reset` | Passed. |
| `wlink erase`, `wlink flash` | Passed. The erase takes 0.16 s. A program of 160 KB takes 25.5 s, and a dump after it is the same as the image. |
| `wlink protect`, `wlink unprotect` | Passed. The option bytes after the unprotect are the same as in the first test. |
| `minichlink -r`, `-w` | Passed. A read of 160 KB takes 6.9 s. A write of 160 KB takes 24.3 s. Three wlink dumps after the write are the same as the image. |

First, the memory reads failed at random points with both images (with and without the timer).
The cause was the wiring between the probe and the target: after a new wiring, all items passed.
The first probe also gave more failures than the second probe. Its wiring is not checked.

Open items:

- The minichlink read of the full CH32V003 flash with a WCH-LinkE.
- The WCH OpenOCD fork (if time allows).
