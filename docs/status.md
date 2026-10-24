# Status

This document lists the functions of the firmware and the state of the hardware tests.
The [plan](plan.md) has the work items.

The Versaloon and CMSIS-DAP images have these functions:

- It sets up the board and shows the target power, the command activity and the USB state on the LEDs.
- It enumerates as a USB composite device: one debug interface and a CDC-ACM serial port.
- It bridges the serial port to the UART.
  The bridge continues to run during long JTAG and SWD commands.
- It starts the bootloader when you hold the key.
  The Versaloon personality also starts it on request, in the `app-8k` image.

The Versaloon personality has the USB identity of the original Versaloon firmware:

- It receives Versaloon commands and replies to the common commands, for example `GET_INFO`.
- It runs the `USB_TO_ALL` command of OpenOCD, with the `INFO`, `DELAY`, `GPIO`, `JTAG_RAW`, `SWD` and `POWER` blocks. OpenOCD controls SRST and TRST through the `GPIO` block.
- It shifts JTAG bits through the `JTAG_RAW` block, with a fixed TCK frequency or with adaptive clocking (RTCK). The Mini and the OtherUse use SPI and DMA up to 9 MHz TCK. The other bits and speeds use GPIO bit-bang.
- It runs SWD transfers with GPIO bit-bang through the `SWD` block, at the SWCLK frequencies of the OpenOCD `vsllink` driver.
- It switches the 3.3 V target supply through the `POWER` block, and replies the target voltage to `GET_TVCC`. It refuses to switch on a target that has its own supply.

The CMSIS-DAP personality is a CMSIS-DAP v2 probe:

- It runs SWD and JTAG transfers, and `DAP_TransferAbort` during a transfer command.
- It has no SWO. The `DAP_UART_*` commands can take the UART from the CDC-ACM serial port (see [cmsis-dap.md](cmsis-dap.md#uart-commands)).

A multi image has more than one personality, for example Versaloon and CMSIS-DAP (the old dual image).
A switch command or the key selects the next personality, and a flash page stores it.

The WCH-Link personality is a WCH-LinkE in RISC-V mode:

- It reads and writes the memory of a CH32V003 (one wire), a CH32V006 (one wire) and a CH32V20x (two wires), and it erases, programs, protects and unprotects the flash.
- It has no serial port, no UART bridge and no debug log. A multi image can have it. The switch commands of the other personalities, the switch request on EP0 and the key switch to it. The switch request on EP0 and the key switch from it.
- Its hardware test is in progress (work item #71). On a CH32V208, wlink (status, dump, erase, flash, protect, unprotect) and minichlink (info, read, write) work. On a CH32V006, wlink (status, dump, regs, erase, flash, protect, unprotect, halt, resume, reset) works and minichlink (read, write) works (see [wch-link.md](wch-link.md#hardware-test)).

Hardware test status:

- The Versaloon personality passed the OpenOCD test in JTAG and SWD mode.
- The CMSIS-DAP personality passed the test on Linux with OpenOCD, pyOCD and probe-rs in SWD mode (see [cmsis-dap.md](cmsis-dap.md#hardware-test)).
- The CMSIS-DAP personality passed the test on Linux with OpenOCD and pyOCD in JTAG mode (see [cmsis-dap.md](cmsis-dap.md#jtag-hardware-test)).
- The multi image `versaloon,cmsis-dap,wch-link` passed the test on Linux (see [usb-device.md](usb-device.md#multi-image-hardware-test)).
  The Versaloon personality passed with OpenOCD on SWD and JTAG.
  The CMSIS-DAP personality passed with OpenOCD, pyOCD and probe-rs on SWD and JTAG. probe-rs ran on JTAG only at 1000 kHz.
- Windows (also the driverless WinUSB binding), the key switch and the WCH-Link personality of the multi image need a test on the hardware.
