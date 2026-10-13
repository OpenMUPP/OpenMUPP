# USB device

The dongle is a USB full-speed composite device with the USB identity of the original Versaloon firmware.
Thus unmodified OpenOCD (`vsllink` driver) finds it.
`src/usb/UsbDescriptors.h` has the descriptors.

| Item | Value |
|---|---|
| VID:PID | `0x0483:0xA038` (the OpenOCD default) |
| Device class | `0xEF/0x02/0x01` (miscellaneous, with interface association descriptors) |
| Manufacturer string | `Versaloon` |
| Product string | `Versaloon`. OpenOCD looks for this word. |
| Serial number string | The 96-bit chip UID in hexadecimal, 24 digits |
| Maximum power | 200 mA, bus-powered |

| Interface | Class | Endpoints | Function |
|---|---|---|---|
| 0 | Vendor-specific (`0xFF`) | bulk OUT `0x03`, bulk IN `0x82`, 64 bytes | Versaloon protocol (OpenOCD) |
| 1 | CDC (`0x02`), ACM | interrupt IN `0x81`, 16 bytes | Serial port, control interface |
| 2 | CDC data (`0x0A`) | bulk OUT `0x04`, bulk IN `0x84`, 64 bytes | Serial port, data interface |

An interface association descriptor (IAD) groups the interfaces 1 and 2.
The Call Management descriptor points to data interface 2.
The original firmware has the wrong value 1 there.
There is one serial port only.

`src/usb/UsbStack.cpp` runs the libArT USB device stack on the STM32F1 USB peripheral.
The USB interrupt only puts the events into a queue.
The main loop runs the USB protocol.
The board definition supplies the USB D+ pull-up pin.
The device connects when `main()` starts the USB device.
A board without a pull-up pin (ST-Link/V2) has a fixed pull-up.

The Versaloon OUT endpoint `0x03` is double-buffered (system-stm32 `hw::UsbDevice`, "Double-buffered bulk OUT").
The hardware receives the next packet of a command while the main loop handles the last packet.
Thus the host gets fewer NAKs, and a large command arrives faster.
The 512-byte packet memory of the STM32F1 has no room for a double-buffered IN endpoint too.
The buffer descriptor table covers only the endpoint numbers 0 to 4, so it takes 40 bytes instead of 64 bytes.

Interface 0 carries the Versaloon command protocol (see [Versaloon commands](#versaloon-commands)).

The serial port is the [UART bridge](uart-bridge.md).
In a debug build with `VSL_DEBUG_CDC`, the serial port carries the debug log instead (see [Debug log](debug-log.md)).

## CMSIS-DAP personality

The firmware has a second USB personality: CMSIS-DAP v2 instead of the Versaloon protocol.
Only one personality is active at a time.
The USB peripheral has 512 bytes of packet memory.
The Versaloon interface and a CMSIS-DAP interface do not fit together.
The build option `VSL_PROTOCOL` selects the personality (see [Select the personality](#select-the-personality)).
A dual image has both personalities, and selects one at each start (see [Dual image](#dual-image)).
The Versaloon personality is the default.

`src/usb/CmsisDapDescriptors.h` has the descriptors.
`src/usb/CmsisDapFunction.h` has the USB function.

| Item | Value |
|---|---|
| VID:PID | `0x0403:0x9B3C` |
| Product string | `Versaloon CMSIS-DAP` |
| Interface 0 string | `Versaloon CMSIS-DAP v2` |
| `bcdUSB` | `0x0201` (USB 2.0 with a BOS descriptor) |
| Other items | As in the Versaloon personality |

The host tools find a CMSIS-DAP v2 interface by the vendor class and the text `CMSIS-DAP` in the interface string.
Interface 0 is vendor-specific (`0xFF`) with bulk OUT `0x03` and bulk IN `0x82`, 64 bytes each.
The interfaces 1 and 2 are the CDC-ACM serial port, as in the Versaloon personality.
The endpoint numbers and sizes are the same as in the Versaloon personality.
Thus the packet memory use does not change from the Versaloon personality.
The endpoints and EP0 use 464 bytes, with a 64-byte buffer descriptor table.
The firmware uses a 40-byte table and a double-buffered OUT endpoint `0x03`: 504 bytes.

`usb::CmsisDapFunction` sends each OUT packet to the [CMSIS-DAP command processor](cmsis-dap.md).
It sends each response as one IN packet.
A response of 64 bytes has no zero-length packet.
The processor holds up to 4 packets.
When all 4 slots are in use, the OUT endpoint NAKs until the host reads a response.
`DAP_Info` reports the packet size 64 and the packet count 4.
When a bus reset or `SET_CONFIGURATION 0` closes the interface, the function calls `Processor::hostGone()`.
A suspend only releases the target pins, because the USB transfer state stays.
When a `DAP_QueueCommands` packet takes the last free slot, the processor runs the queue at once.
Thus queued packets alone do not hold the OUT endpoint, and each queued packet gets its response.

### MS OS 2.0 descriptors

Windows 8.1 and later bind WinUSB to interface 0 of the CMSIS-DAP personality with no driver installation.
The serial port keeps the CDC driver of Windows (`usbser.sys`), which binds with no setup file from Windows 10.
`src/usb/CmsisDapDescriptors.h` has these descriptors, built with `usb/MsOs20.h` of libArT:

1. The device descriptor has `bcdUSB` `0x0201`. Thus Windows reads the BOS descriptor.
2. The BOS descriptor has one device capability: the MS OS 2.0 platform capability.
   It gives the vendor code `0x20` and the size of the descriptor set (170 bytes).
3. Windows reads the descriptor set with a vendor request: `bmRequestType` `0xC0`, `bRequest` `0x20`, `wIndex` `7`.
   `UsbStack` answers it with `msos20::DescriptorSetHandler`, in the Address and Configured states.
4. The set has one function subset, for interface 0.
   It has the compatible ID `WINUSB` and the device interface GUID `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`.
   This is the GUID of the Arm CMSIS-DAP and DAPLink firmware.
   The device has one configuration, so the set has no configuration subset.

The Versaloon personality has `bcdUSB` `0x0200` and no BOS. It still needs a WinUSB driver from Zadig.

A driver that was installed before for the probe, for example with Zadig, has priority over the WinUSB compatible ID.
Thus such a probe keeps its old driver.
Uninstall the device and its driver in the Device Manager (see [Use the probe](cmsis-dap.md#use-the-probe)).
Windows also keeps data about the descriptors of a device in the registry key `HKLM\SYSTEM\CurrentControlSet\Control\usbflags\04039B3Crrrr`.
`rrrr` is the `bcdDevice` in hexadecimal.
If the probe does not get WinUSB after that, delete this key and plug in the probe again.

### Select the personality

The CMake option `VSL_PROTOCOL` selects the personality of the image:

| `VSL_PROTOCOL` | USB interface 0 | VID:PID | Sources |
|---|---|---|---|
| `versaloon` (default) | Versaloon vendor interface (`usb::VersaloonFunction`) | `0483:A038` | `usb/VersaloonFunction.cpp`, `protocol/` (without `cmsisdap/`) |
| `cmsis-dap` | CMSIS-DAP v2 (`usb::CmsisDapFunction`) | `0403:9B3C` | `protocol/cmsisdap/` |
| `dual` | One of the two, selected at each start | One of the two | All, and `personality/`, `board/SettingsFlashPage.cpp` |

The option defines `VSL_WITH_VERSALOON` for the `versaloon` image and `VSL_WITH_CMSIS_DAP` for the `cmsis-dap` image.
The `dual` image gets both defines and `VSL_DUAL_PERSONALITY`.
`usb::UsbStack` and `main.cpp` use the defines to select the USB function, the descriptors and the protocol objects.
The CMake file also removes the source files of the other personality.
Thus the CMSIS-DAP image does not link the Versaloon command and reply buffers (2 x 4096 bytes).
All images share the CDC-ACM serial port, the UART bridge, the debug log and the LEDs.

The CMSIS-DAP image differs from the Versaloon image in these points:

- It has no `VERSALOON_FW_UPDATE` command.
  Start the bootloader with the key (see [Firmware update](firmware-update.md)).
- It has no target power switch and no `GET_TVCC`.
- JTAG uses the CMSIS-DAP JTAG commands instead of the `JTAG_RAW` block (see [JTAG](cmsis-dap.md#jtag)).

The names of the image files have the text `-cmsis-dap` after the board name.

### Dual image

A dual image (`VSL_PROTOCOL=dual`) has both personalities.
One personality is active after each start.
The active personality has its descriptors, its USB function and its commands, as in the image with one personality.
The firmware changes the personality only with a reset.

The firmware stores the personality in the last 1 KB flash page of the application area, at `0x0800FC00`.
The linker script keeps this page out of the image, so a dual image has 1 KB less flash than the other images (see [Flash layout and size limit](firmware-update.md#flash-layout-and-size-limit)).
When the page holds no personality, the image uses `VSL_DEFAULT_PROTOCOL` (default `versaloon`).
`src/personality/PersonalityStore.h` has the page format.
The page is a log of 16-bit records, so the firmware erases the page only one time in 512 switches.

To change the personality, use one of these methods:

- In the Versaloon personality, send `SWITCH_PERSONALITY`: `[0x0E][personality u8]` (see [Versaloon commands](#versaloon-commands)).
- In the CMSIS-DAP personality, send `ID_DAP_Vendor0`: `[0x80][personality u8]` (see [Commands](cmsis-dap.md#commands)). The status of a refused switch is `0xFF`.
- Hold the key down for 3 s while the firmware runs. All LEDs flash three times when the 3 s end, and then stay on. Release the key. The firmware then selects the other personality.

The personality code is `0` for Versaloon and `1` for CMSIS-DAP.
`tools/vsl_personality.py` sends the commands from the host.
It needs the `pyusb` package:

```sh
python3 tools/vsl_personality.py show
python3 tools/vsl_personality.py switch cmsis-dap
python3 tools/vsl_personality.py switch versaloon
```

After a command, the firmware sends the reply, and then waits 10 ms.
Then the firmware disconnects from USB, writes the flash page and resets.
The host sees a disconnect, and then the device with the other VID:PID.
A switch to the active personality does not reset.
It also cancels a switch that the firmware did not do yet.
If the flash write fails, the firmware starts again with the old personality.

The key gesture has these rules:

- The LEDs flash when the key has been down for 3 s.
  A shorter press does nothing.
  The switch itself starts when you release the key, after the flashes.
  The firmware does not reset while the key is down, and waits 50 ms after the release, because a key that is down at reset starts the bootloader.
- The firmware counts the key only after it saw the key released.
  Thus a key that sticks does not change the personality at each start.
- The key at reset starts the bootloader, as before (see [Firmware update](firmware-update.md)).
- The ST-Link/V2 has no key. Use the commands.

The Versaloon personality of a dual image has `VERSALOON_FW_UPDATE`, as the Versaloon image.
The CMSIS-DAP personality has no command to start the bootloader: use the key at reset, or change to the Versaloon personality first.

The dual image links the objects of both personalities.
The Versaloon command and reply buffers (2 x 4096 bytes) are always in RAM.

## Versaloon commands

The host sends one command in one bulk OUT transfer on endpoint `0x03`.
It reads the reply in one bulk IN transfer on endpoint `0x82`.
All values are little-endian.
`src/protocol/` has the protocol code. It does not use the hardware, so the host unit tests check it.

The first byte of a command is the command code:

| Code | Commands | Format |
|---|---|---|
| `0x00`-`0x0F` | Common commands | One packet. |
| `0x20`-`0x7F` | `USB_TO_XXX` commands | `[code][length u16][data]`. `length` is the total size, with the 3-byte header. The command can have many packets. |

The firmware collects the packets until it has `length` bytes (`src/protocol/CommandAssembler.h`).
The command buffer and the reply buffer are separate, and each has 4096 bytes.
Thus a reply can be longer than its command.
`GET_INFO` sends the buffer size to the host.
The host sends no command and expects no reply that is larger than this size.

The common commands are:

| Command | Request | Reply |
|---|---|---|
| `GET_INFO` | `[0x00]` | `[buffer size u16][version text][0x00]` |
| `GET_TVCC` | `[0x01]` | `[target voltage in mV u16]`, from the Vref sense (see [POWER block](#power-block)). The value is 0 when the board has no sense or the measurement fails. |
| `GET_HARDWARE` | `[0x02]` | `[hardware ID u8]`, from the board definition (see [Boards](boards.md)). |
| `SWITCH_PERSONALITY` | `[0x0E][personality u8]` | `[status u8]`. The personality is `0` (Versaloon) or `1` (CMSIS-DAP). Status `0`: the firmware accepts the switch. Status `1`: the firmware refuses it (see below). |
| `FW_UPDATE` | `[0x0F][0xAA]` | `[status u8]` (see [Firmware update](firmware-update.md)). |

The version text has the form `versaloon-next-gen <version>, board <board>`.

`SWITCH_PERSONALITY` changes the USB personality of a dual image (see [Dual image](#dual-image)).
The firmware sends the reply, stores the personality and resets.
An image with one personality refuses the command.
The firmware also refuses a personality code other than `0` and `1`.
A switch to the active personality gets status `0`, and the firmware does not reset.
Other common commands have no reply.
The `USB_TO_XXX` commands go to the `USB_TO_ALL` dispatcher (see [USB_TO_ALL](#usb_to_all)).

The reply is one IN transfer.
When the size of the reply is a multiple of 64 bytes, a zero-length packet (ZLP) follows, so that the host read ends.
A reply of 4096 bytes has no ZLP.
The host reads 4096 bytes, so its read ends without a ZLP, and the ZLP would end the next read.
While the reply is on its way, the firmware does not take the next command.
The OUT endpoint can receive one packet of it into its second buffer, and then NAKs.
Thus the next command waits.

The firmware replies with a ZLP to a wrong command.
The host then sees a reply with the wrong size immediately, without a timeout.
A command is wrong in these cases:

- The header is shorter than 3 bytes, or `length` is smaller than 3.
- The transfer has more bytes than `length`.
- A short packet ends the transfer before `length` bytes (a ZLP is a short packet too).
- A common command, or an unused command code, is in a full packet of 64 bytes.
- `length` is larger than the command buffer (overflow).
- The `USB_TO_ALL` framing is malformed, or its reply is larger than the reply buffer (see [USB_TO_ALL](#usb_to_all)).

The ZLP comes at the end of the transfer of the host.
Thus the next transfer starts as a new command.
After an error on a full packet, the firmware discards the packets until a short packet ends the transfer.
If such a transfer ends with a full packet and no ZLP, the firmware also discards the next transfer.
For an overflow, the firmware discards the packets until `length`.

## USB_TO_ALL

OpenOCD sends all adapter operations in `USB_TO_ALL` commands (`0x7F`).
`src/protocol/UsbToAll.h` runs them.
A `USB_TO_ALL` command contains blocks, and most blocks contain sub-commands:

```text
command:      [0x7F][total length u16][block][block]...
block:        [type][block length u16][payload]
sub-command:  [command | index][payload length u16][payload]
```

The block length includes the 3-byte block header.
The payload length of a sub-command does not include its header.
The top 5 bits of a sub-command are the command: `INIT` `0x00`, `FINI` `0x08`, `CONFIG` `0x10`, `IN_OUT` `0x28`, `IN` `0x30` or `OUT` `0x38`.
The low 3 bits are the port index.

The reply joins the replies of all sub-commands in order: `[status][data]` for each sub-command.
The host knows the data size of each sub-command.
It rejects the whole reply when the reply size is not correct.
Thus a sub-command that fails also sends its full data size.
The status values are:

| Status | Value |
|---|---|
| `OK` | 0 |
| `FAILED` | 1 |
| `TIMEOUT` | 2 |
| `INVALID_INDEX` | 3 |
| `INVALID_PARA` | 4 |
| `INVALID_CMD` | 5 |
| `CMD_NOT_SUPPORT` | 6 |

The firmware has these blocks:

| Block | Type | Request payload | Reply |
|---|---|---|---|
| `INFO` | `0x7E` | none | `[OK][12-byte capability bitmap]` |
| `DELAY` | `0x61` | `[delay u16]` | `[OK]`. When bit 15 is set, bits 0-14 are the delay in ms. Else the delay is in µs. |
| `GPIO` | `0x23` | Sub-commands, see [GPIO block](#gpio-block) | `[status]`, and `[value u16]` for `IN` |
| `JTAG_RAW` | `0x47` | Sub-commands, see [JTAG_RAW block](#jtag_raw-block) | `[status]`, and `[TDO n bytes]` for `IN_OUT` |
| `SWD` | `0x46` | Sub-commands, see [SWD block](#swd-block) | `[status]`, and data for `IN` and `IN_OUT` |
| `POWER` | `0x60` | Sub-commands, see [POWER block](#power-block) | `[status]`, and `[mV u16]` for `IN` |

Bit n of the capability bitmap is set when the firmware has block `0x20 + n`.
The firmware sets the bits of `GPIO`, `SWD`, `JTAG_RAW`, `POWER`, `INFO`, `DELAY` and `USB_TO_ALL`.
A block that the firmware does not have gets one status byte `CMD_NOT_SUPPORT`.
OpenOCD then stops with an error.

### GPIO block

OpenOCD controls the reset lines SRST and TRST through the `GPIO` block (`src/protocol/GpioBlock.h`).
Each bit of a `u16` mask selects one pin of the debug connector:

| Bit | Signal | Mini and OtherUse | ST-Link/V2 |
|---|---|---|---|
| `0x0001` | SRST | PB11 | PB0 |
| `0x0002` | TRST | PB10 | none |
| `0x0004` | USR1 | none | none |
| `0x0008` | USR2 | none | none |
| `0x0010` | TCK | PB13 | PA5 |
| `0x0020` | TDO (input only) | PB14 | PA6 |
| `0x0040` | TDI | PB15 | PA7 |
| `0x0080` | RTCK (input only) | PA8 | none |
| `0x0100` | TMS | PB4 out, PB6 in | PB14 |

The sub-commands use port index 0. The values are `u16`, little-endian:

| Sub-command | Payload | Reply |
|---|---|---|
| `INIT` | none | `[status]` |
| `FINI` | none | `[status]` |
| `CONFIG` | `[mask][io][pull_en][pull]` | `[status]` |
| `OUT` | `[mask][value]` | `[status]` |
| `IN` | `[mask]` | `[status][value]` |

`CONFIG` sets the mode of each pin in `mask`:

| `io` | `pull_en` | `pull` | Pin mode |
|---|---|---|---|
| 1 | 0 | level | Push-pull output at the level |
| 1 | 1 | level | Open-drain output at the level |
| 0 | 0 | any | Floating input |
| 0 | 1 | 1 | Input with pull-up |
| 0 | 1 | 0 | Input with pull-down |

OpenOCD releases SRST as an input with pull-up, and asserts SRST as a push-pull output at the low level.
When the host closes the Versaloon interface, the firmware releases SRST and TRST: they become inputs with pull-up.
The host closes the interface with a USB bus reset, with `SET_CONFIGURATION 0`, or with a USB suspend.
For example, this occurs when OpenOCD stops during `reset halt`, or when the cable is disconnected.
Thus the target does not stay in reset until the next OpenOCD session.
The original firmware kept the reset lines asserted.
`OUT` writes the output level of the pins in `mask`.
On an input with pull, the output register selects the pull direction, as in the original firmware.
`IN` reads the level of the pins in `mask`.
For TMS on the Mini, `IN` reads PB6. A TMS input also makes PB4 an input.

A pin that the board does not have is ignored, and `IN` reads it as 0.
Thus OpenOCD can set TRST on the ST-Link/V2.
These requests get `INVALID_PARA`: a bit above `0x0100`, an output on TDO or RTCK, or a wrong payload size.
A port index other than 0 gets `INVALID_INDEX`.

The firmware checks the framing of all blocks and sub-commands before it runs the first one.
When a length in the framing does not fit, the firmware runs nothing and replies with a ZLP.
When the reply does not fit the reply buffer, the firmware stops at the first sub-command that does not fit, and replies with a ZLP.
The sub-commands before it have run.
The host sends no command with a reply that is larger than the buffer size of `GET_INFO`, so this is a host error.

`DELAY` waits in a busy loop, and the main loop stops for this time.
The longest delay is 32.767 s.
The OUT endpoint NAKs during the delay, and the serial port stops.

A top-level command `0x20`-`0x7E` is one block without the `USB_TO_ALL` header, as in the original firmware.

### Serial port during long commands

A command runs in the main loop, so the main loop stops until the command ends.
A long command can take seconds, for example a `JTAG_RAW` shift at 10 kHz or with a slow RTCK, or an `SWD` batch with many WAIT retries.
The RX ring buffer of the [UART bridge](uart-bridge.md) holds only about 89 ms at 115200 baud.

Thus the JTAG and SWD engines call a serial service during the command (`src/protocol/Background.h`).
The call comes at most once per millisecond, at these points:

- `JTAG_RAW`: between two bytes of a shift, or between two SPI chunks of the [fast path](#jtag-fast-path). TCK is at its idle level: low, or high on a board with the fast path.
- `SWD`: before each transfer and each WAIT retry, and between two bytes of a sequence. SWCLK is high.

The serial service does these steps:

1. It delivers the USB events of the serial port endpoints (`UsbStack::pollSerial()`, `src/usb/SerialEventQueue.h`).
   It stops at the first other event: a SETUP packet, a bus reset, a suspend, a resume, or a Versaloon endpoint event.
   That event waits until the command ends, so the order of the events does not change.
   Thus the service does not start a new command, and the host does not disconnect during the command.
   In the CMSIS-DAP personality, the service also delivers the events of the CMSIS-DAP endpoints.
   A CMSIS-DAP packet only waits in the processor, so this starts no command (see [CMSIS-DAP transport](cmsis-dap.md#transport)).
   Thus `DAP_TransferAbort` can stop a running transfer command.
2. It moves the UART data (`uart::pollBridge()`), or the debug log in a `VSL_DEBUG_CDC` build.

Each call sends one IN packet of up to 64 bytes: 64 bytes per ms.
At 115200 baud, the UART receives 11.5 bytes per ms.

The call makes one clock phase longer by the time of the service, some microseconds.
The clock does not get faster: each half period waits from its clock edge.
For JTAG in timed mode, the low half period starts again after the call.
Thus TDI and TMS change a full half period before the next rising edge, as for all other bits.
For SWD, each bit waits a full half period after the SWDIO write, so SWDIO also keeps its setup time.
The duty cycle of that one clock period changes.
The service takes some microseconds in each millisecond, so a shift gets a little slower.
The time of the service on the target is not measured yet.

Limits:

- A SETUP packet from the host stops the serial port until the command ends.
  For example, a terminal program sends SET_LINE_CODING when it opens the port.
- The `DELAY` block does not call the service (see above).
- The service runs at a byte boundary. At a very slow clock, the time between two calls is 1 ms plus one byte.
  At 10 kHz TCK, this is 1.8 ms.
- With adaptive clocking (RTCK), one byte has no such limit.
  Each of the 16 TCK edges of a byte can wait up to 10 ms for RTCK, so one byte can take up to 160 ms.
  This is more than the 89 ms of the RX ring buffer.
  Data loss occurs only when the target replies to each edge after more than about 5.5 ms.

### JTAG_RAW block

OpenOCD shifts all JTAG bits through the `JTAG_RAW` block (`src/protocol/JtagRawBlock.h`).
The firmware drives the pins with GPIO bit-bang (`src/jtag/JtagBitBang.h`).
On the Mini and the OtherUse, the [JTAG fast path](#jtag-fast-path) shifts the whole bytes with SPI and DMA.
The pins are TCK, TMS, TDI, TDO and RTCK in the table of the [GPIO block](#gpio-block).
TRST and SRST stay in the `GPIO` block.

The sub-commands use port index 0. The values are little-endian:

| Sub-command | Payload | Reply |
|---|---|---|
| `INIT` | none | `[status]` |
| `FINI` | none | `[status]` |
| `CONFIG` | `[kHz u32]` | `[status]` |
| `IN_OUT` | `[bitlen u32][TDI n bytes][TMS n bytes]` | `[status][TDO n bytes]` |

The byte count n is `ceil(bitlen / 8)`.
The first bit of a stream is the LSB of the first byte.
In the last TDO byte, the bits above `bitlen` are 0.

`INIT` makes TCK, TMS and TDI push-pull outputs, with TMS and TDI high.
TCK is low, or high on a board with the fast path.
TDO becomes an input with pull-up, so an open TDO reads 1.
RTCK becomes an input with pull-down.
`INIT` also sets TCK to 1000 kHz.
`FINI` makes TCK, TMS and TDI floating inputs.

`CONFIG` sets the TCK frequency in kHz.
The firmware rounds the half period up to full core clock cycles (72 MHz), so TCK is never faster than the request.
A request above the fastest rate of the bit-bang loop gives that fastest rate.
On a board with the fast path, `CONFIG` can select the SPI instead (see [JTAG fast path](#jtag-fast-path)).
An interrupt can make a TCK period longer, but not shorter.

`CONFIG` with 0 kHz selects adaptive clocking.
After each TCK edge, the firmware waits until RTCK has the same level.
When RTCK does not follow within 10 ms, `IN_OUT` stops and replies `TIMEOUT`.
TCK is then low, or high when RTCK did not follow a rising edge on a board with the fast path.
The original firmware did not report this error.
A board without RTCK (the ST-Link/V2) replies `INVALID_PARA` to 0 kHz.

For each bit, the firmware writes TDI and TMS while TCK is low, and waits for the low half period.
When TCK idles high, each bit starts with the falling edge.
Then it reads TDO and sets TCK high: the target samples TDI and TMS at this rising edge.
After the high half period, TCK goes low, and the target changes TDO.

A wrong payload size gets `INVALID_PARA`, and a port index other than 0 gets `INVALID_INDEX`.
`IN_OUT` replies the n TDO bytes also when it fails, as zeros.

### JTAG fast path

On the Mini and the OtherUse, two SPI peripherals and DMA shift the whole bytes of an `IN_OUT` (`src/jtag/JtagFastPath.h`, `src/board/BoardJtagFast.cpp`).
The bit-bang engine shifts the remaining bits, and all bits at a bit-bang speed.
A board has the fast path when its definition has `jtagFastPath` (see [Boards](boards.md)).
The ST-Link/V2 does not have it.

| Signal | Pin | Peripheral | DMA1 channel |
|---|---|---|---|
| TCK | PB13 | SPI2 SCK, master | - |
| TDO | PB14 | SPI2 MISO | 4 |
| TDI | PB15 | SPI2 MOSI | 5 |
| TCK link | PB3 | SPI1 SCK (remapped), slave | - |
| TMS | PB4 | SPI1 MISO (remapped) | 3 |

The board links TCK (PB13) to PB3, so TCK also clocks SPI1.
Both SPIs use clock polarity high and the second clock edge, LSB first.
TDI and TMS change at the falling edge.
The target and SPI2 sample at the rising edge.
The SPI clock idles high, so TCK also idles high between shifts.
Only the rising edge clocks the TAP, so the idle level has no effect on the target.

SPI2 runs from the 36 MHz APB1 clock, divided by 4 to 256.
Thus the SPI speeds are 9000, 4500, 2250, 1125, 562.5, 281.25 and 140.625 kHz.
The divider 2 (18 MHz) is off (`jtag::spiMinShift`).
SPI1 makes TMS valid up to 25 ns after the falling edge (tv(SO) in the STM32F103x8/B datasheet).
At 18 MHz, the half period is only 27.8 ns, so the target can sample an old TMS bit.
Enable 18 MHz only after a hardware test at 18 MHz passes.
`CONFIG` selects the engine with the faster TCK, and TCK is never faster than the request:

- The SPI gives the fastest SPI speed that is not above the request.
- Bit-bang gives the request. For the choice, its speed counts as the request, up to `jtag::bitBangMaxKHz` (1000 kHz).
- The SPI wins when its speed is equal to or higher than the bit-bang speed.
- Adaptive clocking (0 kHz) always uses bit-bang.

Examples: 9000 kHz and more give 9 MHz SPI, 4500 kHz gives 4.5 MHz SPI, 4000 kHz gives 2.25 MHz SPI, 2000 kHz gives 1.125 MHz SPI, 1000 kHz gives 1 MHz bit-bang.
The value of `jtag::bitBangMaxKHz` is conservative: 1000 kHz is the rate that was checked with the flash benchmark.
The fastest rate of the bit-bang loop is not measured.
A higher value selects bit-bang for some requests above 1125 kHz, and the SPI is then slower than the request less often.
To raise the value, set it to 18000 for a test, and measure TCK at a request of 4000 kHz with a logic analyzer.

A long shift runs in SPI chunks of about 1 ms, and the [serial service](#serial-port-during-long-commands) runs between two chunks.
The pins are GPIO between two transfers, so the bit-bang engine and the SWD engine can use them.
SPI1 also counts the TCK edges of bit-bang and SWD, so each transfer resets SPI1.

Limits:

- The SPI1 slave has no underrun flag. When DMA is late with a TMS byte, a TMS bit is wrong and the firmware does not see it.
  DMA channel 3 has the highest priority to prevent this.
- At 9 MHz, the half period is 55.6 ns. The target clock-to-output time and the cable delay must be less than this, else the TDO bits are wrong.
  Use a lower speed with a long cable.
- The OpenOCD flash speed with the fast path is measured with `tools/flash_benchmark.py` (see [docs/development.md](development.md#flash-speed-benchmark)).
  At 4500 and 9000 kHz, the total flash time is 1 to 2 % longer than with the original firmware.

### SWD block

OpenOCD runs all SWD transfers through the `SWD` block (`src/protocol/SwdBlock.h`).
The firmware drives the pins with GPIO bit-bang (`src/swd/SwdBitBang.h`).
SWCLK is the TCK pin.
SWDIO is the TMS pin: the firmware drives the TMS output pin and reads the TMS input pin (PB4 and PB6 on the Mini).

The sub-commands use port index 0. The values are little-endian:

| Sub-command | Payload | Reply |
|---|---|---|
| `INIT` | none | `[status]` |
| `FINI` | none | `[status]` |
| `CONFIG` | `[trn u8][retry u16][delay u16]` | `[status]` |
| `OUT` (sequence out) | `[bitlen u16][data n bytes]` | `[status]` |
| `IN` (sequence in) | `[bitlen u16]` | `[status][data n bytes]` |
| `IN_OUT` (transfer) | `[request u8][data u32]` | `[status][ack u8][data u32]` |

The byte count n is `ceil(bitlen / 8)`.
The first bit of a sequence is the LSB of the first byte.

`INIT` makes SWCLK and SWDIO push-pull outputs at the high level.
A separate SWDIO input pin gets a pull-up, so an open SWDIO reads 1.
`INIT` also sets the default configuration: one turnaround cycle, no retry, and delay 1 (235 kHz).
`FINI` makes SWCLK and the SWDIO output pin floating inputs.

`CONFIG` has these values:

- `trn`: the turnaround cycles of one transfer, for both turnarounds together, as in the original firmware.
  OpenOCD sends 2, which is one cycle at each turnaround.
  The firmware accepts 2, 4, 6 and 8, and replies `INVALID_PARA` to other values.
- `retry`: the number of times that the firmware repeats a transfer after a WAIT acknowledge.
- `delay`: the SWCLK frequency, as the OpenOCD `vsllink` driver calculates it:

| `delay` | 0 | 1 | 2 | 3 | 4 | 5 | 6 and more |
|---|---|---|---|---|---|---|---|
| SWCLK | 1.85 MHz | 235 kHz | 130 kHz | 102 kHz | 85 kHz | 72 kHz | 500000 / (`delay` + 1) Hz |

The firmware rounds the half period up to full core clock cycles (72 MHz).
Each half period waits from its SWCLK edge, so an interrupt makes a period longer, but not shorter.
The wait subtracts an estimate of the cycles of the loop code (`swd::loopOverheadCycles`).
A smaller value makes SWCLK slower, so the value is a lower bound: SWCLK is not faster than the table at `delay` 1 (measured).
The engines are compiled with `-O2`. The value 24 gives 232 kHz at `delay` 1, measured with a logic analyzer on the Versaloon Mini.
With `-O2`, the value 8 gave 210 kHz.
With `-Os`, the value 21 gave 230 kHz.
`delay` 2 and more are not measured. The margin there is small.
Measure SWCLK again after a change of the engine code.
The ST-Link/V2 board has other SWD pins and is not measured.

At `delay` 0, the firmware runs untimed, without the cycle counter.
Each half period is the loop code and 6 NOP instructions (`swd::untimedPadNops`).
The NOPs keep a minimum low half period: SWDIO has a setup time before the rising edge.
SWCLK is then 1.845 MHz (66 % duty), measured with a logic analyzer on the Versaloon Mini.
The original firmware bursts at 2.25 to 3 MHz.
If a target or a long cable fails at `delay` 0, use `delay` 1 (OpenOCD `adapter speed` below 1850 kHz).

`OUT` drives the bits on SWDIO. `IN` releases SWDIO and reads the bits.
SWCLK is high between bits.
In each bit, SWCLK goes low, the firmware writes SWDIO, and waits for the low half period.
Then the firmware reads SWDIO, and sets SWCLK high: the target samples SWDIO at this rising edge.

`IN_OUT` runs one transfer:

1. The firmware sets the start bit, the parity, the stop bit and the park bit of the request.
   OpenOCD sends them already. Bits 1-4 of the request (APnDP, RnW, A2, A3) stay.
2. The firmware sends the request, releases SWDIO for the turnaround, and reads the 3-bit acknowledge.
3. The firmware runs the data phase for each acknowledge, as the OpenOCD `bitbang` driver and the original firmware do.
   For a read, it reads 32 data bits and the parity bit with SWDIO released, then clocks the turnaround.
   Only an OK acknowledge gives data.
   For a write, it clocks the turnaround, then sends the 32 data bits and the parity bit.
   OpenOCD sets ORUNDETECT in the DP. Then the target expects the data phase also after WAIT and FAULT.
   A write without an acknowledge (multidrop `TARGETSEL`) also needs the data phase.
4. The firmware drives SWDIO low for 8 idle cycles, as the original firmware does.
   The target needs idle cycles to complete a write, and OpenOCD does not send them to this adapter.
5. After WAIT, the firmware writes DP `ABORT` with `ORUNERRCLR` (`0x10`), and then repeats the transfer.
   It does this up to `retry` times.
   With ORUNDETECT, a WAIT sets `STICKYORUN`, and the DP replies FAULT until `ORUNERRCLR` clears it.

The `ack` byte has the acknowledge in bits 0-2 (OK 1, WAIT 2, FAULT 4), and these flags:

| Flag | Meaning |
|---|---|
| `0x80` | FAULT acknowledge |
| `0x40` | WAIT acknowledge after all retries |
| `0x20` | Acknowledge that is not OK, WAIT or FAULT. No target gives 7. |
| `0x10` | Parity error in the read data |

The reply data is the read data.
For a write, and for a read that is not OK, the reply data is the request data.
The status is `OK` only when the `ack` byte is 1 (OK without a parity error). Else the status is `FAILED`.
OpenOCD does not read the `ack` byte, so it sees a failed transfer only through the status.
The original firmware replied `OK` in all cases.

A wrong payload size gets `INVALID_PARA`, and a port index other than 0 gets `INVALID_INDEX`.
`IN` and `IN_OUT` reply their full data size also when they fail.

### POWER block

The `POWER` block (`src/protocol/PowerBlock.h`) switches the 3.3 V supply of the target and measures the target voltage (Vref).
OpenOCD does not use it, but other tools do.
The sub-commands use port index 0:

| Sub-command | Payload | Reply |
|---|---|---|
| `INIT` | none | `[status]` |
| `FINI` | none | `[status]` |
| `CONFIG` | none | `[status]` |
| `OUT` | `[mV u16]` | `[status]` |
| `IN` | any, the firmware ignores it | `[status][mV u16]` |

`OUT` accepts only 0 (supply off) and 3300 (supply on).
Other values give `INVALID_PARA`.
`IN` replies the voltage at the Vref pin.
The voltage is the supply of the adapter when it is on, and the supply of the target when it is off.
`GET_TVCC` replies the same voltage.

The Mini and the OtherUse have the power switch on PB8.
The switch is on when PB8 is driven low, and off when PB8 floats.
The Vref sense is on PB0 (ADC1 channel 8) with a divider of 2.
The firmware calculates the voltage from the mean of 8 conversions, with a 3.3 V ADC reference.

The firmware protects the target and the adapter:

- `OUT 3300` gives `FAILED` and keeps the supply off when the voltage is above 1.8 V.
  The target then has its own supply, and two supplies on one rail can do damage.
- `OUT 3300` gives `FAILED` and keeps the supply off when the firmware cannot measure the voltage.
- The firmware switches the supply on, and waits 10 ms.
  When the voltage is then not above 1.8 V, the rail has a short circuit or an overload.
  The firmware then switches the supply off again and replies `FAILED`.
- While the supply is on, the main loop measures the voltage every 10 ms.
  The firmware switches the supply off when 3 measurements in a row are not above 1.8 V.
  A failed measurement does not count.

The supply stays on when the host goes away, so that a tool can switch the target on and then end.
It does not stay on after a reset of the adapter: the switch pin floats then.

A board can have no switch or no sense (see [Boards](boards.md)):

| Board has | `OUT 0` | `OUT 3300` | `IN` and `GET_TVCC` |
|---|---|---|---|
| Switch and sense | `OK` | With the protection above | Voltage |
| Sense, no switch | `OK` | `CMD_NOT_SUPPORT` | Voltage |
| Switch, no sense | `OK` | `OK`, with no protection, and no automatic switch-off | `IN`: `CMD_NOT_SUPPORT`, `GET_TVCC`: 0 |
| Neither | `OK` | `CMD_NOT_SUPPORT` | `IN`: `CMD_NOT_SUPPORT`, `GET_TVCC`: 0 |

The ST-Link/V2 has a sense but no switch.

## Check the enumeration

The host unit tests check the descriptors.
To check the device on the hardware, connect the dongle and do these steps.

On Linux:

1. Run `lsusb -v -d 0483:a038`. Make sure that the output shows the interfaces and endpoints of the tables above, and the IAD.
2. Make sure that the kernel log (`dmesg`) shows a new `/dev/ttyACM*` port.
3. Connect the UART TX pin to the UART RX pin (PA9 to PA10 on the Mini). Open the port in a terminal program, for example `picocom /dev/ttyACM0`. Make sure that the typed characters come back.
4. Run OpenOCD with `-f interface/vsllink.cfg`.
   Make sure that OpenOCD shows the version text of `GET_INFO`, for example `versaloon-next-gen v0.1.0, board mini`.
   Make sure that OpenOCD shows the capabilities, `USB_TO_XXX abilities: 0x00000008:0x000000C0:0xC0000002`.
   In JTAG mode, OpenOCD then scans the JTAG chain.
   In SWD mode, OpenOCD reads the DPIDR of the target.

On Windows 10 or later:

1. Make sure that the Device Manager shows a new COM port under "Ports (COM & LPT)", with the built-in CDC driver (`usbser.sys`).
2. Make sure that interface 0 shows as a separate device ("Versaloon").
   OpenOCD needs a WinUSB driver for interface 0, for example installed with Zadig.

