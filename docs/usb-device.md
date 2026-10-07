# USB device

The dongle is a USB full-speed composite device with the USB identity of the original Versaloon firmware.
Thus unmodified OpenOCD (`vsllink` driver) finds it.
`src/usb/UsbDescriptors.h` has the descriptors.

| Item | Value |
|---|---|
| VID:PID | `0x0483:0xA038` (the OpenOCD default) |
| Device class | `0xEF/0x02/0x01` (miscellaneous, with interface association descriptors) |
| Manufacturer string | `Versaloon` |
| Product string | `Versaloon`. OpenOCD looks for this word. |
| Serial number string | The 96-bit chip UID in hexadecimal, 24 digits |
| Maximum power | 200 mA, bus-powered |

| Interface | Class | Endpoints | Function |
|---|---|---|---|
| 0 | Vendor-specific (`0xFF`) | bulk OUT `0x03`, bulk IN `0x82`, 64 bytes | Versaloon protocol (OpenOCD) |
| 1 | CDC (`0x02`), ACM | interrupt IN `0x81`, 16 bytes | Serial port, control interface |
| 2 | CDC data (`0x0A`) | bulk OUT `0x04`, bulk IN `0x84`, 64 bytes | Serial port, data interface |

An interface association descriptor (IAD) groups the interfaces 1 and 2.
The Call Management descriptor points to data interface 2.
The original firmware has the wrong value 1 there.
There is one serial port only.

`src/usb/UsbStack.cpp` runs the libArT USB device stack on the STM32F1 USB peripheral.
The USB interrupt only puts the events into a queue.
The main loop runs the USB protocol.
The board definition supplies the USB D+ pull-up pin.
The device connects when `main()` starts the USB device.
A board without a pull-up pin (ST-Link/V2) has a fixed pull-up.

Interface 0 is a placeholder in the current firmware.
It sends each OUT packet back without change, with the same size (`src/usb/VersaloonFunction.cpp`).
The Versaloon command protocol replaces this echo in a later version.
The only exception is the command `VERSALOON_FW_UPDATE` (see [Firmware update](firmware-update.md)).

The serial port is the [UART bridge](uart-bridge.md).
In a debug build with `VSL_DEBUG_CDC`, the serial port carries the debug log instead (see [Debug log](debug-log.md)).

## Check the enumeration

The host unit tests check the descriptors.
To check the device on the hardware, connect the dongle and do these steps.

On Linux:

1. Run `lsusb -v -d 0483:a038`. Make sure that the output shows the interfaces and endpoints of the tables above, and the IAD.
2. Make sure that the kernel log (`dmesg`) shows a new `/dev/ttyACM*` port.
3. Connect the UART TX pin to the UART RX pin (PA9 to PA10 on the Mini). Open the port in a terminal program, for example `picocom /dev/ttyACM0`. Make sure that the typed characters come back.
4. Run OpenOCD with `-f interface/vsllink.cfg`. OpenOCD finds the device, but fails at the protocol, because the protocol is not available yet.

On Windows 10 or later:

1. Make sure that the Device Manager shows a new COM port under "Ports (COM & LPT)", with the built-in CDC driver (`usbser.sys`).
2. Make sure that interface 0 shows as a separate device ("Versaloon").
   OpenOCD needs a WinUSB driver for interface 0, for example installed with Zadig.

