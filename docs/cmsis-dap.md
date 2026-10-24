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
  On the Mini and the OtherUse, the JTAG engine uses the SPI and DMA fast path at a TCK of 1125 kHz or more.

A `cmsis-dap` image has only this personality.
A multi image has `cmsis-dap` and other personalities, for example `VSL_PERSONALITIES=versaloon,cmsis-dap`.
Select the active one with `ID_DAP_Vendor0`, with `SWITCH_PERSONALITY` of the Versaloon protocol, with the switch request on EP0, or with the key (see [Multi image](usb-device.md#multi-image)).
The switch request on EP0 and the request of the MS OS 2.0 descriptor set use the same handler (see [Switch request on EP0](usb-device.md#switch-request-on-ep0)).

## Limits

- JTAG has no adaptive clocking (RTCK). TCK is in whole kHz, at least 1 kHz. See [JTAG](#jtag).
- A JTAG chain has up to 8 devices. The IR length of a device is 1 to 32 bits.
- On JTAG, `DAP_SWJ_Pins` must not leave TCK at a level that is not the idle level of the JTAG engine.
  Else the next scan has an extra TCK edge or a missing TCK edge.
  The idle level is high on the boards with the SPI fast path (the Mini and the OtherUse), and low on the other boards.
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
| `0x80` | `ID_DAP_Vendor0` | Switches the USB personality of a multi image: `[0x80][personality u8]`. The personality is the code of a personality in the image: `0` (Versaloon), `1` (CMSIS-DAP) or `2` (WCH-Link). The response is `[0x80][status u8]`: `0` when the firmware accepts the switch, `0xFF` for a code that is not in the image. The firmware then stores the personality and resets. The bootloader code `0xB0` starts the bootloader, also in an image with one personality (see [Bootloader code](firmware-update.md#bootloader-code)). A multi image other than `app-8k` replies `0xFF` to it. An image with one personality that is not `app-8k` replies `ID_DAP_Invalid` to each code. See [Multi image](usb-device.md#multi-image). |
| `0x81` | `ID_DAP_Vendor1` | Only in an image with `VSL_CMSIS_DAP_RV_BRIDGE`: switches the RISC-V bridge, `[0x81][on u8]`, `1` on, `0` off. The response is `[0x81][status u8]`: `0` when the switch is done, `0xFF` for another value. An image without the bridge replies `ID_DAP_Invalid`. See [Bridge switch](#bridge-switch). |
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
On the Mini and the OtherUse, it shifts whole bytes with SPI and DMA when the SPI rate is not above the request and is at least the bit-bang rate (see `src/jtag/JtagFastPath.h`).
This starts at a TCK of 1125 kHz. The other bits, and the slower rates, use GPIO bit-bang.
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

Status: in progress (#103). An image with the build option `VSL_CMSIS_DAP_RV_BRIDGE` has the bridge. The [OpenOCD configs](#openocd-configs) are in `etc/openocd/`. The hardware test (#109) is in progress: see [Bridge hardware test](#bridge-hardware-test).
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

The CMake option `VSL_CMSIS_DAP_RV_BRIDGE` (default `OFF`) adds the bridge to the `cmsis-dap` personality.
It needs `VSL_PERSONALITIES=cmsis-dap`: the configure step stops with an error for another set of personalities, also for a multi image.
It needs an STM32F1 board, because the one-wire engine uses the timers and DMA1 of the STM32F1.
The image tag ends with `-rv-bridge`, for example `versaloon-ng-mini-cmsis-dap-rv-bridge-app-10k-Release-<version>.bin`.
The presets `mini-cmsis-dap-rv-bridge` and `otheruse-cmsis-dap-rv-bridge` set the option:

```sh
cmake --preset mini-cmsis-dap-rv-bridge
cmake --build --preset mini-cmsis-dap-rv-bridge -j
```

The bridge adds about 6.4 KB to the image: the `app-10k` image of the Mini is 38,240 of 55,296 bytes.

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
The proc also adds `cmsis-dap cmd 0x81 0x00` to `pre_shutdown_commands`. Thus `shutdown` switches the bridge off before OpenOCD closes the adapter.
Ctrl-C and SIGTERM also run `shutdown`.
An error in a `-c` command after `init`, SIGKILL or a crash skips `pre_shutdown_commands`. Then the bridge stays on, and the next plain JTAG session finds the IDCODE `0x15653101`.
Switch the bridge off with `python3 tools/vsl_rv_bridge.py off`, or replug the probe.
The CH32V00X config sets `riscv set_mem_access abstract`: the CH32V006 refuses the program buffer (`cmderr` 2).
The configs use `-chain-position` and `set_mem_access`, because OpenOCD 0.12.0 does not know `-tap` and `mem_access`. Newer OpenOCD versions show a deprecation warning for them.
The configs have no flash bank. Mainline OpenOCD has no CH32V flash driver yet (#110).

Result on 2026-10-24 (OpenOCD `0.12.0+dev-01707-gf7037392b`, OtherUse with the SPI1 remap fix of !86, CH32V006 on one wire): OpenOCD examines the hart (`XLEN=32`, `misa=0x40801014`). Halt, a RAM write and read, and a step pass.
The CH32V006 has no triggers, so OpenOCD cannot set a hardware breakpoint.
The CH32V003 and CH32V20x configs are not tested on hardware.

### Bridge switch

The vendor command `ID_DAP_Vendor1` (`0x81`) switches the bridge: `81 01` on, `81 00` off.
The response is `81 00`.

- The bridge is off at power-up, after a USB reset and after `81 00`.
  Then the JTAG commands use the JTAG port of the board, as in an image without the bridge.
- When the bridge is on, `DAP_Connect` to JTAG, `DAP_JTAG_Sequence`, `DAP_SWJ_Sequence` and the JTAG transfers use the emulated TAP.
  SWD (`DAP_Connect` to SWD) does not change.
- A switch while JTAG is connected stops the old port and starts the new one at once.
  The TAP state and the IR of the new port are not known, as after `DAP_SWJ_Pins`.
- The switch off releases SWCLK and SWDIO, and resets the timer of the one-wire engine.
  The JTAG fast path uses the same pin (PB4) and DMA1 channel 3.
  The release goes in both directions: the `fini()` of the JTAG fast path clears the DMA requests of SPI1 and SPI2.
  The one-wire timer shares DMA1 channel 3 (TIM3) or channel 5 (TIM1) with them.
  The `fini()` also removes the SPI1 remap. A disabled SPI1 with the remap still owns PB4 (MISO), and blocks the TIM3_CH1 output of the one-wire engine.
  A CH32V006 test on 2026-10-24 showed this: with the remap, the one-wire frames sent no bits after the line reset.
- `DAP_SWJ_Clock` sets the TCK of the board port also while the bridge is on. The wire has its own clock: the high speed of the `wch-link` personality.

OpenOCD sends the command with `cmsis-dap cmd 0x81 0x01`.
probe-rs cannot send a vendor command (#103, decision 7). Send the command with `tools/vsl_rv_bridge.py` before probe-rs starts:

```sh
python3 tools/vsl_rv_bridge.py on
probe-rs info --protocol jtag
python3 tools/vsl_rv_bridge.py off
```

The bridge stays on until a USB reset, a power cycle or `81 00`.
The OpenOCD configs send `81 00` at `shutdown` and after Ctrl-C, but not after an error in a `-c` command.
Thus probe-rs must not reset the USB device when it opens it. probe-rs 0.32.0 does not reset it ([Bridge hardware test](#bridge-hardware-test)).

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

The hardware test (#109) has a step for each target: the CH32V006, the CH32V003 and the CH32V208, on the OtherUse and on the Mini.
This section has the results of the steps that are done.

#### CH32V006 on the OtherUse

| Item | Value |
|---|---|
| Date | 2026-10-24 |
| Probe | OtherUse, legacy bootloader, `otheruse-cmsis-dap-rv-bridge` `app-10k` image, `v0.11.0-130-g6487129` |
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
2. When GDB connects, probe-rs reads all 32 general registers. The CH32V006 is RV32E and has only `x0` to `x15`. The abstract command for `x16` (regno `0x1010`) gives `cmderr` 3 (exception), and probe-rs ends the session with "A RISC-V specific error occurred". probe-rs does not use the E extension of `misa` here. The bridge sends the correct result of the target.
3. The script sends `81 00`, `DAP_Connect` to JTAG and `DAP_SWJ_Clock` 9 MHz. Then it sends 20 DR scans of 128 bits with TDO capture. These scans use the SPI fast path: a TCK of 9 MHz is above the fast path limit of 1125 kHz. The test did not measure the pins. Then the script sends `81 01` and reads the IDCODE `0x15653101` from the emulated TAP. OpenOCD then examines the hart, halts it and reads RAM and flash.

Rates, with the target at its reset clock:

| Measurement | Result |
|---|---|
| OpenOCD `riscv dmi_read`, 1000 single reads | 0.87 ms for each read. Each read is one USB round trip |
| OpenOCD memory read, `dump_image` of 8 KB | 4.64 s (1.77 KiB/s) |
| OpenOCD memory write, `load_image` of 8 KB | 4.62 s (1.77 KiB/s) |
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
The cause of the remaining time in the firmware is not measured.

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
They also run `ID_DAP_Vendor1` through the processor, the switch off at `hostGone()`, and `DAP_SWJ_Pins` on SWCLK and SWDIO.

`tests/TestDapUart.cpp` runs `uart::DapUartChannel` with the real CDC-ACM function, the UART bridge and a fake UART.
The tests cover the owner switch, the frame formats, the receiver and transmitter control, the status bits and the buffer limits.
They also run the `DAP_UART_*` commands and `DAP_Info` through the processor.

## Hardware test

This section has the SWD tests. The JTAG tests follow in [JTAG hardware test](#jtag-hardware-test).

Date: 2026-10-10.
Firmware: `dev` at `5f2135f` with E6 (`DAP_TransferAbort`). The build is `v0.2.0-63-g10d84fa-dirty`. The `-dirty` suffix is from the merge of `dev`, which was not committed at build time. The `src/` tree equals commit `75d93cd`. CMSIS-DAP v2 firmware version 2.1.0, `cmsis-dap` personality.
The same checks first ran on `v0.2.0-57-g59614cb` (before E6). All checks passed there too. Only the read speed differs (see below).
Probe: OtherUse board.
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

The Versaloon personality (OtherUse board, same firmware version before E6, `vsllink` driver) gave these times:

| Clock | Write | Read |
|---|---|---|
| 235 kHz (`vsllink` step) | 5.20 s | 4.26 s |
| 1.85 MHz (untimed) | 2.54 s | 0.81 s |

The `vsllink` driver has fixed SWD steps. Thus the comparison is not exact at 250 kHz, where the clocks are 250 kHz and 235 kHz.
At 2000 kHz, both personalities run the untimed engine.
The write time is the same (2.55 s and 2.54 s).
The read time with E6 is 0.70 s for CMSIS-DAP and 0.81 s for the Versaloon personality (91 and 79 KiB/s).
Before E6, the CMSIS-DAP read time was 0.56 s (114 KiB/s).

Maximum SWD clock: 1.85 MHz (the untimed engine, `maxClockHz` in `src/swd/SwdBitBang.h`).
All runs passed up to this clock with the wiring of this test.
A request above 1.85 MHz gives the same clock.

Findings:

- Write time does not improve above 1 MHz. The flash programming time is probably the limit. This test did not measure it.
- The times at 2000 kHz and 4000 kHz are the same within the noise. This is expected: the engine runs untimed from 1.85 MHz, so both requests give the same clock.
- E6 made the read time about 20% to 25% longer at 1000 kHz and 2000 kHz (0.56 s to 0.70 s at 2000 kHz). The 4000 kHz run shows 30%, but it is noisy. Write time and the 250 kHz read time did not change. The cause is not measured. Issue #35 tracks it.
- After a replug, the SWD connect failed (`cannot read IDR`, and the pin line showed `TDI = 0`). It worked again when the target board was reconnected (the pin line showed `TDI = 1`). The cause is probably the wiring. No log proves this, and the exact wire is not known.
- `probe-rs info` does not print the DPIDR value. Without `--verbose`, it prints "Connecting to the chip was unsuccessful", although `download`, `verify`, `gdb` and `reset` connect. With `--verbose` (run before E6), it shows the DPv1 MemoryAP and the ROM table, and stops with "does not support SWD multidrop". This is normal for an STM32F1 target.
- `probe-rs verify` prints "Verification failed: contents do not match" for a wrong image, but it exits with code 0. Check the text, not the exit code. This is how probe-rs behaves. It is not a probe finding.
- In the probe-rs GDB server, the first `pc` after `monitor reset halt` shows a stale value. The next `stepi` shows the correct PC.
- The UART loopback test with the default run (PA9 to PA10) gives 100 passed, 0 failed and 50 skipped. Linux cannot make 1.5 stop bits, so all skips are for that setting. The test during a 256 KiB flash passes with all three tools (60 of 60 runs at 115200 baud, 8N1).

## JTAG hardware test

Date: 2026-10-11.
Firmware: `dev` at `6fc6d8b` with E7 (JTAG). The build is `v0.2.0-101-g6fc6d8b`. The tree was clean. CMSIS-DAP v2 firmware version 2.1.0, `cmsis-dap` personality, `Release` build.
Probe: OtherUse board. The image ran behind `versaloon-bootloader`.
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
2. Connect the UART TX pin to the UART RX pin (PA9 to PA10 on the Mini and the OtherUse).
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
Probe: OtherUse board with the 10k bootloader, `app-10k` image of the `otheruse-cmsis-dap` preset, build `v0.9.0-42-g52e32da`.
The UART TX pin was connected to the UART RX pin (PA9 to PA10).
The run used `--serial-port /dev/ttyACM0`.
All 70 cases passed: the baud rates from 57600 to 2000000 baud, each with the seven frame formats.
The serial port got no data while the DAP commands owned the UART.
A 256 KiB flash with OpenOCD (SWD, 2000 kHz) and the serial loopback test on the COM port ran at the same time.
Both passed.
The SWD and JTAG checks with OpenOCD, pyOCD and probe-rs still passed on this firmware.
The `DAP_UART_*` commands and a flash on the same DAP interface did not run at the same time.
