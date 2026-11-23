# Debug log

The log has two outputs: the CDC-ACM serial port and the debug UART.
Each output has its own CMake option.

## Serial port

The CMake option `MUPP_DEBUG_CDC` sends the libArT log to the CDC-ACM serial port.
The option is `ON` by default in a `Debug` build.
A `Release` build with `MUPP_DEBUG_CDC=ON` fails at configure time.
The USB descriptors are the same in both builds.

With the option, these rules apply:

- The serial port carries only the log. The UART bridge is not built, and the UART of the board stays unused.
  The device ignores the data that the host sends to the port.
- Code logs with the libArT macros, for example `LOG_INFO(debug::appLog, "text %u", value)` (`src/debug/DebugLog.h`).
- A log line has the uptime in seconds, the topic and the level, for example `1.234 [usb][INFO] configured`.
- The log goes into a 1 KB ring buffer in RAM (`src/debug/LogSink.h`). The firmware does not use the heap.
- The main loop moves the log to the port. Logging never waits for the host.
- The firmware sends the log only when a terminal has the port open (DTR on).
  Until then, the log stays in the ring buffer, so the start-up lines are not lost.
- When the ring buffer is full, the firmware drops the new log bytes and keeps the old bytes.
  When there is room again, a line such as `[log: 120 bytes dropped]` shows the number of dropped bytes.
- Log only from the main loop, not from an interrupt handler.

Without the option, CMake defines `LOGGING_DISABLED`.
Then the `LOG_*` macros expand to nothing, and the firmware has no log code.

To read the log on Linux, open the port, for example with `picocom /dev/ttyACM0`.

## Debug UART

The CMake option `MUPP_DEBUG_UART` sends the log to the debug UART of the board (`debugUart` in the board definition).
Only OpenMUPP v1 has a debug UART: UART5 on connector J2, TX on PC12 and RX on PD2.
The option is `ON` by default in a `Debug` build of this board.
A `Release` build with `MUPP_DEBUG_UART=ON` fails at configure time.

- The UART sends the log at 115200 baud, 8 data bits, no parity, 1 stop bit.
  The firmware does not read the RX pin. It sets PD2 as an input with a pull-up.
- The UART output works without `MUPP_DEBUG_CDC`, and with it.
  It also works in an image with only the `wch-link` personality, and while the USB port does not work.
  The CDC output does not work in these cases.
- The UART output has its own 1 KB ring buffer. The rules of the [serial port](#serial-port) for the ring buffer apply to it.
  The UART is always open, so the log does not wait for a terminal.
  With both outputs, each output keeps its own count of dropped bytes.
- DMA2 channel 4 sends the log in blocks of 64 bytes. The main loop never waits for the UART.
- The main loop moves the log to the UART in each iteration.
  The log also moves during a long command of the Versaloon and CMSIS-DAP engines.
- A log line on the UART has the same format as a line on the serial port.
  The USB state lines (`configured`, `terminal open`) show on the UART too.
  The `terminal` lines show only when the image has the serial port.

PC12 is also the SPI3 MOSI pin of the CH32V307 JTAG fast path (remapped SPI3).
The fast path does not use MOSI, but the pin stays shared while the fast path runs.

To read the log, connect a 3.3 V USB-to-serial adapter to J2, for example with `picocom -b 115200 /dev/ttyUSB0`.
