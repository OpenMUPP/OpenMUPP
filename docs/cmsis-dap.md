# CMSIS-DAP command processor

This document describes the CMSIS-DAP v2.1 command processor for SWD and JTAG.
The code is in `src/protocol/cmsisdap/`.
`usb::CmsisDapFunction` (`src/usb/CmsisDapFunction.h`) moves the packets over USB.
See the [CMSIS-DAP personality](usb-device.md#cmsis-dap-personality).
The build option `VSL_PERSONALITIES` with `cmsis-dap` puts the function and the processor into the firmware image (see [Build](#build)).

## Build

Use the preset of the board with the `-cmsis-dap` suffix, for example `mini-cmsis-dap`:

```sh
cmake --preset mini-cmsis-dap
cmake --build --preset mini-cmsis-dap -j
```

Or set the CMake option `VSL_PERSONALITIES` to `cmsis-dap`:

```sh
cmake -B build/mini-dap -DCMAKE_TOOLCHAIN_FILE=cmake/gcc_arm.cmake -DBOARD=mini -DVSL_PERSONALITIES=cmsis-dap
cmake --build build/mini-dap -j
```

The default value is `versaloon`.
The default image does not change.
The build then writes `versaloon-ng-<board>-cmsis-dap-<variant>-<type>-<version>.hex` and `.bin`.
Write the image as in [Firmware update](firmware-update.md).
All boards build with this value.

The `cmsis-dap` image has these properties:

- USB interface 0 is CMSIS-DAP v2, with `0403:9B3C`. The Versaloon interface is not in the image.
- The CDC-ACM serial port, the UART bridge, the debug log and the LEDs are as in the Versaloon image.
- The image does not link the Versaloon command buffers, the `USB_TO_XXX` blocks and the power block.
- The SWD engine, the JTAG engine and the debug connector pins (`DAP_SWJ_Pins`) are the same ones as in the Versaloon image.
  On the Mini, the JTAG engine uses the SPI and DMA fast path at a TCK of 1125 kHz or more. On the STM32H5 boards, it uses the fast path at a TCK of 3906 kHz or more.

A `cmsis-dap` image has only this personality.
A multi image has `cmsis-dap` and other personalities, for example `VSL_PERSONALITIES=versaloon,cmsis-dap`.
Select the active one with `ID_DAP_Vendor0`, with `SWITCH_PERSONALITY` of the Versaloon protocol, with the switch request on EP0, or with the key (see [Multi image](usb-device.md#multi-image)).
The switch request on EP0 and the request of the MS OS 2.0 descriptor set use the same handler (see [Switch request on EP0](usb-device.md#switch-request-on-ep0)).

## Limits

- JTAG has no adaptive clocking (RTCK). TCK is in whole kHz, at least 1 kHz. See [JTAG](#jtag).
- A JTAG chain has up to 8 devices. The IR length of a device is 1 to 32 bits.
- On JTAG, `DAP_SWJ_Pins` must not leave TCK at a level that is not the idle level of the JTAG engine.
  Else the next scan has an extra TCK edge or a missing TCK edge.
  The idle level is high on the boards with the SPI fast path (the Mini and the STM32H5 boards), and low on the ST-Link/V2.
- pyOCD 0.45.1 configures a JTAG chain with one device of IR length 4. A target with more than one TAP, for example the STM32F1, needs another chain setting (see [JTAG hardware test](#jtag-hardware-test)).
- No SWO.
- The `DAP_UART_*` commands use the UART of the UART bridge. Only one side has the UART at a time: the CDC-ACM serial port or the DAP commands. See [UART commands](#uart-commands).
- A `VSL_DEBUG_CDC` build has no UART bridge and no `DAP_UART_*` commands.
- On JTAG, `DAP_TransferAbort` stops a transfer command only between two transfers. It does not stop the WAIT retries of one JTAG transfer. On SWD, it also stops the WAIT retries. It does not stop other commands, for example `DAP_Delay`.
- `DAP_TransferAbort` reaches the processor during a command only when a slot is free. When the running command and 3 other packets use the 4 slots, the OUT endpoint NAKs, and the abort waits until the command ends.
- 64-byte packets, and up to 4 packets in flight.
- No target power switch and no Vref sense in this personality.
- The LEDs show the target power, the command activity, the probe errors and the USB state. `DAP_HostStatus` does not change them. See [boards.md](boards.md#start-up-and-leds).
- The hardware test passed on Linux with OpenOCD, pyOCD and probe-rs on SWD, and with OpenOCD and pyOCD on JTAG. probe-rs passed on JTAG at 1000 kHz in the multi image test (see [usb-device.md](usb-device.md#multi-image-hardware-test)). Windows (also the driverless WinUSB binding), the LED pattern and the udev rule are not tested (see [Hardware test](#hardware-test) and [JTAG hardware test](#jtag-hardware-test)).

## Use the probe

The probe has the USB ID `0403:9B3C` and the product name `Versaloon CMSIS-DAP`.
The host tools find the CMSIS-DAP v2 interface by the text `CMSIS-DAP` in its interface string.

On Linux, a udev rule gives a normal user access to the probe.
For example, put this line into `/etc/udev/rules.d/99-versaloon-cmsis-dap.rules`:

```text
SUBSYSTEM=="usb", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="9b3c", MODE="0660", TAG+="uaccess"
```

Then reload the rules with `udevadm control --reload` and plug in the probe again.

On Windows 10 or later, the probe needs no driver installation and no Zadig.
The device has MS OS 2.0 descriptors (see [MS OS 2.0 descriptors](usb-device.md#ms-os-20-descriptors)).
Windows binds WinUSB to the CMSIS-DAP interface and its own CDC driver (`usbser.sys`) to the serial port.
Windows 8.1 binds WinUSB to the CMSIS-DAP interface too, but the serial port needs a driver setup file there.

If you installed a driver for the probe before, for example with Zadig, Windows keeps that driver.
Uninstall the device in the Device Manager, and also remove its driver.
Windows 10 calls this option "Delete the driver software for this device".
Windows 11 calls it "Attempt to remove the driver for this device".
Then plug in the probe again.

OpenOCD:

```sh
openocd -f interface/cmsis-dap.cfg -c "cmsis-dap vid_pid 0x0403 0x9b3c" -f target/stm32f1x.cfg
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

The processor has no CMSIS header and no hardware header.
It uses these objects:

- A `DapSwd` for the SWD transfers and sequences.
- A `protocol::JtagPort` (`src/protocol/JtagPort.h`, on target `board::BoardJtag`) for JTAG. It is optional.
- A `protocol::GpioPins` (`src/protocol/GpioPins.h`, on target `board::BoardGpio`) for `DAP_SWJ_Pins` and `DAP_ResetTarget`.
- A `Platform` for the microsecond time, the delay and the LEDs.
- A `DeviceInfo` with the strings and the capabilities of `DAP_Info`.
- A `DapUart` (`DapUart.h`, on target `uart::DapUartChannel`) for the `DAP_UART_*` commands. It is optional.

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
Before E6, the commands ran inside the event delivery, with the same effect.
Thus the OUT endpoint stays armed while a command runs, if a slot is free.
During a USB suspend, `CmsisDapFunction::poll()` runs no packet. The stored packets run after the resume.
A packet that arrives during a command waits until that command ends.
The USB function sends the response packets in order.
The packet size is 64 bytes, and up to 4 packets are in flight.
Call `canSubmit()` before `submit()`.
Each packet without a response takes one of the 4 slots.

## Commands

| ID | Command | Note |
|---|---|---|
| `0x00` | `DAP_Info` | Vendor, product, serial number, protocol version `2.1.0`, firmware version, capabilities, packet size and count. Other information has length 0. |
| `0x01` | `DAP_HostStatus` | Connect and running LED. |
| `0x02` | `DAP_Connect` | Port 0 and 1 give SWD. Port 2 gives JTAG. |
| `0x03` | `DAP_Disconnect` | Release the pins of the port. Release the pins that `DAP_SWJ_Pins` drove: nRESET and nTRST get the pull-up. |
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
| `0x1D` | `DAP_SWD_Sequence` | Replies an error when SWD is not connected. |
| `0x1F` | `DAP_UART_Transport` | `0` none, `1` USB COM port (the default), `2` DAP commands. See [UART commands](#uart-commands). |
| `0x20` | `DAP_UART_Configure` | Data bits, parity, stop bits and baud rate. The response has the error bits and the real baud rate. |
| `0x21` | `DAP_UART_Transfer` | Sends and receives UART data. Up to 59 bytes to send and 58 bytes to receive in one packet. |
| `0x22` | `DAP_UART_Control` | Enables, disables and flushes the receiver and the transmitter. |
| `0x23` | `DAP_UART_Status` | The status bits, the bytes in the receive buffer and the bytes not sent yet. |
| `0x80` | `ID_DAP_Vendor0` | Switches the USB personality of a multi image: `[0x80][personality u8]`. The personality is the code of a personality in the image: `0` (Versaloon), `1` (CMSIS-DAP) or `2` (WCH-Link). The response is `[0x80][status u8]`: `0` when the firmware accepts the switch, `0xFF` for a code that is not in the image. The firmware then stores the personality and resets. The bootloader code `0xB0` starts the bootloader, also in an image with one personality (see [Bootloader code](firmware-update.md#bootloader-code)). A multi image without the bootloader entry replies `0xFF` to it. An image with one personality and without the bootloader entry replies `ID_DAP_Invalid` to each code. Only the `app-8k` image and the images of the STM32H5 have the bootloader entry. See [Multi image](usb-device.md#multi-image). |
| `0x81` | `ID_DAP_Vendor1` | Only in an image with `VSL_CMSIS_DAP_RV_BRIDGE`: sets the mode of the RISC-V bridge, `[0x81][mode u8]`, `0` off, `1` on, `2` auto, `3` the stored mode. Bit 7 of the mode also stores the mode. The response is `[0x81][status u8]`: `0` when the mode is set, `0xFF` for another value or a failed store. An image without the bridge replies `ID_DAP_Invalid`. See [Bridge switch](#bridge-switch). |
| `0x7E` | `DAP_QueueCommands` | The packet waits. The next packet with another command runs the queue. A queued packet in the last free slot also runs the queue. |
| `0x7F` | `DAP_ExecuteCommands` | The commands do not nest. |

An unknown command replies `ID_DAP_Invalid` (`0xFF`).
The status of a command is `DAP_OK` (`0x00`) or `DAP_ERROR` (`0xFF`), as in the specification.
Each "replies an error" in the table means the status `0xFF`.
`DAP_Connect` is the exception: it replies port `0` for a failure.
The capabilities in `DAP_Info` show SWD, JTAG, atomic commands, the UART port and the USB COM port.
`DAP_Info` also gives the size of the UART receive buffer (`0xFB`) and transmit buffer (`0xFC`).
A processor without a `DapUart` does not show the UART, and the `DAP_UART_*` commands then reply `ID_DAP_Invalid`.
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

`tools/dap_uart_loopback_test.py` tests the commands with a loopback wire from TX to RX (see [UART hardware test](#uart-hardware-test)).

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

`DAP_Connect` with port 2 releases the SWD pins and starts the JTAG engine.
The JTAG engine is the one of the Versaloon image (`board::BoardJtag`).
On the boards with the fast path, it shifts whole bytes with SPI and DMA when the SPI rate is not above the request and is at least the bit-bang rate (see `src/jtag/JtagFastPath.h`).
This starts at a TCK of 1125 kHz on the Mini, and at 3906 kHz on the STM32H5 boards. The other bits, and the slower rates, use GPIO bit-bang.
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

Status: in progress (#103). Each image with the `cmsis-dap` personality has the bridge, unless the build sets `VSL_CMSIS_DAP_RV_BRIDGE=OFF`. The [OpenOCD configs](#openocd-configs) are in `etc/openocd/`. The hardware test (#109) is done: see [Bridge hardware test](#bridge-hardware-test).
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

The CMake option `VSL_CMSIS_DAP_RV_BRIDGE` (default `ON`) adds the bridge to the `cmsis-dap` personality.
Each image with `cmsis-dap` has the bridge, also a multi image, for example `versaloon,cmsis-dap,wch-link`.
In a multi image, only the active personality sets up the pins and the timer of the wire engine. A personality switch resets the MCU.
An image without `cmsis-dap` ignores the option.
An image with the bridge has the settings page, also an image with one personality: 1 KB less flash on the STM32F103, 8 KB less on the STM32H5 (see [Stored mode](#stored-mode)).
The STM32H5 boards also build the bridge. The bridge is not tested on them.
The bridge is the default, so it adds no image tag. An image with `cmsis-dap` and `VSL_CMSIS_DAP_RV_BRIDGE=OFF` has the tag `-no-rv-bridge`, for example `versaloon-ng-mini-cmsis-dap-no-rv-bridge-app-10k-Release-<version>.bin`.
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

Each config sources `ch32v-cmsis-dap-bridge.cfg`. This file selects the adapter (`0403:9b3c`, JTAG) and creates the TAP and the `riscv` target.
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
Ctrl-C and SIGTERM also run `shutdown`.
An error in a `-c` command after `init`, SIGKILL or a crash skips `pre_shutdown_commands`. Then the bridge stays on, and the next plain JTAG session finds the IDCODE `0x15653101`.
Set the stored mode with `python3 tools/vsl_rv_bridge.py stored`, or replug the probe.
The CH32V00X config sets `riscv set_mem_access abstract`: the CH32V006 refuses the program buffer (`cmderr` 2).
The configs use `-chain-position` and `set_mem_access`, because OpenOCD 0.12.0 does not know `-tap` and `mem_access`. Newer OpenOCD versions show a deprecation warning for them.
The configs have no flash bank. Mainline OpenOCD has no CH32V flash driver yet (#110).

Result on 2026-10-24 (OpenOCD `0.12.0+dev-01707-gf7037392b`, Mini with the SPI1 remap fix of !86, CH32V006 on one wire): OpenOCD examines the hart (`XLEN=32`, `misa=0x40801014`). Halt, a RAM write and read, and a step pass.
The CH32V006 has no triggers, so OpenOCD cannot set a hardware breakpoint.
The [bridge hardware test](#bridge-hardware-test) has the results of the three configs.

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
| Auto | At each `DAP_Connect` to JTAG, the probe finds the target (see [Auto mode](#auto-mode)). |

- The mode at power-up and after a USB reset is the stored mode. Auto is the stored mode until a store.
- In the TAP, `DAP_Connect` to JTAG, `DAP_JTAG_Sequence`, `DAP_SWJ_Sequence` and the JTAG transfers use the emulated TAP.
  SWD (`DAP_Connect` to SWD) does not change in any mode.
- A mode change while JTAG is connected stops the old port and starts the new one at once.
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
probe-rs cannot send a vendor command (#103, decision 7). In the auto mode, probe-rs needs no command:

```sh
probe-rs info --protocol jtag
```

To force the TAP, send the command with `tools/vsl_rv_bridge.py` before probe-rs starts:

```sh
python3 tools/vsl_rv_bridge.py on
probe-rs info --protocol jtag
python3 tools/vsl_rv_bridge.py stored
```

The mode stays until a USB reset, a power cycle or the next `ID_DAP_Vendor1`.
The OpenOCD configs send `81 02` and `81 03` at `shutdown` and after Ctrl-C, but not after an error in a `-c` command.
Thus probe-rs must not reset the USB device when it opens it. probe-rs 0.32.0 does not reset it ([Bridge hardware test](#bridge-hardware-test)).

### Stored mode

The stored mode is the mode at power-up and after a USB reset.
The firmware stores it in the settings page, in the same record as the stored personality (see [Multi image](usb-device.md#multi-image)).
To store a mode, set bit 7 of the mode in `ID_DAP_Vendor1`, or use `--store`:

```sh
python3 tools/vsl_rv_bridge.py off --store
python3 tools/vsl_rv_bridge.py auto --store
```

- A command without bit 7 does not write the flash. Thus the OpenOCD configs, which send `81 01` and `81 03` in each session, do not wear the flash.
- A store of the stored mode does not write the flash.
- A personality switch keeps the stored mode, and a store of the mode keeps the stored personality.
- A page without a mode gives auto. This is true for a page of an older multi image: its records are the records of the auto mode.
- On the STM32F103, a write stalls the CPU for about 50 µs. Each 512 stores, the store also erases the page, and the CPU stalls up to 40 ms. The interrupts wait, so the host sees a late response, and the UART bridge can lose received bytes. Do not store the mode during a target session.
- On the STM32H5, the page is in bank 2, so code in bank 1 does not stall during the write.
- A firmware update keeps the page, as for the stored personality (see [Stored personality](firmware-update.md#stored-personality)). A mass erase erases it.
- Limit (STM32F103): the first flash of an image with one personality finds old data in the page, for example code of an older, larger image. A word of that data can be a record by chance (12 of 65536 values). The image then starts with that mode until the next store. On the STM32H5, a record also needs the other 112 bits of its flash word set, so this does not occur in practice.
- The page keeps the personality of another image. Thus a store of the mode in an image with one personality keeps the stored personality of a multi image.

### Auto mode

In the auto mode, `DAP_Connect` to JTAG finds the target before it starts the port.
The steps are in this order. The first step that finds a target stops the search:

1. JTAG: the probe goes to Test-Logic-Reset and shifts 64 bits through Shift-DR with TDI high.
   A TDO that changes is a JTAG chain: the port is the JTAG port of the board.
   A TDO that stays high (the pull-up) or low is no chain.
2. SWD: the probe sends a line reset, the JTAG-to-SWD sequence and a line reset, then reads DPIDR.
   An ACK OK is an SWD target: the port is the JTAG port of the board.
   The probe then sends the SWD-to-JTAG sequence, so an SWJ-DP is in JTAG mode again.
3. WCH: the probe does the [connect](#connect) on one wire, then on two wires.
   A target that answers selects the emulated TAP.
4. No target: the port is the JTAG port of the board.

Each step releases its pins before the next step.
The result stays until the next `DAP_Connect`, `DAP_Disconnect`, mode change or USB reset.
Thus connect the target, and switch on its supply, before the host connects.

- The detection uses the TCK of `DAP_SWJ_Clock`. The SWD step uses the SWD clock and the settings of the last `DAP_SWJ_Clock`, `DAP_SWD_Configure` and `DAP_TransferConfigure`.
- The SWD step only runs when the JTAG step finds no chain.
  The JTAG host can then not use an SWD target, so the switch to SWD and back does no harm.
  The SWD step keeps WCH frames away from an SWJ-DP or SW-DP that answers the JTAG-to-SWD sequence.
  A dormant or multidrop SW-DP (for example the RP2040) does not answer it, and gets the WCH frames.
- After the WCH step, the first DMI access does the connect again, as after `DAP_Connect` in the on mode.
  The captures show that an attach at `DAP_Connect` passes (image B in [Connect](cmsis-dap-captures/README.md#connect)).
- The JTAG and SWD steps toggle SWCLK and SWDIO before the WCH step. The hardware test of the auto mode is not done yet.

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

Planned changes (#103):

- The firmware retries a wire access 2 or 3 times before `op` 2, but only when the target did not run the access. Examples: a two-wire frame without the handshake of the target at bit 11, or a one-wire frame that did not start.
- A read with a parity error, or a one-wire line that stays low, gives `op` 2 at once. A second access can have side effects, for example on `command`, on `data0` with `abstractauto`, or on `sbdata0`.
- A patch for mainline OpenOCD makes `dmi_read()` and `dmi_write()` check `op` 2.

The bridge does not report a wire failure as `op` 3.
OpenOCD would then repeat accesses that the target already ran, and a permanent failure would stay hidden until a timeout.

### Bridge hardware test

The hardware test (#109) has a step for each target: the CH32V006, the CH32V003 and the CH32V208, on the Mini.
A last step checks the real JTAG port with an STM32F103 after the bridge was on.

#### CH32V006 on the Mini

| Item | Value |
|---|---|
| Date | 2026-10-24 |
| Probe | Mini, legacy bootloader, `mini-cmsis-dap-rv-bridge` `app-10k` image, `v0.11.0-130-g6487129` |
| Target | CH32V006K8U6 on one wire, with its own supply |
| OpenOCD | `0.12.0+dev-01355-gcba596a3e`, `ch32v00x-cmsis-dap.cfg` with `WORKAREASIZE` `0x2000` |
| GDB | `riscv-none-elf-gdb` (xPack GCC 15.2.0-1) |
| probe-rs | 0.32.0, `--chip CH32V006K8U6 --protocol jtag`, after `tools/vsl_rv_bridge.py on` |
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
| Bridge on after `vsl_rv_bridge.py on` | probe-rs | Pass. probe-rs does not reset the USB device when it opens it |
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
The [CH32V006 rates on the WeAct STM32H523](#ch32v006-rates-on-the-weact-stm32h523) show the cause (#114).

The rates with the fix, on 2026-10-26, with a Mini with `versaloon-bootloader` and `app-8k` images of `mini-cmsis-dap-rv-bridge`:

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

This test looked for the firmware time of #114 on an STM32H5 board.
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
The flash test uses a random image of 32 KiB, as in the JTAG hardware test. At 9000 kHz, `tools/stm32f1_clock.tcl` sets the target clock to 64 MHz first.

| Test | Result |
|---|---|
| `vsl_rv_bridge.py on`, then `off`, then an OpenOCD JTAG session | Pass. OpenOCD finds the two TAPs (`0x3ba00477`, `0x06414041`). `reset halt` stops at the reset vector |
| The same switch before each flash run, then `program ... verify` and `dump_image` at 1000 kHz | Pass. The read-back agrees with the image |
| The same at 9000 kHz (SPI fast path) | Pass. The read-back agrees with the image |
| `cmsis-dap cmd 0x81 0x00` in a JTAG session, with the bridge already off | Pass. The next memory read works |
| `cmsis-dap cmd 0x81 0x01`, then `0x81 0x00`, in a JTAG session, then a memory read | Fail: "Invalid ACK (7) in DAP response". See the note |
| The same, then `jtag arp_init`, then memory read and write, `step`, a hardware breakpoint and `reset halt` | Pass |

Note: a switch while JTAG is connected starts the new port with an unknown TAP state (see [Bridge switch](#bridge-switch)).
OpenOCD does not know this. Probably it uses the IR of the target from before the switch. The test did not capture the scans to confirm this.
`jtag arp_init` resets the TAPs and scans the chain again. After it, OpenOCD and the target agree again.
The configs in `etc/openocd/` switch the bridge only before the chain scan and at `shutdown`, so they do not have this problem.

## Tests

`tests/TestCmsisDap.cpp` runs the processor on the real SWD bit-bang engine, with the mock SWD target of `tests/MockSwdPins.h`.
The tests cover each command, WAIT retries, FAULT, parity errors, protocol errors, value match and the timeout of value match, and posted reads.

`tests/TestCmsisDapJtag.cpp` runs the processor on the JTAG bit-bang engine and on the JTAG fast path, with the mock JTAG pins of `tests/MockJtagPins.h`.
The target is the JTAG chain of `tests/MockJtagDp.h`: a TAP state machine, and devices with IDCODE and BYPASS.
One device of the chain is a JTAG-DP with a memory AP.
The tests cover the JTAG commands, IDCODE, DPACC and APACC reads and writes, posted reads, WAIT, value match, block transfers, ABORT and chains with more than one device.
Other tests send `DAP_TransferAbort` and other packets during a transfer command, through the processor and through the USB function.
One test sends the abort during the WAIT retries of one SWD transfer, from the serial service of the engine.

`tests/TestRvDtmTap.cpp` runs the emulated TAP of the RISC-V bridge (`src/protocol/cmsisdap/RvDtmTap.h`, #103).
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

## Hardware test

This section has the SWD tests. The JTAG tests follow in [JTAG hardware test](#jtag-hardware-test).

Date: 2026-10-10.
Firmware: `dev` at `5f2135f` with E6 (`DAP_TransferAbort`). The build is `v0.2.0-63-g10d84fa-dirty`. The `-dirty` suffix is from the merge of `dev`, which was not committed at build time. The `src/` tree equals commit `75d93cd`. CMSIS-DAP v2 firmware version 2.1.0, `cmsis-dap` personality.
The same checks first ran on `v0.2.0-57-g59614cb` (before E6). All checks passed there too. Only the read speed differs (see below).
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
| Memory write and read | pass | not run | read only (`probe-rs read`), run before E6 only |
| UART bridge during flash (256 KiB, image verified after) | pass | pass | pass |

Enumeration: the product string is `Versaloon CMSIS-DAP`.
Interface 0 is a vendor class interface with the string `Versaloon CMSIS-DAP v2`.
The `cdc_acm` driver binds interfaces 1 and 2 and gives `/dev/ttyACM0`.
The `ftdi_sio` driver does not bind to the device.
OpenOCD opens the probe with VID:PID 0x0403:0x9b3c.

Flash speed (64 KiB, median of 3, SWD, target at 64 MHz, firmware with E6):

| SWCLK request | Write | Verify | Read | Read before E6 |
|---|---|---|---|---|
| 250 kHz | 4.51 s | 0.16 s | 3.41 s | 3.40 s |
| 1000 kHz | 2.59 s | 0.13 s | 1.15 s | 0.95 s |
| 2000 kHz | 2.55 s | 0.13 s | 0.70 s | 0.56 s |
| 4000 kHz | 2.55 s | 0.13 s | 0.73 s | 0.56 s |

All runs passed (0 failures in 3 runs for each rate).
Only the run before E6 includes 100 kHz and 500 kHz (write 9.75 s and 2.78 s, read 8.27 s and 1.77 s).

The Versaloon personality (Mini board, same firmware version before E6, `vsllink` driver) gave these times:

| Clock | Write | Read |
|---|---|---|
| 235 kHz (`vsllink` step) | 5.20 s | 4.26 s |
| 1.85 MHz (untimed) | 2.54 s | 0.81 s |

The `vsllink` driver has fixed SWD steps. Thus the comparison is not exact at 250 kHz, where the clocks are 250 kHz and 235 kHz.
At 2000 kHz, both personalities ran the untimed engine. This firmware is from before #118: now CMSIS-DAP runs untimed only from 3 MHz.
The write time is the same (2.55 s and 2.54 s).
The read time with E6 is 0.70 s for CMSIS-DAP and 0.81 s for the Versaloon personality (91 and 79 KiB/s).
Before E6, the CMSIS-DAP read time was 0.56 s (114 KiB/s).

Maximum SWD clock: the untimed engine. In this test, it ran from 1.85 MHz. Since #118, it runs from 3 MHz (`maxClockHz` in `src/swd/SwdBitBang.h`).
Its real rate depends on the code (2.42 to 2.7 MHz on the Mini, see [Clock pacing](clock-pacing.md)).
All runs passed up to this clock with the wiring of this test.
A request above 3 MHz gives the same clock. A request from 1.85 MHz to 3 MHz now gives the timed engine at 1.3 to 1.56 MHz.
This test used the Mini. On the STM32H5, all requests are timed: a request above 1.85 MHz gives a faster clock, up to 5.2 to 6.1 MHz (see [Clock pacing](clock-pacing.md#swd-pacer-on-the-stm32h5)).

Findings:

- Write time does not improve above 1 MHz. The flash programming time is probably the limit. This test did not measure it.
- The times at 2000 kHz and 4000 kHz are the same within the noise. This is expected: the engine ran untimed from 1.85 MHz, so both requests gave the same clock.
- E6 made the read time about 20% to 25% longer at 1000 kHz and 2000 kHz (0.56 s to 0.70 s at 2000 kHz). The 4000 kHz run shows 30%, but it is noisy. Write time and the 250 kHz read time did not change. The cause is not measured. Issue #35 tracks it.
- After a replug, the SWD connect failed (`cannot read IDR`, and the pin line showed `TDI = 0`). It worked again when the target board was reconnected (the pin line showed `TDI = 1`). The cause is probably the wiring. No log proves this, and the exact wire is not known.
- `probe-rs info` does not print the DPIDR value. Without `--verbose`, it prints "Connecting to the chip was unsuccessful", although `download`, `verify`, `gdb` and `reset` connect. With `--verbose` (run before E6), it shows the DPv1 MemoryAP and the ROM table, and stops with "does not support SWD multidrop". This is normal for an STM32F1 target.
- `probe-rs verify` prints "Verification failed: contents do not match" for a wrong image, but it exits with code 0. Check the text, not the exit code. This is how probe-rs behaves. It is not a probe finding.
- In the probe-rs GDB server, the first `pc` after `monitor reset halt` shows a stale value. The next `stepi` shows the correct PC.
- The UART loopback test with the default run (PA9 to PA10) gives 100 passed, 0 failed and 50 skipped. Linux cannot make 1.5 stop bits, so all skips are for that setting. The test during a 256 KiB flash passes with all three tools (60 of 60 runs at 115200 baud, 8N1).

## JTAG hardware test

Date: 2026-10-11.
Firmware: `dev` at `6fc6d8b` with E7 (JTAG). The build is `v0.2.0-101-g6fc6d8b`. The tree was clean. CMSIS-DAP v2 firmware version 2.1.0, `cmsis-dap` personality, `Release` build.
Probe: Mini board. The image ran behind `versaloon-bootloader`.
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
The pyOCD checks come from `tools/pyocd_jtag_test.py`.

All runs passed (0 failures).
OpenOCD uses `DAP_JTAG_Sequence` for all JTAG scans.
pyOCD uses `DAP_Transfer` and `DAP_TransferBlock`.
Thus the pyOCD run is the test of the JTAG-DP transfers of this firmware.

The target runs at 8 MHz after a reset, and TCK must not be above F_CPU / 6.
Thus each run above 1000 kHz sets the target clock to 64 MHz first (`tools/stm32f1_clock.tcl` for OpenOCD, the same register writes in the pyOCD script).
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

`DAP_JTAG_Configure` and `DAP_JTAG_IDCODE` checks (raw commands in `tools/pyocd_jtag_test.py`):

| Check | Result |
|---|---|
| IR lengths 4 and 5, IDCODE of device 0 | `0x3ba00477` |
| IDCODE of device 2 (not in the chain) | error reply |
| 9 devices, IR length 0, IR length 33 | error reply |

Findings:

- OpenOCD in JTAG mode is slow. The write rate is about 4 KiB/s and the read rate about 4.5 KiB/s. The rate does not depend on TCK from 1000 kHz to 9000 kHz. pyOCD is 4 to 8 times faster with the same probe. SWD with OpenOCD (see [Hardware test](#hardware-test)) is faster too. The cause is not measured. OpenOCD sends one `DAP_JTAG_Sequence` per scan, so the USB round trips probably limit the rate. This test did not check whether the firmware or the host causes it.
- pyOCD 0.45.1 does not work with the STM32F1 on JTAG without a change. It sends `DAP_JTAG_Configure` with one device of IR length 4, and the chain has two TAPs. The first transfer fails with ACK 0. With the chain set to the IR lengths 4 and 5, all transfers pass. The test script `tools/pyocd_jtag_test.py` sets the chain to the IR lengths 4 and 5 in a patch of the pyOCD protocol class.
- The pyOCD target `stm32f103ze` (from the Keil DFP) fails on JTAG before the first transfer with `'bytes' object cannot be interpreted as an integer`. The error is in the debug sequence code of pyOCD (`hex()` of a `bytes` result in a trace call). It is not a probe problem. The test uses the built-in target `stm32f103rc`.
- `DAP_JTAG_IDCODE` for device 1 (the boundary-scan TAP) returns 0. The firmware loads the IR value `0x0E`, as the reference implementation of Arm does. This is the IDCODE instruction of the Cortex-M3 DAP. The boundary-scan TAP of the STM32F1 probably does not decode it as IDCODE. OpenOCD reads `0x06414041` from this TAP with its own scan, so the TAP does answer. No debug host uses `DAP_JTAG_IDCODE` of this TAP.
- The probe re-enumerated once on the USB bus, about 2 minutes after the firmware started (device number 7 to 8). The first OpenOCD tries failed with "No such device" in that time. It then stayed on device number 8 for the rest of the test. The cause is not known. A second run did not repeat it.
- The probe-rs JTAG test is not done. The issue asks for OpenOCD and pyOCD only.

## UART hardware test

`tools/dap_uart_loopback_test.py` tests the `DAP_UART_*` commands on the dongle.
pyOCD, OpenOCD and probe-rs do not use these commands, so the script sends them directly on interface 0.
It needs the pyusb Python package and Python 3.

1. Write a `cmsis-dap` image or a multi image with `cmsis-dap`, and select the CMSIS-DAP personality.
2. Connect the UART TX pin to the UART RX pin (PA9 to PA10 on the Mini).
3. Run the script:

```sh
python3 tools/dap_uart_loopback_test.py
python3 tools/dap_uart_loopback_test.py --serial-port /dev/ttyACM0
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
