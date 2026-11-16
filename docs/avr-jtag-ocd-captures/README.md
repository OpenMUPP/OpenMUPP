# AVR JTAG OCD captures

This directory holds the evidence for [avr-jtag-ocd.md](../avr-jtag-ocd.md).
[AVR JTAG OCD research](../avr-jtag-ocd-research.md) tells how we made the captures.

## Setup

| Item | Value |
|---|---|
| Target | Arduino Mega2560 board: ATmega2560 revision F, IDCODE `0x5980103F`, OCD revision 4, 16 MHz, 5 V from its own USB |
| Fuses during the tests | hfuse `0x18` (JTAGEN and OCDEN programmed). The original value on the board was `0xD8`. |
| Debugger | MPLAB PICkit 5, application 02.06.23, USB ID `04d8:9036` |
| Host tools | MPLAB X 6.35 with the PICkit 5 tool pack 3.0.1195, avrdude 8.3 |
| JTAG clock | About 450 kHz (avrdude), 200 kHz (MPLAB X) |
| Logic analyzer | Saleae Logic Pro 8, threshold 3.3 V, 50 MS/s (MPLAB X sessions) and 100 MS/s (avrdude) |
| Channels | 0: TDO, 1: TCK, 2: RESET, 3: TDI, 4: TMS. The probe connected on the PICkit 5 side of the cable. |

Test program: a loop that increments `counter8`, calls `step_me()` and toggles the LED on PB7.
`step_me()` writes `buffer[]` and adds to `counter16` (data address `0x210`).
In the MPLAB X build, `main` starts at word `0xA3` and `step_me` at word `0xCC`.
The program for the `BREAK` test has a `break` instruction at the start of `step_me` (word `0x88`).

## Files

| File | Content |
|---|---|
| `m2560-avrdude-sig.csv` | Raw logic analyzer data of `avrdude -c pickit5_jtag -p m2560` (signature read). One row for each state change, time in seconds. |
| `m2560-avrdude-sig-jtag.txt` | The same capture, decoded with `jtagdec.py`. It shows the JTAG programming flow of the datasheet. |
| `m2560-mplab-session-jtag.txt` | Decoded JTAG of an MPLAB X debug session: program, start, pause, single steps, a hardware breakpoint, a watch, reset, end |
| `m2560-mplab-session-calls.txt` | The debugger scripts that MPLAB X sent in that session, with time and parameters, from a USB capture |
| `m2560-mplab-swbp-jtag.txt` | Decoded JTAG of an MPLAB X session with software breakpoints: set, hit, single steps, continue, remove |
| `m2560-mplab-swbp-calls.txt` | The script calls of that session |
| `m2560-ocd-experiments.txt` | OCD register reads after single scripts: each type of stop, each breakpoint mode, the mask mode, the `BREAK` instruction, a write test |
| `jtagdec.py` | The JTAG decoder: `python3 jtagdec.py file.csv` |

The raw logic analyzer data of the two MPLAB X sessions (23 MB and 72 MB) is not in the repository.
The decoded files have all IR and DR scans of these captures, but not the exact timing of each TCK edge.

The USB captures are not in the repository. MPLAB X sends the Microchip debugger scripts inline, so the USB data contains Microchip code.
The `-calls.txt` files list only the script names and parameters.

## Decoded JTAG format

`jtagdec.py` follows the TAP state machine on each rising edge of TCK. It prints one line for each scan:

```
     37.494516  IR[ 4] in=0x8 out=0x1
     37.494565  IR[ 4] in=0xB out=0x1
     37.494699  DR[21] in=0x0D0000 out=0x0D000C
```

- The first column is the time in seconds from the start of the capture.
- `IR[n]` or `DR[n]` is the scan type and its length in bits.
- `in` is the data on TDI, `out` is the data on TDO. Both are LSB first: bit 0 is the first bit of the scan.
- `RESET -> 0` and `RESET -> 1` show the edges of the RESET line.
- `idle xN` counts TCK cycles in Run-Test/Idle. `tms1 xN (reset path)` counts cycles in Test-Logic-Reset.

## Time alignment

The `-calls.txt` files use the time of the USB capture. The `-jtag.txt` files use the time of the logic analyzer.
The difference is constant in one session:

| Session | Logic analyzer time |
|---|---|
| `m2560-mplab-session` | USB time − 1.170 s |
| `m2560-mplab-swbp` | USB time − 1.1625 s |

We found the offset from the FORCE_BREAK scans (IR `0x8`): there is one for each `Halt_JTAG` call.
A script starts 1 to 3 ms before its USB time in the logic analyzer data. Thus use the scan content to find the exact border between two scripts.
