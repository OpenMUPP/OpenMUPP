# AVR JTAG on-chip debug protocol

This document describes the on-chip debug (OCD) protocol of the classic AVR chips with a JTAG port, for example the ATmega16, ATmega32U4 and ATmega2560.
Microchip does not publish this protocol. The datasheets name the JTAG instructions `0x8` to `0xB` only as "private".
The firmware does not use this protocol yet. It is the base for a later JTAG debug function of the AVR support.

The facts come from captures of a real debugger (MPLAB PICkit 5 with MPLAB X) and from tests on an ATmega2560.
[AVR JTAG OCD research](avr-jtag-ocd-research.md) tells how we got each fact and which steps went wrong.
A statement that has no capture or test behind it says so. No vendor code or vendor script is in this repository.

Status: the instructions, the OCD registers, the breakpoints, the instruction scan and the flash write in debug mode are known for OCD revision 4 (ATmega2560).
OCD revision 3 (for example the ATmega32U4) is known from the debugger scripts only, not from a test.
Some bits are open (see [Open questions](#open-questions)).

## Notation

- A number with `0x` is hexadecimal. Example: `0x0D`.
- JTAG data register (DR) values are shown LSB first, as the decoder [`jtagdec.py`](avr-jtag-ocd-captures/jtagdec.py) prints them. Bit 0 is the first bit that goes into TDI.
- "Reg N" is OCD register N, not a CPU register. CPU registers are r0 to r31.
- All program addresses (PC, breakpoints) are word addresses, unless the text says "byte address".

## Captures

The captures are in [`avr-jtag-ocd-captures/`](avr-jtag-ocd-captures/README.md), with the setup, the file names and the decoder.
The target is an ATmega2560 (revision F, IDCODE `0x5980103F`) at 5 V and 16 MHz.
A file name in parentheses, for example (`m2560-mplab-session-jtag.txt`), names the capture that shows a fact.

## Fuses

The OCD needs two fuses in the high fuse byte. Both are programmed (0) for debug:

| Fuse | Bit | Without it |
|---|---|---|
| JTAGEN | 6 | The JTAG port is off. The pins (PF4 to PF7 on the ATmega2560 and ATmega32U4) are normal I/O pins. |
| OCDEN | 7 | JTAG programming works, but the OCD does not answer. The PICkit 5 reports status `0x104`, "Could not connect to OCD" (`m2560-mplab-no-ocden`, not in the repository). |

The debugger can program OCDEN over JTAG, because JTAGEN is enough for JTAG programming. MPLAB X does not set OCDEN by itself.
A probe must tell the user about OCDEN, or set it with the user's consent.

## JTAG instructions

The IR is 4 bits.

| IR | Name | DR length | Use |
|---|---|---|---|
| `0x1` | IDCODE | 32 | Device ID |
| `0x4` | PROG_ENABLE | 16 | JTAG programming (datasheet) |
| `0x5` | PROG_COMMANDS | 15 | JTAG programming (datasheet) |
| `0x8` | FORCE_BREAK | — | Stop the core. The debugger sends it once for each halt. |
| `0x9` | RUN | — | Start the core, or execute one step (with BCR bit 13) |
| `0xA` | Instruction execute | 32 (revision 3), 38 (revision 4) | Execute one AVR instruction in the stopped core |
| `0xB` | OCD register access | 21 | Read or write an OCD register |
| `0xC` | AVR_RESET | 1 | Hold the core in reset (datasheet) |

The names of `0x8` to `0xB` are ours. In the MPLAB X session, the count of `0x8` scans is equal to the count of halts, and the count of `0x9` scans is equal to the count of run and step commands (`m2560-mplab-session-jtag.txt`, `m2560-mplab-session-calls.txt`).

## OCD register access

IR `0xB` selects a 21-bit DR:

| Bits | Content |
|---|---|
| 20 | 1 = write, 0 = read |
| 19..16 | Register number |
| 15..0 | Data |

A write is one scan with bit 20 set.
A read is two scans. The first scan sends the register number with bit 20 clear. The second scan returns `register << 16 | data`.
While the core runs, all registers read 0 (`m2560-ocd-experiments.txt`).

## OCD registers

| Reg | Use |
|---|---|
| 0 | One-shot program breakpoint, address bits 15..0. BCR bit 11 enables it. The hit clears bit 11. |
| 1 | Program breakpoint, address bits 15..0. BCR bit 10 enables it. It stays enabled after a hit. |
| 2 | Program or data breakpoint, or the mask for reg 3. BCR bit 8 enables it. The mode is in BCR bits 6..5. |
| 3 | Program or data breakpoint. BCR bit 7 enables it. The mode is in BCR bits 4..3. |
| 4..7 | Address bits 23..16 for reg 0 to reg 3. Only OCD revision 4 has them. The MPLAB X scripts write `0x00FF` to an unused one. |
| 8 | BCR, the break control register |
| 9 | BSR, the break status register |
| C | OCDR: the last value that the core wrote to the I/O address `OCD_DATAREG`, in bits 15..8 |
| D | Control and status register |
| A, B, E, F | Not seen with a value other than 0 |

`OCD_DATAREG` is `0x31` on the ATmega2560 and the ATmega32U4. The device description files (ATDF) of Microchip give it per device, with `OCD_REVISION`.
The EDBG host tools send these values in the device context, so a probe needs no device table.

### BCR (reg 8)

| Bit | Meaning |
|---|---|
| 15, 14 | Set while the core is stopped (`0xC000`). The exact meaning is open. |
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
| 0 | Data read or data write | After the instruction that makes the access |
| 1 | Data write | After the instruction |
| 2 | Data read | After the instruction |
| 3 | Program address (instruction fetch) | Before the instruction at the address |

Evidence: a data breakpoint on `counter16` (`0x210`) stops after `lds r18, 0x0210` in modes 0 and 2, and after `sts 0x0210, r20` in mode 1.
In mode 3 it does not stop on the data address, but it stops at a program address (`m2560-ocd-experiments.txt`).

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

### Control and status register (reg D)

| Bit | Meaning |
|---|---|
| 15 | Write 1: OCD on. The debugger writes `0x8000` when it enters debug mode. |
| 4 | Set when the OCDR holds data from the core. The debugger checks it before it reads reg C. To confirm. |
| 3 | Set while the core is stopped. The exact meaning is open. |
| 2 | The core is stopped |

The value is `0x000C` while the core is stopped.

## Instruction execute (IR 0xA)

The debugger reads and writes the CPU state with instructions that the stopped core executes.

| OCD revision | DR length | Layout |
|---|---|---|
| 3 | 32 | The instruction in bits 31..16 (from the debugger scripts, no test) |
| 4 | 38 | The instruction in bits 37..22. Bits 21..0 return a PC value. |

Example on revision 4: the scan `0x383C800000` executes `LDI r31, 0x02` (opcode `0xE0F2`) (`m2560-mplab-session-jtag.txt`, time 138.6136 s).

Each injected instruction moves the PC. The PC value in bits 21..0 grows by 2 for each injected instruction. The cause of the step of 2 is not known.

### Read and write the CPU state

| Value | Method |
|---|---|
| Read r0..r31 | For each register: `OUT OCD_DATAREG, rN`, read reg D until bit 4 is set, read reg C. A read of all 32 registers moves the PC by 64. |
| Read an I/O register | `IN r16, A`, then `OUT OCD_DATAREG, r16`. This changes r16. |
| Write r16..r31 | `LDI rN, value` |
| Write r0..r15 | `LDI r16, value`, then `MOV rN, r16`. This changes r16. |
| Write EIND | `LDI r16, value`, then `OUT 0x3C, r16`. This changes r16. |
| Write the PC | `LDI r31`, `LDI r30`, `WDR` (purpose not known), `LDI r16` + `OUT 0x3C, r16` (EIND), then `EIJMP`. This changes r16, r30, r31 and EIND. |

Evidence: the instruction scans before 138.6136 s in `m2560-mplab-session-jtag.txt` (write) and the scans of the register read after each stop (read).

Thus the order is important. Before each run or step, MPLAB X writes back:

1. r0..r31,
2. the PC,
3. r31, r30, EIND and r16 again.

After the `EIJMP`, the PC value in bits 21..0 is Z + 1. Each injected instruction after the jump adds 2.
The five instructions of step 3 (`LDI r31`, `LDI r30`, `LDI r16`, `OUT 0x3C`, `LDI r16`) add 10.
Thus MPLAB X loads Z with the target PC minus 11. Example: Z = `0x00BE` for the PC `0xC9`.
A probe must count the injected instructions after the jump in the same way, or execute the jump last.

## Debug operations

| Operation | Sequence |
|---|---|
| Enter debug mode | Read reg D. Write reg D = `0x8000`. |
| Halt | IR `0x8` (FORCE_BREAK). Read reg D until bit 2 is set. |
| Status | Read reg D. Bit 2 = stopped. |
| Run | Write back the CPU state, then IR `0x9` (RUN) |
| Single step | Write back the CPU state, BCR bit 13 = 1, then IR `0x9` |
| After each stop | Read the BSR. Write BCR with bit 14 set and the step bit clear (MPLAB X does it with its status script). |
| Reset | IR `0xC` (AVR_RESET) = 1, then 0. Before the release, BCR = bits 9 and 7, mode 3, reg 2 = reg 3 = 0. This mask matches all addresses, so the core stops at the reset vector. Then the BSR shows bit 3. |

## Software breakpoints

The `BREAK` instruction (`0x9598`) stops the core when OCDEN is programmed.
The PC then points at the `BREAK`, and the BSR shows bit 0.
A plain run from that PC executes the `BREAK` again (`m2560-ocd-experiments.txt`).

MPLAB X puts `BREAK` into the flash with a cycle on one flash page (256 bytes on the ATmega2560): read the page, change the word, erase the page, write the page.
It uses one cycle to set a breakpoint and one cycle to remove it.
To continue from a breakpoint, it uses two cycles: it writes the original instruction back, executes one single step, and writes `BREAK` again (`m2560-mplab-swbp-calls.txt`).
One cycle takes about 1.1 s at 200 kHz TCK. Thus a probe uses the hardware breakpoints first.

## Flash write in debug mode

The debugger writes the flash while the core stays in debug mode. It uses only IR `0xA`, `0xB` and `0x9`, not the JTAG programming instructions.
The AVR executes `SPM` only from the boot section. Thus the debugger moves the PC into the boot section first, and executes each `SPM` as a single step.
The sequence below comes from the instruction scans of `m2560-mplab-swbp-jtag.txt` (time 141.75 s to 142.92 s).

1. Read EIND (`0x3C`) and RAMPZ (`0x3B`) through the OCDR, to restore them later. Write RAMPZ = 0.
2. Load Z = `0xFE00` and EIND = 1, then execute `EIJMP`. The PC is now word `0x1FE00`. This is the first word of the smallest boot section (512 words), so it is in the boot section for each BOOTSZ value.
3. Erase the page:
   1. Z = page byte address. `r16 = 0x03` (PGERS and SPMEN). Y = `0x0057` (SPMCSR), then `ST Y, r16`.
   2. Write BCR = `0xA000` (single step). Execute `SPM` through IR `0xA`, then IR `0x9` (RUN).
   3. Write BCR = `0xC000`. Read SPMCSR (`LD r16, Z` with Z = `0x0057`, then `OUT OCD_DATAREG, r16`) until the operation is complete.
   4. Execute `EIJMP` back to `0x1FE00`. Then do step 1 and step 2 again with `r16 = 0x11` (RWWSRE and SPMEN), to enable the RWW section.
4. Fill the page buffer. For each word:
   1. Z = byte address. r0:r1 = data. `r16 = 0x01` (SPMEN), `ST Y, r16`.
   2. Execute `SPM` as a single step.
   3. Read SPMCSR, then `EIJMP` back to `0x1FE00`.
5. Write the page: as step 3, with the page write command. Then enable the RWW section.
6. Restore RAMPZ, EIND and the CPU registers.

The boot lock bits must allow `SPM` in the boot section.
We decoded step 5 from the script structure and from the count of RUN scans, not instruction by instruction.

## Open questions

1. The meaning of BCR bits 15 and 14, and of reg D bit 3. Reg D bit 4 is not confirmed.
2. BSR bit 6.
3. The cause of the PC step of 2 for each injected instruction, and the exact formula for the PC in bits 21..0.
4. OCD revision 3: no test yet. The ATmega32U4 is the planned test target.
5. The fastest TCK for the OCD. MPLAB X used 200 kHz. avrdude used about 450 kHz for JTAG programming. The datasheet limit for JTAG is a TCK below 1/4 of the CPU clock.
6. The register values that MPLAB X gives to its last script of the flash write. They look like saved register values. This is not confirmed.
