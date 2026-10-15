# Status

This document lists the functions of the firmware and the state of the hardware tests.
The [plan](plan.md) has the work items.

The firmware has these functions:

- It sets up the board and shows the target power, the command activity and the USB state on the LEDs.
- It enumerates as a USB composite device: one debug interface and a CDC-ACM serial port.
- It bridges the serial port to the UART.
  The bridge continues to run during long JTAG and SWD commands.
- It starts the bootloader on request, for a firmware update.

The Versaloon personality has the USB identity of the original Versaloon firmware:

- It receives Versaloon commands and replies to the common commands, for example `GET_INFO`.
- It runs the `USB_TO_ALL` command of OpenOCD, with the `INFO`, `DELAY`, `GPIO`, `JTAG_RAW`, `SWD` and `POWER` blocks. OpenOCD controls SRST and TRST through the `GPIO` block.
- It shifts JTAG bits through the `JTAG_RAW` block, with a fixed TCK frequency or with adaptive clocking (RTCK). The Mini and the OtherUse use SPI and DMA up to 9 MHz TCK. The other bits and speeds use GPIO bit-bang.
- It runs SWD transfers with GPIO bit-bang through the `SWD` block, at the SWCLK frequencies of the OpenOCD `vsllink` driver.
- It switches the 3.3 V target supply through the `POWER` block, and replies the target voltage to `GET_TVCC`. It refuses to switch on a target that has its own supply.

The CMSIS-DAP personality is a CMSIS-DAP v2 probe:

- It runs SWD and JTAG transfers, and `DAP_TransferAbort` during a transfer command.
- It has no SWO and no `DAP_UART_*` commands. Use the CDC-ACM serial port for the UART.

The dual image has both personalities.
A switch command or the key selects the other personality, and a flash page stores it.

The WCH-Link personality is a WCH-LinkE in RISC-V mode:

- It reads and writes the memory of a CH32V003 (one wire) and a CH32V20x (two wires), and it erases, programs, protects and unprotects the flash.
- It has no serial port, no UART bridge and no debug log, and it is not part of the dual image.
- On a CH32V208 (work item #71), wlink (status, dump, erase, flash, protect, unprotect) and minichlink (info, read, write) work (see [docs/wch-link.md](wch-link.md#hardware-test)).

Hardware test status:

- The Versaloon personality passed the OpenOCD test in JTAG and SWD mode.
- The CMSIS-DAP personality passed the test on Linux with OpenOCD, pyOCD and probe-rs in SWD mode (see [docs/cmsis-dap.md](cmsis-dap.md#hardware-test)).
- The CMSIS-DAP personality passed the test on Linux with OpenOCD and pyOCD in JTAG mode (see [docs/cmsis-dap.md](cmsis-dap.md#jtag-hardware-test)).
- probe-rs in JTAG mode, Windows (also the driverless WinUSB binding) and the dual image need a test on the hardware.
