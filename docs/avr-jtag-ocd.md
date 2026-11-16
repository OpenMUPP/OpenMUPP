# AVR JTAG on-chip debug protocol

This document describes the on-chip debug (OCD) protocol of the classic AVR chips with a JTAG port.
Examples of these chips are the ATmega16, ATmega32U4 and ATmega2560.
Microchip does not publish this protocol. The datasheets name the JTAG instructions `0x8` to `0xB` only as "private".
The firmware does not use this protocol yet. It is the base for a later JTAG debug function of the AVR support.

The facts come from captures of a real debugger (MPLAB PICkit 5 with MPLAB X) and from tests on an ATmega2560.
[AVR JTAG OCD research](avr-jtag-ocd-research.md) tells how we got each fact and which steps went wrong.
A statement that has no capture or test behind it says so. No vendor code or vendor script is in this repository.

Status: the instructions, the OCD registers, the breakpoints, the instruction scan and the flash write in debug mode are known for OCD revision 4 (ATmega2560).
OCD revision 3 (for example the ATmega32U4) is known from the debugger scripts only, not from a test.
Some bits are open (see [Open questions](#open-questions)).

## Terms

| Term | Meaning |
|---|---|
| IR, DR | The JTAG instruction register and data register |
| TAP | The JTAG test access port, with its state machine |
| TCK, TMS, TDI, TDO | The JTAG signals: test clock, test mode select, test data in, test data out |
| LSB | Least significant bit |
| Reference debugger | The PICkit 5 with MPLAB X, the source of the captures |
| Probe | The OpenMUPP probe that will use this protocol |
| Reg N | OCD register N. CPU registers are r0 to r31. |
| SPM | The AVR instruction that writes the flash (store program memory) |
| SPMCSR | The control register of SPM, data address `0x57` on the ATmega2560 |
| EIND, RAMPZ | AVR I/O registers that extend the address of `EIJMP` and of the flash access to more than 16 bits. I/O addresses `0x3C` and `0x3B`. |
| RWW | The read-while-write section of the flash |
| EDBG | The Microchip debugger protocol that avrdude and Bloom use |

## Notation

- A number with `0x` is hexadecimal. Example: `0x0D`.
- DR values are LSB first, as the decoder [`jtagdec.py`](avr-jtag-ocd-captures/jtagdec.py) prints them. Bit 0 is the first bit that goes into TDI.
- All program addresses (PC, breakpoints) are word addresses, unless the text says "byte address".

## Captures

The captures are in [`avr-jtag-ocd-captures/`](avr-jtag-ocd-captures/README.md), with the setup, the file names and the decoder.
The target is an ATmega2560 (revision F, IDCODE `0x5980103F`) at 5 V and 16 MHz.
A file name in parentheses, for example (`m2560-mplab-session-jtag.txt`), names the capture that shows a fact.
A time, for example 30.2920 s, is the time in that file.

## Fuses

The OCD needs two fuses in the high fuse byte. Both are programmed (0) for debug:

| Fuse | Bit | Without it |
|---|---|---|
| JTAGEN | 6 | The JTAG port is off. The pins (PF4 to PF7 on the ATmega2560 and ATmega32U4) are normal I/O pins. |
| OCDEN | 7 | JTAG programming works, but the OCD does not answer. The reference debugger reports status `0x104`, "Could not connect to OCD". |

The debugger can program OCDEN over JTAG, because JTAGEN is enough for JTAG programming. MPLAB X does not set OCDEN by itself.
The probe must tell the user about OCDEN, or set it with the user's consent.

## JTAG instructions

The IR is 4 bits.

| IR | Name | DR length | Use |
|---|---|---|---|
| `0x1` | IDCODE | 32 | Device ID |
| `0x4` | PROG_ENABLE | 16 | JTAG programming (datasheet) |
| `0x5` | PROG_COMMANDS | 15 | JTAG programming (datasheet) |
| `0x8` | FORCE_BREAK | — | Stop the core. The reference debugger sends it once for each halt. |
| `0x9` | RUN | — | Start the core, or execute one step (with BCR bit 13) |
| `0xA` | Instruction execute | 32 (revision 3), 38 (revision 4) | Execute one AVR instruction in the stopped core, or read a flash word |
| `0xB` | OCD register access | 21 | Read or write an OCD register |
| `0xC` | AVR_RESET | 1 | Hold the core in reset (datasheet) |

The names of `0x8` to `0xB` are ours.
Evidence for `0x8` and `0x9`: in the MPLAB X session, there are 3 FORCE_BREAK scans for 3 halts.
There are 21 RUN scans for 4 run commands and 17 single steps (`m2560-mplab-session-jtag.txt`, `m2560-mplab-session-calls.txt`).

## OCD register access

IR `0xB` selects a 21-bit DR:

| Bits | Content |
|---|---|
| 20 | 1 = write, 0 = read |
| 19..16 | Register number |
| 15..0 | Data |

A write is one scan with bit 20 set.
A read is two scans. The first scan sends the register number with bit 20 clear. The second scan returns `register << 16 | data`.
In the second scan, the reference debugger sends the same read again, or a read of reg 0 (`in=0x000000`). It never sends a write in that scan.
Each scan returns the result of the scan before it. After a write, the next scan returns the write command (example: `out=0x1100CC` at 138.5879 s).

While the core runs, reg D reads `0x0001` in the MPLAB X captures, where BCR bit 14 is set. In the experiments, BCR was 0, and all registers read 0 while the core ran, reg D too (`m2560-ocd-experiments.txt`).

## OCD registers

| Reg | Use |
|---|---|
| 0 | One-shot program breakpoint, address bits 15..0. BCR bit 11 enables it. The hit clears bit 11. |
| 1 | Program breakpoint, address bits 15..0. BCR bit 10 enables it. It stays enabled after a hit. |
| 2 | Program or data breakpoint, or the mask for reg 3. BCR bit 8 enables it. The mode is in BCR bits 6..5. |
| 3 | Program or data breakpoint. BCR bit 7 enables it. The mode is in BCR bits 4..3. |
| 4..7 | Probably the address bits 23..16 for reg 0 to reg 3, only on OCD revision 4. Only the values 0 and `0xFF` were seen. The scripts write `0x00FF` and `0xFFFF` to an unused register, and both read back as `0x00FF`. |
| 8 | BCR, the break control register |
| 9 | BSR, the break status register |
| C | OCDR: the last value that the core wrote to the I/O address `OCD_DATAREG`, in bits 15..8 |
| D | Control and status register |
| A, B, E, F | Not seen with a value other than 0 |

`OCD_DATAREG` is `0x31` on the ATmega2560 and the ATmega32U4.
The device description files (ATDF) of Microchip give it for each device, with `OCD_REVISION`.
The EDBG host tools send these values in the device context, so the probe needs no device table.

### BCR (reg 8)

| Bit | Meaning |
|---|---|
| 15, 14 | The reference debugger writes these bits. The hardware does not set them. The meaning is open. See [Debug operations](#debug-operations) for the values. |
| 13 | Single step. The step clears it. |
| 11 | Enable reg 0 (one-shot) |
| 10 | Enable reg 1 |
| 9 | Mask mode: reg 2 is a mask for reg 3 |
| 8 | Enable reg 2 |
| 7 | Enable reg 3 |
| 6..5 | Mode of reg 2 |
| 4..3 | Mode of reg 3 |

Mode values for reg 2 and reg 3:

| Mode | Stop on | Stop position |
|---|---|---|
| 0 | Data read. Probably also data write: see the note below. | After the instruction that makes the access |
| 1 | Data write | After the instruction |
| 2 | Data read | After the instruction |
| 3 | Program address (instruction fetch) | Before the instruction at the address |

Evidence: a data breakpoint on `counter16` (`0x210`) stops after `lds r18, 0x0210` in modes 0 and 2.
It stops after `sts 0x0210, r20` in mode 1.
In mode 3 it does not stop on the data address, but it stops at a program address (`m2560-ocd-experiments.txt`).
The test program reads `counter16` before it writes it. Thus the test does not show that mode 0 stops on a write.

Mask mode: a 1 bit in reg 2 means "compare this address bit".
With reg 3 = `0x00C0`, reg 2 = `0xFFF8` and BCR `0xC298`, the core stops at `0xC0`.
With reg 2 = `0xFFF0`, it stops at `0xCC`, the first executed address in the range `0xC0` to `0xCF`.

### BSR (reg 9)

The BSR shows the cause of the last stop:

| Bit | Cause |
|---|---|
| 0 | `BREAK` instruction |
| 1 | FORCE_BREAK (halt) |
| 2 | Reg 2 hit |
| 3 | Reg 3 hit |
| 4 | Reg 1 hit |
| 5 | Reg 0 hit |
| 7 | Single step |

Bit 6 was not seen.
The reference debugger does not read the BSR. These values come from the experiments (`m2560-ocd-experiments.txt`).

### Control and status register (reg D)

| Bit | Meaning |
|---|---|
| 15 | Probably a device-specific control bit. The ATmega2560 script writes `0x8000` here only behind a device condition that is false for this chip, and the captures have no write to reg D. We did not check the scripts of other devices. |
| 4 | Set when the OCDR holds data from the core. The reference debugger reads reg D until bit 4 is set, then it reads reg C. |
| 3 | Set while the core is stopped. The meaning is open. |
| 2 | The core is stopped |
| 0 | Set while the core runs, but only when BCR bit 14 is set (see above). The meaning is open. Use bit 2, not bit 0, to see if the core runs. |

Values: `0x0001` while the core runs (MPLAB X captures), `0x0000` while the core runs with BCR = 0 (experiments). `0x000C` while the core is stopped.
After FORCE_BREAK, reg D read `0x0000` for 70 ms before it changed to `0x000C` (30.2926 s to 30.3625 s).

## Instruction execute (IR 0xA)

The debugger reads and writes the CPU state with instructions that the stopped core executes.

| OCD revision | DR length | Layout |
|---|---|---|
| 3 | 32 | The instruction in bits 31..16 (from the debugger scripts, no test) |
| 4 | 38 | The instruction in bits 37..22. Bits 21..0: see below. |

Example on revision 4: the scan `0x383C800000` executes `LDI r31, 0x02` (opcode `0xE0F2`) (`m2560-mplab-session-jtag.txt`, 138.6136 s).

On revision 4, bits 21..0 of TDO return a PC value ("PC field" below):

- At a stop, the PC field of the first scan is PC + 1. Examples: `0xCD` after the reg 1 breakpoint at `0xCC` (138.8806 s), `0x1F001` after the reset stop at `0x1F000` (30.4458 s).
- Each injected instruction adds 2 to the PC field.
- After an `EIJMP`, the PC field is Z + 1.

These three rules describe the captures, but they do not agree with each other in one model. Thus the formula for the PC is open (see [Open questions](#open-questions)).

### Read a flash word

On revision 4, the reference debugger reads the flash with two IR `0xA` scans for each word (`m2560-mplab-swbp-jtag.txt`, 141.715 s to 141.749 s):

1. Bits 21..0 = word address. Bits 37..22 hold the word of the read before, with its two low bits clear (example: `0x4094` after `0x4097`). The meaning of these bits is open.
2. Bits 37..22 = `0xFFFF`, bits 21..0 = the current PC field (`in=0x3FFFC00106` at 141.7179 s). The scan returns the flash word in bits 37..22.

The data agree with this rule, which is an inference: each IR `0xA` scan with bits 21..0 not 0 reads the flash word at that address, and the next scan returns it.
Example: the second scan of a PC read sends the PC field `0xCD` (138.8809 s), and the next scan returns `0x70EF`, the word at `0xCD`.
A second scan with bits 21..0 = 0 was not tested.

Example: address `0xCC` returns `0x2FE8` (`mov r30, r24`), and address `0xCD` returns `0x70EF` (`andi r30, 0x0F`). Both are correct for the test program.
These scans do not move the PC: the PC field stays `0x106`.

### Read and write the CPU state

| Value | Method |
|---|---|
| Read the PC | The PC field of the first IR `0xA` scan after a stop, minus 1 |
| Read r0..r31 | For each register: `OUT OCD_DATAREG, rN`, read reg D until bit 4 is set, read reg C. A read of all 32 registers adds 64 to the PC field. |
| Read an I/O register | `IN r16, A`, then `OUT OCD_DATAREG, r16`. This changes r16. |
| Write r16..r31 | `LDI rN, value` |
| Write r0..r15 | `LDI r16, value`, then `MOV rN, r16`. This changes r16. |
| Write EIND | `LDI r16, value`, then `OUT 0x3C, r16`. This changes r16. |
| Write the PC | `LDI r31`, `LDI r30`, `WDR` (purpose not known), `LDI r16` + `OUT 0x3C, r16` (EIND), then `EIJMP`. This changes r16, r30, r31 and EIND. |

Evidence: the instruction scans before 138.6136 s in `m2560-mplab-session-jtag.txt` (write) and the scans of the register read after each stop (read).

Thus the order is important. Before each run or step, the reference debugger writes back:

1. r0..r31,
2. the PC,
3. r31, r30, EIND and r16 again.

On revision 4, the reference debugger loads Z with the target PC minus 11. Example: Z = `0x00BE` for the PC `0xC9` (the `SetPC_JTAG` parameter was `0xC9`).
The five instructions of step 3 (`LDI r31`, `LDI r30`, `LDI r16`, `OUT 0x3C`, `LDI r16`) come after the jump.
With the PC field rules above, they add 10 to Z + 1.
On revision 3, the script subtracts 5, not 11 (from the script, no test).
The probe must use the same offset, or execute the jump last.

## Debug operations

These are the sequences of the reference debugger on the ATmega2560 (`m2560-mplab-session-jtag.txt`).

| Operation | Sequence |
|---|---|
| Enter debug mode | Read IDCODE (30.2901 s). The script first reads reg D. It resets the TAP, sets up the pins and reads IDCODE only when the low byte of reg D is `0x00` or `0xFF`. The capture shows only this setup path, with no reg D read before it. |
| Halt | Read BCR, write BCR = (BCR & `0x9FFC`) \| `0x4000`. Read reg D (`0x0001`, the core runs). IR `0x8` (FORCE_BREAK). Read reg D until bit 2 is set. Then write BCR = BCR & ~`0x03E0` (clears bits 9 to 5: mask mode, reg 2 and the mode fields), and then BCR = `0xC000`. (30.2915 s to 30.3638 s) |
| Status | Read reg D. Bit 2 = stopped. |
| Run | Write back the CPU state, then IR `0x9` (RUN) |
| Single step | Write back the CPU state. Write BCR = (BCR & `0x9FFC`) \| `0x2000`: bit 14 clear, bit 13 set, usually `0xA000`. Then IR `0x9`. |
| After each stop | Read reg D. Write BCR = (BCR & `0x9FFC`) \| `0x4000`. Then read reg 8 and write it with bit 2 clear. |
| Reset | See below |

The probe can also read the BSR after a stop to get the cause of the stop. The reference debugger does not do this.

Reset (`DebugReset_JTAG`, 30.3666 s to 30.4441 s):

1. The script sends the scan `0x194000`, a write of `0x4000` to reg 9 (BSR). It probably wants to set BCR bit 15: it adds `0x8000` to BCR = `0xC000`, and the carry goes into the register field. The BSR does not change: the next scan returns `0x0002`. The probe does not send this scan.
2. Write BCR = `0xC000`. IR `0xC` (AVR_RESET) = 1.
3. Write BCR = `0xC298`: bits 9 and 7, mode 3 for reg 3. Write reg 2 = reg 3 = reg 6 = reg 7 = 0. This mask matches all addresses.
4. IR `0xC` = 0. The core starts, and stops at the first instruction.
5. Read reg D until bit 2 is set. Write BCR = `0xC000` to remove the mask breakpoint.

After the stop, the BSR shows bit 3.
In the capture the stop is at `0x1F000`, not at `0x0000`, because BOOTRST was programmed (hfuse `0x18`).

## Software breakpoints

The `BREAK` instruction (`0x9598`) stops the core when OCDEN is programmed.
The PC then points at the `BREAK`, and the BSR shows bit 0.
A plain run from that PC executes the `BREAK` again (`m2560-ocd-experiments.txt`).

MPLAB X puts `BREAK` into the flash with a cycle on one flash page (256 bytes on the ATmega2560).
The cycle has four parts: read the page, change the word, erase the page, write the page.
MPLAB X uses one cycle to set a breakpoint and one cycle to remove it.
To continue from a breakpoint, it uses two cycles: it writes the original instruction back, executes one single step, and writes `BREAK` again (`m2560-mplab-swbp-calls.txt`).
One cycle takes about 1.25 s at a TCK of 200 kHz. The page write is 1.1 s of it.
Thus the probe uses the hardware breakpoints first.

## Flash write in debug mode

The debugger writes the flash while the core stays in debug mode. It uses only IR `0xA`, `0xB` and `0x9`, not the JTAG programming instructions.
The AVR executes `SPM` only from the boot section. Thus the debugger moves the PC into the boot section first, and executes each `SPM` as a single step.
The sequence below comes from the instruction scans of `m2560-mplab-swbp-jtag.txt` (141.70 s to 142.92 s).

1. Save the state:
   1. Copy SPMCSR to r14 (`LD r14, Z` with Z = `0x0057`). This changes Z and r14, so it comes first.
   2. Read SREG (`IN r16, 0x3F`, value `0x35` in the capture), and write it back.
   3. Read EIND (`0x3C`) and RAMPZ (`0x3B`) through the OCDR.
   4. Write RAMPZ = bits 17..16 of the page byte address. The capture writes 0, because its page is below 64 KB.
2. Load Z = `0xFE00` and EIND = 1, then execute `EIJMP`. The PC is now word `0x1FE00`. This is the first word of the smallest boot section (512 words), so it is in the boot section for each BOOTSZ value.
3. Erase the page:
   1. Z = bits 15..0 of the page byte address. `r16 = 0x03` (PGERS and SPMEN). Y = `0x0057` (SPMCSR), then `ST Y, r16`.
   2. Write BCR = `0xA000` (single step). Execute `SPM` through IR `0xA`, then IR `0x9` (RUN).
   3. Write BCR = `0xC000`. Read SPMCSR once (`LD r16, Z` with Z = `0x0057`, then `OUT OCD_DATAREG, r16`).
   4. Execute `EIJMP` back to `0x1FE00`. Then do sub-steps 3.1 and 3.2 again with `r16 = 0x11` (RWWSRE and SPMEN), to enable the RWW section.
4. Fill the page buffer. For each word:
   1. Z = bits 15..0 of the byte address. r0:r1 = data. `r16 = 0x01` (SPMEN), `ST Y, r16`.
   2. Execute `SPM` as a single step.
   3. Read SPMCSR, then `EIJMP` back to `0x1FE00`.
5. Write the page: Z = bits 15..0 of the page byte address, `r16 = 0x05` (PGWRT and SPMEN), `ST Y, r16`, `SPM` as a single step (142.8983 s to 142.9000 s). Then enable the RWW section with `r16 = 0x11`.
6. Restore the state: SREG, SPMCSR (`ST Z, r14`), EIND and RAMPZ. Then write back the CPU registers that the sequence changes: r0, r1, r14, r16 and r28 to r31.

The reference debugger reads SPMCSR only once after each `SPM`. After the page write, the read gave `0x45` (142.9028 s): SPMEN was still set. MPLAB X did not read again.
The probe must poll SPMCSR until SPMEN is clear, because a page write takes some milliseconds.

At the end of the erase, the sequence wrote RAMPZ = `0x0C` (141.8070 s), not the saved value `0x00`. The cause is open (see open question 6).

The boot lock bits must allow `SPM` in the boot section.
`SPM` uses RAMPZ:Z as the byte address (datasheet). The capture writes only one page below 64 KB, so the RAMPZ value for a higher page is from the datasheet, not from a capture.

## Open questions

1. The meaning of BCR bits 15 and 14, and of reg D bits 3 and 0.
2. BSR bit 6.
3. The formula for the PC field of the 38-bit scan. The rules "PC + 1 at a stop", "+2 for each injected instruction" and "Z + 1 after `EIJMP`" do not agree in one model.
4. The two low bits of the instruction field in the first scan of a flash read.
5. OCD revision 3: no test yet. The ATmega32U4 is the planned test target.
6. The values that MPLAB X gives to its last script of the flash write (for example `0x35`, the saved SREG value), and the write of RAMPZ = `0x0C`.
7. The fastest TCK for the OCD. MPLAB X used 200 kHz. avrdude used about 450 kHz for JTAG programming. The datasheet limit for JTAG is a TCK below 1/4 of the CPU clock.
8. Mode 0 for a data write: not tested.
