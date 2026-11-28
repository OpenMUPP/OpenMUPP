# CMSIS-DAP command processor

This document describes the CMSIS-DAP v2.1 command processor for SWD and JTAG.
The code is in `src/protocol/cmsisdap/`.
`usb::CmsisDapFunction` (`src/usb/CmsisDapFunction.h`) moves the packets over USB.
See the [CMSIS-DAP personality](usb-device.md#cmsis-dap-personality).
The build option `MUPP_PERSONALITIES` with `cmsis-dap` puts the function and the processor into the firmware image (see [Build](#build)).

## Build

Use the preset of the board with the `-cmsis-dap` suffix, for example `mini-cmsis-dap`:

```sh
cmake --preset mini-cmsis-dap
cmake --build --preset mini-cmsis-dap -j
```

Or set the CMake option `MUPP_PERSONALITIES` to `cmsis-dap`:

```sh
cmake -B build/mini-dap -DCMAKE_TOOLCHAIN_FILE=cmake/gcc_arm.cmake -DBOARD=mini -DMUPP_PERSONALITIES=cmsis-dap
cmake --build build/mini-dap -j
```

The build then writes `openmupp-<board>-cmsis-dap-<variant>-<type>-<version>.hex` and `.bin`.
Write the image as in [Firmware update](firmware-update.md).
All boards build with this value.

The `cmsis-dap` image has these properties:

- USB interface 0 is CMSIS-DAP v2, with `1209:0001`. The Versaloon interface is not in the image.
- The CDC-ACM serial port, the UART bridge, the debug log and the LEDs are as in the Versaloon image.
- The image does not link the Versaloon command buffers, the `USB_TO_XXX` blocks and the power block.
- The SWD engine, the JTAG engine and the debug connector pins (`DAP_SWJ_Pins`) are the same ones as in the Versaloon image.
  On the Mini and the CH32V307 boards, the JTAG engine uses the SPI and DMA fast path at a TCK of 1125 kHz or more.

A `cmsis-dap` image has only this personality.
A multi image has `cmsis-dap` and other personalities, for example `MUPP_PERSONALITIES=versaloon,cmsis-dap`.
Select the active one with `ID_DAP_Vendor0`, with `SWITCH_PERSONALITY` of the Versaloon protocol, with the switch request on EP0, or with the key (see [Multi image](usb-device.md#multi-image)).
The switch request on EP0 and the request of the MS OS 2.0 descriptor set use the same handler (see [Switch request on EP0](usb-device.md#switch-request-on-ep0)).

## Limits

- JTAG has no adaptive clocking (RTCK). TCK is in whole kHz, at least 1 kHz. See [JTAG](#jtag).
- A JTAG chain has up to 8 devices. The IR length of a device is 1 to 32 bits.
- On JTAG, `DAP_SWJ_Pins` must not leave TCK at a level that is not the idle level of the JTAG engine.
  Else the next scan has an extra TCK edge or a missing TCK edge.
  The idle level is high on the boards with the SPI fast path (the Mini and the CH32V307 boards), and low on the ST-Link/V2-1 and the mini clone.
- pyOCD 0.45.1 configures a JTAG chain with one device of IR length 4. A target with more than one TAP, for example the STM32F1, needs another chain setting (see [JTAG hardware test](#jtag-hardware-test)).
- SWO only on the CH32V307 boards, through `DAP_SWO_Data` or the SWO trace endpoint. UART (NRZ) mode up to 9 Mbaud, Manchester mode up to 500 kbit/s (a baud rate of 1 MHz, see [SWO Manchester mode](#swo-manchester-mode)). See [SWO commands](#swo-commands).
- The `DAP_UART_*` commands use the UART of the UART bridge. Only one side has the UART at a time: the CDC-ACM serial port or the DAP commands. See [UART commands](#uart-commands).
- A `MUPP_DEBUG_CDC` build has no UART bridge and no `DAP_UART_*` commands.
- On JTAG, `DAP_TransferAbort` stops a transfer command only between two transfers. It does not stop the WAIT retries of one JTAG transfer. On SWD, it also stops the WAIT retries. It does not stop other commands, for example `DAP_Delay`.
- `DAP_TransferAbort` reaches the processor during a command only when a slot is free. When the running command and 3 other packets use the 4 slots, the OUT endpoint NAKs, and the abort waits until the command ends.
- 64-byte packets, and up to 4 packets in flight. On the CH32V307 at high speed, 512-byte packets (see [High speed](usb-device.md#high-speed-ch32v307)).
- The LEDs show the target power, the command activity, the probe errors and the USB state. `DAP_HostStatus` does not change them. See [boards.md](boards.md#start-up-and-leds).
- The hardware test passed on Linux with OpenOCD, pyOCD and probe-rs on SWD, and with OpenOCD and pyOCD on JTAG. probe-rs passed on JTAG at 1000 kHz in the multi image test (see [usb-device.md](usb-device.md#multi-image-hardware-test)). See also [Hardware test](#hardware-test) and [JTAG hardware test](#jtag-hardware-test).

## Use the probe

The probe has the USB ID `1209:0001` and the product name `OpenMUPP CMSIS-DAP`.
The host tools find the CMSIS-DAP v2 interface by the text `CMSIS-DAP` in its interface string.

On Linux, a udev rule gives a normal user access to the probe.
Copy `etc/70-openmupp-cmsis-dap.rules` to `/etc/udev/rules.d/`.
The file number must stay below 73, because systemd applies the `uaccess` tag in `73-seat-late.rules`.
The rule checks the product string, because other devices use the test ID too.
The file has this line:

```text
SUBSYSTEM=="usb", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", ATTRS{product}=="OpenMUPP CMSIS-DAP", MODE="0660", TAG+="uaccess"
```

Then reload the rules with `udevadm control --reload` and plug in the probe again.

On Windows 10 or later, the probe needs no driver installation and no Zadig.
The device has MS OS 2.0 descriptors (see [MS OS 2.0 descriptors](usb-device.md#ms-os-20-descriptors)).
Windows binds WinUSB to the CMSIS-DAP interface and its own CDC driver (`usbser.sys`) to the serial port.
With `MUPP_AVR_USB_ID=medbg`, WinUSB binds interface 1: the HID interface is interface 0 (see [avr.md](avr.md#bloom)).
Windows 8.1 binds WinUSB to the CMSIS-DAP interface too, but the serial port needs a driver setup file there.

If you installed a driver for the probe before, for example with Zadig, Windows keeps that driver.
Uninstall the device in the Device Manager, and also remove its driver.
Windows 10 calls this option "Delete the driver software for this device".
Windows 11 calls it "Attempt to remove the driver for this device".
Then plug in the probe again.

The CMSIS-DAP and WCH-Link personalities share the ID `1209:0001`, the serial number and the `bcdDevice`.
The WCH-Link personality needs a Zadig WinUSB driver on Windows, and Windows keeps that driver for the CMSIS-DAP personality too.
Then the serial port of the CMSIS-DAP personality does not work.
In a multi image with both personalities, remove the Zadig driver before you use the CMSIS-DAP personality.
Delete the registry key `usbflags\12090001rrrr` too (see [usb-device.md](usb-device.md)).

OpenOCD:

```sh
openocd -f interface/cmsis-dap.cfg -c "cmsis-dap vid_pid 0x1209 0x0001" -f target/stm32f1x.cfg
```

Older OpenOCD versions use the command `cmsis_dap_vid_pid`.

pyOCD finds the probe by its interface string.
Use `pyocd list` to see the probe and `pyocd gdbserver -t <target>` to start a GDB server.

probe-rs finds the probe in the same way.
Use `probe-rs list` and then `probe-rs info`.

## Layers

| File | Task |
|---|---|
| `DapProcessor.h`, `DapProcessor.cpp` | Parse the request packets, run the commands, write the response packets. |
| `DapSwd.h` | The interface of the SWD engine, as the processor needs it. |
| `BitBangDapSwd.h` | The adapter from `DapSwd` to the SWD bit-bang engine (`swd::BitBangPort`). |
| `DapJtag.h`, `DapJtag.cpp` | The JTAG-DP: the device chain, the IR and DR scans, and the transfers on a JTAG port. |
| `DapUart.h` | The interface of the UART, as the `DAP_UART_*` commands need it. |
| `src/uart/DapUartChannel.h` | The `DapUart` on the UART of the UART bridge, and the owner switch between the serial port and the DAP commands. |
| `DapSwo.h` | The interface of the SWO receiver, as the `DAP_SWO_*` commands need it. |
| `src/swo/SwoReceiver.h` | The `DapSwo` of the CH32V307: UART mode on USART2 and DMA1 channel 6, Manchester mode on TIM5 and DMA2 channels 1 and 2. |
| `src/swo/ManchesterDecoder.h` | The software decoder of the SWO Manchester mode. |

The processor has no CMSIS header and no hardware header.
It uses these objects:

- A `DapSwd` for the SWD transfers and sequences.
- A `protocol::JtagPort` (`src/protocol/JtagPort.h`, on target `board::BoardJtag`) for JTAG. It is optional.
- A `protocol::GpioPins` (`src/protocol/GpioPins.h`, on target `board::BoardGpio`) for `DAP_SWJ_Pins` and `DAP_ResetTarget`.
- A `Platform` for the microsecond time, the delay and the LEDs.
- A `DeviceInfo` with the strings and the capabilities of `DAP_Info`.
- A `DapUart` (`DapUart.h`, on target `uart::DapUartChannel`) for the `DAP_UART_*` commands. It is optional.
- A `DapSwo` (`DapSwo.h`, on target `swo::SwoReceiver`) for the `DAP_SWO_*` commands. It is optional. `setSwo()` gives it to the processor, and tells it if the transport has the SWO trace endpoint.

## Transport

The USB function calls `submit()` for each request packet.
`submit()` only stores the packet.
`UsbStack::poll()` then calls `CmsisDapFunction::poll()` in the main loop.
Each call runs one stored packet with `runNext()` and sends its response immediately.
Between two packets, `UsbStack::poll()` delivers the USB events again.
Thus the InComplete event of the last response frees the IN endpoint, and the next response goes out while the next packet runs.
This loop has no limit. It ends only when no packet is stored after an event delivery.
A host that sends packets in a pipeline (pyOCD, OpenOCD) keeps a packet stored during a long memory read or a flash download.
During that time, the rest of the main loop does not run: the LEDs do not change, the key gesture of a multi image waits, and the debug log waits.
The serial service still runs during the commands, so the UART bridge loses no data.
Before `DAP_TransferAbort`, the commands ran inside the event delivery, with the same effect.
Thus the OUT endpoint stays armed while a command runs, if a slot is free.
During a USB suspend, `CmsisDapFunction::poll()` runs no packet. The stored packets run after the resume.
A packet that arrives during a command waits until that command ends.
The USB function sends the response packets in order.
The packet size is 64 bytes, and up to 4 packets are in flight.
The transport can set a larger packet size with `setPacketSize()`, up to `Processor::maxPacketSize`: 512 bytes on the CH32V307, 64 bytes on the STM32 boards.
Set it while no packet is in flight. `CmsisDapFunction` sets it at `SET_CONFIGURATION` from the endpoint size.
Call `canSubmit()` before `submit()`.
Each packet without a response takes one of the 4 slots.

## CMSIS-DAP v1 (HID)

The build option `MUPP_CMSIS_DAP_HID` (default `OFF`) adds a CMSIS-DAP v1 interface to the CMSIS-DAP personality, next to the v2 interface.
The AVR tools (avrdude, Bloom) need it, because they use CMSIS-DAP v1 with the Microchip EDBG commands.
The STM32F1 boards do not have it: their USB packet memory is full.

- **Interface**: interface 3 (interface 0 with `MUPP_AVR_USB_ID=medbg`, see [avr.md](avr.md#bloom)), HID class, interrupt OUT `0x05` and interrupt IN `0x85` (see [usb-device.md](usb-device.md#cmsis-dap-personality)).
- **Packet size**: one command or response is one report of 64 bytes, also at high speed. `DAP_Info` on this interface reports the packet size 64. The host pads a command with zeros, and the probe pads each response with zeros.
- **Processor**: the interface has its own `cmsisdap::Processor`, on the same SWD and JTAG engines as the v2 interface. It has the commands of the v2 interface except `ID_DAP_Vendor0`, `ID_DAP_Vendor2`, `ID_DAP_Vendor3`, `ID_DAP_Vendor4`, `ID_DAP_Vendor5`, `ID_DAP_Vendor6`, the `DAP_UART_*` commands and the `DAP_SWO_*` commands: they reply `ID_DAP_Invalid`. It has the RISC-V bridge: `ID_DAP_Vendor1` sets its mode, and `DAP_SWJ_Pins` tells it that the pins changed.
- **Target pins**: the first interface that drives the target pins owns them (`protocol/TargetPinOwner.h`). `DAP_Connect`, `DAP_SWJ_Pins` with an output and `DAP_ResetTarget` take the pins. `DAP_Connect` takes the owner role and sets the port, but it does not change the pins: the pins change at the first transfer or sequence command (see `DAP_Connect` in the command table). While the other interface owns them, `DAP_Connect` replies port 0, `DAP_SWJ_Pins` only reads the pins, and `DAP_ResetTarget` replies `DAP_ERROR`. `DAP_Disconnect`, a bus reset or a suspend of the owner frees them. A host tool that stops without `DAP_Disconnect` (killed, crashed) keeps the pins until a bus reset or a replug. While the other interface owns the pins, `ID_DAP_Vendor1` replies `DAP_ERROR`, because the bridge is the JTAG port of both interfaces. The clock and transfer settings of an interface reach the engines only while it owns the pins, and `DAP_Connect` applies them.
- **Scheduling**: `UsbStack::poll()` runs one command of each interface per round, so one interface does not block the other.
- **Transfer abort**: `DAP_TransferAbort` stops a running command on both interfaces. During a command, the serial-only event delivery also gives the packets of both CMSIS-DAP interfaces (`usb/SerialEventQueue.h`), so a report of one interface does not hold the serial port during a command of the other.
- **Tools**: OpenOCD selects the v2 interface first. Use `cmsis-dap backend hid` for the v1 interface. pyOCD also selects v2 first.
- **AVR commands**: with `MUPP_CMSIS_DAP_AVR`, the vendor commands `0x80`-`0x82` of this interface are the AVR commands of the EDBG tools, not `ID_DAP_Vendor0`-`2`. Thus `ID_DAP_Vendor1` (the bridge mode) is then only on the v2 interface. See [AVR commands](#avr-commands).

## AVR commands

The build option `MUPP_CMSIS_DAP_AVR` (default `OFF`) adds the AVR commands of the Microchip EDBG tools to the CMSIS-DAP v1 (HID) interface.
avrdude (`jtagice3_isp`, `jtagice3_dw`, `jtagice3`) and Bloom use these commands.
The option needs `MUPP_CMSIS_DAP_HID`.
Only the CH32V307 boards have it, because the AVR ISP engine has no code for the other chips.
The protocol is in "EDBG-based Tools Protocols" (Microchip DS50002630A), chapters 3.2.1 and 5.
The code is `src/protocol/avr/EdbgTransport.h`.

### Transport

The host sends a command frame in fragments with `AVR_CMD`, and reads the response frame in fragments with `AVR_RSP`.
Each fragment has the field `m << 4 | n`: fragment `m` of `n`, with `n` 15 or less.

| ID | Command | Request | Response |
|---|---|---|---|
| `0x80` | `AVR_CMD` | `[0x80][fragment][size u16][data]` | `[0x80][code]`: `0x00` more fragments to come, `0x01` the frame is complete, `0xFF` the fragment is rejected. |
| `0x81` | `AVR_RSP` | `[0x81]` | `[0x81][fragment][size u16][data]`, 60 bytes of data in each fragment. Fragment `0x00` and size 0: no response. |
| `0x82` | `AVR_EVT` | `[0x82]` | `[0x82][size u16][event frame]`. Size 0: no event. The AVR8 scope gives break events (see [AVR8 generic scope](#avr8-generic-scope)). |

The sizes are big-endian.
The probe sends the `AVR_CMD` response of the last fragment first.
Then the next AVR command (`AVR_RSP`, `AVR_EVT` or `AVR_CMD`) runs the frame before it gives its own response.
Thus the response is ready for the next `AVR_RSP`, and an event of the command comes after it.
AVaRICE reads the `AVR_CMD` response for 200 ms only, the `AVR_RSP` response for 500 ms, and the `AVR_EVT` response for 200 ms.
One flash page write takes about 0.5 s at 200 kHz (256 bytes, ATmega2560, issue #121). This is near the 500 ms of AVaRICE.
On JTAG, a run, run to or step with a software breakpoint change thus gives its response at once, and the `AVR_EVT` polls write the pages (see [AVR8 scope on JTAG](#avr8-scope-on-jtag)).
After a late response, AVaRICE reads each later response one report too late, and the session stops (issue #115). Only a restart of AVaRICE repairs it.
A long command holds the HID interface, as a long DAP command does.
A `DAP_Disconnect`, USB suspend or bus reset drops a frame that did not run.

All fields come from the host, and the probe checks them:

- A fragment 1 starts a new frame. It drops a frame that is not complete, and a response that the host did not read.
- The probe rejects these fragments with code `0xFF` and drops the frame: `m` or `n` is 0, `m` is more than `n`, `m` is not the next fragment, `n` is not the `n` of fragment 1, the size is more than the report has, or the frame is longer than 512 bytes. The next `AVR_RSP` then gives no response.
- The command frame is `[0x0E][version 0][sequence u16][handler][payload]`. The response frame is `[0x0E][sequence u16][handler][payload]`, with the sequence of the command. The sequence is little-endian. A frame shorter than 5 bytes, or with another start byte or version, gets no response.
- In a `DAP_ExecuteCommands`, the AVR commands reply `ID_DAP_Invalid`, because each one takes the rest of the report.

### Scopes

The handler selects the scope.
The responses of the scopes, except AVR ISP, are OK `[0x80][0]`, LIST `[0x81][0][data]`, DATA `[0x84][1][data][0]` and FAILED `[0xA0][0][code]`.
SET and GET are `[command][0][context][address][bytes][data]`: a range of parameters in one context.

| Handler | Scope | Commands |
|---|---|---|
| `0x00` | Discovery | `QUERY`: the handlers (context `0x00`), the tool name (`0x80`) and the serial number (`0x81`). |
| `0x01` | Housekeeping | Sign-on (`0x10`) and sign-off (`0x11`): OK. `GET` of context 0: the hardware version, the firmware major and minor version, and the build number (addresses 0-4). `GET` of context 1, address 0: the target voltage in mV, little-endian (Vref, PB0 on the CH32V307 boards). `SET` fails: the parameters are read-only. The firmware upgrade command (`0x50`) fails with code `0x10` (not supported), and the probe continues. |
| `0x11` | AVR ISP | The STK500v2 commands of the ISP engine (`src/protocol/avr/IspScope.h`). |
| `0x12` | AVR8 generic | debugWIRE programming and debug, and JTAG programming (`src/protocol/avr/Avr8Scope.h`). See [AVR8 generic scope](#avr8-generic-scope). avrdude also sets its parameters before ISP programming. |
| `0x20` | EDBG control | `SET` and `GET` of the target power (context 0, address `0x10`, 1 byte): 0 off, 1 on. See [Target power](#target-power). The other parameters fail with code `0x01`. |

Another handler fails with code `0x10`.
A host that goes away (bus reset or deconfigure) loses its frame, its response, the AVR8 parameters and the breakpoints.
`DAP_Disconnect` and USB suspend also drop the AVR8 parameters and the breakpoints. Thus each host connection starts without a device context.
The end of a debugWIRE or JTAG session (sign-off, "disable debugWIRE") drops the device context. Thus each session needs its own device context.

### AVR8 generic scope

The AVR8 scope runs the debugWIRE (dW) target operations of `src/avr/DebugWireTarget.h`, and the JTAG programming and debug of `src/avr/AvrJtagTarget.h` (see [AVR8 scope on JTAG](#avr8-scope-on-jtag)).
The probe has one scope for each interface. `Avr8Select` sends `SET` to both scopes, and the other commands to the active scope.
Without an active scope, they go to the scope of the variant and the physical interface of the host.
The host sends the device parameters, so the probe has no part database.
Values are little-endian. Flash, SRAM and EEPROM addresses are byte addresses. The PC, `RUN_TO` and the break event use word addresses.

| Context | Parameters |
|---|---|
| 0 | Address 0: variant (1 = debugWIRE, 2 = mega for JTAG). Address 1: function (1 = programming, 2 = debugging). |
| 1 | Address 0: physical interface (4 = JTAG, 5 = debugWIRE). Address `0x01`: JTAG daisy chain, 4 bytes. Only 0 is accepted, else code `0x39`. Address `0x20`: JTAG programming clock, address `0x21`: JTAG debug clock, kHz u16 each, 200 kHz by default. 0 kHz gives code `0x3B`. |
| 2 | The device context, as `struct mega_device_desc` of avrdude: flash page size (`0x00`), flash size (`0x02`), boot start (`0x0A`), EEPROM size (`0x10`), DWDR (`0x18`), EEARH (`0x19`), EEARL (`0x1A`), EECR (`0x1B`), EEDR (`0x1C`), SPMCSR (`0x1D`). |
| 3 | Address 0: 1 lets the timers of the target run while it is stopped. |

The host can set the parameters in any order. Bloom sets the JTAG clock and the daisy chain before the variant.

avrdude and Bloom send DWDR and the EEPROM registers as I/O addresses (`0x00`-`0x3F`), and SPMCSR as a data space address (I/O address + `0x20`).
The probe converts SPMCSR to its I/O address.
The probe uses the boot start only to know if the device has a boot section.
The boot start is not a reliable flag: Bloom sends 0 without a boot section, but avrdude sends a value for all parts.
Thus the probe takes a boot section only when the boot start is not 0 and the flash is larger than 4 KB.
No debugWIRE part with 4 KB flash (for example the ATmega48) has a boot section.
On a tinyAVR with more than 4 KB flash, the RWW enable after a page write is CTPB (clear the page buffer) and has no effect.
For SPM, the probe sets the PC to the start of the last flash page.
This address is in the boot section for all BOOTSZ fuse values.
The boot address of avrdude is the start of the last 4 pages, and that address is not in the smallest boot section of an ATmega168.

| Command | Function |
|---|---|
| `0x10` Activate physical | Needs variant 1, physical 5 and a valid device context, else code `0x33`. The probe connects and stops the target with a break. |
| `0x11` Deactivate physical | A stopped target loses its software breakpoints, gets a reset and runs. Then the probe releases the line. Without an active physical: OK. |
| `0x12` Get ID | DATA `[ID low][ID high][0x1E][0]`, for example `0F 95 1E 00` on an ATmega328P. |
| `0x13` Attach, `0x14` Detach | Attach with break: the target stays stopped, and a break event comes. Detach: the probe removes the software breakpoints from flash, and the target runs. |
| `0x15`, `0x16` Programming mode | OK. The memory commands work without a programming mode. |
| `0x17` Disable debugWIRE | dW is off until the next power cycle. ISP then works. |
| `0x21` Read memory, `0x22` masked read | `[type][address u32][length u32]`, for the masked read also a bit mask (bit 0: do not read, the byte is 0). |
| `0x23` Write memory | `[type][address u32][length u32][async][data]` |
| `0x30` Reset, `0x31` Stop, `0x34` Step | The target stops, and a break event comes. |
| `0x32` Run, `0x33` Run to | Run to uses the hardware breakpoint for one run. |
| `0x35`, `0x36` PC read and write | PC response `[0x83][0][pc u32]` |
| `0x40`, `0x41` Hardware breakpoint | Number 1 only: dW has one hardware breakpoint. Other numbers: code `0x81`. |
| `0x43`, `0x44`, `0x45` Software breakpoints | Up to 32. |

Erase (`0x20`), CRC (`0x24`) and page erase (`0x50`) fail with code `0x90`.
Without an active physical, the other commands fail with code `0x31`. Unknown commands fail with code `0x91`.

The memory types:

- SRAM (`0x20`): the data space with the registers, I/O and SRAM. DWDR reads as 0, and a write does not change it.
- SPM (`0xA0`) and flash page (`0xB0`): a write must have whole pages at a page address, else code `0x37`. The probe erases and writes each page.
- EEPROM (`0x22`) and EEPROM page (`0xB1`): one byte at a time.
- Signature (`0xB4`): 3 bytes, `0x1E` and the dW ID.

A read gives up to 505 bytes.
An address outside the device gives code `0x38`. A command that is too short gives code `0x35`.

Software breakpoints: the probe writes the `BREAK` instruction into flash at the next run, run to or step.
Detach removes all software breakpoints from flash.
After a clear, the probe writes the original word back at the same time.
A flash read gives the flash with the `BREAK` instructions, as the EDBG tools do.
A flash write keeps the breakpoints of its page, with the new original words.
To run or step from a software breakpoint, the probe writes the original word, steps, and writes `BREAK` again.
Thus each run from a software breakpoint costs two page writes. The flash of an AVR has about 10 000 write cycles.

The break event is the frame `[0x0E][0][sequence u16][0x12][0x40][pc u32][cause][0x01][0x00]`.
The cause is 1 at a breakpoint, else 0.
While the target runs, each `AVR_EVT` checks the dW line one time.

A bus reset, a USB suspend and `DAP_Disconnect` on the HID interface end the dW session.
The probe stops the target, removes the software breakpoints from flash, lets the program run and releases the line.

### AVR8 scope on JTAG

avrdude (`type = "jtagice3"`) sends variant 2 (mega), function 1 (programming) and physical 4 (JTAG).
Bloom sends function 2 (debugging). The probe has the JTAG programming mode and the debug mode (attach) on the same pins.
The device context has the flash page size (`0x00`, 2 to 256 bytes), the flash size (`0x02`, up to 256 KiB), the EEPROM size (`0x10`), the EEPROM page size (`0x12`), the OCD revision (`0x13`, 0 to 4) and the full-page bit stream flag (`0x15`).
A value out of range gives code `0x33` at activate physical.

| Command | Function |
|---|---|
| `0x10` Activate physical | The probe drives the pins at the programming clock and reads the IDCODE. DATA `[IDCODE u32]`. An IDCODE that is not from Atmel (bits 11..0 not `0x03F`) gives code `0x20`. The reset flag is not used. AVaRICE sends the device context after this command. Without a device context (all 0), get ID, detach and deactivate physical work. Each other command takes the device context first, and gives code `0x33` when it is not valid. |
| `0x11` Deactivate physical | Detach, then the probe leaves the programming mode and releases the pins. The device context stays for the next activate physical, because Bloom sends it only one time. |
| `0x12` Get ID | DATA `[IDCODE u32]`, for example `3F 70 58 49` (an ATmega32U4: part number `0x9587`). |
| `0x13` Attach | The debug mode at the debug clock. Not in the programming mode (code `0x32`). When the OCD answers, the core stops and a break event comes, also with break 0 (Bloom waits for it). Without an OCD answer (OCDEN not programmed), attach gives OK, and each command that needs the OCD gives code `0x50`. A second attach is a stop. |
| `0x14` Detach | The probe removes the hardware breakpoints and the software breakpoints, and a stopped core runs. |
| `0x15`, `0x16` Programming mode | The JTAG programming mode. The core is in reset in the programming mode. Enter in the debug mode removes the hardware breakpoints and the software breakpoints, and ends the debug mode. A chip erase removes all software breakpoints from the list of the probe. Bloom loads a program in this way, and then sends deactivate physical, activate physical and attach. |
| `0x20` Erase | `[mode][address u32]`: mode 0 is the chip erase. Other modes give code `0x39`. |
| `0x21` Read memory, `0x22` masked read, `0x23` Write memory | In the programming mode or in the debug mode, else code `0x32`. The masked read is for the debug mode only. |
| `0x30` Reset, `0x31` Stop, `0x34` Step | Debug mode only. The core stops, and a break event comes. Without attach, the command starts the debug mode first, as attach does, but without the break event of attach. AVaRICE sends no attach. In the programming mode, the command gives code `0x32`. |
| `0x32` Run, `0x33` Run to | Debug mode only. Without attach, the command starts the debug mode first, as reset does. Run to uses OCD register 0 for one run. |
| `0x35`, `0x36` PC read and write | Debug mode only. The PC can be above 16 bits (ATmega2560). |
| `0x40`, `0x41` Hardware breakpoint | Debug mode only. Numbers 1 to 3 (OCD registers 1 to 3), else code `0x81`. Type 1 (program): a byte address, as Bloom sends it. Type 2 (data) and type 3 (data mask): see [Data breakpoints](#data-breakpoints). Other types give code `0x90`. The probe writes the breakpoints at the next run or step. |
| `0x43`, `0x44`, `0x45` Software breakpoints | Set and clear: debug mode only, else code `0x32`. Up to 32, byte addresses. |

Software breakpoints: the probe writes the `BREAK` instruction into flash at the next run, run to or step, as on debugWIRE.
The probe writes the flash page with `SPM` in the debug mode (see [avr-jtag-ocd.md](avr-jtag-ocd.md#flash-write-in-debug-mode)).
A run, run to or step with a software breakpoint change gives its OK response before the page write (issue #121):

- Each later `AVR_EVT` poll does one part of the page write, about 50 ms at the debug clock. The poll gives no event.
- The parts are: the page read, the page buffer loads, the page erase and write, and the read back. Only the erase and write part changes the flash: the page buffer keeps its data over the page erase (datasheet, "alternative 1"). Thus a host that goes away between two polls leaves the old page in flash.
- The core starts, or does the step, after the last page. The break event of the command comes after that.
- A page write that fails gives a break event at the PC, with cause 0. The core does not start. The breakpoint change stays for the next commit.
- After a failed page write, the next run, run to or step with a breakpoint change gives the failure at once (code `0x13` for a read back that does not agree), without a new page write. Thus a host that resumes at once, for example avr-gdb `next`, shows the error and does not try the same page write again and again. A run or step without a change runs and drops the failure.
- Stop, reset, attach, detach, deactivate physical, enter programming mode and a `DAP_Disconnect` cancel the page writes that did not start the erase. The core does not start. Stop gives its OK response and a break event at the PC at once. The breakpoint changes stay for the next commit, or detach removes them.
- Each other command first completes the page writes, then the run or step, and then runs. This command can take about 0.5 s at 200 kHz for each 256-byte page.

AVaRICE polls `AVR_EVT` every 50 ms while it waits for the break event, so the page write completes in the polls.
The erase and write part does not get shorter at a lower debug clock. It needs about 2400 TCK cycles and about 9 ms of flash time. At a debug clock below about 16 kHz, this poll takes more than the 200 ms of AVaRICE.
Detach and deactivate physical still remove the software breakpoints before they give the response.
AVaRICE sends the next command at once after them, so a part of the page write in a poll does not help there.
At 200 kHz such a detach takes about 0.5 s for a 256-byte page, near the 500 ms of AVaRICE. This page write uses the programming clock (200 kHz by default), not the debug clock. Thus a faster `-B` of AVaRICE does not make it shorter (issue [#126](https://github.com/OpenMUPP/OpenMUPP/issues/126)). AVaRICE sends it at the end of the session only.
To run or step from a software breakpoint, the OCD executes the original instruction in place of the `BREAK`, without a page write.
The probe does these instructions itself on the CPU context and the data space: `RJMP`, `RCALL`, `JMP`, `CALL`, `LDS`, `STS`, the branches (`BRBS`, `BRBC`), the skips (`CPSE`, `SBRC`, `SBRS`, `SBIC`, `SBIS`), `RET` and `RETI`.
It injects each other instruction that does not use the PC or the flash.
For an indirect jump or call, `LPM`, `ELPM`, `SPM`, `SLEEP` and `BREAK`, the probe writes the original word, steps, and writes `BREAK` again (two page writes).
AVaRICE waits 500 ms for the response. Two page writes take about 0.5 s at 200 kHz for a 128-byte page, and about 1 s for a 256-byte page (issue #115).
Thus a step or run from a software breakpoint on one of these instructions still stops the AVaRICE session.
The data breakpoints do not stop on the memory access of an executed original instruction.
Bloom uses the hardware breakpoints first, and sends a software breakpoint only when no hardware breakpoint is free.
"Disable debugWIRE" fails with code `0x90`.
A JTAG scan that fails, a write that does not end in time, a core that does not stop in time, or a CPU context that the probe could not read gives code `0x13`.
After a failed context read, run, step, PC and memory access fail until a reset.

The memory types in the programming mode:

- Flash page (`0xB0`): a write must have whole pages at a page address, else code `0x37`. A write needs a chip erase first. A read can have any address and length.
- EEPROM page (`0xB1`): the probe writes each page that the data touches.
- Fuses (`0xB2`): address 0 is the low fuse, 1 the high fuse, 2 the extended fuse.
- Lock bits (`0xB3`): address 0.
- Signature (`0xB4`) and calibration byte (`0xB5`), read only: the index 0 to `0xFF` of the signature row. avrdude reads the signature at index 0 to 2, and the even and odd bytes of its `sigrow` and `sernum` memories with these two types.

The memory types in the debug mode (Bloom). SRAM, EEPROM and SPM need a stopped core, else code `0x32`:

- SRAM (`0x20`): the data space with the registers, I/O and SRAM. The OCDR reads as 0, and a write does not change it. The registers r0 to r31 come from the CPU context, and a write goes to the target at the next run or step.
- EEPROM (`0x22`): one byte at a time. The probe reads each byte back after the write.
- SPM (`0xA0`): the flash, read only. A write gives code `0x34`.
- Fuses (`0xB2`), also with a running core: for Bloom `manage_ocden_fuse_bit`. The probe enters the programming mode, reads or writes the fuses, leaves the programming mode and attaches again. The core is reset, and no break event comes.

The break event has the same frame as on debugWIRE. The cause is 1 when the break status register of the OCD shows a hit of OCD register 0 to 3 (a program or data breakpoint) or a `BREAK` instruction, else 0.
While the core runs, each `AVR_EVT` reads the OCD control register one time.

A bus reset, a USB suspend and `DAP_Disconnect` on the HID interface end the JTAG session.
The probe removes the hardware breakpoints, lets a stopped core run, leaves the programming mode, releases the core from reset and releases the pins.

#### Data breakpoints

`HW_BREAK_SET` is `[0x40][version][type][number][address u32][mode]`.
A data breakpoint has type 2. It stops the core when an instruction reads or writes one data space address, for example `LDS`, `STS`, `LD` or `ST` on SRAM.

| Field | Value |
|---|---|
| Number | 2 or 3. Number 1 gives code `0x81`, because OCD register 1 has no data mode. |
| Address | The data space address, below `0x10000`, else code `0x38` |
| Mode | `0x00` read, `0x01` write, `0x02` access (read or write). Another mode gives code `0x39`. |

The probe writes OCD mode 2 for read, 1 for write and 0 for access (see [avr-jtag-ocd.md](avr-jtag-ocd.md#bcr-reg-8)).
The core stops after the instruction that makes the access. The break event has cause 1.
A data mask has type 3 (AVaRICE "range BP"). The address field is the mask: a 1 bit compares the address bit.
The probe uses mask bits 15..0 and ignores the mode.
The mask is on number 2 or 3, and applies to the data breakpoint on the other number. Number 1 gives code `0x81`.
A mask on a number in use, or next to a program breakpoint or a mask, gives code `0x81`.
A program breakpoint next to a mask also gives code `0x81`.
A mask without a data breakpoint has no effect.
A clear of the data breakpoint also clears its mask. AVaRICE clears only the data breakpoint when gdb removes a range watchpoint.

Numbers 2 and 3 hold a program breakpoint or a data breakpoint, first come, first served.
A data breakpoint on a number in use gives code `0x81`.
A program breakpoint on a number with a data breakpoint also gives code `0x81`.
A program breakpoint on a number with a program breakpoint replaces it, as before.
Clear the number first to change the type.

The EDBG protocol specification (DS50002630A, Table 7-31) gives only type 1 and mode 3 (program).
The type 2 and mode values come from [AVaRICE](https://github.com/avrdudes/avarice) at commit `6ca2ef5` (`src/jtag3bp.cc`, the JTAGICE3 and EDBG driver).
They are the same as the values of `CMND_SET_BREAK` of the JTAGICE mkII (AVaRICE `src/jtag2bp.cc`).
No host tool that we know sends data breakpoints over EDBG to this probe today.
`tools/hwtest/avr_data_break_test.py` sends them for the hardware test (see [avr.md](avr.md#jtag-data-breakpoints)).

### Target pins

The HID interface owns the target pins for the AVR commands, as for its DAP commands (see [CMSIS-DAP v1 (HID)](#cmsis-dap-v1-hid)).

- `ENTER_PROGMODE` and activate physical take the pins. While the v2 interface owns them, they fail and change no pin.
- ISP, debugWIRE and JTAG use the same pins. `ENTER_PROGMODE` of ISP fails during a dW or JTAG session, and activate physical fails with code `0x33` in the ISP programming mode.
- `LEAVE_PROGMODE`, deactivate physical, disable debugWIRE, and a start command that fails, give the pins back. When `DAP_Connect` of the HID interface took the pins first (avrdude does this), `DAP_Disconnect` gives them back.
- `DAP_Disconnect` on the HID interface, a USB suspend and a bus reset end the programming mode: RESET is released, and the target runs.

### avrdude

avrdude finds the probe by its USB ID and needs `CMSIS-DAP` in the product string.
Programmer entries with `type = "jtagice3_isp"` (ISP), `type = "jtagice3_dw"` (debugWIRE, as `xplainedmini_dw`) or `type = "jtagice3"` (JTAG, as `atmelice`) and the USB ID of the probe work without a change in avrdude.
The JTAG entry needs `prog_modes = PM_JTAG` for `-x jtagchain`, and `extra_features = HAS_BITCLOCK_ADJ`, else `-B` gives a warning.
The file `etc/avrdude-openmupp.conf` has these entries. See [avr.md](avr.md) for the wiring and the commands.
avrdude then sends `DAP_Connect` and `DAP_HostStatus`, the sign-on, the AVR8 parameters, the ISP or AVR8 commands, the sign-off and `DAP_Disconnect`.

## Commands

| ID | Command | Note |
|---|---|---|
| `0x00` | `DAP_Info` | Vendor, product, serial number, protocol version `2.1.0`, firmware version, capabilities, packet size and count. Other information has length 0. On OpenMUPP v1, the vendor string also shows the adapter on J4 (see [Adapter ID and board ID](boards.md#adapter-id-and-board-id)). |
| `0x01` | `DAP_HostStatus` | Connect and running LED. |
| `0x02` | `DAP_Connect` | Port 0 and 1 give SWD. Port 2 gives JTAG. The command sets the pin owner and the port only. The pins change at the first transfer, sequence or IDCODE command. Until then, SWCLK and SWDIO (TCK and TMS) keep their state. On OpenMUPP v1, the command reads the adapter ID. It replies port 0 on an adapter fault and on an unknown board revision (see [Adapter ID and board ID](boards.md#adapter-id-and-board-id)). |
| `0x03` | `DAP_Disconnect` | Release the pins of the port, when a command drove them. Release the pins that `DAP_SWJ_Pins` drove: nRESET and nTRST get the pull-up. |
| `0x04` | `DAP_TransferConfigure` | Idle cycles, WAIT retries, match retries. |
| `0x05` | `DAP_Transfer` | Read, write, posted AP read, value match, match mask. On JTAG, the device index selects the device. |
| `0x06` | `DAP_TransferBlock` | Read or write block. On JTAG, the device index selects the device. |
| `0x07` | `DAP_TransferAbort` | No response. It stops a running transfer command. See [Transfer abort](#transfer-abort). |
| `0x08` | `DAP_WriteABORT` | Replies an error when no port is connected. On JTAG, an ABORT scan of the device of the index. A device index that is not in the chain replies an error. |
| `0x09` | `DAP_Delay` | |
| `0x0A` | `DAP_ResetTarget` | A 10 ms pulse on SRST. A board without SRST replies that it did no reset. |
| `0x10` | `DAP_SWJ_Pins` | SWCLK, SWDIO, TDI, TDO, nTRST, nRESET. nTRST and nRESET are open-drain. The wait is at most 3 s. |
| `0x11` | `DAP_SWJ_Clock` | In Hz, for SWCLK and TCK. See the SWD engine for the available SWCLK rates. TCK is in kHz, rounded down, and at least 1 kHz. |
| `0x12` | `DAP_SWJ_Sequence` | 1 to 256 bits. On JTAG, the bits go to TMS, and TDI is high. |
| `0x13` | `DAP_SWD_Configure` | Turnaround and data phase. |
| `0x14` | `DAP_JTAG_Sequence` | TDI bits with one TMS level, and the TDO bits on request. Replies an error when JTAG is not connected. |
| `0x15` | `DAP_JTAG_Configure` | The IR length of each device in the chain. Replies an error for more than 8 devices or a bad IR length. The chain is then empty. |
| `0x16` | `DAP_JTAG_IDCODE` | Reads IDCODE of the device of the index. Replies an error when JTAG is not connected or the device is not in the chain. |
| `0x17` | `DAP_SWO_Transport` | `0` none, `1` `DAP_SWO_Data`, `2` SWO trace endpoint. See [SWO commands](#swo-commands). |
| `0x18` | `DAP_SWO_Mode` | `0` off, `1` UART, `2` Manchester. Stops the capture. |
| `0x19` | `DAP_SWO_Baudrate` | UART: the response has the real baud rate of the USART divider, or 0. Above 9 Mbaud: 9 Mbaud. Manchester: the TPIU clock, two times the bit rate, as an even number, or 0. Above 1 MHz: 1 MHz. |
| `0x1A` | `DAP_SWO_Control` | Bit 0: start or stop the capture. |
| `0x1B` | `DAP_SWO_Status` | The trace status and the number of bytes in the trace buffer. |
| `0x1C` | `DAP_SWO_Data` | The trace status and up to packet size - 4 bytes of trace data. |
| `0x1D` | `DAP_SWD_Sequence` | Replies an error when SWD is not connected. |
| `0x1E` | `DAP_SWO_ExtendedStatus` | The trace status (bit 0) and the count (bit 1). Bit 2 (index and timestamp) is ignored. |
| `0x1F` | `DAP_UART_Transport` | `0` none, `1` USB COM port (the default), `2` DAP commands. See [UART commands](#uart-commands). |
| `0x20` | `DAP_UART_Configure` | Data bits, parity, stop bits and baud rate. The response has the error bits and the real baud rate. |
| `0x21` | `DAP_UART_Transfer` | Sends and receives UART data. Up to 59 bytes to send and 58 bytes to receive in one packet. |
| `0x22` | `DAP_UART_Control` | Enables, disables and flushes the receiver and the transmitter. |
| `0x23` | `DAP_UART_Status` | The status bits, the bytes in the receive buffer and the bytes not sent yet. |
| `0x80` | `ID_DAP_Vendor0` | Switches the USB personality of a multi image: `[0x80][personality u8]`. The personality is the code of a personality in the image: `0` (Versaloon), `1` (CMSIS-DAP) or `2` (WCH-Link). The response is `[0x80][status u8]`: `0` when the firmware accepts the switch, `0xFF` for a code that is not in the image. The firmware then stores the personality and resets. The bootloader code `0xB0` starts the bootloader, also in an image with one personality (see [Bootloader code](firmware-update.md#bootloader-code)). A multi image without the bootloader entry replies `0xFF` to it. An image with one personality and without the bootloader entry replies `ID_DAP_Invalid` to each code. Only the `app-8k` image has the bootloader entry, and the `standalone` image of the WeAct STM32H523 (an earlier development board). See [Multi image](usb-device.md#multi-image). |
| `0x81` | `ID_DAP_Vendor1` | Only in an image with `MUPP_CMSIS_DAP_RV_BRIDGE`: sets the mode of the RISC-V bridge, `[0x81][mode u8]`, `0` off, `1` on, `2` auto, `3` the stored mode. Bit 7 of the mode also stores the mode. The response is `[0x81][status u8]`: `0` when the mode is set, `0xFF` for another value or a failed store. An image without the bridge replies `ID_DAP_Invalid`. See [Bridge switch](#bridge-switch). |
| `0x82` | `ID_DAP_Vendor2` | Switches the supply of the target and reads the target voltage (Vref): `[0x82][op u8]`. Op `0` switches the supply off, `1` switches it on at 3.3 V, `2` only reads. `[0x82][3][mV u16]` switches it on at 3.3 V or 5 V. The response is `[0x82][status u8][mV u16][state u8]`. See [Target power](#target-power). |
| `0x83` | `ID_DAP_Vendor3` | Only on a board with an adapter ID (OpenMUPP v1), in an image with the RISC-V bridge (`MUPP_CMSIS_DAP_RV_BRIDGE`): reads and writes the setting "VPP without an adapter", `[0x83][op u8]`. Op `0` stores off, `1` stores on, `2` only reads. The response is `[0x83][status u8][setting u8]`. Other boards and images reply `ID_DAP_Invalid`. See [VPP without an adapter](#vpp-without-an-adapter). |
| `0x84` | `ID_DAP_Vendor4` | Only on a board with an adapter ID (OpenMUPP v1): reads the board ID and the adapter ID, `[0x84]`. The response is `[0x84][status u8][board kind u8][board slot u8][adapter kind u8][adapter slot u8][allowed u8]`. Other boards reply `ID_DAP_Invalid`. See [Board ID and adapter ID](#board-id-and-adapter-id). |
| `0x85` | `ID_DAP_Vendor5` | Only on a board with VPP (OpenMUPP v1): sets VPP or switches it off, for the hardware test, `[0x85][op u8]`. Op `0` switches VPP off, `2` only reads. `[0x85][1][mV u16]` switches VPP on at mV. The response is `[0x85][status u8][level mV u16][sense mV u16][sense2 mV u16]`. Other boards reply `ID_DAP_Invalid`. See [VPP control](#vpp-control). |
| `0x86` | `ID_DAP_Vendor6` | Only on a board with `UART_TX_DIR` (OpenMUPP v1): sets the wire mode of the UART, `[0x86][op u8]`. Op `0` sets normal mode (two wires), `1` single-wire mode, `2` only reads. The response is `[0x86][status u8][mode u8]`. Other boards reply `ID_DAP_Invalid`. See [UART wire mode](#uart-wire-mode). |
| `0x7E` | `DAP_QueueCommands` | The packet waits. The next packet with another command runs the queue. A queued packet in the last free slot also runs the queue. |
| `0x7F` | `DAP_ExecuteCommands` | The commands do not nest. |

An unknown command replies `ID_DAP_Invalid` (`0xFF`).
The status of a command is `DAP_OK` (`0x00`) or `DAP_ERROR` (`0xFF`), as in the specification.
Each "replies an error" in the table means the status `0xFF`.
`DAP_Connect` is the exception: it replies port `0` for a failure.
The capabilities in `DAP_Info` show SWD, JTAG, SWO UART, SWO Manchester, atomic commands, SWO streaming trace, the UART port and the USB COM port.
`DAP_Info` also gives the size of the UART receive buffer (`0xFB`) and transmit buffer (`0xFC`), and the size of the SWO trace buffer (`0xFD`).
A processor without a `DapUart` does not show the UART, and the `DAP_UART_*` commands then reply `ID_DAP_Invalid`.
A processor without a `DapSwo` does not show SWO, and the `DAP_SWO_*` commands then reply `ID_DAP_Invalid`.
A processor without a JTAG port does not show JTAG, and `DAP_Connect` to JTAG then replies 0 (failed).

The transfer commands follow the reference implementation of Arm.
After an error, the processor skips the rest of the request data.
After a `DAP_Transfer` write, the processor reads RDBUFF to check the write.

The processor checks a request before the first transfer.
A request that is too short gives `ID_DAP_Invalid`.
A response that does not fit in a packet gives `ID_DAP_Invalid` too.
In both cases, the target sees no transfer from that command.
In `DAP_ExecuteCommands`, this stops the list, and the command count is the number of commands that ran.

### Transfer abort

The host sends `DAP_TransferAbort` to stop a long `DAP_Transfer` or `DAP_TransferBlock`.
The packet arrives on the same bulk OUT endpoint as the running command.
During a command, the SWD engine calls the serial service between transfers (see [Serial port during long commands](usb-device.md#serial-port-during-long-commands)).
The serial service also delivers the CMSIS-DAP OUT packets, so the abort reaches the processor during the command.

The processor handles `DAP_TransferAbort` in `submit()`:

- The abort takes no slot and has no response.
- It applies to all packets that came before it: the packet that runs and the packets that wait.
  A `DAP_QueueCommands` packet that waits is one of them.
- A packet that comes after the abort runs normally.
- When no packet runs or waits, the abort has no effect.
  The CMSIS-DAP specification requires no response for it.

`DAP_Transfer` and `DAP_TransferBlock` check the abort before each transfer.
On SWD, the engine also checks the abort after each WAIT acknowledge (see [SWD engine rules](#swd-engine-rules)).
The abort then stops the WAIT retries of the transfer, and the response value is WAIT.
After an abort, the command stops and skips the rest of its request data.
The response gives the number of transfers that the command did.
A value match also stops, with the mismatch bit set.
The command then ends as after the last transfer: it reads RDBUFF for a posted AP read, and to check the last write.
The data of the posted AP read is in the response, and the count includes it.
A transfer command of a packet that waits does no transfer and replies a count of 0.
The other commands of the packet run, for example in `DAP_ExecuteCommands`.

When the host goes away, the transport calls `hostGone()`.
It disconnects, releases the pins, and drops the queued packets and the responses that the transport did not send.


## Target power

The vendor command `ID_DAP_Vendor2` (`0x82`) switches the supply of the target, as the `POWER` block of the Versaloon personality does (see [POWER block](usb-device.md#power-block)).
Both use the same protections (`src/protocol/TargetPower.h`).
Most boards have a 3.3 V supply only.
OpenMUPP v1 also has a 5 V supply (see [Target power on OpenMUPP v1](boards.md#target-power-on-openmupp-v1)).

| Request | Operation |
|---|---|
| `82 00` | Switch the supply off. |
| `82 01` | Switch the supply on at 3.3 V, with the protections. |
| `82 02` | No change. Only read the voltage. |
| `82 03 E4 0C` | Switch the supply on at 3.3 V (3300 mV, little-endian), with the protections. |
| `82 03 88 13` | Switch the supply on at 5 V (5000 mV, little-endian), with the protections. |

Op `3` accepts only 3300 and 5000 mV.
`82 01` is the same as `82 03 E4 0C`, so the requests of older host tools keep their function.
A request with the other voltage changes the supply with the sequence of [Target power sequence](boards.md#target-power-sequence).
A request with the voltage that is on already changes nothing.

The response is `[0x82][status u8][mV u16][state u8]`:

| Status | Meaning |
|---|---|
| `0x00` | Done. |
| `0x01` | Refused: a protection keeps the supply off or as it was (see below), or the HID interface owns the target pins. |
| `0xFF` | Not supported: op `0`, `1` or `3` on a board without that switch, a voltage that is not 3300 or 5000 mV, or an op that is not `0` to `3`. |

`mV` is the target voltage after the operation, little-endian.
It is 0 when the board has no Vref sense or the measurement fails.

| `state` bits | Meaning |
|---|---|
| 0 to 1 | The supply after the operation: 0 off, 1 3.3 V, 2 5 V. |
| 7 | A power fault switched the supply off. The firmware sets this bit in one response only, the first after the fault. |

A power fault is a fault signal of a supply switch, for example an overcurrent.
Only OpenMUPP v1 has these signals.

A switch-on from off is refused, and the supply stays off, in these cases:

- The voltage is above 1.8 V. The target then has its own supply.
- The firmware cannot measure the voltage.
- The voltage is not above 1.8 V 10 ms after the switch-on. The rail then has a short circuit or an overload, and the firmware switches the supply off again.

A board without a Vref sense switches on without these checks.

A switch-on at 5 V is also refused when the USB supply of the probe is below 4.4 V or above 5.5 V. The supply then stays as it was.
A change from 5 V to 3.3 V is refused, and the supply stays off, when the voltage does not go below 3.4 V in 500 ms.

With the HID interface, one rule covers both interfaces.
Only the interface that owns the target pins switches the supply on, or any interface while no interface owns them.
The switch-off always works, also for the other interface: this is for safety, and for the recovery after a host that stopped.
`82 01` on the v2 interface gives status `0x01` while the HID interface owns the pins.
With `MUPP_CMSIS_DAP_AVR`, the EDBG control scope switches the supply on the HID interface (see [Scopes](#scopes)).
Its switch-on fails with code `0x16` when the v2 interface owns the pins or a protection refuses.
On a board without a power switch, its `SET` and `GET` fail with code `0x01`.
While the supply is on, the main loop switches it off when the voltage stays low, as in the Versaloon personality.
The Versaloon personality switches on at 3.3 V only.
The EDBG control scope also switches on at 3.3 V only, and it keeps a supply that is on, for example 5 V.

The supply is off at power-up.
It stays on when the host goes away, so that a tool can switch the target on and then end.

After `82 00`, the rail capacitance can keep the voltage above 1.8 V for some time.
An `82 01` in this time is refused, because the firmware sees a target with its own supply.
Thus, before `82 01`, wait until `82 02` gives a voltage below 1.8 V.

`tools/mupp_probe.py` sends the command from the host. It needs the `pyusb` package and no connection to the target:

```sh
python3 tools/mupp_probe.py power on
python3 tools/mupp_probe.py power off
python3 tools/mupp_probe.py power status
```

The tool shows the target voltage. It stops with exit code 1 when the firmware refuses the operation.
`--serial` selects a dongle.

In an OpenOCD session, `cmsis-dap cmd` sends the command, for example for a power cycle of the target.
OpenOCD accepts this command only after `init`.
An example with a wait of 200 ms:

```
cmsis-dap cmd 0x82 0x00
sleep 200
cmsis-dap cmd 0x82 0x01
```

## VPP without an adapter

The vendor command `ID_DAP_Vendor3` (`0x83`) reads and writes the setting "VPP without an adapter" (decision D2 of the v1 plan).
Only boards with an adapter ID have the command: OpenMUPP v1.
The image must also have the RISC-V bridge (`MUPP_CMSIS_DAP_RV_BRIDGE`, on by default): the setting is in its settings page.
Other images reply `ID_DAP_Invalid`.

| Request | Operation |
|---|---|
| `83 00` | Store the setting off. |
| `83 01` | Store the setting on. |
| `83 02` | No change. Only read the setting. |

The response is `[0x83][status u8][setting u8]`.
The status is `0x00` when the operation is done.
It is `0xFF` for another op and for a failed store.
`setting` is the stored value after the operation: `0` off, `1` on.

The firmware stores the setting in the settings page, together with the personality and the bridge mode.
The default is off.
An older image does not know a stored "on" value, so it then starts with its default personality and bridge mode.
A store stalls the CPU for the flash write (see `src/board/SettingsFlashPage.h`).

When the setting is on, VPP is also allowed with no adapter on J4 (pin 16 open).
The setting never allows VPP on an adapter fault (pin 16 shorted), an unknown adapter or an adapter that is not slot 8 or 9.
See [Adapter ID and board ID](boards.md#adapter-id-and-board-id).

## VPP control

The vendor command `ID_DAP_Vendor5` (`0x85`) controls VPP on J4 pin 19, for the hardware test of the VPP driver (`src/protocol/Vpp.h`).
Only boards with VPP have the command: OpenMUPP v1.
Other boards reply `ID_DAP_Invalid`.
Only the v2 interface has the command. The HID interface replies `ID_DAP_Invalid`.

| Request | Operation |
|---|---|
| `85 00` | Switch VPP off. |
| `85 01 <mV u16>` | Switch VPP on at mV, or change the level. |
| `85 02` | No change. Only read. |

The response is `[0x85][status u8][level mV u16][sense mV u16][sense2 mV u16]`.

| Field | Value |
|---|---|
| `status` | `0x00`: done. `0x01`: refused. The level is out of range or above a limit, the VPP rule does not allow VPP, or the other interface owns the target pins. `0x02`: `VPP_SENSE` did not show the level in 2 s, or the ADC failed. `0x03`: `VPP_SENSE2` shows a short or an overload on pin 19. `0xFF`: another op. |
| `level` | The VPP level after the operation. `0` when VPP is off. |
| `sense` | `VPP_SENSE` (PA4): the boost output, before the output switch. `0` when the ADC fails. |
| `sense2` | `VPP_SENSE2` (PC1): J4 pin 19. `0` when the ADC fails. |

After the status `0x02` or `0x03`, VPP is off.
After a refused level, VPP stays as it was.
A refusal of the VPP rule switches VPP off.
A switch-on can take up to 2 s, because the boost output discharges slowly for a step down.
The command takes the rest of the packet. Thus in `DAP_ExecuteCommands` and `DAP_QueueCommands` it replies `ID_DAP_Invalid`.
The firmware uses the sequence and the limits in [VPP sequence](boards.md#vpp-sequence).
VPP goes off at a USB reset, a suspend and `85 00`.

`tools/mupp_probe.py` sends the command from the host:

```sh
python3 tools/mupp_probe.py vpp on 12000
python3 tools/mupp_probe.py vpp status
python3 tools/mupp_probe.py vpp off
```

The script waits up to 3 s for the response of `vpp on`.
VPP stays on after the script ends, until `vpp off`, a USB reset or a suspend.

## Board ID and adapter ID

The vendor command `ID_DAP_Vendor4` (`0x84`) reads the board ID and the adapter ID (see [Adapter ID and board ID](boards.md#adapter-id-and-board-id)).
Only boards with an adapter ID have the command: OpenMUPP v1.
Other boards reply `ID_DAP_Invalid`.
Only the v2 interface has the command. The HID interface replies `ID_DAP_Invalid`.

The request is `[0x84]`.
The response is `[0x84][status u8][board kind u8][board slot u8][adapter kind u8][adapter slot u8][allowed u8]`.

| Field | Value |
|---|---|
| `status` | `0x00`, or `0xFF` when the ADC fails on the adapter ID. The adapter fields and `allowed` are then `0`. |
| `kind` | `0`: no ID resistor (pin open). `1`: a known slot. `2`: a fault (pin shorted to GND). `3`: a free slot (6 or 7). For the board ID, `2` is also an ADC failure at start-up. |
| `slot` | `1` to `9` for the kinds `1` and `3`. Else `0`. |
| `allowed` | `1` when a connect starts the protocol now. `0` when the firmware refuses the connect. |

The board ID is the reading at start-up.
The firmware reads the adapter ID again for each command.
The command is not a connect: it does not change the decision of the last connect, the vendor string of `DAP_Info` or the VPP rule.
Thus the command does not release a refused connect.

`tools/mupp_probe.py` sends the command from the host, together with `DAP_Info` `0x09` (the firmware version):

```sh
python3 tools/mupp_probe.py adapter
```

```text
Firmware: v0.13.0-554-g6712cbe
Board:    OpenMUPP v1, revision 1
Adapter:  slot 4, Arm adapter
Connect:  allowed
```

## UART commands

The `DAP_UART_*` commands of CMSIS-DAP v2.1 give access to the UART of the board.
They use the same UART and the same two ring buffers of 1 KB as the [UART bridge](uart-bridge.md).
`DAP_Info` thus reports 1023 bytes for each buffer.

`DAP_UART_Transport` selects the owner of the UART:

| Value | Owner | Note |
|---|---|---|
| `0` | None | The firmware discards the received data. |
| `1` | USB COM port | The default. The UART bridge moves the data. |
| `2` | DAP commands | `DAP_UART_Configure`, `_Control`, `_Status` and `_Transfer` move the data. |

Only the owner gets data.
While the DAP commands own the UART, the bridge discards the data from the serial port, and it sends no data to the serial port.
While the serial port owns the UART, the DAP commands send and receive no data.
A change of the owner stops the transmitter and empties both ring buffers.
Thus no data of one side goes to the other side.
The byte in the shift register of the UART still goes out.
When the serial port gets the UART again, the UART gets the line coding of the serial port again.
A line coding that the host sets on the serial port in the meantime is kept.

When the DAP commands get the UART, it is not configured, and the receiver and the transmitter are disabled.
Send these commands first:

1. `DAP_UART_Transport` `2`.
2. `DAP_UART_Configure` with the frame format and the baud rate.
3. `DAP_UART_Control` `0x11` to enable the receiver and the transmitter.

`DAP_UART_Configure` uses the control byte of the CMSIS-Driver USART interface:

| Bits | Field | Values |
|---|---|---|
| 3..0 | Data bits | `0` or `8`: 8 data bits. `7`: 7 data bits. |
| 5..4 | Parity | `0` none, `1` even, `2` odd. |
| 7..6 | Stop bits | `0` 1 stop bit, `1` 2 stop bits, `2` 1.5 stop bits. |

The formats are the formats of the UART bridge (see [UART bridge](uart-bridge.md)).
7 data bits need a parity bit.
Other values set the error bit of their field: bit 0 data bits, bit 1 parity, bit 2 stop bits.
The baud rate has the same limits as in the UART bridge.
A baud rate out of these limits gives no error bit, but the response baud rate is 0, as in the reference implementation of Arm.
Else the response has the real baud rate of the UART, for example 230032 for 230400 baud.
A configuration that fails disables the receiver and the transmitter.

The UART gets a new format when the transmit buffer is empty and the last byte is sent.
Until then, `DAP_UART_Transfer` takes no data to send and gives no received data, and the receive count of `DAP_UART_Status` is 0.
At the change, the firmware discards the bytes that the UART received in the old format.
The errors before the change are not reported.

Other rules:

- `DAP_UART_Control` enables the receiver or the transmitter only after a configuration. Else it replies an error.
- A disable bit has priority over its enable bit.
- An enable of the receiver empties the receive buffer.
- A disable or a flush of the transmitter discards the bytes that are not sent yet.
- While the receiver is disabled, the firmware discards the received data.
- The status has these bits: bit 0 receiver enabled, bit 1 data lost, bit 2 framing error, bit 3 parity error, bit 4 transmitter enabled.
- "Data lost" includes the bytes that the full receive buffer dropped and the overruns of the UART. "Framing error" includes the noise errors.
- Each error bit shows the errors since the last read of the status, the last enable of the receiver or the last format change. `DAP_UART_Status` and `DAP_UART_Transfer` read the status.
- The UART receives also while the receiver is disabled. The errors of that time are not reported.
- `DAP_UART_Transfer` takes the bytes that fit in the transmit buffer, and gives the received bytes that fit in the response.
  The response gives the status after the transfer.
- A `DAP_UART_Transfer` with a Tx Count that is larger than its data gives `ID_DAP_Invalid`.
- While the serial port owns the UART, `DAP_UART_Configure` replies all error bits and baud rate 0. `DAP_UART_Control` replies an error. The status and the counts are 0.
- When the host goes away (USB reset or unconfigure), the serial port gets the UART again.
  A host tool that ends without this event must send `DAP_UART_Transport` `1` itself.

`tools/hwtest/dap_uart_loopback_test.py` tests the commands with a loopback wire from TX to RX (see [UART hardware test](#uart-hardware-test)).

## UART wire mode

The vendor command `ID_DAP_Vendor6` (`0x86`) selects the wire mode of the UART (see [Single-wire mode](uart-bridge.md#single-wire-mode)).
Only boards with a TX level shifter and its direction pin `UART_TX_DIR` have the command: OpenMUPP v1.
Other boards reply `ID_DAP_Invalid`.
Only the v2 interface has the command. The HID interface replies `ID_DAP_Invalid`.

The request is `[0x86][op u8]`.
The response is `[0x86][status u8][mode u8]`.

| Field | Value |
|---|---|
| `op` | `0`: normal mode, TX and RX on two wires (the default). `1`: single-wire mode. `2`: only read the mode. |
| `status` | `0x00`, or `0xFF` for another op. |
| `mode` | The mode after the command: `0` normal, `1` single wire. |

The mode applies to the UART bridge and to the `DAP_UART_*` commands.
Single-wire mode uses the half-duplex mode of UART7 on J4 pin 13 (UART TX). The line needs a pull-up to `VTREF`: board revision 2 has it, and on revision 1 the target or the adapter must supply it.
When the UART sends at the time of the command, the new mode takes effect after the last stop bit.
The mode stays when the host goes away. A reset of the probe sets normal mode.

`tools/mupp_probe.py` sends the command from the host:

```sh
python3 tools/mupp_probe.py uart single   # single-wire mode
python3 tools/mupp_probe.py uart normal   # normal mode
python3 tools/mupp_probe.py uart status   # only show the mode
```

## SWO commands

The `DAP_SWO_*` commands of CMSIS-DAP v2.1 capture the SWO trace output of the target (issue [#5](https://github.com/OpenMUPP/OpenMUPP/issues/5)).
Only the boards with an SWO pin in the board definition have them: the YD-CH32V307VCT6 (with the wire from PB14 to PA3) and openmupp-v1.
On these boards, the TDO net also goes to PA3 (USART2 RX). See [boards.md](boards.md).
The other boards report no SWO in `DAP_Info`, and the commands reply `ID_DAP_Invalid`.
Only the v2 interface has the commands. The Versaloon personality has no SWO.
The CMake build sets `MUPP_CMSIS_DAP_SWO` for the CH32V307 images with the CMSIS-DAP personality.

The receiver is `swo::SwoReceiver` (`src/swo/SwoReceiver.h`). It has two modes:

- UART mode: USART2 receives on PA3 (8N1, receiver only). A circular DMA on DMA1 channel 6 writes into a trace buffer of 4 KB.
  The baud rate is 144 MHz / (16 × divider). The range is 2198 baud to 9 Mbaud.
- Manchester mode: TIM5 captures the edges on PA3, and the CPU decodes them into the same trace buffer.
  The range is 69 bit/s to 500 kbit/s, a baud rate of 138 Hz to 1 MHz. See [SWO Manchester mode](#swo-manchester-mode).
- PB14 (TDO) stays an input while the capture runs.

Use this sequence to start a capture:

1. `DAP_SWO_Transport` `1` (`DAP_SWO_Data`) or `2` (SWO trace endpoint).
2. `DAP_SWO_Mode` `1` (UART) or `2` (Manchester).
3. `DAP_SWO_Baudrate` with the baud rate of the target. In Manchester mode, this is the TPIU clock: two times the bit rate. The response has the real baud rate, for example 6857143 for 7 Mbaud in UART mode.
4. `DAP_SWO_Control` `1`.

Then read the data with `DAP_SWO_Data` (transport `1`) or from the SWO trace endpoint (transport `2`).
Stop the capture with `DAP_SWO_Control` `0`.

Rules:

- The commands follow the reference implementation of Arm (`SWO.c` of CMSIS-DAP).
- While the capture runs, `DAP_SWO_Transport` replies an error.
- `DAP_SWO_Mode` stops the capture and clears the baud rate. Thus send `DAP_SWO_Baudrate` after `DAP_SWO_Mode`.
  Another mode than `0`, `1` and `2` replies an error and sets mode off.
- `DAP_SWO_Baudrate` replies the real baud rate and stores it. In UART mode, a baud rate above 9 Mbaud gives 9 Mbaud.
  In Manchester mode, a baud rate above 1 MHz gives 1 MHz.
  It replies 0 in mode off, and for a baud rate below the minimum (2198 baud UART, 138 Hz Manchester). It then stores 0 and stops the capture.
- While the capture runs, `DAP_SWO_Baudrate` changes the baud rate at once. The capture goes on, and the trace buffer stays.
- A start needs a mode and a baud rate. Else `DAP_SWO_Control` replies an error.
- A start empties the trace buffer and clears the errors. A start while the capture runs does nothing.
- After a stop, the bytes in the trace buffer stay readable.
- The trace status has these bits: bit 0 capture active, bit 6 error (a framing or noise error), bit 7 overrun (data lost).
  "Data lost" includes an overflow of the trace buffer, an overrun of the USART, and an overflow of the edge buffers in Manchester mode.
- The receiver does not pause on a full trace buffer, so bit 1 (capture paused) is always 0.
  An overflow discards the data in the trace buffer, and sets the overrun bit.
- Each error bit shows the errors since the last read of the status or the last start.
  `DAP_SWO_Status`, `DAP_SWO_Data`, and `DAP_SWO_ExtendedStatus` with bit 0 read the status. A read of only the count does not clear the error bits.
- After an overflow, the count is 0, and the next `DAP_SWO_Data` empties the trace buffer.
- `DAP_SWO_Data` gives the bytes that fit in the response: up to 60 bytes at full speed, and up to 508 bytes at high speed.
  It reads the status before the data. With a transport other than `1`, it gives no data.
- With transport `2`, the SWO trace endpoint sends the data (see [SWO trace endpoint](#swo-trace-endpoint)). The status commands work as with transport `1`.
- `DAP_SWO_ExtendedStatus` gives only the fields that the request asks for. Bit 2 (index and timestamp) is ignored, because the capabilities do not include the test domain timer.
- `DAP_Disconnect` does not stop the capture.
- `DAP_Connect` to JTAG stops the capture, because SWO is an SWD function. While the JTAG port is connected, a start replies an error.
- When the host goes away (USB reset or unconfigure), the capture stops. The transport becomes none, the mode off and the baud rate 0.

### SWO Manchester mode

The TPIU Manchester format has these properties:

- The line is low when it is idle.
- Each bit has two halves. A 1 is high, then low. A 0 is low, then high. Thus each bit has an edge in its middle.
- A frame starts with a start bit (a 1). One or more bytes follow, with the least significant bit first. After the frame, the line stays low for at least one bit time.
- The bytes of a frame have no start bit and no gap between them. The frame ends only when the trace FIFO of the target is empty.
- The TPIU clock gives one half of a bit. Thus the bit rate is half of the TPIU clock (TRACECLKIN / (`TPIU_ACPR` + 1) / 2).

The baud rate of `DAP_SWO_Baudrate` in Manchester mode is the TPIU clock, two times the bit rate.
The CMSIS-DAP documentation does not tell if the value is the bit rate or the clock.
OpenOCD sets `TPIU_ACPR` from TRACECLKIN / baud rate, and it sends the same baud rate to the probe.
Thus the firmware uses the clock, and OpenOCD works with the probe.
A test on a Nucleo-F401RE target decoded the OpenOCD captures at 50, 250 and 500 kbit/s with no lost lines.

The capture on the CH32V307:

- The general-purpose timers of the CH32V307 capture only one edge direction on each channel. The `CCER` register has no `CCxNP` bit, and the dual-edge register `TIMx_AUX` is only on some chip lots.
  Thus TIM5 channel 4 captures the rising edges of PA3, and channel 3 captures the falling edges of the same input (`CC3S` = `10`).
- TIM5_CH4 is on DMA2 channel 1 and TIM5_CH3 on DMA2 channel 2. Each circular DMA writes the 16-bit capture times into an edge buffer of 1024 edges.
- TIM5 is on APB1. The Vref measurement changes only PCLK2, so the timer clock does not change during a measurement.
- The prescaler gives 16 to 32 timer ticks for each bit. Thus the 16-bit counter wraps after 2048 bit times or more (at least 4.1 ms at 500 kbit/s).
- The input filter rejects pulses shorter than 8 timer clocks (56 ns).
- PA3 has the internal pull-down, for the idle level (low). The target side of openmupp-v1 has a 10 kΩ pull-up. Thus without a target the line is high, and the decoder waits for a frame.

The decoder (`src/swo/ManchesterDecoder.h`):

- It uses only the 16-bit difference of two capture times. Thus the wrap of the timer has no effect inside a frame.
- An edge in the middle of a bit is valid from 0.75 to 1.25 bit times after the middle of the bit before. Thus the bit time of the target can be from about 15 % shorter to 25 % longer than the nominal bit time, less the jitter of the edges. The nominal bit time is two TPIU clocks of `DAP_SWO_Baudrate`.
  The limit for a shorter bit time is smaller, because a start bit after the shortest gap between two frames (1.5 bit times after the last bit middle) must come after the window of a bit middle.
- A frame ends at a rising edge that is too late for the middle of a bit, or when the line stays low for more than 1.5 bit times.
- A bit time out of range, a glitch, or a frame that does not end at a byte boundary sets the error bit of the trace status. The decoder then waits for a low line of more than 1.5 bit times before the next frame.
- Limit: the decoder starts only at the start bit of a frame. A capture that starts in a frame waits for the end of that frame. A target that sends with no pause keeps one frame open, and the capture gives no data and no error. Start the capture before the target sends, for example: start the capture, then reset the target.
- The decoder runs in the main loop, in the serial service of a long command, and before each read of the trace data or the status. The DMA interrupts of the edge buffers end the sleep of the main loop.
- Limit: a gap of more than one timer period (65536 ticks) between two edges looks like the gap modulo 65536. The main loop must see the end of a frame within one timer period. The main loop runs at least each millisecond, so this is true up to the maximum bit rate.

The maximum bit rate is 500 kbit/s. The CPU time of the decoder sets this limit:

- The decoder uses about 35 to 50 RISC-V instructions for each edge (from the disassembly of the Release image). This is an estimate of 60 to 80 clock cycles for each edge, at 144 MHz. It is not measured.
- A frame has one or two edges for each bit. A byte of `0x00` or `0xFF` has two edges for each bit.
- At 500 kbit/s, the worst case is 1 million edges each second. This takes about 40 % to 55 % of the CPU time.
  At 1 Mbit/s, the worst case takes all of the CPU time, and the USB transport stops. Thus the firmware limits the bit rate to 500 kbit/s.
- Each edge buffer holds 1024 edges: at least 2 ms of trace at 500 kbit/s. When the main loop stops for a longer time, the decoder loses edges and sets the overrun bit.
- During a command, the serial service runs only between two SWD transfers or sequence bytes. Below an SWD clock of about 25 kHz, one transfer takes longer than 2 ms. Thus a slow SWD clock and a high SWO bit rate can lose trace data (the overrun bit is set).
- The CPU time is high only while the target sends trace data. The idle line has no edges.

Shared resources:

- DMA2 channel 2 is also the TMS channel of the JTAG fast path. `src/board/SharedDmaChannel.h` keeps the two users apart:
  - Each setup of the JTAG port (`JtagSpi::init()`) stops the capture. All users of the JTAG port do this setup: the v2 and HID processors, the RISC-V bridge and the AVR JTAG target.
  - While a JTAG user has the channel (from `JtagSpi::init()` to `JtagSpi::fini()`), a start of the capture fails, and `DAP_SWO_Control` replies an error.
  - Also, `DAP_Connect` to JTAG on the v2 interface stops the capture, because SWO is an SWD function.
- A stop puts back the channel without the circular mode and without interrupts, because the JTAG fast path does not set them.
- A stop resets TIM5, so the timer sends no more DMA requests on DMA2 channel 2.
- The one-wire engine (DMA2 channels 10 and 11), the UART bridge (DMA2 channels 8 and 9) and the UART mode (DMA1 channel 6) use other channels.
- TIM2_CH4 on DMA1 channel 7 is not used: the SWIM plan uses TIM2_CH2 on PA1, which shares DMA1 channel 7.

### SWO trace endpoint

CMSIS-DAP v2 defines the SWO trace endpoint: an optional third endpoint of the CMSIS-DAP v2 interface, bulk IN.
The CH32V307 images have it as endpoint `0x86`, with 64-byte packets at full speed and 512-byte packets at high speed.
The other images do not have it, and `DAP_Info` then does not report SWO streaming trace.

- With `DAP_SWO_Transport` `2`, `CmsisDapFunction` sends the trace data on the endpoint, also after a stop of the capture.
- It sends the bytes that are in the trace buffer, up to one packet (`wMaxPacketSize`). When the host acknowledges a packet, the next packet goes out at once.
  Thus a packet can be short. The host reads with a large buffer and a timeout.
- `CmsisDapFunction` checks for new data in each main loop iteration, also between two commands. The main loop sleeps for at most 1 ms (one SysTick).
- The serial service also delivers the acknowledge of the endpoint during a long command (see [Serial port during long commands](usb-device.md#serial-port-during-long-commands)). Thus the trace continues during the command, and an acknowledge does not block `DAP_TransferAbort` and the serial port events.
- When the host clears a halt of the endpoint, the packet on its way goes out again.
- A bus reset or `SET_CONFIGURATION 0` drops the packet on its way. The capture stops, and the transport becomes none.
- The libArT `VendorBulkFunction` gives the events of the third endpoint to `CmsisDapFunction` (`otherInPacketSent()`, `otherInPacketsDropped()`).

## SWD engine rules

The adapter sets the CMSIS-DAP rules of the engine (see `TransferRules` in `src/swd/SwdBitBang.h`).
It never calls `configure()`, which sets the clock from an OpenOCD delay value.
Thus the order of `DAP_SWJ_Clock`, `DAP_SWD_Configure` and `DAP_TransferConfigure` does not matter.

The adapter is also the `RetryStop` of the engine.
After each WAIT acknowledge, the engine asks it for `DAP_TransferAbort`, and the adapter asks the processor.
When an abort is set, the engine stops the WAIT retries and returns WAIT.
The engine asks only after a WAIT, so a transfer without WAIT has no extra cost.
The abort arrives through the serial service, which the engine calls before each retry, at most once per millisecond.
Thus the engine does one more attempt after the abort arrives.
In an image with CMSIS-DAP, the processor sets the `RetryStop` at start, also when the Versaloon personality is active.
In the Versaloon personality, the processor gets no packets, so the stop always returns false, and the WAIT retries do not change.
A Versaloon-only image has no `RetryStop`.
`DAP_Connect` sets the configuration and the clock again, because the engine can run another personality in between.

## JTAG

`DAP_Connect` with port 2 releases the SWD pins and selects the JTAG engine. The JTAG engine starts at the first JTAG command.
The JTAG engine is the one of the Versaloon image (`board::BoardJtag`).
On the boards with the fast path, it shifts whole bytes with SPI and DMA when the SPI rate is not above the request and is at least the bit-bang rate (see `src/jtag/JtagFastPath.h`).
This starts at a TCK of 1125 kHz on the Mini and the CH32V307 boards. The other bits, and the slower rates, use GPIO bit-bang.
`DAP_SWJ_Clock` sets TCK in kHz, rounded down, and at least 1 kHz.

`DAP_JTAG_Configure` gives the device chain: the IR length of each device.
Device 0 is next to TDO.
After a reset of the debug unit, the chain is empty.
Thus the host must send `DAP_JTAG_Configure` before the first transfer.

A transfer command, `DAP_WriteABORT` and `DAP_JTAG_IDCODE` select the device with their device index.
A scan puts the other devices in BYPASS.
Each scan starts and ends in Run-Test/Idle, as in the reference implementation.
The host must put the TAP into Run-Test/Idle first, for example with `DAP_SWJ_Sequence`.

The JTAG-DP (`JtagDp` in `DapJtag.h`) gives each transfer the result that it has on SWD.
Thus the processor uses the same transfer rules for SWD and JTAG:

- An AP read is posted. Its data comes with the next AP read or with a read of RDBUFF.
- A DP read returns its own data. The JTAG-DP posts all reads, so `JtagDp` reads RDBUFF after a DP read.
- After the last write of a command, the processor reads RDBUFF to check the write.

The JTAG-DP acknowledge is OK/FAULT or WAIT.
After WAIT, `JtagDp` does the scan again, up to the WAIT retries of `DAP_TransferConfigure`.
A FAULT shows in the sticky bits of CTRL/STAT, as on each JTAG-DP.
Each DR scan of a transfer ends with the idle cycles of `DAP_TransferConfigure`.

`JtagDp` keeps the IR of the device from one transfer to the next, to skip IR scans.
These commands make the next transfer write the IR again: `DAP_Connect`, `DAP_JTAG_Configure`, a sequence command and `DAP_SWJ_Pins`.
A transfer to another device also writes the IR again.

## RISC-V DTM bridge

Status: in progress. Each image with the `cmsis-dap` personality has the bridge, unless the build sets `MUPP_CMSIS_DAP_RV_BRIDGE=OFF`. The [OpenOCD configs](#openocd-configs) are in `etc/openocd/`. The hardware test is done: see [Bridge hardware test](#bridge-hardware-test).
Test images showed OpenOCD and probe-rs on a CH32V208 (two wires), and OpenOCD on a CH32V006 (one wire), through the bridge ([captures](cmsis-dap-captures/README.md#bridge-captures)).

The bridge lets unmodified OpenOCD and probe-rs debug a CH32V target through this personality.
The host sees a CMSIS-DAP adapter in JTAG mode, with one RISC-V Debug Transport Module (DTM) on the chain.
`cmsisdap::RvDtmTap` (`src/protocol/cmsisdap/RvDtmTap.h`) is a JTAG port without pins.
It follows the TMS and TDI bits of `DAP_JTAG_Sequence` and `DAP_SWJ_Sequence`.
It does each DMI access on the WCH wire (one wire or two wires), with the wire engine of the `wch-link` personality.

| IR (5 bits) | Register | Content |
|---|---|---|
| `0x01` | IDCODE | `0x15653101` |
| `0x10` | DTMCS | `version` 1 (debug spec 0.13), `abits` 7, `idle` 0, `dmistat` |
| `0x11` | DMI (41 bits) | `address[40:34]`, `data[33:2]`, `op[1:0]` |
| Others | BYPASS | 1 bit |

### Build

The CMake option `MUPP_CMSIS_DAP_RV_BRIDGE` (default `ON`) adds the bridge to the `cmsis-dap` personality.
Each image with `cmsis-dap` has the bridge, also a multi image, for example `versaloon,cmsis-dap,wch-link`.
In a multi image, only the active personality sets up the pins and the timer of the wire engine. A personality switch resets the MCU.
An image without `cmsis-dap` ignores the option.
An image with the bridge has the settings page, also an image with one personality: 1 KB less flash on the STM32F103. On the CH32V307 the page is outside the flash of the image (see [Stored mode](#stored-mode)).
The bridge is the default, so it adds no image tag. An image with `cmsis-dap` and `MUPP_CMSIS_DAP_RV_BRIDGE=OFF` has the tag `-no-rv-bridge`, for example `openmupp-mini-cmsis-dap-no-rv-bridge-app-8k-Release-<version>.bin`.
The `cmsis-dap` and `dual` presets build the bridge:

```sh
cmake --preset mini-cmsis-dap
cmake --build --preset mini-cmsis-dap -j
```

The bridge adds about 6.4 KB to a `cmsis-dap` image.

### OpenOCD configs

`etc/openocd/` has a config for each target family:

| Config | Targets | Wire | `WORKAREASIZE` default |
|---|---|---|---|
| `ch32v003-cmsis-dap.cfg` | CH32V003 | One wire | `0x800` (2 KB) |
| `ch32v00x-cmsis-dap.cfg` | CH32V002, CH32V004, CH32V005, CH32V006, CH32V007 | One wire | `0x1000` (4 KB, the CH32V002) |
| `ch32v20x-cmsis-dap.cfg` | CH32V20x | Two wires | `0x2800` (10 KB, the CH32V203x6) |

Each config sources `ch32v-cmsis-dap-bridge.cfg`. This file selects the adapter (`1209:0001`, JTAG) and creates the TAP and the `riscv` target.
The work area starts at `0x20000000`, without backup.
The default `WORKAREASIZE` is the RAM of the smallest part. Set the RAM of your part before the config, for example 8 KB on the CH32V006:

```sh
openocd -c "set WORKAREASIZE 0x2000" -f etc/openocd/ch32v00x-cmsis-dap.cfg
```

The common file replaces the `jtag_init` proc of OpenOCD. The proc sends `cmsis-dap cmd 0x81 0x01` before the chain scan.
A TAP `setup` event is too late: OpenOCD sends it after the chain scan.
An image without the bridge replies `ID_DAP_Invalid`. OpenOCD logs the cause of the failure.
Then `init` stops with "The probe has no RISC-V bridge".
The proc also adds `cmsis-dap cmd 0x81 0x02` and `cmsis-dap cmd 0x81 0x03` to `pre_shutdown_commands`. Thus `shutdown` sets auto and then the stored mode before OpenOCD closes the adapter.
A firmware without the stored mode refuses `81 03`, and stays in auto. The change to auto keeps the port, so the first command causes no pin activity.
A firmware without the auto mode (on and off only) refuses `81 02` and `81 03`, so the bridge stays on after `shutdown`. Use the configs with a firmware that has the stored mode.
Ctrl-C and SIGTERM also run `shutdown`.
An error in a `-c` command after `init`, SIGKILL or a crash skips `pre_shutdown_commands`. Then the bridge stays on, and the next plain JTAG session finds the IDCODE `0x15653101`.
Set the stored mode with `python3 tools/mupp_probe.py bridge stored`, or replug the probe.
The CH32V00X config sets `riscv set_mem_access abstract`: the CH32V006 refuses the program buffer (`cmderr` 2).
The configs use `-chain-position` and `set_mem_access`, because OpenOCD 0.12.0 does not know `-tap` and `mem_access`. Newer OpenOCD versions show a deprecation warning for them.
The configs add a flash bank only when OpenOCD has the `ch32v` flash driver (see [Flash with OpenOCD](#flash-with-openocd)).

Result on 2026-10-24 (OpenOCD `0.12.0+dev-01707-gf7037392b`, Mini with the SPI1 remap fix, CH32V006 on one wire): OpenOCD examines the hart (`XLEN=32`, `misa=0x40801014`). Halt, a RAM write and read, and a step pass.
The CH32V006 has no triggers, so OpenOCD cannot set a hardware breakpoint.
The [bridge hardware test](#bridge-hardware-test) has the results of the three configs.

### Flash with OpenOCD

Mainline OpenOCD has no CH32V flash driver yet.
A patch series for mainline OpenOCD adds the `ch32v` flash driver. The series is on the OpenOCD Gerrit, changes 10176 to 10183.
The driver is change [10182](https://review.openocd.org/c/openocd/+/10182). Get the full series from the last change, [10183](https://review.openocd.org/c/openocd/+/10183).
Until the merge, build OpenOCD with that series.

With the `ch32v` driver, each config adds two flash banks:

| Bank | Address | Driver |
|---|---|---|
| `<CHIPNAME>.flash` | `0x08000000` | `ch32v` |
| `<CHIPNAME>.alias` | `0x00000000` | `virtual`, the same flash. The WCH SDK links programs to this address |

Without the driver, OpenOCD shows `Error: flash driver 'ch32v' not found`, and the config shows the line `Info : The config has no flash bank: see the error above`.
The config then has no flash bank, and debug works as before.

The driver probes the family from the chip ID and the flash size from `ESIG_FLACAP`.
It writes whole pages with a loader in the work area. The work area must hold the loader (196 bytes) and one page.
This command writes and verifies a program:

```sh
openocd -f etc/openocd/ch32v003-cmsis-dap.cfg -c "program firmware.elf verify reset exit"
```

The driver has these commands. Changes to the option bytes take effect after a reset (`reset halt`):

| Command | Function |
|---|---|
| `ch32v mass_erase 0` | Erases the full flash |
| `ch32v lock 0` | Sets the read protection (`RDPR` `0x00`) |
| `ch32v unlock 0` | Removes the read and write protection. A protected chip erases its flash |
| `ch32v options_read 0` | Shows the option bytes, `FLASH_OBR` and `FLASH_WPR` |
| `ch32v options_write 0 <user> [<data0> <data1>]` | Writes `USER`, and optionally `DATA0` and `DATA1` |
| `flash protect 0 <first> <last> on` | Sets the write protection of blocks (1 KB on the CH32V003, 2 KB on the CH32V00X, 4 KB on the CH32V20x) |

Results with the `ch32v` driver are in [Flash test with OpenOCD](#flash-test-with-openocd).

### Bridge switch

The vendor command `ID_DAP_Vendor1` (`0x81`) sets the mode of the bridge: `81 00` off, `81 01` on, `81 02` auto.
`81 03` sets the stored mode (see [Stored mode](#stored-mode)).
With bit 7 of the mode set, the command also stores the mode: `81 80` off, `81 81` on, `81 82` auto.
`81 83` gives `81 FF`.
The response is `81 00`. Another mode gives `81 FF`.
A failed store also gives `81 FF`, but the command still sets the mode.

| Mode | JTAG port |
|---|---|
| Off | The JTAG port of the board, as in an image without the bridge. |
| On | The emulated TAP. |
| Auto | At the first JTAG command after each `DAP_Connect` to JTAG, the probe finds the target (see [Auto mode](#auto-mode)). |

- The mode at power-up and after a USB reset is the stored mode. Auto is the stored mode until a store.
- In the TAP, `DAP_Connect` to JTAG, `DAP_JTAG_Sequence`, `DAP_SWJ_Sequence` and the JTAG transfers use the emulated TAP.
  SWD (`DAP_Connect` to SWD) does not change in any mode.
- A mode change while the JTAG port is in use stops the old port and starts the new one at once.
  The port is in use after the first JTAG command that follows `DAP_Connect`.
  Between `DAP_Connect` and that command, a mode change only sets the mode: the next JTAG command starts the port.
  The TAP state and the IR of the new port are not known, as after `DAP_SWJ_Pins`.
  The change to auto keeps the port until the next `DAP_Connect`. Thus the OpenOCD `shutdown` causes no pin activity on the target when the stored mode is auto.
- The change from the TAP to the board port releases SWCLK and SWDIO, and resets the timer of the one-wire engine.
  The JTAG fast path uses the same pin (PB4) and DMA1 channel 3.
  The release goes in both directions: the `fini()` of the JTAG fast path clears the DMA requests of SPI1 and SPI2.
  The one-wire timer shares DMA1 channel 3 (TIM3) or channel 5 (TIM1) with them.
  The `fini()` also removes the SPI1 remap. A disabled SPI1 with the remap still owns PB4 (MISO), and blocks the TIM3_CH1 output of the one-wire engine.
  A CH32V006 test on 2026-10-24 showed this: with the remap, the one-wire frames sent no bits after the line reset.
- `DAP_SWJ_Clock` sets the TCK of the board port also while the TAP is the port. The wire has its own clock: the high speed of the `wch-link` personality.

OpenOCD sends the command with `cmsis-dap cmd 0x81 0x01`.
probe-rs cannot send a vendor command. In the auto mode, probe-rs needs no command:

```sh
probe-rs info --protocol jtag
```

To force the TAP, send the command with `tools/mupp_probe.py bridge` before probe-rs starts:

```sh
python3 tools/mupp_probe.py bridge on
probe-rs info --protocol jtag
python3 tools/mupp_probe.py bridge stored
```

The mode stays until a USB reset, a power cycle or the next `ID_DAP_Vendor1`.
The OpenOCD configs send `81 02` and `81 03` at `shutdown` and after Ctrl-C, but not after an error in a `-c` command.
Thus probe-rs must not reset the USB device when it opens it. probe-rs 0.32.0 does not reset it ([Bridge hardware test](#bridge-hardware-test)).

### Stored mode

The stored mode is the mode at power-up and after a USB reset.
The firmware stores it in the settings page, in the same record as the stored personality (see [Multi image](usb-device.md#multi-image)).
To store a mode, set bit 7 of the mode in `ID_DAP_Vendor1`, or use `--store`:

```sh
python3 tools/mupp_probe.py bridge off --store
python3 tools/mupp_probe.py bridge auto --store
```

- A command without bit 7 does not write the flash. Thus the OpenOCD configs, which send `81 01`, `81 02` and `81 03` in each session, do not wear the flash.
- A store of the stored mode does not write the flash.
- A personality switch keeps the stored mode, and a store of the mode keeps the stored personality.
- A page without a mode gives auto. This is true for a page of an older multi image: its records are the records of the auto mode.
- On the STM32F103, a write stalls the CPU for about 50 µs. Each 512 stores, the store also erases the page, and the CPU stalls up to 40 ms. The interrupts wait, so the host sees a late response, and the UART bridge can lose received bytes. Do not store the mode during a target session.
- On the STM32H5, the page is in bank 2, so code in bank 1 does not stall during the write.
- On the CH32V307, the firmware sets HCLK to half for each write and each erase (one erase in 128 stores, up to 16 ms). For that time, the baud rate of the UART bridge is wrong and the SysTick runs at half rate. Do not store the mode during a target session.
- A firmware update keeps the page, as for the stored personality (see [Stored personality](firmware-update.md#stored-personality)). A mass erase erases it.
- Limit (STM32F103): the first flash of an image with one personality finds old data in the page, for example code of an older, larger image. A word of that data can be a record by chance (12 of 65536 values). The image then starts with that mode until the next store. On the STM32H5, a record also needs the other 112 bits of its flash word set, so this does not occur in practice. The limit is also true on the CH32V307. There, a write that a power loss cut can also read as a record.
- The page keeps the personality of another image. Thus a store of the mode in an image with one personality keeps the stored personality of a multi image.

### Auto mode

In the auto mode, the first JTAG command after `DAP_Connect` finds the target before it starts the port.
The steps are in this order. The first step that finds a target stops the search:

1. JTAG: the probe goes to Test-Logic-Reset and shifts 64 bits through Shift-DR with TDI high.
   A TDO that changes is a JTAG chain: the port is the JTAG port of the board.
   A TDO that stays high (the pull-up) or low is no chain.
2. SWD: the probe sends a line reset, the JTAG-to-SWD sequence and a line reset, then reads DPIDR.
   An ACK OK is an SWD target: the port is the JTAG port of the board.
   The probe then sends the SWD-to-JTAG sequence, so an SWJ-DP is in JTAG mode again.
3. WCH: the probe does the [connect](#connect), first on the wire of the last connect (one wire after the start), then on the other wire.
   A target that answers selects the emulated TAP.
4. No target: the port is the JTAG port of the board.

Each step releases its pins before the next step.
The result stays until the next `DAP_Connect`, `DAP_Disconnect`, mode change or USB reset.
Thus connect the target, and switch on its supply, before the first JTAG command.

The detection runs after the `DAP_SWJ_Pins` commands that the host sends between `DAP_Connect` and its first JTAG command.
With `connect_assert_srst` in OpenOCD, the host asserts SRST first. The detection then runs with SRST asserted.
A JTAG chain or an SW-DP normally answers while the target is in reset.
A target that does not answer while in reset is not found. The port is then the board port.
For such a target, connect without `connect_assert_srst`.

- The detection uses the TCK of `DAP_SWJ_Clock`. The SWD step uses the SWD clock and the settings of the last `DAP_SWJ_Clock`, `DAP_SWD_Configure` and `DAP_TransferConfigure`.
- The SWD step only runs when the JTAG step finds no chain.
  The JTAG host can then not use an SWD target, so the switch to SWD and back does no harm.
  The SWD step keeps WCH frames away from an SWJ-DP or SW-DP that answers the JTAG-to-SWD sequence.
  A dormant or multidrop SW-DP (for example the RP2040) does not answer it, and gets the WCH frames.
- After the WCH step, the first DMI access does the connect again, as after `DAP_Connect` in the on mode.
  The captures show that an attach at `DAP_Connect` passes (image B in [Connect](cmsis-dap-captures/README.md#connect)).
- The JTAG and SWD steps toggle SWCLK and SWDIO before the WCH step. The CH32V006 and the CH32V208 still connect after them (see [Auto mode hardware test](#auto-mode-hardware-test)).

### Connect

The first DMI access after one of these events connects to the target:

- `DAP_Connect` to JTAG, or the bridge switch on while JTAG is connected
- Test-Logic-Reset of the TAP
- `DAP_SWJ_Pins` with SWCLK or SWDIO selected

The connect finds the wire: one wire first, then two wires, as the connect of the `wch-link` personality.
After a connect on two wires, the next connect tries two wires first.
On each wire, it writes `5aa50400` to the DM registers `0x7e` and `0x7d`, and reads `dmstatus`.
A `dmstatus` with the version of debug spec 0.13 selects the wire.
The CH32V006 answers on one wire only.
The connect does not halt the hart, and it does no clock setup (unlike the `wch-link` connect of the CH32V00X).
OpenOCD and the flash driver own the target state.

The connect is at the first DMI access, not at the switch or at `DAP_Connect`.
A test image attached at boot, and the first DMI access then failed ([Connect](cmsis-dap-captures/README.md#connect)).
A connect that fails on both wires gives `op` 2 and the sticky error (see [DMI errors](#dmi-errors)).
After `dmireset`, the next access tries the connect again.

### IDCODE

The IDCODE is `0x15653101`: version 1, part `0x5653` ("VS"), and a manufacturer code with continuation code 1 and ID 0.

- JEP106 assigns no ID 0. Thus no real part has this manufacturer code, and no tool shows a manufacturer name for it. OpenOCD shows `<invalid>`.
- probe-rs 0.32.0 rejects an IDCODE whose 11-bit manufacturer field (bits 11:1) is 0 or `0x7F` (`IdCode::valid()`). The field of this IDCODE is `0x80`. A capture with the first value, `0x15653001` (field 0), showed the rejection.
- WCH has no JEP106 manufacturer code. The WCH fork of OpenOCD uses `0x00001` for its targets, which probe-rs rejects.
- OpenOCD accepts each IDCODE. The [OpenOCD configs](#openocd-configs) give this value with `-expected-id`.

### DMI errors

The bridge follows the debug spec 0.13 (section 6.1.5):

- A failed access gives `op` 2 at the next Capture-DR, and sets a sticky error.
- While the error is set, the bridge starts no access. DTMCS `dmistat` reads 2.
- `dmireset`, `dmihardreset` and Test-Logic-Reset clear the error.
- `op` 3 (busy) never occurs.

probe-rs reports `op` 2 as an error (`DtmOperationFailed`).
Mainline OpenOCD checks `op` 2 only in its batch memory read, and sends `dmireset` only after `op` 3.
Thus, with OpenOCD, a failed single access looks like a success.
After it, the sticky error blocks all accesses until Test-Logic-Reset, and OpenOCD does not log the cause.
A capture on 2026-10-24 (CH32V208, two wires, a test image) shows this: the first DMI read failed, and OpenOCD then read `dmcontrol` for 5 s ([openocd-ch32v208-attach-at-boot.pcapng](cmsis-dap-captures/openocd-ch32v208-attach-at-boot.pcapng)).
That access failed because the test image called `attach()` at boot, not because of a wire error ([Connect](cmsis-dap-captures/README.md#connect)).
The [unplug capture](cmsis-dap-captures/README.md#errors) shows the same limit with a real wire error.
On one wire, a frame without an answer of the target reads `ffffffff` and gives `op` 0, not `op` 2 ([One wire](cmsis-dap-captures/README.md#one-wire)).
Thus the host sees data that the target did not send. The planned changes below do not cover this yet.

Planned changes:

- The firmware retries a wire access 2 or 3 times before `op` 2, but only when the target did not run the access. Examples: a two-wire frame without the handshake of the target at bit 11, or a one-wire frame that did not start.
- A read with a parity error, or a one-wire line that stays low, gives `op` 2 at once. A second access can have side effects, for example on `command`, on `data0` with `abstractauto`, or on `sbdata0`.
- A patch for mainline OpenOCD makes `dmi_read()` and `dmi_write()` check `op` 2.

The bridge does not report a wire failure as `op` 3.
OpenOCD would then repeat accesses that the target already ran, and a permanent failure would stay hidden until a timeout.

### Bridge hardware test

The hardware test has a step for each target: the CH32V006, the CH32V003 and the CH32V208, on the Mini.
A last step checks the real JTAG port with an STM32F103 after the bridge was on.

#### CH32V006 on the Mini

| Item | Value |
|---|---|
| Date | 2026-10-24 |
| Probe | Mini, legacy bootloader, `mini-cmsis-dap-rv-bridge` `app-10k` image, `v0.11.0-130-g6487129` |
| Target | CH32V006K8U6 on one wire, with its own supply |
| OpenOCD | `0.12.0+dev-01355-gcba596a3e`, `ch32v00x-cmsis-dap.cfg` with `WORKAREASIZE` `0x2000` |
| GDB | `riscv-none-elf-gdb` (xPack GCC 15.2.0-1) |
| probe-rs | 0.32.0, `--chip CH32V006K8U6 --protocol jtag`, after `tools/mupp_probe.py bridge on` |
| Test programs | A counter loop of 14 bytes in RAM at `0x20000000`. The same loop in flash at `0x00000000`, with 16 KB of random data after it. Each loop writes its counter to `0x20001000` |

| Test | Tool | Result |
|---|---|---|
| Examine | OpenOCD | Pass. IDCODE `0x15653101`, `XLEN=32`, `misa=0x40801014` (RV32EMC and X) |
| `halt`, `resume` | OpenOCD | Pass. One OpenOCD `resume` resumes the hart: `dmstatus` shows `allrunning`, and the counter increases. The CH32V006 needs two `dmcontrol` writes (see [Debug module layer](wch-link.md#debug-module-layer)), and OpenOCD resumes it |
| `step` | OpenOCD, GDB `stepi` | Pass. See note 1 |
| `reg` | OpenOCD, GDB | Pass: `pc`, `a0`, `a1`, `misa`, `dcsr` |
| `mdw`, `mww` | OpenOCD, GDB | Pass on a halted hart. On a running hart, OpenOCD `mdw` fails with "Failed to read priv register" |
| Software breakpoint in RAM | OpenOCD `bp`, GDB `break` | Pass. The hart stops at each pass of the loop, and `rbp` puts the original instruction back |
| Hardware breakpoint | OpenOCD `bp ... hw`, GDB `hbreak` | Not available: the CH32V006 has no triggers ("Found 0 triggers") |
| `load_image` to RAM | OpenOCD, GDB `load` | Pass, with `verify_image` |
| Reset by `ndmreset` | OpenOCD `reset halt`, `reset run` | Pass. `reset halt` stops at `pc` 0. After `reset run`, `dmstatus` shows `allhavereset` and `allrunning` |
| WCH registers `0x7c` to `0x7f` | OpenOCD `riscv dmi_read` | Pass: `11038403`, `5aa58401`, `00000400`, `00600620` |
| `shutdown` | OpenOCD | Pass. The bridge is off after it: a plain JTAG session finds no TAP |
| `info` | probe-rs | probe-rs finds the DTM, but it cannot identify the chip. It ignores `--chip`. The same result as on the CH32V208 ([captures](cmsis-dap-captures/README.md#results)) |
| `read` | probe-rs | Pass. 16 KB of flash agree with the image |
| `download --verify` | probe-rs, built-in flash algorithm | Pass. 16 KB in 15.7 s, with the verify. After `probe-rs reset`, the program runs |
| `gdb` | probe-rs | Fail, in probe-rs. See note 2 |
| Bridge on after `mupp_probe.py bridge on` | probe-rs | Pass. probe-rs does not reset the USB device when it opens it |
| Fast path first, then bridge on, in one USB session | Python script, then OpenOCD | Pass. See note 3 |

Notes:

1. One time, the first `step` did not change `pc`. The flash was erased at that time, and the hart reset again and again. With a program in flash, every `step` passed: after `reset halt`, after a `pc` write, and after `halt`.
2. When GDB connects, probe-rs reads all 32 general registers. The CH32V006 is RV32E and has only `x0` to `x15`. The abstract command for `x16` (regno `0x1010`) gives `cmderr` 3 (exception), and probe-rs ends the session with "A RISC-V specific error occurred". probe-rs does not use the E extension of `misa` here. The bridge sends the correct result of the target. An original WCH-LinkE (firmware 2.19) with the probe-rs `wlink` driver gives the same failure on the same CH32V006.
3. The script sends `81 00`, `DAP_Connect` to JTAG and `DAP_SWJ_Clock` 9 MHz. Then it sends 20 DR scans of 128 bits with TDO capture. These scans use the SPI fast path: a TCK of 9 MHz is above the fast path limit of 1125 kHz. The test did not measure the pins. Then the script sends `81 01` and reads the IDCODE `0x15653101` from the emulated TAP. OpenOCD then examines the hart, halts it and reads RAM and flash.

Rates, with the target at its reset clock:

| Measurement | Result |
|---|---|
| OpenOCD `riscv dmi_read`, 1000 single reads | 0.87 ms for each read. Each read is one USB round trip |
| OpenOCD memory read, `dump_image` of 8 KB | 4.64 s (1.72 KiB/s) |
| OpenOCD memory write, `load_image` of 8 KB | 4.62 s (1.73 KiB/s) |
| probe-rs `read` of 16 KB | 4.28 s (3.7 KiB/s), with the attach |
| probe-rs `download --verify` of 16 KB | 15.7 s |
| One-wire DMI frame, Saleae on SWDIO | 44 µs to 52 µs |
| Time between two DMI frames in one USB packet | 334 µs minimum |

OpenOCD reads memory with the abstract command, one word at a time.
For each word, it sends 3 USB packets with 7 DMI scans, and the wire carries about 3 DMI frames.
OpenOCD does not use `aampostincrement` on this target.
The USB capture shows that the host sends the next packet a few µs after the reply.
The probe takes about 1.1 ms for a packet with 3 DMI scans, and about 0.2 ms for a packet with only TMS moves.
Thus the firmware takes about 300 µs for each DMI scan, and the wire uses only about 50 µs of it.
`adapter speed` (100 kHz to 10 MHz) does not change the rate.
The [CH32V006 rates on the WeAct STM32H523](#ch32v006-rates-on-the-weact-stm32h523) show the cause.

The rates with the fix, on 2026-10-26, with a Mini with `openmupp-bootloader` and `app-8k` images of `mini-cmsis-dap-rv-bridge`:

| Measurement | Before (`v0.12.0-60-gd7f95ac`) | After the fix |
|---|---|---|
| OpenOCD `riscv dmi_read`, 1000 single reads | 0.908 ms for each read | 0.531 ms for each read |
| OpenOCD memory read, `dump_image` of 8 KB | 4.80 s (1.67 KiB/s) | 2.93 s (2.73 KiB/s) |
| OpenOCD memory write, `load_image` of 4 KB to RAM | 2.38 s (1.68 KiB/s) | 1.45 s (2.76 KiB/s) |
| probe-rs `read` of 16 KB, with the attach | 4.41 s (3.6 KiB/s) | 2.95 s to 3.00 s (5.4 KiB/s) |
| One-wire DMI frame, Saleae on SWDIO, OpenOCD `dump_image` of 1 KB | 50 µs | 50 µs |
| Time between two DMI frames in one USB packet | 350 µs to 375 µs | 150 µs to 175 µs |
| Time between two DMI frames in two USB packets | 825 µs to 1000 µs | 500 µs to 650 µs |

The data of the reads is the same before and after the fix, and `verify_image` passes after each `load_image`.

#### CH32V003 on the Mini

The probe, OpenOCD, GDB and probe-rs are the same as for the CH32V006.

| Item | Value |
|---|---|
| Date | 2026-10-24 |
| Target | CH32V003 on one wire, with its own supply. probe-rs uses `--chip CH32V003F4P6` |
| OpenOCD config | `ch32v003-cmsis-dap.cfg`, default `WORKAREASIZE` (`0x800`) |
| Test programs | The counter loops of the CH32V006 test, with the counter at `0x20000400` (2 KB RAM). The flash program has 8 KB of random data |

| Test | Tool | Result |
|---|---|---|
| Examine | OpenOCD | Pass. IDCODE `0x15653101`, `XLEN=32`, `misa=0x40800014` (RV32EC and X). Chip ID at `0x1ffff7c4`: `00300500` |
| `halt`, `resume` | OpenOCD | Pass. The counter increases after `resume` |
| `step` | OpenOCD, GDB `stepi` | Pass |
| `reg` | OpenOCD, GDB | Pass: `pc`, `a0`, `a1`, `x15`, `misa`, `dcsr` |
| `mdw`, `mww` | OpenOCD, GDB | Pass on a halted hart. OpenOCD uses the program buffer |
| Software breakpoint in RAM | OpenOCD `bp`, GDB `break` | Pass |
| Hardware breakpoint | OpenOCD `bp ... hw` | Not available: no triggers |
| `load_image` to RAM | OpenOCD, GDB `load` | Pass, with `verify_image` |
| Reset by `ndmreset` | OpenOCD `reset halt`, `reset run` | Pass. `reset halt` stops at `pc` 0. Unlike the CH32V006, `dmstatus` does not show `allhavereset` after the reset |
| WCH registers `0x7c` to `0x7f` | OpenOCD `riscv dmi_read` | Pass: `00010403`, `5aa50401`, `5aa50401`, `00300500` |
| `shutdown` | OpenOCD | Pass. The bridge is off after it |
| `info` | probe-rs | probe-rs finds the DTM, but it cannot identify the chip. The same result as on the CH32V006 |
| `read` | probe-rs | Pass. 8 KB of flash agree with the image |
| `download --verify` | probe-rs, built-in flash algorithm | Pass. 8 KB in 20.1 s, with the verify. After `probe-rs reset`, the program runs |
| `gdb` | probe-rs | Fail, in probe-rs: the same `x16` read as on the CH32V006 (note 2 above) |

After a GDB `monitor reset halt`, OpenOCD shows "Register fp is dirty!" and "Register s1 is dirty!".
OpenOCD uses `s0` and `s1` as scratch registers for the program buffer, and it discards their values at the reset.

Rates, with the target at its reset clock:

| Measurement | Result |
|---|---|
| OpenOCD `riscv dmi_read`, 1000 single reads | 0.87 ms for each read |
| OpenOCD memory read, `dump_image` of 2 KB RAM | 0.27 s (7.4 KiB/s) |
| OpenOCD memory read, `dump_image` of 8 KB flash | 1.03 s (7.8 KiB/s) |
| OpenOCD memory write, `load_image` of 2 KB RAM | 0.25 s (8.0 KiB/s) |
| probe-rs `read` of 8 KB | 2.23 s (3.6 KiB/s), with the attach |
| probe-rs `download --verify` of 8 KB | 20.1 s |

OpenOCD is about 4 times faster on the CH32V003 than on the CH32V006.
On the CH32V003, it uses the program buffer. This test did not capture the USB packets of the CH32V003.
The CH32V006 refuses the program buffer, so OpenOCD uses the abstract command there (see [OpenOCD configs](#openocd-configs)).

#### CH32V208 on the Mini

The probe, OpenOCD, GDB and probe-rs are the same as for the CH32V006.

| Item | Value |
|---|---|
| Date | 2026-10-24 |
| Target | CH32V208WBU6 on two wires (SWDIO, SWCLK), with its own supply. BOOT0 is tied low. probe-rs uses `--chip CH32V208WBU6` |
| OpenOCD config | `ch32v20x-cmsis-dap.cfg`, default `WORKAREASIZE` (`0x2800`) |
| Test programs | The counter loops of the CH32V006 test (counter at `0x20001000`, 16 KB flash program) |

| Test | Tool | Result |
|---|---|---|
| Examine | OpenOCD | Pass. IDCODE `0x15653101`, `XLEN=32`, `misa=0x40901105` (RV32IMAC, U and X) |
| `halt`, `resume` | OpenOCD | Pass |
| `step` | OpenOCD, GDB `stepi` | Pass |
| `reg` | OpenOCD, GDB | Pass: `pc`, `a0`, `a1`, `x31`, `dcsr` |
| `mdw`, `mww` | OpenOCD, GDB | Pass on a halted hart |
| Software breakpoint in RAM | OpenOCD `bp`, GDB `break` | Pass |
| Hardware breakpoint | OpenOCD `bp ... hw` | Fail, in OpenOCD: "Found 0 triggers". See note 1 |
| Hardware breakpoint | probe-rs `gdb`, GDB `hbreak` | Pass in flash. probe-rs finds 4 `mcontrol` triggers |
| `load_image` to RAM | OpenOCD, GDB `load` | Pass, with `verify_image` |
| Reset by `ndmreset` | OpenOCD `reset run` | Pass |
| Reset by `ndmreset` | OpenOCD `reset halt` | Fail, "timed out while waiting for target halted". With BOOT0 low, 8 of 8 runs failed. Before that, 1 of 6 runs passed. See note 2 |
| WCH registers `0x7c` to `0x7f` | OpenOCD `riscv dmi_read` | `00000000`, `00000000`, `00000000`, `2080053c` |
| `shutdown` | OpenOCD | Pass. The bridge is off after it |
| `info` | probe-rs | probe-rs finds the DTM, but it cannot identify the chip. The same result as on the CH32V006 |
| `read` | probe-rs | Pass. 16 KB of flash agree with the image |
| `download --verify` | probe-rs, built-in flash algorithm | Pass in 4 of 4 runs with BOOT0 low. 16 KB in 14.5 s, with the verify. After `probe-rs reset`, the program runs. See note 3 |
| `gdb` | probe-rs, GDB | Pass: connect, `stepi`, registers up to `x31`, memory read and write, `hbreak` |
| Wrong wiring | OpenOCD | See note 4 |

Notes:

1. On the CH32V208, the `tinfo` CSR (`0x7a4`) reads 0, without an exception. OpenOCD takes `tinfo` 0 as "no trigger" (`get_trigger_types()` in `src/target/riscv/riscv.c`), and stops the count. It does not look at the type in `tdata1`. `tdata1` of trigger 0 reads `20000000` (type 2, `mcontrol`). probe-rs uses the type in `tdata1`, and finds 4 triggers.
2. After a reset with `haltreq`, the CH32V208 sets `allhavereset` before it halts. A poll of `dmstatus` after the reset reads `allrunning` and `allhavereset` first, and `allhalted` about 0.8 ms later. OpenOCD waits only for `allhavereset` (`deassert_reset()` in `src/target/riscv/riscv-013.c`). Then it writes `ackhavereset`, and this write also clears `haltreq`. When `haltreq` goes to 0 before the hart halts, the hart continues to run. OpenOCD then halts the hart to set `dcsr.ebreak*`, resumes it, and waits for a halt until the timeout. The debug spec 0.13 expects the hart to halt when `haltreq` stays set during the reset. In a manual sequence with `riscv dmi_write`, the `ackhavereset` write came 5 ms after the reset. Then the hart halted in 10 of 10 tries. The CH32V006 and the CH32V003 halt in time.
3. In a first run, BOOT0 was not connected. Then address `0x00000000` showed the system bootloader (the same words as `0x1fff8000`) after some resets, and the user flash after others. The test image is linked at `0x00000000`, so probe-rs compared against the bootloader, and the verify failed in 3 of 6 runs. The user flash at `0x08000000` was correct each time. With BOOT0 low, address `0x00000000` showed the user flash after 10 of 10 resets.
4. SWCLK was not connected in a first run. OpenOCD waited 5 s, and stopped with "Debug Module did not become active". DTMCS read `0x871`: `dmistat` 2, the sticky DMI error. OpenOCD does not show this error (see [DMI errors](#dmi-errors)).

Rates, with the target at its reset clock:

| Measurement | Result |
|---|---|
| OpenOCD `riscv dmi_read`, 1000 single reads | 0.83 ms for each read |
| OpenOCD memory read, `dump_image` of 8 KB RAM | 0.95 s (8.4 KiB/s) |
| OpenOCD memory write, `load_image` of 8 KB RAM | 0.90 s (8.9 KiB/s) |
| OpenOCD memory read, `dump_image` of 16 KB flash | 1.89 s (8.5 KiB/s) |
| probe-rs `read` of 16 KB | 2.77 s (5.8 KiB/s), with the attach |
| probe-rs `download --verify` of 16 KB | 14.5 s |

#### CH32V006 rates on the WeAct STM32H523

This test looked for the firmware time of a DMI scan on an STM32H5 board.
It measured rates only, not the full test list of the Mini.

| Item | Value |
|---|---|
| Date | 2026-10-25 |
| Probe | WeAct STM32H523, `weacth523-cmsis-dap` with `-DVSL_CMSIS_DAP_RV_BRIDGE=ON`, standalone image. Before the fix: `v0.12.0-60-gd7f95ac` |
| Target | CH32V006K8U6 on one wire, on the wiring of the [WeAct one-wire test](wch-link.md#ch32v006-on-the-weact-stm32h523-one-wire) |
| OpenOCD | The same as for the CH32V006 on the Mini, `adapter serial` of the WeAct |
| Profile | A Mini in CMSIS-DAP mode on the SWD port of the WeAct. pyOCD read `DWT_PCSR` (the PC sample register) during the OpenOCD run, without a halt of the core |

The profile before the fix:

- 41% of the samples were in the main loop: the firmware waited for the next USB packet.
- 18% were in the one-wire frame (`oneWireFrame()`), mostly the wait for the wire.
- 14% were in `memset`, called from `JtagDp::start()`.

OpenOCD sends about 10 sequences in `DAP_JTAG_Sequence` for each DMI scan, and 9 of them have 1 bit.
`JtagDp::start()` cleared the three scan buffers (38 bytes each) for each sequence.
`JtagDp::sequence()` then copied the TDI bits one at a time into the buffer.
The fix clears only the bytes of the scan, and gives the bytes of a sequence to the port as they are.

| Measurement | Before | After |
|---|---|---|
| OpenOCD `riscv dmi_read`, 1000 single reads | 0.284 ms for each read | 0.221 ms for each read |
| OpenOCD memory read, `dump_image` of 8 KB | 1.56 s (5.1 KiB/s) | 1.28 s (6.3 KiB/s) |
| OpenOCD memory write, `load_image` of 4 KB to RAM | not measured | 0.62 s (6.5 KiB/s), `verify_image` passes |
| One-wire DMI frame, Saleae on SWDIO | 50 µs | 50 µs |
| Time between two DMI frames in one USB packet | 50 µs to 75 µs | 25 µs to 50 µs |
| Time between two DMI frames in two USB packets | 225 µs to 300 µs | 175 µs to 275 µs |

After the fix, timestamps of the cycle counter in `Processor::execute()` and `Processor::jtagSequence()` (a test image) showed this:

- A sequence of 1 bit takes 1.0 µs. A sequence with a DMI frame takes 63 µs to 67 µs.
- The firmware time outside the sequences is 16 µs to 21 µs for each packet.
- The host sends the next packet 72 µs to 111 µs after the end of the last one. This time includes the USB transfer of the reply and of the request.

Thus the wire and USB now use most of the time.
The remaining firmware time is spread over the TAP emulation and the packet code.
A build with `-O2` for `DapJtag.cpp` and `RvDtmTap.cpp` did not make the sequences faster.
The instruction cache misses are below 1%.

#### Flash test with OpenOCD

The test uses the `ch32v` flash driver (see [Flash with OpenOCD](#flash-with-openocd)).

| Item | Value |
|---|---|
| Date | 2026-11-02 |
| Probe | `cmsis-dap` image, version 0.13 (`bcdDevice`), serial `320069063053363247680543` |
| OpenOCD | `0.12.0+dev-01780-g1f6863394` with the `ch32v` patch series, an earlier version of Gerrit changes [10176](https://review.openocd.org/c/openocd/+/10176) to [10183](https://review.openocd.org/c/openocd/+/10183) |
| Test data | Random files of 8000 bytes (the end is not on a page boundary) and of 16 KB |

CH32V003 (`ch32v003-cmsis-dap.cfg`, default `WORKAREASIZE`):

| Test | Result |
|---|---|
| `flash probe` | Pass: `CH32V003, chip ID 0x00300500, 16 KiB flash`, 16 protection blocks of 1 KB |
| `program` of 8000 bytes, `verify` | Pass |
| `program` to `0x00000000` (the alias bank), `verify_image` at `0x08000000` | Pass |
| `flash erase_sector 0 2 3` | Pass: only `0x08000080` to `0x080000ff` read `0xffffffff` |
| `flash fillw` at `0x08000084` (not on a page boundary) | Pass: OpenOCD pads the page, and the other words of the page stay erased |
| `ch32v mass_erase`, `flash erase_check` | Pass: "Bank is erased" |
| `flash write_image erase` of 16 KB | Pass: 2.55 s (6.3 KiB/s), with the erase. `verify_image`: 0.16 s |
| `flash protect 0 2 3 on`, `reset halt` | Pass: `FLASH_WPR` `0xfffffff3`. A write to block 2 fails with `WRPRTERR`. A write to block 4 passes |
| `flash protect 0 0 last off`, `reset halt` | Pass: `FLASH_WPR` `0xffffffff` |
| `ch32v options_write 0 0x17 0x42 0x99`, `reset halt` | Pass: `FLASH_OBR` shows `DATA0` `0x42` and `DATA1` `0x99`. Then back to `0x00` and `0xff` |
| `ch32v lock`, `reset halt` | Pass: `RDPRT` 1. A flash read fails ("progbuf startup failed") |
| `ch32v unlock`, `reset halt` | Pass: `RDPRT` 0. The chip erased the flash itself. `ndmreset` loads the new `RDPR` |

For comparison: probe-rs needs 20.1 s for 8 KB on the same chip ([CH32V003 on the Mini](#ch32v003-on-the-mini)).

CH32V006 (`ch32v00x-cmsis-dap.cfg` with `WORKAREASIZE` `0x2000`, and the OpenOCD config `target/wch/ch32v00x.cfg`):

| Test | Result |
|---|---|
| `flash probe` | Pass: `CH32V00X, chip ID 0x00600620, 62 KiB flash`, 31 protection blocks of 2 KB. The chip ID is at `0x1ffff704`. At `0x1ffff7c4`, the CH32V006 reads `0xffffffff` |
| `program` of 8000 bytes, `verify` | Pass |
| `flash write_image erase` of 62 KB | Pass: 23.3 s (2.7 KiB/s), with the erase. `verify_image`: 0.55 s |
| `flash erase_sector 0 2 3` | Pass: only `0x08000200` to `0x080003ff` read `0xffffffff` |
| `flash fillw` at `0x08000204` | Pass |
| `flash protect 0 2 3 on`, `reset halt` | Pass: `FLASH_WPR` `0xfffffff3`. A write to `0x08001000` (block 2) fails with `WRPRTERR`. A write to block 4 passes |
| `ch32v options_write 0 0xff 0x42 0x99`, `reset halt` | Pass |
| `ch32v lock`, `reset halt` | Pass: `RDPRT` 1, a flash read fails |
| `ch32v unlock`, `reset halt` | Pass: `RDPRT` 0, and the flash is erased. `ndmreset` loads the new `RDPR`. No power cycle is necessary |

The RAM write of the abstract command limits the write rate (see [CH32V006 on the Mini](#ch32v006-on-the-mini): `load_image` at 2.76 KiB/s).

Two problems occurred during the test. The driver and the configs now prevent them:

- Without the abstract memory access (`riscv mem_access abstract`, or the older name `riscv set_mem_access abstract` of the OpenMUPP configs), OpenOCD sends 16-bit writes through the program buffer. The CH32V006 refuses the program buffer, and the write times out with `abstractcs.busy` 1. The option bytes were then erased (`RDPR` `0xff`), and the chip was read protected. The CH32V00X configs select the abstract command.
- With `OBPG` and `STRT` in one `FLASH_CTLR` write after the option byte erase, the CH32V006 does not program the first half word (`RDPR`). The other half words are programmed. The driver now writes `OBPG` alone first.

CH32V208 (`ch32v20x-cmsis-dap.cfg`, and the OpenOCD config `target/wch/ch32v20x.cfg`, default `WORKAREASIZE`):

| Test | Result |
|---|---|
| `flash probe` | Pass: `CH32V20x, chip ID 0x2080053c, 160 KiB flash`, 32 protection blocks of 4 KB. The chip ID is at `0x1ffff704` |
| `ch32v options_read` | Pass: `USER` `0x3f`. The values agree with the option byte area |
| Erased flash | Reads `0xe339e339`, as the manual says. `flash erase_check` uses this value |
| `program` of 8000 bytes, `verify` | Pass |
| `flash write_image erase` of 160 KB | Pass: 13.3 s (12.0 KiB/s), with the erase. `verify_image` at `0x00000000`: 0.70 s |
| `flash erase_sector 0 2 3` | Pass: only `0x08000200` to `0x080003ff` read `0xe339e339` |
| `flash fillw` at `0x08000204` | Pass. The padding words read `0xffffffff` |
| `ch32v mass_erase`, `flash erase_check` | Pass, with a fix in the driver. See the note below the table |
| `flash protect 0 2 3 on`, `reset halt` | Pass: `FLASH_WPR` `0xfffffff3`. A write to `0x08002000` (block 2) fails with `FLASH_STATR` `0x12` (`WRBSY`, `WRPRTERR`). The next write to block 4 passes |
| `ch32v options_write 0 0x3f 0x42 0x99`, `reset halt` | Pass |
| `ch32v lock`, `reset halt` | Pass: `RDPRT` 1, a flash read fails |
| `ch32v unlock`, `reset halt` | Pass: `RDPRT` 0, and the flash reads `0xe339e339` |

The `ch32v mass_erase` and `flash erase_check` test failed at first: in 2 of 2 runs of the full test list, `erase_check` showed sector 0 as not erased.
After a mass erase of a full flash, the first reads of the CH32V208 give wrong data, also after `BSY` is 0 and `EOP` is 1.
A read 1 ms after the end of the erase gave wrong data in the first 16 bytes. A read after 3 ms gave `0xe339e339`.
The driver now waits 10 ms after a mass erase. Then the sequence that failed (160 KB write, sector erase, `flash fillw`, mass erase, `erase_check`) passed in 5 of 5 runs.

The test found a second problem on the CH32V208, and the loader now prevents it: a word write to a protected page sets `WRPRTERR` at once, but `WRBSY` stays 1. The first loader waited for `WRBSY` until the timeout of OpenOCD.

The final loader adds the `WRPRTERR` check in the `WRBSY` wait of the CH32V20x method only, and its error path clears the flags.
The CH32V003 and CH32V006 tests ran again on 2026-11-02 with the final loader and the OpenOCD configs `target/wch/ch32v003.cfg` and `target/wch/ch32v00x.cfg`.
The results did not change: 16 KB in 2.53 s, and 62 KB in 23.2 s.
The CH32V006 test also passed with the `EOP` wait of the driver, and 6 of 6 runs of `ch32v mass_erase` and `flash erase_check` passed.

The patch series also fixes the two OpenOCD problems of the [CH32V208 test](#ch32v208-on-the-mini):

| Test | Before | With the patch series |
|---|---|---|
| `reset halt` | 0 of 8 runs | 10 of 10 runs stop at `pc` 0 |
| Triggers | "Found 0 triggers" (`tinfo` reads 0) | "Found 4 triggers" |
| `bp 0x00000004 2 hw` in flash, `resume` | Not available | Pass: "halted due to breakpoint" at `pc` `0x4`. The CH32V208 hardwires `dmode` in `tdata1` to 0. OpenOCD now accepts this |

#### OpenOCD messages at examine

On the three targets, OpenOCD logs "Vector support with vlenb=0" and "S?aia detected with IMSIC" at examine.
These targets have no vector extension and no AIA.
OpenOCD prints the messages when a read of the CSR `vlenb`, or of `mtopi` and `mtopei`, succeeds (`src/target/riscv/riscv-013_reg.c`).
The WCH cores give 0 for these reads, without an exception.
The `tinfo` read of the CH32V208 (note 1 of the CH32V208) has the same cause.
The messages have no other effect in this test.

On the CH32V006, the first `halt` after `init` showed "halted due to undefined". The next halts showed "halted due to debug-request".
OpenOCD sets the debug reason to "undefined" at examine (`examine()` in `src/target/riscv/riscv-013.c`).

#### Real JTAG after the bridge

This step checks that the JTAG port of the board works after the bridge was on.
The probe and OpenOCD are the same as for the CH32V006.
The target is the STM32F103 of the [JTAG hardware test](#jtag-hardware-test) (DBGMCU IDCODE `10036414`), with `interface/cmsis-dap.cfg` and `target/stm32f1x.cfg`.
The flash test uses a random image of 32 KiB, as in the JTAG hardware test. At 9000 kHz, `tools/hwtest/stm32f1_clock.tcl` sets the target clock to 64 MHz first.

| Test | Result |
|---|---|
| `mupp_probe.py bridge on`, then `off`, then an OpenOCD JTAG session | Pass. OpenOCD finds the two TAPs (`0x3ba00477`, `0x06414041`). `reset halt` stops at the reset vector |
| The same switch before each flash run, then `program ... verify` and `dump_image` at 1000 kHz | Pass. The read-back agrees with the image |
| The same at 9000 kHz (SPI fast path) | Pass. The read-back agrees with the image |
| `cmsis-dap cmd 0x81 0x00` in a JTAG session, with the bridge already off | Pass. The next memory read works |
| `cmsis-dap cmd 0x81 0x01`, then `0x81 0x00`, in a JTAG session, then a memory read | Fail: "Invalid ACK (7) in DAP response". See the note |
| The same, then `jtag arp_init`, then memory read and write, `step`, a hardware breakpoint and `reset halt` | Pass |

Note: a switch while JTAG is connected starts the new port with an unknown TAP state (see [Bridge switch](#bridge-switch)).
OpenOCD does not know this. Probably it uses the IR of the target from before the switch. The test did not capture the scans to confirm this.
`jtag arp_init` resets the TAPs and scans the chain again. After it, OpenOCD and the target agree again.
The configs in `etc/openocd/` switch the bridge only before the chain scan and at `shutdown`, so they do not have this problem.

#### Auto mode hardware test

This step checks the port that the [auto mode](#auto-mode) selects for each type of target.
No tool sends `ID_DAP_Vendor1` in the sessions: `mupp_probe.py bridge auto` sets the mode before each step.
OpenOCD uses `interface/cmsis-dap.cfg` with its own TAP and target lines, not the configs in `etc/openocd/`, because these configs send `81 01`.

| Item | Value |
|---|---|
| Date | 2026-10-28 |
| Probes | Two Minis, Mini 1 and Mini 2 (`app-8k`), multi image `cmsis-dap,wch-link` with `VSL_WCH_LINK_WCH_USB_ID=ON`, commit `ec2e4c8` |
| Host | Linux, OpenOCD `0.12.0+dev-01355-gcba596a3e`, probe-rs 0.32.0, wlink |
| TCK | 1000 kHz |

| Target | Probe | Expected port | Result |
|---|---|---|---|
| CH32V006, one wire | Mini 1 | Emulated TAP | Pass. probe-rs `info --protocol jtag` finds IDCODE `0x15653101` and reads `dmstatus`. OpenOCD examines the hart (`misa=0x40801014`) and reads the WCH registers `0x7c` to `0x7f` (`11038403`, `5aa58401`, `00000400`, `00600620`) |
| CH32V208, two wires | Mini 1 | Emulated TAP | Pass. OpenOCD examines the hart (`misa=0x40901105`), reads the chip ID `2080053c` and the flash size (160 KB), and writes and reads RAM. `wlink status` attaches before and after the session |
| STM32F103, JTAG | Mini 1, Mini 2 | Board port | Pass. OpenOCD finds the two TAPs (`0x3ba00477`, `0x06414041`) and examines the Cortex-M3. probe-rs reads memory with `--chip STM32F103RE` |
| STM32F103, SWD only (no TDO) | Mini 2 | Board port | Pass. The JTAG scan finds no chain and no emulated TAP. An SWD session after the auto connects reads DPIDR `0x1ba01477` |
| STM32H523, SWD only | Mini 1 | Board port | Pass. The JTAG scan finds no chain. An SWD session reads DPIDR `0x6ba02477` |
| No target | Mini 1 | Board port | Pass. The JTAG scan finds TDO low and no emulated TAP |

Time of the auto detection at 1000 kHz, 5 runs each, measured on the host with pyusb as the time of `DAP_Connect` to JTAG. The detection now runs in the first JTAG command after `DAP_Connect`, with the same steps:

| Target | Auto | Off |
|---|---|---|
| STM32H523, SWD only (Mini 1) | 0.54 to 0.62 ms | 0.12 to 0.14 ms |
| STM32F103, SWD only (Mini 2) | 0.52 to 0.55 ms | 0.11 to 0.15 ms |
| SWD target that does not answer (Mini 1) | 3.14 to 3.18 ms | Not measured |
| No target (Mini 1) | 0.24 to 0.38 ms | 0.11 to 0.18 ms |

A logic analyzer capture of the SWD target that does not answer shows the steps of one connect, in this order:

1. The JTAG step: 75 TCK clocks.
2. The SWD step: 136 bits of JTAG-to-SWD, then the DPIDR read. The target does not give an ACK OK.
3. The WCH step: frames on two wires, then about 2 ms later three frames on one wire.

Thus a connect that runs the WCH step takes about 3 ms.
The SWD-only connects of 0.5 ms have no time for the WCH step: the search stops at the SWD step.
No capture of these connects confirms this.
The connect without a target takes less time than the connect with an SWD target. The cause is not known, because no capture of it exists.

Notes:

1. probe-rs `info` without `--chip` cannot identify the CH32V006, the CH32V208 or the STM32F103 ("The connected chip could not automatically be determined"). The scan of the chain passes before this message.
2. OpenOCD `scan_chain` without a target stops with an assertion (`buf_get_u32: Assertion 'num >= 1 && num <= 32' failed`). The same occurs with the bridge off, so the cause is OpenOCD with a chain of only zeros.
3. On the CH32V208, `resume` fails (`dmstatus=0x00000c82`) and `pc` reads `0x00000000`. This also occurs with the bridge on, so the auto mode does not cause it.
4. A bad SWD connection to the STM32H523 gave DPIDR `0xfffe053f`, and later no answer. Then the SWD step finds no target, and the WCH step sends its frames to the STM32H523. This agrees with the [auto mode](#auto-mode) rules.

## Tests

`tests/TestCmsisDap.cpp` runs the processor on the real SWD bit-bang engine, with the mock SWD target of `tests/MockSwdPins.h`.
The tests cover each command, WAIT retries, FAULT, parity errors, protocol errors, value match and the timeout of value match, and posted reads.

`tests/TestCmsisDapJtag.cpp` runs the processor on the JTAG bit-bang engine and on the JTAG fast path, with the mock JTAG pins of `tests/MockJtagPins.h`.
The target is the JTAG chain of `tests/MockJtagDp.h`: a TAP state machine, and devices with IDCODE and BYPASS.
One device of the chain is a JTAG-DP with a memory AP.
The tests cover the JTAG commands, IDCODE, DPACC and APACC reads and writes, posted reads, WAIT, value match, block transfers, ABORT and chains with more than one device.
Other tests send `DAP_TransferAbort` and other packets during a transfer command, through the processor and through the USB function.
One test sends the abort during the WAIT retries of one SWD transfer, from the serial service of the engine.

`tests/TestRvDtmTap.cpp` runs the emulated TAP of the RISC-V bridge (`src/protocol/cmsisdap/RvDtmTap.h`).
The TAP is a JTAG port without pins: it follows TMS and TDI and gives TDO.
It has a 5-bit IR, IDCODE, DTMCS (`version` 1, `abits` 7), DMI and BYPASS.
Update-DR of the DMI register does the access on the wire, with the wire engine of the `wch-link` personality (`wchlink::Target`).
The next Capture-DR gives the result: `op` 0 (success) or 2 (failed).
A failed access sets a sticky error. `dmireset`, `dmihardreset` and Test-Logic-Reset clear it.
The tests check the TAP state machine and each register.
They run the DMI accesses on the wire engine with mock pins and the mock DM, on one wire and on two wires: read, write, failure and reset.
They also send the requests of the OpenOCD and probe-rs captures (`tests/replay/dap-*.trace`) through the processor, with the TAP as its JTAG port.
The scan chain rules of OpenOCD (`jtag_examine_chain()`, `jtag_validate_ircapture()`) and the DR length of probe-rs must pass on the replies.
Other tests check the connect: the wire, the unlock writes, no halt, a connect without a target, and the connect again after each event.

`tests/TestRvBridge.cpp` runs the bridge switch (`src/protocol/cmsisdap/RvBridge.h`) on a fake board port and the TAP.
The tests check the default port, the switch with and without a connection, the TCK of the board port and the pin release.
They also run `ID_DAP_Vendor1` through the processor, the store flag, the stored mode at `hostGone()`, and `DAP_SWJ_Pins` on SWCLK and SWDIO.

`tests/TestDapUart.cpp` runs `uart::DapUartChannel` with the real CDC-ACM function, the UART bridge and a fake UART.
The tests cover the owner switch, the frame formats, the receiver and transmitter control, the status bits and the buffer limits.
They also run the `DAP_UART_*` commands and `DAP_Info` through the processor.

`tests/TestDapSwo.cpp` runs the `DAP_SWO_*` commands and `DAP_Info` through the processor, on a fake SWO receiver with the baud rates of the CH32V307 USART2.
The tests cover the transport, the mode, the baud rate, the start and the stop, the status bits and their clear, the data limits of the packet sizes, `DAP_ExecuteCommands`, and the host that goes away.
`tests/TestSwoRing.cpp` checks the trace buffer and the baud rate calculation of the receiver.
The last tests of `tests/TestDapSwo.cpp` run the SWO trace endpoint through `CmsisDapFunction` on the fake USB controller, at full speed and high speed: the packets, transport `1` against `2`, the halt clear and the bus reset.
`tests/TestCmsisDapDescriptors.cpp` checks the SWO trace endpoint in the configuration descriptor.

`tests/TestDapTargetPower.cpp` runs `ID_DAP_Vendor2` through the processor, on the fake power switch and voltage sense of `tests/FakePower.h`.
The tests cover off, on and read, each protection, a board without a switch or a sense, an unknown op, the host that goes away, and a response that does not fit.

`tests/TestDapVppSetting.cpp` runs `ID_DAP_Vendor3` through the processor, on a fake store.

`tests/TestDapIdReport.cpp` runs `ID_DAP_Vendor4` through the processor, on a fake ID source.

`tests/TestDapUartWire.cpp` runs `ID_DAP_Vendor6` through the processor, on a fake UART.
`tests/TestSingleWireLine.cpp` checks the order of the pin changes in single-wire mode (`src/uart/SingleWireLine.h`).

`tests/TestEdbg.cpp` runs the AVR commands through the HID function, on the ISP engine with the mock AVR of `tests/MockIspTarget.h`.
The tests run the avrdude session, a flash page in 5 command fragments and a read in 5 response fragments.
They check each rejected fragment, the bad frame headers, the response that the host did not read, and each scope.
They also check the target pins between the two interfaces, the end of the programming mode when the host goes away, and the target power rule.

`tests/TestEdbgDebugWire.cpp` runs the AVR8 scope through the HID function, on the dW engine with the mock ATmega328P of `tests/MockAvrDw.h`.
The tests run the avrdude session (signature, flash, EEPROM) and a Bloom debug session (registers, SRAM, PC, run, stop, step, reset, hardware and software breakpoints, break events).
They also check the device context, the pins between ISP and debugWIRE, a missing target, and a host that goes away.
`tests/TestEdbgJtag.cpp` runs the avrdude and Bloom JTAG sessions on the mock ATmega32U4 of `tests/MockAvrJtag.h`: all memories, the parameters, the checks of the host fields, the pins and a host that goes away.
It also sets data breakpoints in each mode, as AVaRICE sends them.

## Hardware test

This section has the SWD tests. The JTAG tests follow in [JTAG hardware test](#jtag-hardware-test).

Date: 2026-10-10.
Firmware: `dev` at `5f2135f` with `DAP_TransferAbort`. The build is `v0.2.0-63-g10d84fa-dirty`. The `-dirty` suffix is from the merge of `dev`, which was not committed at build time. The `src/` tree equals commit `75d93cd`. CMSIS-DAP v2 firmware version 2.1.0, `cmsis-dap` personality.
The same checks first ran on `v0.2.0-57-g59614cb` (before `DAP_TransferAbort`). All checks passed there too. Only the read speed differs (see below).
Probe: Mini board.
Target: STM32F103ZET6 (512 KiB flash) on SWD.
Host: Linux 6.18.55.
Tools: OpenOCD 0.12.0+dev-00801-g133dd9d66, pyOCD 0.45.1 (Keil.STM32F1xx_DFP 2.4.1), probe-rs 0.32.0.

| Check | OpenOCD | pyOCD | probe-rs |
|---|---|---|---|
| Connect, DPIDR `0x1ba01477` | pass | pass | connect passes, DPIDR value not shown (see findings) |
| Flash and verify (32 KiB) | pass | pass | pass |
| Read back (`cmp`) | pass | pass | not run |
| Halt, step, reset | pass | pass | pass (halt and step through GDB) |
| Memory write and read | pass | not run | read only (`probe-rs read`), run before `DAP_TransferAbort` only |
| UART bridge during flash (256 KiB, image verified after) | pass | pass | pass |

Enumeration: the product string is `Versaloon CMSIS-DAP`.
Interface 0 is a vendor class interface with the string `Versaloon CMSIS-DAP v2`.
The `cdc_acm` driver binds interfaces 1 and 2 and gives `/dev/ttyACM0`.
The `ftdi_sio` driver does not bind to the device.
OpenOCD opens the probe with VID:PID 0x0403:0x9b3c (the ID before the pid.codes ID).

Flash speed (64 KiB, median of 3, SWD, target at 64 MHz, firmware with `DAP_TransferAbort`):

| SWCLK request | Write | Verify | Read | Read before `DAP_TransferAbort` |
|---|---|---|---|---|
| 250 kHz | 4.51 s | 0.16 s | 3.41 s | 3.40 s |
| 1000 kHz | 2.59 s | 0.13 s | 1.15 s | 0.95 s |
| 2000 kHz | 2.55 s | 0.13 s | 0.70 s | 0.56 s |
| 4000 kHz | 2.55 s | 0.13 s | 0.73 s | 0.56 s |

All runs passed (0 failures in 3 runs for each rate).
Only the run before `DAP_TransferAbort` includes 100 kHz and 500 kHz (write 9.75 s and 2.78 s, read 8.27 s and 1.77 s).

The Versaloon personality (Mini board, same firmware version before `DAP_TransferAbort`, `vsllink` driver) gave these times:

| Clock | Write | Read |
|---|---|---|
| 235 kHz (`vsllink` step) | 5.20 s | 4.26 s |
| 1.85 MHz (untimed) | 2.54 s | 0.81 s |

The `vsllink` driver has fixed SWD steps. Thus the comparison is not exact at 250 kHz, where the clocks are 250 kHz and 235 kHz.
At 2000 kHz, both personalities ran the untimed engine. The firmware of this test ran untimed from 1.85 MHz. Now CMSIS-DAP runs untimed only from 3 MHz.
The write time is the same (2.55 s and 2.54 s).
The read time with `DAP_TransferAbort` is 0.70 s for CMSIS-DAP and 0.81 s for the Versaloon personality (91 and 79 KiB/s).
Before `DAP_TransferAbort`, the CMSIS-DAP read time was 0.56 s (114 KiB/s).

Maximum SWD clock: the untimed engine. In this test, it ran from 1.85 MHz. Now it runs from 3 MHz (`maxClockHz` in `src/swd/SwdBitBang.h`).
Its real rate depends on the code (2.42 to 2.7 MHz on the Mini, see [Clock pacing](clock-pacing.md)).
All runs passed up to this clock with the wiring of this test.
A request above 3 MHz gives the same clock. A request from 1.85 MHz to 3 MHz now gives the timed engine at 1.3 to 1.56 MHz.
This test used the Mini. On the STM32H5, all requests are timed: a request above 1.85 MHz gives a faster clock, up to 5.2 to 6.1 MHz (see [Clock pacing](clock-pacing.md#swd-pacer-on-the-stm32h5)).

Findings:

- Write time does not improve above 1 MHz. The flash programming time is probably the limit. This test did not measure it.
- The times at 2000 kHz and 4000 kHz are the same within the noise. This is expected: the engine ran untimed from 1.85 MHz, so both requests gave the same clock.
- `DAP_TransferAbort` made the read time about 20% to 25% longer at 1000 kHz and 2000 kHz (0.56 s to 0.70 s at 2000 kHz). The 4000 kHz run shows 30%, but it is noisy. Write time and the 250 kHz read time did not change. The cause is not measured.
- After a replug, the SWD connect failed (`cannot read IDR`, and the pin line showed `TDI = 0`). It worked again when the target board was reconnected (the pin line showed `TDI = 1`). The cause is probably the wiring. No log proves this, and the exact wire is not known.
- `probe-rs info` does not print the DPIDR value. Without `--verbose`, it prints "Connecting to the chip was unsuccessful", although `download`, `verify`, `gdb` and `reset` connect. With `--verbose` (run before `DAP_TransferAbort`), it shows the DPv1 MemoryAP and the ROM table, and stops with "does not support SWD multidrop". This is normal for an STM32F1 target.
- `probe-rs verify` prints "Verification failed: contents do not match" for a wrong image, but it exits with code 0. Check the text, not the exit code. This is how probe-rs behaves. It is not a probe finding.
- In the probe-rs GDB server, the first `pc` after `monitor reset halt` shows a stale value. The next `stepi` shows the correct PC.
- The UART loopback test with the default run (PA9 to PA10) gives 100 passed, 0 failed and 50 skipped. Linux cannot make 1.5 stop bits, so all skips are for that setting. The test during a 256 KiB flash passes with all three tools (60 of 60 runs at 115200 baud, 8N1).

## JTAG hardware test

Date: 2026-10-11.
Firmware: `dev` at `6fc6d8b` with JTAG support. The build is `v0.2.0-101-g6fc6d8b`. The tree was clean. CMSIS-DAP v2 firmware version 2.1.0, `cmsis-dap` personality, `Release` build.
Probe: Mini board. The image ran behind `openmupp-bootloader`.
Target: STM32F103ZET6 (512 KiB flash) on JTAG. The wiring is the one of the SWD test, with TDI, TDO and TMS added.
Host: Linux 6.18.55.
Tools: OpenOCD 0.12.0+dev-00801-g133dd9d66, pyOCD 0.45.1.

The chain has two TAPs: the Cortex-M3 DAP (IR length 4, IDCODE `0x3ba00477`) and the boundary-scan TAP (IR length 5, IDCODE `0x06414041`).
The DAP is next to TDO, so it is device 0.

| Check | OpenOCD | pyOCD |
|---|---|---|
| Find the TAPs, read IDCODE | pass (scan chain finds both TAPs) | pass (IDCODE of device 0; the script sets the chain) |
| Flash and verify at 1000, 4500 and 9000 kHz | pass (32 KiB random image) | pass (32 KiB image, see below) |
| Read back at 1000, 4500 and 9000 kHz | pass (`cmp`) | pass (compared in Python) |
| Reset and halt, `pc` and `sp` from the vector table | pass (2 KiB loop image) | pass |
| Single step | pass (2 KiB loop image) | pass |
| Hardware breakpoint, resume and halt | pass (2 KiB loop image) | pass |
| RAM write and read (words, 8 KiB block) | pass (words only) | pass |

The OpenOCD flash checks use a random 32 KiB image.
The OpenOCD debug checks use a 2 KiB image that has a vector table and the loop `adds r1, #1` / `b`.
The pyOCD script builds one 32 KiB image for all checks: the vector table, the same loop and random data.
The pyOCD checks come from `tools/hwtest/pyocd_jtag_test.py`.

All runs passed (0 failures).
OpenOCD uses `DAP_JTAG_Sequence` for all JTAG scans.
pyOCD uses `DAP_Transfer` and `DAP_TransferBlock`.
Thus the pyOCD run is the test of the JTAG-DP transfers of this firmware.

The target runs at 8 MHz after a reset, and TCK must not be above F_CPU / 6.
Thus each run above 1000 kHz sets the target clock to 64 MHz first (`tools/hwtest/stm32f1_clock.tcl` for OpenOCD, the same register writes in the pyOCD script).
The target clock goes back to 8 MHz at each reset.
The two requests above 1125 kHz use the SPI and DMA fast path of the firmware.
This test did not measure the real TCK.

JTAG flash time of 32 KiB (target at 64 MHz, one run per rate):

| TCK request | OpenOCD write | OpenOCD verify | OpenOCD read | pyOCD write | pyOCD read |
|---|---|---|---|---|---|
| 1000 kHz | 8.66 s | 0.23 s | 7.66 s | 2.36 s | 1.37 s |
| 4500 kHz | 8.07 s | 0.22 s | 7.11 s | 1.03 s | 0.94 s |
| 9000 kHz | 8.16 s | 0.21 s | 7.14 s | 0.98 s | 0.90 s |

The OpenOCD verify time is much shorter than the read time. Thus OpenOCD probably checks a CRC on the target and does not read the flash data over the probe. This test did not confirm it.
The pyOCD write time at 1000 kHz was 1.67 s in an earlier run of the same check. Thus the write times vary by about 40%.

`DAP_JTAG_Configure` and `DAP_JTAG_IDCODE` checks (raw commands in `tools/hwtest/pyocd_jtag_test.py`):

| Check | Result |
|---|---|
| IR lengths 4 and 5, IDCODE of device 0 | `0x3ba00477` |
| IDCODE of device 2 (not in the chain) | error reply |
| 9 devices, IR length 0, IR length 33 | error reply |

Findings:

- OpenOCD in JTAG mode is slow. The write rate is about 4 KiB/s and the read rate about 4.5 KiB/s. The rate does not depend on TCK from 1000 kHz to 9000 kHz. pyOCD is 4 to 8 times faster with the same probe. SWD with OpenOCD (see [Hardware test](#hardware-test)) is faster too. The cause is not measured. OpenOCD sends one `DAP_JTAG_Sequence` per scan, so the USB round trips probably limit the rate. This test did not check whether the firmware or the host causes it.
- pyOCD 0.45.1 does not work with the STM32F1 on JTAG without a change. It sends `DAP_JTAG_Configure` with one device of IR length 4, and the chain has two TAPs. The first transfer fails with ACK 0. With the chain set to the IR lengths 4 and 5, all transfers pass. The test script `tools/hwtest/pyocd_jtag_test.py` sets the chain to the IR lengths 4 and 5 in a patch of the pyOCD protocol class.
- The pyOCD target `stm32f103ze` (from the Keil DFP) fails on JTAG before the first transfer with `'bytes' object cannot be interpreted as an integer`. The error is in the debug sequence code of pyOCD (`hex()` of a `bytes` result in a trace call). It is not a probe problem. The test uses the built-in target `stm32f103rc`.
- `DAP_JTAG_IDCODE` for device 1 (the boundary-scan TAP) returns 0. The firmware loads the IR value `0x0E`, as the reference implementation of Arm does. This is the IDCODE instruction of the Cortex-M3 DAP. The boundary-scan TAP of the STM32F1 probably does not decode it as IDCODE. OpenOCD reads `0x06414041` from this TAP with its own scan, so the TAP does answer. No debug host uses `DAP_JTAG_IDCODE` of this TAP.
- The probe re-enumerated once on the USB bus, about 2 minutes after the firmware started (device number 7 to 8). The first OpenOCD tries failed with "No such device" in that time. It then stayed on device number 8 for the rest of the test. The cause is not known. A second run did not repeat it.
- The probe-rs JTAG test is not done. The test covers OpenOCD and pyOCD only.

## UART hardware test

`tools/hwtest/dap_uart_loopback_test.py` tests the `DAP_UART_*` commands on the dongle.
pyOCD, OpenOCD and probe-rs do not use these commands, so the script sends them directly on interface 0.
It needs the pyusb Python package and Python 3.

1. Write a `cmsis-dap` image or a multi image with `cmsis-dap`, and select the CMSIS-DAP personality.
2. Connect the UART TX pin to the UART RX pin (PA9 to PA10 on the Mini).
3. Run the script:

```sh
python3 tools/hwtest/dap_uart_loopback_test.py
python3 tools/hwtest/dap_uart_loopback_test.py --serial-port /dev/ttyACM0
```

The script gets the UART with `DAP_UART_Transport` `2`.
For each baud rate and frame format, it sends a byte pattern with `DAP_UART_Transfer` and compares the echo.
A test fails when a byte is wrong or missing, or when the status shows lost data, a framing error or a parity error.
At most half of a ring buffer is on the way, so the receive buffer does not overflow.
At the end, the script gives the UART back to the serial port.
With `--serial-port`, the script also checks that the serial port gets no data during the tests.

The default run tests the baud rates from 9600 to 2000000 baud, and the formats 8N1, 8E1, 8O1, 7E1, 7O1, 8N2 and 8N1.5.
Useful options: `--bauds`, `--formats`, `--size`, `--repeat` and `--serial`.

The unit tests of the script need no pyusb and no hardware:

```sh
python3 -m unittest discover -s tools -p "test_*.py"
```

Status: the host tests pass. The hardware test passed on 2026-10-15.
Probe: Mini board with the 10k bootloader, `app-10k` image of the `mini-cmsis-dap` preset, build `v0.9.0-42-g52e32da`.
The UART TX pin was connected to the UART RX pin (PA9 to PA10).
The run used `--serial-port /dev/ttyACM0`.
All 70 cases passed: the baud rates from 57600 to 2000000 baud, each with the seven frame formats.
The serial port got no data while the DAP commands owned the UART.
A 256 KiB flash with OpenOCD (SWD, 2000 kHz) and the serial loopback test on the COM port ran at the same time.
Both passed.
The SWD and JTAG checks with OpenOCD, pyOCD and probe-rs still passed on this firmware.
The `DAP_UART_*` commands and a flash on the same DAP interface did not run at the same time.
