# Debug log over the serial port

The CMake option `VSL_DEBUG_CDC` sends the libArT log to the CDC-ACM serial port.
The option is `ON` by default in a `Debug` build.
A `Release` build with `VSL_DEBUG_CDC=ON` fails at configure time.
The USB descriptors are the same in both builds.

With the option, these rules apply:

- The serial port carries only the log. The UART bridge is not built, and USART1 stays unused.
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

