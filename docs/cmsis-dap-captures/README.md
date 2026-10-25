# CMSIS-DAP captures for the RISC-V DTM bridge

This directory holds the USB captures of OpenOCD and probe-rs in JTAG mode for the CMSIS-DAP RISC-V bridge (#103).

- The B0 captures (#104) have no target. They show which DAP commands the TAP emulator must follow.
- The bridge captures have the emulated TAP and a CH32V208 or a CH32V006. They show the host tools on the bridge. See [Bridge captures](#bridge-captures).

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

To print the requests of a capture as a trace for the host tests (`tests/replay/dap-*.trace`):

```
python3 tools/usb-pcap/dap.py docs/cmsis-dap-captures/<file> <device address> 3 --trace
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
With a TAP on the chain, probe-rs `info` probes for an Arm DP first. The bridge captures show this (see below).
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
- The host tests (`tests/TestRvDtmTap.cpp`) send these requests to the TAP emulator (#105). The tools do not see its replies.
  The bridge captures below close most of these gaps.
- The flash and halt commands of both tools are not in these captures.

## Bridge captures

Date: 2026-10-24.
These captures run the host tools on the emulated TAP (#105, #106). The target is a CH32V208 on two wires, or a CH32V006 on one wire.
The firmware does not use the TAP yet (#107). Thus test images route the CMSIS-DAP JTAG port to the TAP all the time.
Do not merge their changes.

| Image | Base commit | `attach()` | Unlock writes | `0xBAD0000n` marker | Change |
|---|---|---|---|---|---|
| A | `4905274` | At boot, in `startDap()` | No | No | The `main.cpp` and `cmake/Personalities.cmake` parts of the patch, without the `RvDtmTap` part. Plus `dapRvTarget.attach(rvswd::Wire::Two);` in `startDap()` |
| B | `df6009d` | At `DAP_Connect`, in `RvDtmTap::init()` | Yes | Yes | The patch, plus `dmiWrite(0x7e, 0x5aa50400)` and `dmiWrite(0x7d, 0x5aa50400)` after `attach()` in `RvDtmTap::init()` |
| C | `df6009d` | At `DAP_Connect`, in `RvDtmTap::init()` | No | Yes | [bridge-test-image.patch](bridge-test-image.patch) |
| D | `df6009d` | At `DAP_Connect`, in `RvDtmTap::init()` | Yes | Yes | Image B, with `rvswd::Wire::One` in place of `rvswd::Wire::Two` |

Images A, B and C use two wires. Image D uses one wire. Image A has IDCODE `0x15653001`, the other images have `0x15653101`.
With the marker, a failed DMI access puts its `rvswd::Status` into the DMI data as `0xBAD0000n`, to show the reason in a capture.
Image A has no marker: its failed accesses show the data of the last good read (`d=00000000`). This data does not give the reason.

| Item | Value |
|---|---|
| Probe | Versaloon OtherUse, test image (`otheruse-cmsis-dap`, `app-10k`), USB ID `0403:9b3c` |
| Target | CH32V208 on two wires (SWDIO, SWCLK), or CH32V006 on one wire (SWDIO). Each target has its own supply |
| OpenOCD | `0.12.0+dev-01707-gf7037392b` (local build), `interface/cmsis-dap.cfg` and [ch32v-minimal.cfg](ch32v-minimal.cfg) |
| probe-rs | 0.32.0 |
| Decoder | [tools/usb-pcap/dtm.py](../../tools/usb-pcap/dtm.py): the IR, DR and DMI scans, with the TDO bits of the replies |

To list the DTM scans of a capture:

```
python3 tools/usb-pcap/dtm.py docs/cmsis-dap-captures/<file> <device address>
```

| File | Device address | Image | Command | Result |
|---|---|---|---|---|
| `openocd-ch32v208-attach-at-boot.pcapng` | 38 | A | `openocd ... -c init -c halt -c "reg pc" -c "mdw 0x08000000 4" -c resume -c shutdown` | The first DMI read (`dmcontrol`) fails (`op` 2). OpenOCD reads `dmcontrol` again for 5 s and stops. The file has the first 2000 packets only |
| `openocd-ch32v208.pcapng` | 41 | B | Same | Pass: OpenOCD examines the hart (`XLEN=32`, `misa=0x40901105`), halts, reads `pc` and the flash |
| `probe-rs-info-ch32v208.pcapng` | 41 | B | `probe-rs info --protocol jtag` | probe-rs finds the DTM, but cannot find the chip type without `--chip` |
| `probe-rs-read-ch32v208.pcapng` | 41 | B | `probe-rs read --chip CH32V208WBU6 --protocol jtag b32 0x08000000 4` | Pass: the same flash words as OpenOCD |
| `openocd-ch32v208-unplug.pcapng` | 41 | B | OpenOCD server. SWDIO is disconnected, then connected again | See [Errors](#errors) |
| `openocd-ch32v208-no-unlock.pcapng` | 43 | C | `openocd ... -c init -c halt -c "mdw 0x08000000 4" -c shutdown`, after a power cycle of the target | Pass |
| `openocd-ch32v006.pcapng` | 55 | D | `openocd ... -c init -c halt -c "reg pc" -c "mww 0x20000000 0x87654321" -c "mdw 0x20000000 1" -c resume -c shutdown` | Pass: OpenOCD examines the hart (`XLEN=32`, `misa=0x40801014`: RV32EMC, plus non-standard extensions), halts, and reads back the RAM word. `pc` is 0 |

### Results

- Both tools find the TAP. OpenOCD shows the IDCODE `0x15653101` with the manufacturer `<invalid>`. probe-rs accepts it. probe-rs rejected the first IDCODE, `0x15653001` (see `docs/cmsis-dap.md`, "IDCODE").
- Both tools read DTMCS (`0x71`). probe-rs writes `dmireset` (`0x10000`) at the start: once in `info`, twice in `read`.
- Both tools use no idle cycles after a DMI scan (`idle` 0 in DTMCS). No capture has `op` 3.
- OpenOCD sends many DMI scans after one IR scan. probe-rs sends an IR scan before each DMI scan: 544 IR scans for 544 DMI scans in the read.
- probe-rs `info` sends `DAP_JTAG_Configure` (one device, IR length 5) and two `DAP_Transfer` commands to probe for an Arm DP. The TAP puts the DPACC instruction in BYPASS, so the probe fails. probe-rs then finds the RISC-V DTM.
- probe-rs also measures the IR length with IR scans of 8, 16 and 257 bits.
- probe-rs `info` also probes for other TAP types after the DTM: 3 IR scans of `0x08`, each with a DR scan of 8 bits. Then 200 IR scans of `0x1c`, each with a DR scan of 8 bits and a DR scan of 32 bits. The TAP puts these instructions in BYPASS. These scans are most of the `info` capture (203 IR scans against 20 DMI scans).

### Connect

These results are for two wires (images A, B and C).
Image A calls `attach()` at boot, and its first DMI access fails.
Image B calls `attach()` and writes `5aa50400` to `0x7e` and `0x7d` at `DAP_Connect`, as the `wch-link` personality does. It passes.
Image C calls `attach()` at `DAP_Connect`, without the unlock writes, and it passes after a power cycle of the target.
Thus the time of `attach()` causes the failure, not the unlock writes. The bridge must attach when it switches on (#107).
The cause in image A is not known. A probable cause: a command before the first scan changes the shared SWCLK and SWDIO pins.

### One wire

The CH32V006 runs, in order. The target has its own supply, and it stays on unless a step says otherwise.
Each step other than step 3 runs OpenOCD with `init`, `halt` and memory accesses.

1. Image D without the unlock writes: fail. Each DMI read gives `ffffffff` with `op` 0.
2. Image D: fail, the same as step 1.
3. A test image with the `cmsis-dap` and `wch-link` personalities, the same bridge code as image D, and the USB ID of WCH. In the `wch-link` personality, `wlink status` passes on the same wiring.
4. The same image, switched to the `cmsis-dap` personality: pass.
5. Power cycle of the target only, then the same image: pass.
6. Image D again, then a power cycle of the target: pass. The file `openocd-ch32v006.pcapng` is this run.

On one wire, `ffffffff` is a frame without an answer of the target.
Steps 1 and 2 had no power cycle of the target. Thus these runs do not show if a power cycle alone recovers the target, or if the `wlink status` connect was necessary.
The cause of the failure in steps 1 and 2 is not known. The runs of steps 1 to 5 are not in the repository.

### Errors

The capture `openocd-ch32v208-unplug.pcapng` shows the DMI error model (`docs/cmsis-dap.md`, "DMI errors") on hardware:

1. With the target connected, `halt` and `mdw` pass.
2. Without SWDIO, the poll of OpenOCD reads `dmstatus` with `op` 2. OpenOCD takes the data `0xbad00002` (status 2: no target) as the value. It logs "Debugger is not authenticated to target Debug Module" and marks the target as not examined.
3. With SWDIO connected again, the sticky error stays. OpenOCD sends no `dmireset` in this capture, so each command fails.
4. `jtag arp_init` (Test-Logic-Reset) clears the error. `arp_examine`, `halt` and `mdw` then pass.

### Open points of these captures

- OpenOCD logs "Vector support with vlenb=0" and "S?aia detected with IMSIC" at examine, and "halted due to undefined" after the recovery. The hardware test (#109) explains these messages: see [OpenOCD messages at examine](../cmsis-dap.md#openocd-messages-at-examine).
- The flash commands of both tools are not in these captures.
- probe-rs on a one-wire target is not in these captures.
- The one-wire captures have no run of image C (without the unlock writes).
- On one wire, a frame without an answer gives `ffffffff` with `op` 0, not `op` 2. Thus the host sees no error. Check this in the error model (`docs/cmsis-dap.md`, "DMI errors").
