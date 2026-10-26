# ST-Link/V2 mini clone

The board `stlinkv2miniclone` is a USB stick debug probe.
Shops sell it as "ST-Link V2 mini".
It is a clone, not the official ST-Link/V2 design (board `stlinkv2`).
The board has no markings.

This document gives the pin map of the board, and how we measured it.

| Top side | Bottom side |
|---|---|
| ![Top side of the ST-Link/V2 mini clone](images/stlinkv2-mini-clone-top.jpg) | ![Bottom side of the ST-Link/V2 mini clone](images/stlinkv2-mini-clone-bottom.jpg) |

The top side has the MCU, the 8 MHz crystal and the two LEDs.
The four wires on the top side go to the SWD pads of the MCU (see [SWD pads of the MCU](#swd-pads-of-the-mcu)).

Status: the pin map is measured.
The firmware has not run on this board yet.

## MCU

The MCU is an STM32F103 in a 48-pin package.
The text on the package is not readable.
We read these values with SWD:

| Value | Read | Meaning |
|---|---|---|
| DPIDR | `0x1BA01477` | Arm Cortex-M3 debug port of ST. Some STM32 copies (for example CKS32) give a different value. |
| `DBGMCU_IDCODE` | `0x20030410` | STM32F103 medium density, revision X |
| Flash size register | 128 KB | The firmware uses only 64 KB (see [MCU family](boards.md#mcu-family)). |

The crystal is 8 MHz.

## Header

The header has 2 rows of 5 pins.
The text on the board names the pins: one row on each side of the board.
The triangle on the header connector marks pin 1, at the RST end.

| Pin | Signal | MCU pins | Pin | Signal | MCU pins |
|---|---|---|---|---|---|
| 1 | RST | PB6, PB5 | 2 | SWCLK (CLK) | PA5, PB13 |
| 3 | SWIM | PB7 to PB11 | 4 | SWDIO (DIO) | PB14, PB12 |
| 5 | GND | | 6 | GND | |
| 7 | 3.3V | | 8 | 3.3V | |
| 9 | 5V | | 10 | 5V | |

The pin numbers come from the position of the triangle.
We did not measure them: we measured the signal of each labelled pin.

The board connects 2 or more MCU pins to each signal pin of the header.
The firmware drives one MCU pin of each signal:

| Signal | Pin of the firmware | Other MCU pins | Board definition field |
|---|---|---|---|
| SWCLK, TCK | PA5 | PB13 | `debug.tck` |
| SWDIO, TMS | PB14 (output), PB12 (input) | - | `debug.tmsOut` (PB14), `debug.tmsIn` (PB12) |
| RST (target reset) | PB6 | PB5 | `debug.srst` |
| SWIM | - | PB7 to PB11 | not used |

The other MCU pins stay floating inputs after the reset.
Do not set them to output: they connect to the pin that the firmware drives.

SWDIO uses two MCU pins, as on the Versaloon Mini.
PB14 is the output. It has the timer output of the WCH-Link one-wire engine (TIM1_CH2N).
PB12 is the input, with the internal pull-up on.
The pull-up holds an open SWDIO line high.
Thus SWD without a target reads the acknowledge `0b111`, and the one-wire read bits have a pull-up (see [One-wire timer](wch-link.md#one-wire-timer)).
The SWIM pins have a pull-up on the board.
The firmware does not support SWIM (the STM8 debug protocol).

The 3.3V pins always supply power.
No GPIO switches them, so the board has no target power switch.
We did not measure the 5V pins.

## Signals that the header does not have

The board definition must give TDI, TDO and a UART.
These pins of the official ST-Link/V2 design are not connected on this board:

| Field | MCU pin | Result |
|---|---|---|
| `debug.tdi` | PA7 | Not connected. The board has no JTAG. Use SWD. |
| `debug.tdo` | PA6 | Not connected |
| `uart` | USART2, PA2 (TX) and PA3 (RX) | Not connected. The serial port of the USB-CDC bridge does not reach a target. |

## Other signals

| Signal | MCU pin | Note |
|---|---|---|
| LEDs | PA9 | Two LEDs on one pin. High: blue LED on. Low: red LED on. Digital input: the red LED glows a little. Analog mode: both LEDs are off. The firmware uses PA9 as a two-colour state LED: blue when idle, off for activity, red for a fault (see [Start-up and LEDs](boards.md#start-up-and-leds)). |
| Vref sense | PA0 | Divider 2 (1.648 V at VDDA 3.289 V). The divider reads the 3.3 V of the board. The header 3.3V pins are an output, so the probe cannot measure a target voltage. |
| USB | PA11, PA12 | The D+ pull-up is fixed. No GPIO switches it. |
| BOOT1 | PB2 | Resistor to GND |
| SWD of the MCU | PA13, PA14 | Pads on the top side, see [SWD pads of the MCU](#swd-pads-of-the-mcu) |

The board has no key and no power LED.

Other documents name the ST-Link/V2 for a board with a fixed USB pull-up, no key and no RTCK.
These statements also apply to this board.
The differences are the RST pin (PB6, not PB0), the SWDIO input pin (PB12), and the missing JTAG and UART pins on the header.

## SWD pads of the MCU

The top side has four pads on the long edge, next to the USB connector.
From the USB connector, the pads are:

1. VCC
2. SWDIO
3. SWCLK
4. GND

Connect a debug probe to these pads to write the firmware.

## Read protection of the original firmware

The original firmware has read protection level 1.
A debugger cannot read the flash.
When a debugger connects, the original firmware stops with a lockup (PC `0xFFFFFFFE`).
On the STM32F1, read protection blocks flash access for the CPU when a debugger connects.

The debugger can still write the RCC, GPIO and ADC registers, and the SRAM.
Thus you can measure the pin map without an erase of the flash.

To remove the read protection, do a mass erase.
The mass erase deletes the original firmware.
We did not do these steps on this board yet:

```sh
openocd -f interface/cmsis-dap.cfg -f target/stm32f1x.cfg -c "init; reset halt; stm32f1x unlock 0; exit"
```

Then disconnect the power of the board, and connect it again.

## How we measured the pin map

A Versaloon probe (CMSIS-DAP personality) was connected to the SWD pads.
OpenOCD wrote the GPIO registers.
The CPU was in lockup, so the original firmware did not change the pins.
A logic analyser (Saleae Logic Pro 8) was connected to the header pins SWIM, RST, SWCLK, SWDIO and the two 3.3V pins.

We did these tests:

1. Pull test. All free GPIO pins were inputs with the internal pull-down, then with the internal pull-up. A pin that did not follow the internal pull has an external pull-up, an external pull-down or a driver.
2. Drive sweep. Each free GPIO pin in turn was an output, in a fixed sequence. The pin at position N in the sequence sent N + 1 pulses. Thus the pulse count on a header pin identifies the MCU pin. After each step, OpenOCD read all GPIO inputs. When two MCU pins show the same input change, the board connects them.
3. USB pull-up test. During the drive sweep, D+ (PA12) stayed high with the internal pull-down. Thus no GPIO controls the D+ pull-up.
4. ADC test. The ADC measured PA0 to PA7, PB0 and PB1. The internal reference (VREFINT) gave the scale.
5. LED test. PA9 was high, then low, then an input, and we looked at the LEDs.

The test did not include PA11 to PA14: these pins are USB and SWD.
