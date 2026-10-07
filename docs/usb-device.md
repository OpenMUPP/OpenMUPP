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

Interface 0 carries the Versaloon command protocol (see [Versaloon commands](#versaloon-commands)).

The serial port is the [UART bridge](uart-bridge.md).
In a debug build with `VSL_DEBUG_CDC`, the serial port carries the debug log instead (see [Debug log](debug-log.md)).

## Versaloon commands

The host sends one command in one bulk OUT transfer on endpoint `0x03`.
It reads the reply in one bulk IN transfer on endpoint `0x82`.
All values are little-endian.
`src/protocol/` has the protocol code. It does not use the hardware, so the host unit tests check it.

The first byte of a command is the command code:

| Code | Commands | Format |
|---|---|---|
| `0x00`-`0x0F` | Common commands | One packet. |
| `0x20`-`0x7F` | `USB_TO_XXX` commands | `[code][length u16][data]`. `length` is the total size, with the 3-byte header. The command can have many packets. |

The firmware collects the packets until it has `length` bytes (`src/protocol/CommandAssembler.h`).
The command buffer and the reply buffer are separate, and each has 4096 bytes.
Thus a reply can be longer than its command.
`GET_INFO` sends the buffer size to the host.
The host sends no command and expects no reply that is larger than this size.

The common commands are:

| Command | Request | Reply |
|---|---|---|
| `GET_INFO` | `[0x00]` | `[buffer size u16][version text][0x00]` |
| `GET_TVCC` | `[0x01]` | `[target voltage in mV u16]`. The value is 0, because the voltage sense is not available yet. |
| `GET_HARDWARE` | `[0x02]` | `[hardware ID u8]`, from the board definition (see [Boards](boards.md)). |
| `FW_UPDATE` | `[0x0F][0xAA]` | `[status u8]` (see [Firmware update](firmware-update.md)). |

The version text has the form `versaloon-next-gen <version>, board <board>`.
Other common commands have no reply.
The `USB_TO_XXX` commands have no reply yet.

The reply is one IN transfer.
When the size of the reply is a multiple of 64 bytes, a zero-length packet (ZLP) follows, so that the host read ends.
A reply of 4096 bytes has no ZLP.
The host reads 4096 bytes, so its read ends without a ZLP, and the ZLP would end the next read.
While the reply is on its way, the OUT endpoint NAKs. Thus the next command waits.

The firmware replies with a ZLP to a wrong command.
The host then sees a reply with the wrong size immediately, without a timeout.
A command is wrong in these cases:

- The header is shorter than 3 bytes, or `length` is smaller than 3.
- The transfer has more bytes than `length`.
- A short packet ends the transfer before `length` bytes (a ZLP is a short packet too).
- A common command, or an unused command code, is in a full packet of 64 bytes.
- `length` is larger than the command buffer (overflow).
- The `USB_TO_XXX` code returns a reply that is larger than the reply buffer.

The ZLP comes at the end of the transfer of the host.
Thus the next transfer starts as a new command.
After an error on a full packet, the firmware discards the packets until a short packet ends the transfer.
If such a transfer ends with a full packet and no ZLP, the firmware also discards the next transfer.
For an overflow, the firmware discards the packets until `length`.

## Check the enumeration

The host unit tests check the descriptors.
To check the device on the hardware, connect the dongle and do these steps.

On Linux:

1. Run `lsusb -v -d 0483:a038`. Make sure that the output shows the interfaces and endpoints of the tables above, and the IAD.
2. Make sure that the kernel log (`dmesg`) shows a new `/dev/ttyACM*` port.
3. Connect the UART TX pin to the UART RX pin (PA9 to PA10 on the Mini). Open the port in a terminal program, for example `picocom /dev/ttyACM0`. Make sure that the typed characters come back.
4. Run OpenOCD with `-f interface/vsllink.cfg`.
   Make sure that OpenOCD shows the version text of `GET_INFO`, for example `versaloon-next-gen v0.1.0, board mini`.
   After this, OpenOCD fails, because the `USB_TO_XXX` commands are not available yet.

On Windows 10 or later:

1. Make sure that the Device Manager shows a new COM port under "Ports (COM & LPT)", with the built-in CDC driver (`usbser.sys`).
2. Make sure that interface 0 shows as a separate device ("Versaloon").
   OpenOCD needs a WinUSB driver for interface 0, for example installed with Zadig.

