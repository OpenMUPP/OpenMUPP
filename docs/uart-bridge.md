# UART bridge

The serial port (interfaces 1 and 2) is a USB-to-UART bridge.
It uses the UART of the board definition: USART1 on PA9 (TX) and PA10 (RX) on the Mini and the OtherUse.
The UART has no flow control lines.

- `src/uart/UartBridge.h` moves the data between the serial port and two ring buffers of 1 KB each.
  This code does not use the hardware, so the host unit tests check it.
- `src/uart/UartPort.cpp` runs the USART with interrupts. The interrupt fills the RX ring buffer and empties the TX ring buffer.
  Thus the serial port does not lose data when the main loop is busy for a long time.
  A ring buffer holds about 89 ms of data at 115200 baud.
- `src/uart/SerialBridge.cpp` connects the two parts. The main loop runs the bridge after the USB device.

The bridge uses the line coding that the host sets (SET_LINE_CODING).
`src/uart/FrameFormat.h` maps it to the UART:

| Data bits | Parity | Stop bits |
|---|---|---|
| 8 | none, odd or even | 1, 1.5 or 2 |
| 7 | odd or even | 1, 1.5 or 2 |

- The baud rate must be in the range of the baud rate generator. For USART1 at 72 MHz, this is 1099 baud to 4.5 Mbaud.
- The real baud rate must be within 2 % of the requested baud rate. Above 2.88 Mbaud, only some baud rates are in this limit.
- The bridge refuses all other line codings: 5, 6 or 16 data bits, 7 data bits without parity, and mark or space parity.
  The device then stalls the request, and the UART keeps its old line coding.
- Until the host sets a line coding, the UART uses 115200 baud, 8 data bits, no parity and 1 stop bit (8N1).
- A new line coding takes effect after the UART has sent all data that the host sent before the change.

Flow control and data loss:

- Host to UART: when the TX ring buffer is full, the bridge reads no more data from the serial port.
  The USB OUT endpoint then sends NAK, and the host waits. No data is lost.
- UART to host: when the RX ring buffer is full, the interrupt drops the new bytes.
  The bridge counts the dropped bytes (`UartBridge::rxDropped()`).
- The bridge also counts the bytes that the UART lost in an overrun (`UartBridge::rxOverruns()`).
  An overrun occurs when the interrupt reads a byte too late, for example at very high baud rates.
  To prevent overruns, the UART interrupt has a higher priority than the USB interrupt (`src/Hardware.h`).
  At 2 Mbaud, the interrupt must read each byte within 5 µs.
- The bridge counts the received bytes with a framing, noise or parity error (`UartBridge::rxErrors()`).
  These bytes still go to the host. Many errors usually show a wrong baud rate or parity setting.
- When the host has not configured the device, the bridge drops the received UART data.

The bridge stores the DTR and RTS state of the host. It does not drive a pin with this state.
It does not support SEND_BREAK: the device stalls this request.

## Loopback test

`tools/serial_loopback_test.py` tests the serial port on the dongle.
It sends a byte pattern at each line coding and checks that the same bytes come back.
It needs the pyserial Python package and Python 3.

1. Connect the UART TX pin to the UART RX pin (PA9 to PA10 on the Mini and the OtherUse).
2. Run the script with the serial port of the dongle:

```sh
python3 tools/serial_loopback_test.py /dev/ttyACM0   # Linux
py tools\serial_loopback_test.py COM5                # Windows
```

The default run tests all combinations of these settings:

- Baud rate: 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000 and 2000000.
- Data bits and parity: 8N, 8E, 8O, 7E and 7O (the formats of the UART, see above).
- Stop bits: 1, 1.5 and 2.

The script prints PASS, FAIL, SKIP or ERROR for each combination.
It exits with 0 when no combination fails and at least one passes.
SKIP means that the host cannot make the combination.
Only Windows can make 1.5 stop bits. On other hosts, pyserial sets 2 stop bits instead, so the script skips 1.5 there.
ERROR means that the port failed, for example because the dongle reset or was unplugged. The script then stops.

The script also fails a combination when the data comes back faster than the baud rate allows.
This finds a bridge that ignores the baud rate.
A loopback cannot check the data bits and the parity, because TX and RX use the same setting.
A second, reference serial adapter on the UART pins is necessary for that check.

Useful options: `--bauds`, `--formats`, `--stop-bits`, `--size` (bytes for each combination, 1 or more) and `--repeat` (number of runs, 1 or more).
To test the serial port during a long JTAG or SWD transfer, start the transfer with OpenOCD, and run the script with a large `--repeat` at the same time.
Use `loop://` as the port to run the script without a dongle (pyserial loopback).

The unit tests of the script need no pyserial and no hardware:

```sh
python3 -m unittest discover -s tools -p "test_*.py"
```

Status: the host tests pass. The test on a Mini and an OtherUse is not done yet.

