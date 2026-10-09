# CMSIS-DAP command processor

This document describes the CMSIS-DAP v2.1 command processor for SWD.
The code is in `src/protocol/cmsisdap/`.
The processor is not yet part of a USB function.
Thus the firmware does not answer CMSIS-DAP requests yet.
A later work item adds the USB personality (see the phase E context in issue #22).

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
| `0x03` | `DAP_Disconnect` | Release SWCLK and SWDIO. |
| `0x04` | `DAP_TransferConfigure` | Idle cycles, WAIT retries, match retries. |
| `0x05` | `DAP_Transfer` | Read, write, posted AP read, value match, match mask. |
| `0x06` | `DAP_TransferBlock` | Read or write block. |
| `0x07` | `DAP_TransferAbort` | No response. It has no effect yet, because nothing runs at the same time. |
| `0x08` | `DAP_WriteABORT` | |
| `0x09` | `DAP_Delay` | |
| `0x0A` | `DAP_ResetTarget` | A 10 ms pulse on SRST. A board without SRST replies that it did no reset. |
| `0x10` | `DAP_SWJ_Pins` | SWCLK, SWDIO, TDI, TDO, nTRST, nRESET. nTRST and nRESET are open-drain. The wait is at most 3 s. |
| `0x11` | `DAP_SWJ_Clock` | In Hz. See the SWD engine for the available rates. |
| `0x12` | `DAP_SWJ_Sequence` | 1 to 256 bits. |
| `0x13` | `DAP_SWD_Configure` | Turnaround and data phase. |
| `0x1D` | `DAP_SWD_Sequence` | |
| `0x7E` | `DAP_QueueCommands` | The packet waits. The next packet with another command runs the queue. |
| `0x7F` | `DAP_ExecuteCommands` | The commands do not nest. |

An unknown command replies `ID_DAP_Invalid` (`0xFF`).
The JTAG commands and the UART commands come with their own work items.
The capabilities in `DAP_Info` show SWD and atomic commands only.

The transfer commands follow the reference implementation of Arm.
After an error, the processor skips the rest of the request data.
After a `DAP_Transfer` write, the processor reads RDBUFF to check the write.

A request that is too short gives `ID_DAP_Invalid`.
A response that does not fit in a packet gives `ID_DAP_Invalid` too.
In `DAP_ExecuteCommands`, this stops the list, and the command count is the number of commands that ran.

## SWD engine rules

The adapter sets the CMSIS-DAP rules of the engine (see `TransferRules` in `src/swd/SwdBitBang.h`).
It never calls `configure()`, which sets the clock from an OpenOCD delay value.
Thus the order of `DAP_SWJ_Clock`, `DAP_SWD_Configure` and `DAP_TransferConfigure` does not matter.

## Tests

`tests/TestCmsisDap.cpp` runs the processor on the real SWD bit-bang engine, with the mock SWD target of `tests/MockSwdPins.h`.
The tests cover each command, WAIT retries, FAULT, parity errors, protocol errors, value match and the timeout of value match, and posted reads.
