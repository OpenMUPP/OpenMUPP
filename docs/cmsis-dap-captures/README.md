# CMSIS-DAP captures for the RISC-V DTM bridge

This directory holds the USB captures of OpenOCD and probe-rs in JTAG mode, with no target.
They are the evidence for the TAP emulator of the CMSIS-DAP RISC-V bridge (#103, work item B0 = #104).
The captures show which DAP commands the emulator must follow.

## Setup

| Item | Value |
|---|---|
| Probe | Versaloon OtherUse, `triple` image with the `cmsis-dap` personality active, USB ID `0403:9b3c`, CMSIS-DAP protocol version 2.1.0 |
| Firmware commit | Not recorded. The image is a local build of `dev` |
| Target | None. TCK, TMS, TDI and TDO have no connection, so TDO reads all ones |
| OpenOCD | `0.12.0+dev-01707-gf7037392b` (local build, 2026-10-24), `interface/cmsis-dap.cfg` and [ch32v-minimal.cfg](ch32v-minimal.cfg) |
| probe-rs | 0.32.0 |
| USB capture | Linux `usbmon` with `tshark -i usbmonN`, for the bus of the probe |
| Decoder | [tools/usb-pcap/dap.py](../../tools/usb-pcap/dap.py) |

## Captures

| File | Command | Result |
|---|---|---|
| `openocd-jtag-no-target.pcapng` | `openocd -f interface/cmsis-dap.cfg -f ch32v-minimal.cfg -c init -c shutdown` | The scan chain interrogation fails (all ones). OpenOCD then uses the configured chain: IR 5 bits, DTMCS, DMI |
| `probe-rs-info-jtag-no-target.pcapng` | `probe-rs info --protocol jtag` | "JTAG DR scan chain is empty". probe-rs also tries the ARM switch sequence |
| `probe-rs-reset-ch32v003-jtag-no-target.pcapng` | `probe-rs reset --chip CH32V003F4P6 --protocol jtag` | "JTAG DR scan chain is empty" |

`probe-rs info` ignores `--chip`. The third capture is the RISC-V path with a named chip.

To list the commands of a capture:

```
python3 tools/usb-pcap/dap.py docs/cmsis-dap-captures/<file> <device address> 3
python3 tools/usb-pcap/dap.py docs/cmsis-dap-captures/<file> <device address> 3 --scans
```

The device address is 35 in all captures.
The `--scans` option runs the TMS bits through the JTAG state machine.
It prints the IR and DR scans.

## Findings

### Commands that carry TMS

| Command | OpenOCD | probe-rs |
|---|---|---|
| `DAP_JTAG_Sequence` (`0x14`) | Yes. It carries all scans | Yes. It carries all scans |
| `DAP_SWJ_Sequence` (`0x12`) | Yes. 8 bits of TMS = 1 to reach Test-Logic-Reset, before each scan group | Only in `probe-rs info`. 51 bits of ones, then the 16-bit SWD-to-JTAG sequence `0xE73C`, then 6 bits of ones. The RISC-V path (`reset --chip`) does not use it |

The emulator must follow TMS from both commands.
`DAP_SWJ_Sequence` drives TDI high.

### Other commands

| Command | OpenOCD | probe-rs |
|---|---|---|
| `DAP_JTAG_Configure` (`0x15`) | Not sent | Not sent |
| `DAP_JTAG_IDCODE` (`0x16`) | Not sent | Not sent |
| `DAP_Transfer`, `DAP_TransferBlock` | Not sent | Not sent |
| `DAP_Connect` | Port 2 (JTAG) | Port 2 (JTAG) |
| `DAP_SWJ_Pins` (`0x10`) | Reads the pins, then sets `0xa0` (nTRST and nRESET high) | Sets `0x20` (nTRST high) |
| `DAP_SWJ_Clock` | 1000000 Hz | 1000000 Hz |
| `DAP_TransferConfigure` | idle 0, wait 64 | idle 0, wait 65535 |
| `DAP_HostStatus` | Yes | Yes |
| `DAP_Info` | `0xf0`, `0x04`, `0x03`, `0xff`, `0xfe` | `0xff`, `0xfe`, `0xf0` |

Neither tool sent `DAP_Transfer`, `DAP_JTAG_Configure` or `DAP_JTAG_IDCODE`, because both stopped at the empty chain.
With a TAP on the chain, probe-rs `info` can probe for an ARM DP first. Check this again with the emulator (#105).
The packet size is 64 bytes and the packet count is 4 (`DAP_Info` `0xff` and `0xfe`).
The tools split the scans to fit this size.

### How the tools split the scans

- Each `DAP_JTAG_Sequence` holds up to 64 bits per entry.
  A long shift is a list of 64-bit entries with TMS = 0 and TDO capture on.
- OpenOCD and the 256-bit probe-rs scans end a shift with a separate 1-bit entry with TMS = 1. For example, a 32-bit DR scan is 31 bits and 1 bit.
- The 257-bit probe-rs scan is the exception. Its exit bit is the first bit of a `5b tms=1` entry without TDO capture, and the scan runs over four commands.
  The emulator must leave Shift-DR on any entry with TMS = 1, whatever the length of the entry.
- OpenOCD sends each TMS move as 1-bit entries, one entry for each bit.
  One request holds up to 61 bytes, so the entry count changes. This capture has requests of 12 and 28 entries.
  A scan can run over two commands, for example the 672-bit IDCODE scan.
- probe-rs sends the TMS moves as short multi-bit entries, for example `5b tms=1` then `1b tms=0`.
- One scan can start in one command and end in the next.
  The emulator must keep the TAP state between commands.
- The emulator must return the TDO bits of every entry with the capture flag.

### Scans seen

| Tool | Scan | Purpose |
|---|---|---|
| OpenOCD | DR, 672 bits of ones, from Test-Logic-Reset | Read the IDCODE chain |
| OpenOCD | IR, 7 bits of ones | Read the IR capture value. The chain has 5 IR bits and OpenOCD scans 2 more |
| OpenOCD | IR `0x10`, DR 32 bits, IR `0x11` | Select DTMCS, read it, then select DMI. OpenOCD does this although the IR capture check failed |
| probe-rs | DR, 256 bits of ones, twice (`reset`) or four times (`info`) | Detect the chain length and read the IDCODE of the first device |
| probe-rs | DR, 257 bits: 128 zeros, then 129 ones. The last bit has TMS = 1 and no TDO capture | Detect the chain length |

Both tools go to Test-Logic-Reset with TMS = 1 bits before the scans.
OpenOCD ends in Run-Test/Idle after the last IR scan. It does not return to Test-Logic-Reset.
Only OpenOCD shifts an IR (`0x7f`, `0x10` and `0x11`). probe-rs shifts no IR.

## Limits of these captures

- Without a TAP, both tools stop early. probe-rs does not reach the IR and DR scans of DTMCS and DMI.
  OpenOCD reaches them only because it uses the configured chain after the error.
- The shape of the DMI scans (41 bits), the number of idle cycles in Run-Test/Idle and the polls of the busy status are not visible.
- Neither tool shows what it sends when it sees a TAP. The list of commands that the tools never sent can change.
- Repeat the captures with the TAP emulator (#105) to close these gaps.
- The flash and halt commands of both tools are not in these captures.
