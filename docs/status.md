# Status

This document lists the functions of the firmware and the state of the hardware tests.
The main target is the OpenMUPP v1 probe board (CH32V307, see [OpenMUPP v1 probe board](boards.md#openmupp-v1-probe-board)).
The firmware also builds for the boards in [Other boards](../README.md#other-boards) of the README.
The project developed and tested most functions on these boards before OpenMUPP v1 was available.

## OpenMUPP v1

The firmware has these functions on OpenMUPP v1:

- The board definition has the pins of the level shifters, the power functions and the ID dividers. The start-up sets these pins to a safe state.
- The debug engines and the UART bridge reach the target through the level shifters. Each engine runs a connect self-test on TCK and TDI when it starts.
- The firmware switches the target supply at 3.3 V or 5 V.
  It refuses 5 V when `+5V` is below 4.4 V or above 5.5 V.
  On a power fault, it turns the supply off and reports the fault (see [Target power on OpenMUPP v1](boards.md#target-power-on-openmupp-v1)).
- It reads the adapter ID and the board ID.
  It refuses the target on an adapter fault or an unknown board revision. The state LED is then continuously on.
- USB runs at high speed (480 Mbit/s) in the CMSIS-DAP image.
- The DFU bootloader `openmupp-bootloader` writes the `app-8k` image with `dfu-util` (see [OpenMUPP v1: DFU bootloader](firmware-update.md#openmupp-v1-dfu-bootloader)).
- `tools/mupp_probe.py` starts the bootloader (`switch bootloader`) and resets the probe (`reset`).
- The UART bridge uses UART7 with DMA. It has a single-wire mode on the TX line (see [Single-wire mode](uart-bridge.md#single-wire-mode)).
- A Debug build sends the debug log to UART5 (see [Debug log](debug-log.md#debug-uart)).
- A host control (`ID_DAP_Vendor5`) sets VPP for the hardware test (see [VPP control](cmsis-dap.md#vpp-control)). The AVR and PIC high-voltage protocols do not use VPP yet.

## Personalities

The Versaloon and CMSIS-DAP images have these functions:

- They set up the board and show the target power, the command activity and the USB state on the LEDs.
- They enumerate as a USB composite device: one debug interface and a CDC-ACM serial port.
- They bridge the serial port to the UART.
  The bridge continues to run during long JTAG and SWD commands.
- A host request starts the bootloader in the `app-8k` image (see [Start the bootloader](firmware-update.md#start-the-bootloader)).
  On the STM32F1 boards, the key at a reset also starts it.
  On OpenMUPP v1, the key is on BOOT0: the key at a reset starts the ROM ISP bootloader of WCH.

The Versaloon personality has the USB identity of the original Versaloon firmware:

- It receives Versaloon commands and replies to the common commands, for example `GET_INFO`.
- It runs the `USB_TO_ALL` command of OpenOCD, with the `INFO`, `DELAY`, `GPIO`, `JTAG_RAW`, `SWD` and `POWER` blocks. OpenOCD controls SRST and TRST through the `GPIO` block.
- It shifts JTAG bits through the `JTAG_RAW` block, with a fixed TCK frequency or with adaptive clocking (RTCK).
  The Mini and the CH32V307 boards have a fast path with SPI and DMA, up to 9 MHz TCK.
  The other bits and speeds use GPIO bit-bang.
  On the CH32V307, the fast path needs a link from TCK (PB13) to PC10 (see [JTAG fast path](usb-device.md#jtag-fast-path)).
  OpenMUPP v1 has this link on the board, through R10.
  On the YD-CH32V307VCT6, the fast path is tested with OpenOCD and pyOCD with a wire as the link. With a resistor as the link, its JTAG-DP transfers fail ([issue #16](https://github.com/OpenMUPP/OpenMUPP/issues/16)).
- It runs SWD transfers with GPIO bit-bang through the `SWD` block, at the SWCLK frequencies of the OpenOCD `vsllink` driver.
- It switches the 3.3 V target supply through the `POWER` block, and replies the target voltage to `GET_TVCC`. It refuses to switch on a target that has its own supply.

The CMSIS-DAP personality is a CMSIS-DAP v2 probe:

- It runs SWD and JTAG transfers, and `DAP_TransferAbort` during a transfer command.
- It switches the target supply (3.3 V, and 5 V on OpenMUPP v1) and reads the target voltage with `ID_DAP_Vendor2`, with the protections of the `POWER` block (see [cmsis-dap.md](cmsis-dap.md#target-power)).
- It has a RISC-V bridge: the JTAG port can be an emulated RISC-V DTM on the WCH debug wire (see [RISC-V DTM bridge](cmsis-dap.md#risc-v-dtm-bridge)).
- On the CH32V307 boards, it captures SWO in UART mode and in Manchester mode with `DAP_SWO_Data` or the SWO trace endpoint (see [cmsis-dap.md](cmsis-dap.md#swo-commands)). The `DAP_UART_*` commands can take the UART from the CDC-ACM serial port (see [cmsis-dap.md](cmsis-dap.md#uart-commands)).
- On the CH32V307 boards, its HID interface has the AVR commands of the Microchip EDBG tools (`MUPP_CMSIS_DAP_AVR`): ISP programming, debugWIRE programming and debug, JTAG programming, and JTAG debug with hardware breakpoints, software breakpoints and data breakpoints (see [avr.md](avr.md)).

A multi image has more than one personality, for example CMSIS-DAP and WCH-Link.
A switch command, the switch request on EP0 or the key selects the next personality, and a flash page stores it.

The WCH-Link personality is a WCH-LinkE in RISC-V mode:

- It reads and writes the memory of a CH32V003 (one wire), a CH32V006 (one wire) and a CH32V20x (two wires), and it erases, programs, protects and unprotects the flash.
- It has no serial port and no UART bridge. Only a board with a debug UART has a debug log.
- A multi image can have it. The switch commands of the other personalities, the switch request on EP0 and the key switch to it. The switch request on EP0 and the key switch from it.

## Hardware test status

### OpenMUPP v1

These tests passed on OpenMUPP v1 with the `openmuppv1-cmsis-dap` image:

- USB at high speed.
- The board ID and the adapter ID: the Arm adapter reads as slot 4, and the pin-header adapter reads as slot 5. `tools/mupp_probe.py adapter` shows them.
- The target supply at 3.3 V and at 5 V. On a USB port with `+5V` at about 5.8 V, the firmware refuses 5 V and the supply stays as it was. The blue LED shows the target voltage.
- The UART bridge against a second OpenMUPP v1: 115200 baud, 1 Mbaud and 6 Mbaud, with 8N1, 8E1 and 7O1, in both directions, with no errors.
- SWD to an STM32F103 target, also after `tools/mupp_probe.py reset`.
- SWO on a Nucleo-F401RE target at 3.3 V ([issue #5](https://github.com/OpenMUPP/OpenMUPP/issues/5)):
  - UART mode from 10 kbaud to 9 Mbaud, with `DAP_SWO_Data` and the SWO trace endpoint: no lost lines.
  - Manchester mode at 250 and 500 kbit/s with `DAP_SWO_Data` and the trace endpoint, and with OpenOCD at 50, 250 and 500 kbit/s: no lost lines.
  - The host tools OpenOCD and pyOCD at 1 Mbaud in UART mode: no lost lines. probe-rs decoded the capture, with one bad spot near the start.
- `tools/mupp_probe.py switch bootloader`: the probe enumerates as `OpenMUPP Bootloader` with the same serial number.

These hardware tests on OpenMUPP v1 are open:

- The VPP driver ([issue #154](https://github.com/OpenMUPP/OpenMUPP/issues/154)).
- The single-wire mode of the UART ([issue #151](https://github.com/OpenMUPP/OpenMUPP/issues/151)).
- The TCK link of the JTAG fast path ([issue #16](https://github.com/OpenMUPP/OpenMUPP/issues/16)).
- SWO at low target voltages through the level shifter ([issue #5](https://github.com/OpenMUPP/OpenMUPP/issues/5)).
- The functions that need a target or an adapter: JTAG, the one-wire RISC-V engine, AVR ISP, debugWIRE and AVR JTAG.
  The same functions passed on the YD-CH32V307VCT6 board (see below).

### YD-CH32V307VCT6

The YD-CH32V307VCT6 board has the MCU family of OpenMUPP v1, but no level shifters and no target supply.
These tests passed on it:

- The JTAG fast path with OpenOCD from 1125 to 9000 kHz, and JTAG-DP transfers with pyOCD at 2250, 4500 and 9000 kHz, with a wire as the TCK link (see [CH32V307 JTAG](clock-pacing.md#ch32v307-jtag)).
- The UART bridge loopback from 9600 baud to 6 Mbaud, and the `DAP_UART_*` loopback up to 2 Mbaud (see [YD-CH32V307VCT6 development board](boards.md#yd-ch32v307vct6-development-board)).
- AVR: ISP programming and JTAG programming on an ATmega32U4. JTAG debug with hardware breakpoints, software breakpoints and data breakpoints on an ATmega32U4 and an ATmega2560. debugWIRE on an ATtiny85 (see [avr.md](avr.md#hardware-test) and [JTAG debug hardware test](avr.md#jtag-debug-hardware-test)).
- The WCH-Link personality with a CH32V003 on one wire: wlink (status, dump, regs, halt, resume, reset, erase, flash, protect, unprotect) and minichlink (info, read, write) (see [wch-link.md](wch-link.md#ch32v003-on-the-yd-ch32v307vct6-one-wire)).

### STM32F1 boards

These tests passed on the Versaloon Mini, except where an item names another board:

- The Versaloon personality with OpenOCD in JTAG and SWD mode.
- The CMSIS-DAP personality on Linux with OpenOCD, pyOCD and probe-rs in SWD mode (see [cmsis-dap.md](cmsis-dap.md#hardware-test)), and with OpenOCD and pyOCD in JTAG mode (see [cmsis-dap.md](cmsis-dap.md#jtag-hardware-test)).
- The RISC-V bridge of the CMSIS-DAP personality with a CH32V003, a CH32V006 and a CH32V208 (see [Bridge hardware test](cmsis-dap.md#bridge-hardware-test)).
- The multi image `versaloon,cmsis-dap,wch-link` on the Mini, on Linux (see [usb-device.md](usb-device.md#multi-image-hardware-test)).
  The Versaloon personality passed with OpenOCD on SWD and JTAG.
  The CMSIS-DAP personality passed with OpenOCD, pyOCD and probe-rs on SWD and JTAG. probe-rs ran on JTAG only at 1000 kHz.
- The WCH-Link personality with a CH32V208 on two wires and a CH32V003 on one wire: wlink (status, dump, regs, halt, resume, reset, erase, flash, protect, unprotect) and minichlink (info, read, write) (see [wch-link.md](wch-link.md#hardware-test)).
  On the Mini, a minichlink read of the full CH32V003 flash fails.
- The WCH-Link personality on the ST-Link/V2-1 with a CH32V006 on one wire: probe-rs `info`, a read and a download with verify (see [Use the probe](wch-link.md#use-the-probe)).
- The ST-Link/V2 mini clone: the bootloader, and SWD with a target (see [ST-Link/V2 mini clone](stlinkv2-mini-clone.md#hardware-test)).

The WeAct STM32H523 core board was an earlier development board. These tests also passed on it:

- The multi image `versaloon,cmsis-dap` (see [usb-device.md](usb-device.md#multi-image-hardware-test)).
- The WCH-Link personality with a CH32V006 on one wire and a CH32V208 on two wires (see [wch-link.md](wch-link.md#ch32v006-on-the-weact-stm32h523-one-wire)).

### Open tests on all boards

- Windows, also the driverless WinUSB binding.
- The key switch on the STM32F1 boards.
- The WCH-Link personality of a multi image.
