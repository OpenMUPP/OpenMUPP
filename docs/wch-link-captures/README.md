# WCH-LinkE captures

This directory holds the captures of a real WCH-LinkE.
They are the evidence for [wch-link-protocol.md](../wch-link-protocol.md).

## Setup

| Item | Value |
|---|---|
| Probe | WCH-LinkE, USB ID `1a86:8010`, `bcdDevice` 2.19, product string `WCH-Link` (from `lsusb`, not in the captures) |
| Probe type | wlink reports `WCH-Link v2.19(v39) (WCH-LinkE-CH32V305)`. The info response holds `02 13 12 00` (see [Command `0x0d`](../wch-link-protocol.md#command-0x0d-info-connect-quit)). The source of `v39` is open |
| Target 1 | CH32V003F4P6 (16 KB flash), chip ID `0x00300500`, one-wire interface on PD1 |
| Target 2 | CH32V208WBU6 (160 KB flash), chip ID `0x2080053c`, two-wire interface on PA13 (SWDIO) and PA14 (SWCLK), external power |
| Host tools | wlink 0.1.2 and minichlink (a local build, version not recorded). The `v006-` captures use wlink 0.1.3 and minichlink from ch32fun commit `a800cbd` |
| Target 3 (`v006-` files) | CH32V006K8U6 (62 KB flash), chip ID `0x00600620`, QingKe V2C core. The probe is the same WCH-LinkE as for the other targets (firmware 2.19, info response `02 13 12 00`). wlink 0.1.3 shows its version as `v3.3(v39)`. The cause of this display is open |
| USB capture | Linux `usbmon` on the bus of the probe, filtered to the probe address |
| Logic analyzer | Saleae Logic Pro |

## USB captures (`.pcapng`)

Each file has the traffic of one tool command.
The file name gives the target, the tool and the command, for example `v003-wlink-status.pcapng`.

| Part of the name | Meaning |
|---|---|
| `v003-` | Target CH32V003 |
| `v208-` | Target CH32V208 |
| `v006-` | Target CH32V006 |
| `wlink-` | wlink command |
| `mc-` | minichlink command |
| `raw-errors` | Packets sent by hand: unknown commands, wrong lengths and unknown sub-commands |

Notes:

- The captures of `wlink flash` and the capture `v208-mc-write.pcapng` are not in this repository. They contain a flash stub that the tool sends to the target, and this project has no license for that code. References to `v003-wlink-flash.pcapng`, `v003-wlink-flash-noreset.pcapng`, `v003-wlink-flash-protected.pcapng`, `v006-wlink-flash.pcapng`, `v208-wlink-flash.pcapng` and `v208-mc-write.pcapng` name these captures.
- The `v006-` set has no `raw-errors` or `connect-fail` file.
- `v006-wlink-reset2.pcapng` is the reset that follows `v006-wlink-protect.pcapng`.
- `v006-wlink-dump-verify.pcapng` reads back 4 KB of random data after a flash.
- There is no minichlink halt and resume capture for the V006. `minichlink -T` waits for input. The resume file showed no command and was dropped. `v006-mc-reboot.pcapng` is the minichlink reboot (`-b`).

- `v003-mc-write-head.pcapng` holds only the first 4000 frames of the minichlink write. The full capture had 13 MB.
- On the CH32V208, the minichlink write has 4 KB only.

## Logic analyzer captures (`.csv`)

Each digital file has one row for each state change, with the time in seconds.
Time zero is the first falling edge of SWDIO (SWIO on one wire).
The logic threshold was not recorded.
The analog files have one row for each sample, with the voltages of SWDIO and SWCLK.

| File | Command | Sample rate | Channels |
|---|---|---|---|
| `v208-wlink-status-timing.csv` | `wlink status` (speed `01`) | 500 MS/s | 0: SWDIO (PA13), 1: SWCLK (PA14), 2: NRST |
| `v208-wlink-status-medium-timing.csv` | `wlink --speed medium status` (speed `02`) | 500 MS/s | As above |
| `v208-wlink-status-low-timing.csv` | `wlink --speed low status` (speed `03`) | 500 MS/s | As above |
| `v208-wlink-status-analog-connect.csv` | `wlink status`, the first five connect frames (−0.2 to 1.2 ms) | 12.5 MS/s | SWDIO and SWCLK in volts |
| `v208-wlink-status-analog-frames.csv` | `wlink status`, the first frames of 54 clocks (46.0 to 47.1 ms) | 12.5 MS/s | SWDIO and SWCLK in volts |
| `v003-wlink-status-timing.csv` | `wlink status` (speed `01`) | 500 MS/s | 0: SWIO (PD1). 1 and 2: not connected (constant 0) |
| `v003-wlink-status-medium-timing.csv` | `wlink --speed medium status` | 500 MS/s | 0: SWIO (PD1) |
| `v003-wlink-status-low-timing.csv` | `wlink --speed low status` | 500 MS/s | 0: SWIO (PD1) |
| `v003-wlink-dump-timing.csv` | `wlink dump 0x08000000 256` (speed `01`) | 500 MS/s | 0: SWIO (PD1) |
| `v003-wlink-dump-low-timing.csv` | `wlink --speed low dump 0x08000000 256` | 500 MS/s | 0: SWIO (PD1) |

The `v006-wlink-*-timing.csv` files use the same format and a 500 MS/s rate.
Channel 0 is SWIO (SWDIO) on PD1 and channel 1 is SWCLK on PB3. Both channels are on the target board, not on the probe. Channels 2 and 3 are not connected.
The trigger is a falling edge on SWDIO.
Until 76 ms, the probe tries two wires and the V006 does not answer. After that, the traffic is on channel 0 only. `wch_link_frames_1wire.py` decodes it from 76 ms: `python3 wch_link_frames_1wire.py v006-wlink-status-timing.csv 76`.

## Decoders

| Tool | Input | Output |
|---|---|---|
| `tools/wch_link_frames_2wire.py` | The CH32V208 digital files | The DMI accesses of the 54-clock frames (see [Wire frame format](../wch-link-protocol.md#wire-frame-format)) |
| `tools/wch_link_frames_1wire.py` | The CH32V003 digital files | The DMI accesses of the one-wire frames (see [One-wire frame format](../wch-link-protocol.md#one-wire-frame-format-ch32v003)) |
