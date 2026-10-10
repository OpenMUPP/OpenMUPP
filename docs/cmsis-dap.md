# CMSIS-DAP command processor

This document describes the CMSIS-DAP v2.1 command processor for SWD.
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
- The image does not link the Versaloon command buffers, the `USB_TO_XXX` blocks, the JTAG engine and the power block.
- The SWD engine and the debug connector pins (`DAP_SWJ_Pins`) are the same ones as in the Versaloon image.

Only one personality is in an image.
To change the personality, write another image.
The runtime selection is a later work item.

## Limits

- SWD only. `DAP_Connect` to JTAG replies "failed". The JTAG commands come with a later work item.
- No SWO and no `DAP_UART_*` commands. Use the CDC-ACM serial port for the UART.
- `DAP_TransferAbort` has no effect, because no command runs while another command runs.
- 64-byte packets, and up to 4 packets in flight.
- No target power switch and no Vref sense in this personality.
- The LEDs show the heartbeat and the USB state. `DAP_HostStatus` does not change them.
- Windows needs a WinUSB driver for interface 0, for example from Zadig, until the driverless descriptors exist.
- The hardware test passed for OpenOCD, pyOCD and probe-rs. The UART bridge test during a flash is open (see [Hardware test](#hardware-test)).

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

The processor has no CMSIS header and no hardware header.
It uses these objects:

- A `DapSwd` for the SWD transfers and sequences.
- A `protocol::GpioPins` (on target `board::BoardGpio`) for `DAP_SWJ_Pins` and `DAP_ResetTarget`.
- A `Platform` for the microsecond time, the delay and the LEDs.
- A `DeviceInfo` with the strings and the capabilities of `DAP_Info`.

## Transport

The USB function calls `submit()` for each request packet.
It sends the response packets in order.
The packet size is 64 bytes, and up to 4 packets are in flight.
Call `canSubmit()` before `submit()`.
Each packet without a response takes one of the 4 slots.

## Commands

| ID | Command | Note |
|---|---|---|
| `0x00` | `DAP_Info` | Vendor, product, serial number, protocol version `2.1.0`, firmware version, capabilities, packet size and count. Other information has length 0. |
| `0x01` | `DAP_HostStatus` | Connect and running LED. |
| `0x02` | `DAP_Connect` | Port 0 and 1 give SWD. JTAG replies 0 (failed) until the JTAG commands exist. |
| `0x03` | `DAP_Disconnect` | Release SWCLK and SWDIO. Release the pins that `DAP_SWJ_Pins` drove: nRESET and nTRST get the pull-up. |
| `0x04` | `DAP_TransferConfigure` | Idle cycles, WAIT retries, match retries. |
| `0x05` | `DAP_Transfer` | Read, write, posted AP read, value match, match mask. |
| `0x06` | `DAP_TransferBlock` | Read or write block. |
| `0x07` | `DAP_TransferAbort` | No response. It has no effect yet, because nothing runs at the same time. |
| `0x08` | `DAP_WriteABORT` | Replies an error when no port is connected. |
| `0x09` | `DAP_Delay` | |
| `0x0A` | `DAP_ResetTarget` | A 10 ms pulse on SRST. A board without SRST replies that it did no reset. |
| `0x10` | `DAP_SWJ_Pins` | SWCLK, SWDIO, TDI, TDO, nTRST, nRESET. nTRST and nRESET are open-drain. The wait is at most 3 s. |
| `0x11` | `DAP_SWJ_Clock` | In Hz. See the SWD engine for the available rates. |
| `0x12` | `DAP_SWJ_Sequence` | 1 to 256 bits. |
| `0x13` | `DAP_SWD_Configure` | Turnaround and data phase. |
| `0x1D` | `DAP_SWD_Sequence` | |
| `0x7E` | `DAP_QueueCommands` | The packet waits. The next packet with another command runs the queue. A queued packet in the last free slot also runs the queue. |
| `0x7F` | `DAP_ExecuteCommands` | The commands do not nest. |

An unknown command replies `ID_DAP_Invalid` (`0xFF`).
The JTAG commands and the UART commands come with their own work items.
The capabilities in `DAP_Info` show SWD and atomic commands only.

The transfer commands follow the reference implementation of Arm.
After an error, the processor skips the rest of the request data.
After a `DAP_Transfer` write, the processor reads RDBUFF to check the write.

The processor checks a request before the first transfer.
A request that is too short gives `ID_DAP_Invalid`.
A response that does not fit in a packet gives `ID_DAP_Invalid` too.
In both cases, the target sees no transfer from that command.
In `DAP_ExecuteCommands`, this stops the list, and the command count is the number of commands that ran.

When the host goes away, the transport calls `hostGone()`.
It disconnects, releases the pins, and drops the queued packets and the responses that the transport did not send.

## SWD engine rules

The adapter sets the CMSIS-DAP rules of the engine (see `TransferRules` in `src/swd/SwdBitBang.h`).
It never calls `configure()`, which sets the clock from an OpenOCD delay value.
Thus the order of `DAP_SWJ_Clock`, `DAP_SWD_Configure` and `DAP_TransferConfigure` does not matter.
`DAP_Connect` sets the configuration and the clock again, because the engine can run another personality in between.

## Tests

`tests/TestCmsisDap.cpp` runs the processor on the real SWD bit-bang engine, with the mock SWD target of `tests/MockSwdPins.h`.
The tests cover each command, WAIT retries, FAULT, parity errors, protocol errors, value match and the timeout of value match, and posted reads.

## Hardware test

Date: 2026-10-10.
Firmware: `v0.2.0-57-g59614cb` (CMSIS-DAP v2 firmware version 2.1.0), `cmsis-dap` personality.
Target: STM32F103ZET6 (512 KiB flash) on SWD.
Host: Linux 6.18.55.
Tools: OpenOCD 0.12.0+dev-00801-g133dd9d66, pyOCD 0.45.1 (Keil.STM32F1xx_DFP 2.4.1), probe-rs 0.32.0.

| Check | OpenOCD | pyOCD | probe-rs |
|---|---|---|---|
| Connect, DPIDR `0x1ba01477` | pass | pass | pass |
| Flash and verify (32 KiB) | pass | pass | pass |
| Read back (`cmp`) | pass | pass | not run |
| Halt, step, reset | pass | pass | pass (halt and step through GDB) |
| Memory write and read | pass | not run | read only (`probe-rs read`) |
| UART bridge during flash | not run | not run | not run |

Enumeration: the product string is `Versaloon CMSIS-DAP`.
Interface 0 is a vendor class interface with the string `Versaloon CMSIS-DAP v2`.
Interfaces 1 and 2 are bound to `cdc_acm` and give `/dev/ttyACM0`.
The `ftdi_sio` driver does not bind to the device.
OpenOCD opens the probe with VID:PID 0x0403:0x9b3c.

Flash speed (64 KiB, median of 3, SWD, target at 64 MHz):

| SWCLK request | Write | Verify | Read |
|---|---|---|---|
| 100 kHz | 9.75 s | 0.22 s | 8.27 s |
| 250 kHz | 4.50 s | 0.16 s | 3.40 s |
| 500 kHz | 2.78 s | 0.14 s | 1.77 s |
| 1000 kHz | 2.58 s | 0.13 s | 0.95 s |
| 2000 kHz | 2.53 s | 0.13 s | 0.56 s |
| 4000 kHz | 2.53 s | 0.13 s | 0.56 s |

All runs passed (0 failures in 3 runs for each rate).
The comparison with the Versaloon personality is open.

Findings:

- Write time does not improve above 1 MHz. The write time is flash programming time, not SWD time.
- Read time is the same at 2000 kHz and 4000 kHz. The cause is not known. The engine can round the clock down, or the USB path is the limit. A logic analyzer on SWCLK can show which.
- `probe-rs info` stops after it finds the DPv1 MemoryAP and the ROM table, with the message "does not support SWD multidrop". This is normal for an STM32F1 target.
- `probe-rs verify` prints "Verification failed: contents do not match" for a wrong image, but it exits with code 0. Check the text, not the exit code. This is a probe-rs behavior, not a probe finding.
- In the probe-rs GDB server, the first `pc` after `monitor reset halt` shows a stale value. The next `stepi` shows the correct PC.
- The UART loopback test received no bytes at 115200 baud. The PA9 to PA10 wire was probably not connected. The test must be repeated.

