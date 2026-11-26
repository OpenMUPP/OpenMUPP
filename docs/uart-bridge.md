# UART bridge

The serial port (interfaces 1 and 2) is a USB-to-UART bridge.
It uses the UART of the board definition: USART1 on PA9 (TX) and PA10 (RX) on the Mini, UART7 on PC2 (TX) and PC3 (RX) on the YD-CH32V307VCT6, and UART7 on PA6 (TX) and PA7 (RX) on OpenMUPP v1 (UART7 remap).
The UART has no flow control lines.

- `src/uart/UartBridge.h` moves the data between the serial port and two ring buffers of 1 KB each.
  This code does not use the hardware, so the host unit tests check it.
- `src/uart/UartPort.cpp` runs the USART with interrupts. The interrupt fills the RX ring buffer and empties the TX ring buffer.
  Thus the serial port does not lose data when the main loop is busy for a long time.
  A ring buffer holds about 89 ms of data at 115200 baud.
  On the CH32V307, DMA moves the bytes instead. See [DMA on the CH32V307](#dma-on-the-ch32v307).
- `src/uart/SerialBridge.cpp` connects the two parts. The main loop runs the bridge after the USB device.
- `src/uart/DapUartChannel.h` gives the UART to the `DAP_UART_*` commands of the CMSIS-DAP personality.

## Owner of the UART

In the CMSIS-DAP personality, the `DAP_UART_*` commands can take the UART from the serial port (`DAP_UART_Transport`).
See [UART commands](cmsis-dap.md#uart-commands).
Only one side has the UART at a time. The serial port has it after the start.
While the DAP commands have the UART, the bridge discards the data from the host, and it sends no data to the host.
The OUT endpoint does not hold the host in this time.
The bridge still takes a line coding (SET_LINE_CODING).
When the serial port gets the UART again, the bridge sets its line coding on the UART again.
A change of the owner empties both ring buffers.
A USB reset gives the UART back to the serial port.

## Line coding

The bridge uses the line coding that the host sets (SET_LINE_CODING).
`src/uart/FrameFormat.h` maps it to the UART:

| Data bits | Parity | Stop bits |
|---|---|---|
| 8 | none, odd or even | 1, 1.5 or 2 |
| 7 | odd or even | 1, 1.5 or 2 |

- The baud rate must be in the range of the baud rate generator. For USART1 at 72 MHz (STM32F1), this is 1099 baud to 4.5 Mbaud.
- On the CH32V307, UART7 runs at 144 MHz (APB1). The range is 2198 baud to 9 Mbaud. The bridge refuses lower baud rates, for example 1200 baud.
  A Vref measurement changes only the APB2 clock, so the UART7 baud rate does not change (see [YD-CH32V307VCT6](boards.md#yd-ch32v307vct6-development-board)).
- The real baud rate must be within 2 % of the requested baud rate. Above 2.88 Mbaud at 72 MHz, and above 5.76 Mbaud at 144 MHz, only some baud rates are in this limit.
- The bridge refuses all other line codings: 5, 6 or 16 data bits, 7 data bits without parity, and mark or space parity.
  The device then stalls the request, and the UART keeps its old line coding.
- Until the host sets a line coding, the UART uses 115200 baud, 8 data bits, no parity and 1 stop bit (8N1).
- A new line coding takes effect after the UART has sent all data that the host sent before the change.

Flow control and data loss:

- Host to UART: when the TX ring buffer is full, the bridge reads no more data from the serial port.
  The USB OUT endpoint then sends NAK, and the host waits. No data is lost.
- UART to host: when the RX ring buffer is full, the interrupt drops the new bytes.
  The bridge counts the dropped bytes (`UartBridge::rxDropped()`).
- A long JTAG or SWD command stops the main loop.
  During such a command, the JTAG and SWD engines run the bridge each millisecond.
  Thus the RX ring buffer does not overflow at 115200 baud.
  See [Serial port during long commands](usb-device.md#serial-port-during-long-commands).
- The bridge also counts the bytes that the UART lost in an overrun (`UartBridge::rxOverruns()`).
  An overrun occurs when the interrupt reads a byte too late, for example at very high baud rates.
  To prevent overruns, the UART interrupt has a higher priority than the USB interrupt (`src/Hardware.h`).
  At 2 Mbaud, the interrupt must read each byte within 5 µs. This limit does not apply to the DMA of the CH32V307.
- The bridge counts the received bytes with a framing, noise or parity error (`UartBridge::rxErrors()`).
  The ring buffers also count the parity errors (`UartBuffers::rxParityErrors`) and the other errors (`UartBuffers::rxFramingErrors`) separately, for the status of the `DAP_UART_*` commands.
  These bytes still go to the host. Many errors usually show a wrong baud rate or parity setting.
- When the host has not configured the device, the bridge drops the received UART data.

The bridge stores the DTR and RTS state of the host. It does not drive a pin with this state.
It does not support SEND_BREAK: the device stalls this request.

## DMA on the CH32V307

On the CH32V307, DMA2 moves the UART7 data, so the CPU gets no interrupt for each byte.
With one interrupt for each received byte, the bridge lost bytes at 2 Mbaud ([issue #20](https://github.com/OpenMUPP/OpenMUPP/issues/20)).
The interrupt for each sent byte also made the SWD clock slower ([issue #33](https://github.com/OpenMUPP/OpenMUPP/issues/33)).

- RX: DMA2 channel 9 writes the received bytes into a circular buffer of 256 bytes.
  The interrupt of the channel copies the new bytes into the RX ring buffer.
  It runs at the half and at the end of the buffer, thus each 128 bytes.
  The main loop also does the copy in each pass, and before each flush of the RX ring (`UartPort::pollRx()`). Thus a short message does not wait for 128 bytes, and a flush also discards the bytes in the DMA buffer.
  At 2 Mbaud, 128 bytes take 640 µs. The interrupt must run within this time, else the DMA writes over bytes that are not copied.
  The copy then counts one overrun (`rxOverruns()`) and drops the buffer. For example, the debugWIRE engine locks the interrupts for each byte: 1.28 ms at 7812 baud (an AVR at 1 MHz).
  The copy does not see all laps, and counts no overrun for these laps:
  - The last copy ended exactly at the half or the end of the buffer.
  - The DMA wrote more than 256 bytes plus the distance from the last copy to the last flag position (the half or the end) on the way. This is more than 384 to 512 bytes.
- TX: DMA2 channel 8 sends a block of the TX ring buffer, 256 bytes at most.
  At the end of the block, the interrupt releases the block and starts the next block.
- 7 data bits: the DMA copies the parity bit in bit 7. The copy clears bit 7.
- Receive errors: the DMA reads the data register, so the interrupt reads only the status register.
  It counts one error when it finds an error flag and the DMA has received bytes since the last count.
  Thus two errors between two checks give one count. The counters show that errors occurred, not their exact number.
  The status read of `UartPort::isTxIdle()` can also clear an error flag before the check. The bridge reads it only while a new line coding waits.

The JTAG fast path uses DMA2 channel 2, and the one-wire engine uses DMA2 channels 10 and 11.
USART1 is not a good choice for the bridge: its DMA channels (DMA1 channels 4 and 5) are also the SPI2 channels of the JTAG fast path.
USART1 is also on APB2, and each Vref measurement changes the APB2 clock.

## Single-wire mode

OpenMUPP v1 can use the UART on one wire, for example for a half-duplex bus.
`ID_DAP_Vendor6` selects the mode (see [UART wire mode](cmsis-dap.md#uart-wire-mode)).
Other boards do not have the mode, because they have no TX level shifter with a direction pin.

Connect J4 pin 13 (UART TX) to J4 pin 14 (UART RX), and connect the target to this joined line.
On an adapter, connect J5 pin 2 (RX) to J5 pin 3 (TX).
The board has no link between TX and RX.
The link is necessary for these reasons:

- RX has a 10 kΩ pull-up to `VTREF` (R25). Thus the joined line stays high when no side drives it.
  The TX line has no pull-up on the target side.
- RX has a fixed level shifter (U4), so the probe always reads the line through RX.

The firmware sets the TX level shifter (U6) with `UART_TX_DIR` (PC9), in `src/uart/SingleWireLine.h`:

- While the UART does not send, `UART_TX_DIR` is low, so U6 does not drive the target.
  U6 then drives PA6, so PA6 is a floating input. R53 keeps PA6 high while U6 changes direction.
- Before the UART sends a block, the firmware disables the receiver, sets `UART_TX_DIR` high and gives PA6 back to UART7.
  The DMA interrupt of the TX channel does this.
- After the last block, the firmware enables the transfer complete interrupt (TC) of UART7.
  After the last stop bit, this interrupt sets PA6 to an input, sets `UART_TX_DIR` low and enables the receiver.

PA6 is an output only while U6 drives the target. Thus PA6 and U6 never drive against each other.
The receiver is off while the UART sends, so the host does not receive its own data (no echo).
A mode change while the UART sends takes effect after the last stop bit.
The mode applies to the serial port and to the `DAP_UART_*` commands.

The probe does not check if the line is free before it sends.
The protocol on the line must prevent collisions.
Data from the target while the probe sends is lost.

Status: the host tests pass. The hardware test of [issue #148](https://github.com/OpenMUPP/OpenMUPP/issues/148) is not done yet.

## Loopback test

`tools/hwtest/serial_loopback_test.py` tests the serial port on the dongle.
It sends a byte pattern at each line coding and checks that the same bytes come back.
It needs the pyserial Python package and Python 3.

1. Connect the UART TX pin to the UART RX pin (PA9 to PA10 on the Mini).
2. Run the script with the serial port of the dongle:

```sh
python3 tools/hwtest/serial_loopback_test.py /dev/ttyACM0   # Linux
py tools\hwtest\serial_loopback_test.py COM5                # Windows
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

Status: the host tests pass. On the YD-CH32V307VCT6 (UART7, wire from PC2 to PC3), all combinations with 1 and 2 stop bits pass from 9600 baud to 1 Mbaud. The test host (Linux) cannot make 1.5 stop bits.
At 2 Mbaud, some runs lose bytes ([issue #20](https://github.com/OpenMUPP/OpenMUPP/issues/20)).

