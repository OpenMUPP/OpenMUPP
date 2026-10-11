# CMSIS-DAP command processor

This document describes the CMSIS-DAP v2.1 command processor for SWD and JTAG.
The code is in `src/protocol/cmsisdap/`.
`usb::CmsisDapFunction` (`src/usb/CmsisDapFunction.h`) moves the packets over USB.
See the [CMSIS-DAP personality](usb-device.md#cmsis-dap-personality).
The build option `VSL_PROTOCOL=cmsis-dap` puts the function and the processor into the firmware image (see [Build](#build)).

## Build

Set the CMake option `VSL_PROTOCOL` to `cmsis-dap`:

```sh
cmake -B build/mini-dap -DCMAKE_TOOLCHAIN_FILE=cmake/gcc_arm.cmake -DBOARD=mini -DVSL_PROTOCOL=cmsis-dap
cmake --build build/mini-dap -j
```

The default value is `versaloon`.
The default image does not change.
The build then writes `versaloon-next-gen-<board>-cmsis-dap-<variant>-<type>-<version>.hex` and `.bin`.
Write the image as in [Firmware update](firmware-update.md).
All boards build with both values.

The `cmsis-dap` image has these properties:

- USB interface 0 is CMSIS-DAP v2, with `0403:9B3C`. The Versaloon interface is not in the image.
- The CDC-ACM serial port, the UART bridge, the debug log and the heartbeat are as in the Versaloon image.
- The image does not link the Versaloon command buffers, the `USB_TO_XXX` blocks and the power block.
- The SWD engine, the JTAG engine and the debug connector pins (`DAP_SWJ_Pins`) are the same ones as in the Versaloon image.
  On the Mini and the OtherUse, the JTAG engine uses the SPI and DMA fast path.

Only one personality is in an image.
To change the personality, write another image.
The runtime selection is a later work item.

## Limits

- JTAG has no adaptive clocking (RTCK). TCK is in whole kHz, at least 1 kHz. See [JTAG](#jtag).
- A JTAG chain has up to 8 devices. The IR length of a device is 1 to 32 bits.
- The hardware test with OpenOCD and pyOCD in JTAG mode is not done yet.
- No SWO and no `DAP_UART_*` commands. Use the CDC-ACM serial port for the UART.
- `DAP_TransferAbort` stops a transfer command only between two transfers. It does not stop the WAIT retries of one transfer. It does not stop other commands, for example `DAP_Delay`.
- `DAP_TransferAbort` reaches the processor during a command only when a slot is free. When the running command and 3 other packets use the 4 slots, the OUT endpoint NAKs, and the abort waits until the command ends.
- 64-byte packets, and up to 4 packets in flight.
- No target power switch and no Vref sense in this personality.
- The LEDs show the heartbeat and the USB state. `DAP_HostStatus` does not change them.
- Windows needs a WinUSB driver for interface 0, for example from Zadig, until the driverless descriptors exist.
- The hardware test with OpenOCD, pyOCD and probe-rs is a later work item. The host tools below follow their documentation.

## Use the probe

The probe has the USB ID `0403:9B3C` and the product name `Versaloon CMSIS-DAP`.
The host tools find the CMSIS-DAP v2 interface by the text `CMSIS-DAP` in its interface string.

On Linux, a udev rule gives a normal user access to the probe.
For example, put this line into `/etc/udev/rules.d/99-versaloon-cmsis-dap.rules`:

```text
SUBSYSTEM=="usb", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="9b3c", MODE="0660", TAG+="uaccess"
```

Then reload the rules with `udevadm control --reload` and plug in the probe again.

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

The processor has no CMSIS header and no hardware header.
It uses these objects:

- A `DapSwd` for the SWD transfers and sequences.
- A `protocol::JtagPort` (`src/protocol/JtagPort.h`, on target `board::BoardJtag`) for JTAG. It is optional.
- A `protocol::GpioPins` (on target `board::BoardGpio`) for `DAP_SWJ_Pins` and `DAP_ResetTarget`.
- A `Platform` for the microsecond time, the delay and the LEDs.
- A `DeviceInfo` with the strings and the capabilities of `DAP_Info`.

## Transport

The USB function calls `submit()` for each request packet.
`submit()` only stores the packet.
`UsbStack::poll()` then calls `CmsisDapFunction::poll()` in the main loop.
This calls `runNext()` for each stored packet, in order, and sends each response immediately.
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
| `0x7E` | `DAP_QueueCommands` | The packet waits. The next packet with another command runs the queue. A queued packet in the last free slot also runs the queue. |
| `0x7F` | `DAP_ExecuteCommands` | The commands do not nest. |

An unknown command replies `ID_DAP_Invalid` (`0xFF`).
The UART commands come with their own work item.
The capabilities in `DAP_Info` show SWD, JTAG and atomic commands.
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
After an abort, the command stops and skips the rest of its request data.
The response gives the number of transfers that the command did.
A value match also stops, with the mismatch bit set.
The command then ends as after the last transfer: it reads RDBUFF for a posted AP read, and to check the last write.
The data of the posted AP read is in the response, and the count includes it.
A transfer command of a packet that waits does no transfer and replies a count of 0.
The other commands of the packet run, for example in `DAP_ExecuteCommands`.

When the host goes away, the transport calls `hostGone()`.
It disconnects, releases the pins, and drops the queued packets and the responses that the transport did not send.

## SWD engine rules

The adapter sets the CMSIS-DAP rules of the engine (see `TransferRules` in `src/swd/SwdBitBang.h`).
It never calls `configure()`, which sets the clock from an OpenOCD delay value.
Thus the order of `DAP_SWJ_Clock`, `DAP_SWD_Configure` and `DAP_TransferConfigure` does not matter.
`DAP_Connect` sets the configuration and the clock again, because the engine can run another personality in between.

## JTAG

`DAP_Connect` with port 2 releases the SWD pins and starts the JTAG engine.
The JTAG engine is the one of the Versaloon image (`board::BoardJtag`).
On the Mini and the OtherUse, it shifts whole bytes with SPI and DMA, and the other bits with GPIO bit-bang.
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

## Tests

`tests/TestCmsisDap.cpp` runs the processor on the real SWD bit-bang engine, with the mock SWD target of `tests/MockSwdPins.h`.
The tests cover each command, WAIT retries, FAULT, parity errors, protocol errors, value match and the timeout of value match, and posted reads.

`tests/TestCmsisDapJtag.cpp` runs the processor on the JTAG bit-bang engine and on the JTAG fast path, with the mock JTAG pins of `tests/MockJtagPins.h`.
The target is the JTAG chain of `tests/MockJtagDp.h`: a TAP state machine, and devices with IDCODE and BYPASS.
One device of the chain is a JTAG-DP with a memory AP.
The tests cover the JTAG commands, IDCODE, DPACC and APACC reads and writes, posted reads, WAIT, value match, block transfers, ABORT and chains with more than one device.
Other tests send `DAP_TransferAbort` and other packets during a transfer command, through the processor and through the USB function.
