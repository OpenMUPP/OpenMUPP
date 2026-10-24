# CMSIS-DAP captures for the RISC-V DTM bridge

This directory holds the USB captures of OpenOCD and probe-rs on a JTAG target.
They are the evidence for the TAP emulator of the CMSIS-DAP RISC-V bridge (#103, work item B0 = #104).
The captures show which DAP commands the emulator must follow.

## Setup

| Item | Value |
|---|---|
| Probe | Versaloon OtherUse, `triple` image with the `cmsis-dap` personality active, USB ID `0403:9b3c`, CMSIS-DAP firmware version 2.1.0 |
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
| `DAP_SWJ_Pins` (`0x10`) | Reads the pins, then sets `0xa0` (nTRST and nRESET high) | Sets `0x20` (nRESET high) |
| `DAP_SWJ_Clock` | 1000000 Hz | 1000000 Hz |
| `DAP_TransferConfigure` | idle 0, wait 64 | idle 0, wait 65535 |
| `DAP_HostStatus` | Yes | Yes |
| `DAP_Info` | `0xf0`, `0x04`, `0x03`, `0xff`, `0xfe` | `0xff`, `0xfe`, `0xf0` |

Neither tool uses the DAP transfer commands, because both tools see a RISC-V TAP and no ARM DP.
The emulator does not need to answer `DAP_Transfer`, `DAP_JTAG_Configure` or `DAP_JTAG_IDCODE` for these tools.
The packet size is 64 bytes and the packet count is 4 (`DAP_Info` `0xff` and `0xfe`).
The tools split the scans to fit this size.

### How the tools split the scans

- Each `DAP_JTAG_Sequence` holds up to 64 bits per entry.
  A long shift is a list of 64-bit entries with TMS = 0 and TDO capture on.
- The last bit of a shift is its own entry with TMS = 1. For example, a 32-bit DR scan is 31 bits and 1 bit.
- OpenOCD sends each TMS move as 1-bit entries, one entry for each bit.
  One command holds up to 12 entries (one request of 61 bytes).
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
| probe-rs | DR, 256 bits of ones, twice | Detect the chain length and read the IDCODE of the first device |
| probe-rs | DR, 257 bits: 128 zeros then 128 ones | Detect the chain length |

Both tools return to Test-Logic-Reset with TMS = 1 bits before and after the scans.
Neither tool shifts an IR other than `0x10` and `0x11` (OpenOCD) in these captures.

## Limits of these captures

- Without a TAP, both tools stop early. probe-rs does not reach the IR and DR scans of DTMCS and DMI.
  OpenOCD reaches them only because it uses the configured chain after the error.
- The shape of the DMI scans (41 bits), the number of idle cycles in Run-Test/Idle and the polling of the busy status are not visible.
- Repeat the captures with the TAP emulator (#105) to close these gaps.
- The flash and halt commands of both tools are not in these captures.
