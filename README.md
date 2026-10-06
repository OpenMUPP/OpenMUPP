# versaloon-next-gen

Firmware for the Versaloon Mini and OtherUse debug adapters (STM32F103C8/CB, 12 MHz crystal, 72 MHz).
The firmware is compatible with the original Versaloon USB protocol, so unmodified OpenOCD can use it.
It also has a USB-CDC (Communication Device Class) serial port, which is bridged to a UART.

The project is at an early stage. The current firmware sets up the board, shows a heartbeat on an LED and enumerates as a USB composite device.
It bridges the serial port to the UART (see [UART bridge](#uart-bridge)).
The Versaloon protocol is not available yet (see [USB device](#usb-device)).
See [docs/plan.md](docs/plan.md) for the plan and the work items.

## Boards

| `BOARD` | Hardware | Crystal | Status |
|---|---|---|---|
| `mini` | Versaloon Mini | 12 MHz | target |
| `otheruse` | OtherUse | 12 MHz | target |
| `stlinkv2` | ST-Link/V2 clone | 8 MHz | compile-only |

All boards use an STM32F103x8 or STM32F103xB.
CMSIS has one device define (`STM32F103xB`) for both parts.
The build sets `SYSTEM_STM32_HSE_VALUE` of system-stm32 from the board crystal.

### Board definitions

Each board has one `constexpr` board definition in `src/board/boards/`.
The board definition holds all board-specific data:

- The pin map: JTAG and SWD, UART, Vref sense, power switch, LEDs, key and USB pull-up.
- The crystal frequency.
- The JTAG fast path. It is only available when the board links TCK (PB13) to PB3.
- The hardware ID that `VERSALOON_GET_HARDWARE` returns.

`src/board/BoardDefinition.h` defines the types. It does not use CMSIS, so the host unit tests check all boards.
CMake defines `VSL_BOARD_<BOARD>` (for example `VSL_BOARD_MINI`), and `src/board/Board.h` selects `board::current` from it.
The protocol and the engines read board data only through `board::current`.
`src/board/BoardPins.h` maps a board pin to the system-stm32 `GpioPin` and `FastPin` drivers.

Some functions come from the pin map, not from a separate field:

- `board::adcChannelOf()` gives the ADC channel of the Vref pin.
- `board::swjConfig()` gives the debug port (SWJ) setting of the dongle MCU.
  The setting frees all debug port pins that the board uses, and keeps the rest.
  The Mini and the OtherUse use PA14 for an LED, so they disable the debug port.
  The ST-Link/V2 keeps JTAG and SWD.
- `board::withSwd()` gives the board of a debug build with `VSL_DEBUG_SWD`.
  It removes the LEDs on the SWD pins (PA13, PA14), so the debug port keeps SWD.
  On the Mini and the OtherUse, the USB LED (PA14) is then not available.

The build fails when a board definition is not valid.
Examples are a pin that is used two times, or a crystal that is different from `SYSTEM_STM32_HSE_VALUE`.

To add a board:

1. Add a header with the board definition in `src/board/boards/`.
2. Add the board to `src/board/Board.h`.
3. Add the board and its crystal to `VSL_BOARDS` and `SYSTEM_STM32_HSE_VALUE` in `CMakeLists.txt`.
4. Add a build job to `.gitlab-ci.yml` and a test to `tests/TestBoards.cpp`.

### Start-up and LEDs

The system-stm32 start-up code calls `os_startup_initialize_hardware()` (`src/initialize_hardware.cpp`) before `main()`.
This function does these steps:

1. It sets the system clock to 72 MHz from the board crystal, and the SysTick to 1 ms.
2. It sets the debug port (SWJ) of the dongle MCU from `board::swjConfig()`.
   The Mini and the OtherUse disable the debug port. A debug probe can then connect only under reset.
   With `VSL_DEBUG_SWD` (default in a `Debug` build), SWD stays on and JTAG is off.
   Then a debug probe can run and step the firmware.
3. It puts the board pins into a safe state:
   - All LEDs are off, but the power LED is on.
   - The USB pull-up is off, so the host does not see the device.
   - The target power is off (the switch pin floats).
   - The key pin has a pull resistor to the released level.

The JTAG, SWD, UART and Vref pins stay inputs until their drivers configure them.

`main()` starts the USB device when the clock comes from the crystal (see [USB device](#usb-device)).
The main loop runs the USB device and the UART bridge. It shows a heartbeat on the state LED:

| State LED | Meaning |
|---|---|
| Short flash, one time each second | The firmware runs. The clock comes from the crystal. |
| Fast flash, five times each second | The crystal or the PLL did not start. The clock is HSI (8 MHz) and USB cannot work. |
| On or off all the time | The main loop does not run. |

The USB LED is on when the host has configured the USB device.
A build with `VSL_DEBUG_SWD` has no USB LED on the Mini and the OtherUse.

## USB device

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
The only exception is the command `VERSALOON_FW_UPDATE` (see [Firmware update](#firmware-update)).

The serial port is the [UART bridge](#uart-bridge).
In a debug build with `VSL_DEBUG_CDC`, the serial port carries the debug log instead (see [Debug log](#debug-log-over-the-serial-port)).

### Check the enumeration

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

### UART bridge

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
- The bridge counts the received bytes with a framing, noise or parity error (`UartBridge::rxErrors()`).
  These bytes still go to the host. Many errors usually show a wrong baud rate or parity setting.
- When the host has not configured the device, the bridge drops the received UART data.

The bridge stores the DTR and RTS state of the host. It does not drive a pin with this state.
It does not support SEND_BREAK: the device stalls this request.

### Debug log over the serial port

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

## Serial port test

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
- Data bits and parity: 8N, 8E, 8O, 7E and 7O (the formats of the UART, see [UART bridge](#uart-bridge)).
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

## Firmware update

The firmware runs behind `versaloon-bootloader`.
The bootloader is a USB CDC serial port with the ID `0483:5740`.
It uses the STM32 serial bootloader protocol (ST AN3155), so `stm32flash` can write the firmware.

### Start the bootloader

Use one of these methods:

- Send the Versaloon command `VERSALOON_FW_UPDATE` to interface 0: the two bytes `0x0F 0xAA`.
- Hold the key (PB9) down and reset the board.

The firmware replies to `VERSALOON_FW_UPDATE` with one status byte:

| Status | Meaning |
|---|---|
| `0x00` | Accepted. The firmware starts the bootloader. |
| `0x01` | Refused. The second byte is not the key `0xAA`, or the firmware is linked without the bootloader (`VSL_BOOTLOADER=OFF`). The firmware continues to run. |

After the reply, the firmware does these steps:

1. It waits 10 ms, so the host can read the reply. The firmware does not wait for the host to read the reply.
2. It enables the `PWR` and `BKP` clocks and the write access to the backup domain.
3. It writes the boot request `0xB007` to the backup register `BKP->DR1`.
4. It resets the MCU with `NVIC_SystemReset()`. The host sees a USB disconnect.

After the reset, the bootloader finds the boot request, clears it and stays in bootloader mode.
The next reset starts the firmware again.
`src/boot/BootloaderEntry.h` and `src/boot/Bootloader.cpp` have the code.

### Write the firmware

1. Start the bootloader (see above). On Linux, a new port `/dev/ttyACM*` with the ID `0483:5740` shows.
2. Write the `.bin` file of a build with `VSL_BOOTLOADER=ON` at `0x08002800`:

   ```sh
   stm32flash -w versaloon-next-gen.bin -v -S 0x08002800 /dev/ttyACM0
   ```

3. Reset the board, or disconnect it and connect it again. The firmware starts.

Do not write a build with `VSL_BOOTLOADER=OFF` through the bootloader.
That build is linked at `0x08000000`, so it does not start at `0x08002800`.

### Start-up behind the bootloader

The bootloader starts the firmware after a system reset.
Thus all peripherals are in their reset state when the firmware starts.
The bootloader sets `VTOR` to the firmware vector table at `0x08002800`.
The system-stm32 start-up code sets `VTOR` again from the linker script.
The firmware initializes its DATA and BSS sections. It does not use RAM content from the bootloader.

## Dependencies

The libraries are Git submodules in `dependencies/`:

- `libArT`: core library (streams, logging, USB device stack).
- `system-stm32`: start-up code, CMSIS and peripheral drivers. The `dev` branch has the STM32F1 support.
- `libart-stm32`: STM32 adapters for libArT. The `dev` branch has the STM32F1 support.

Clone with the submodules:

```sh
git clone --recurse-submodules ssh://git@gitlab.vandertogt.org:3022/embedded-projects/versaloon-next-gen.git
```

In an existing clone, run `git submodule update --init --recursive`.

The build also needs:

- CMake 3.20 or later.
- The Arm GNU Toolchain (`arm-none-eabi`), for example 14.3.rel1 as in CI.
- A host C++ compiler with C++23 support, for the unit tests.
- `clang-format`, for the `clangformat` target.
- Internet access at configure time: CMake FetchContent downloads CMSIS, ETL and doctest.

## Build the firmware

Configure and build for one board:

```sh
cmake -B build/mini -DCMAKE_TOOLCHAIN_FILE=cmake/gcc_arm.cmake -DBOARD=mini \
      -DGCC_TOOLCHAIN_PREFIX=/opt/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi
cmake --build build/mini -j
```

Use `-DBOARD=otheruse` or `-DBOARD=stlinkv2` for the other boards.
If `arm-none-eabi-gcc` is in `/usr/bin`, you can omit `GCC_TOOLCHAIN_PREFIX`.

The build writes these files into the build directory:

- `versaloon-next-gen.elf`, `versaloon-next-gen.hex` and `versaloon-next-gen.bin`.
- Copies of the `.hex` and `.bin` files with the board, the link variant, the build type and the version in the name.
  An example is `versaloon-next-gen-mini-bootloader-Release-v0.1.0.hex`.
- `versaloon-next-gen.map`, the linker map.

The CMake options are:

| Option | Default | Description |
|---|---|---|
| `BOARD` | `mini` | Board to build for (see [Boards](#boards)). |
| `VSL_BOOTLOADER` | `ON` | `ON`: link at `0x08002800`, behind `versaloon-bootloader`. `OFF`: link at `0x08000000`, for development with a debug probe and no bootloader. The firmware then refuses `VERSALOON_FW_UPDATE`. |
| `CMAKE_BUILD_TYPE` | `Release` | `Release` or `Debug`. |
| `VSL_DEBUG_SWD` | `ON` in a `Debug` build, else `OFF` | Keep SWD of the dongle MCU on. The board has no LEDs on the SWD pins (see [Board definitions](#board-definitions)). Not allowed in a `Release` build. |
| `VSL_DEBUG_CDC` | `ON` in a `Debug` build, else `OFF` | Send the debug log to the serial port, instead of the UART bridge (see [Debug log](#debug-log-over-the-serial-port)). Not allowed in a `Release` build. |
| `GCC_TOOLCHAIN_PREFIX` | `/usr` | Install directory of the Arm GNU Toolchain. |

### Flash layout and size limit

`versaloon-bootloader` uses the first 10 KB of flash (`0x08000000` to `0x080027FF`).
The OtherUse has an STM32F103C8 with 64 KB flash.
Thus the application must not be larger than 54 KB.
This is a hard limit for all boards.

- `ldscripts/mem-f103-bootloader.ld` has a 54 KB FLASH region at `0x08002800`. The link fails when the application is too large.
- `ldscripts/mem-f103.ld` has a 64 KB FLASH region at `0x08000000` (`VSL_BOOTLOADER=OFF`).
- CI reports the image size with `ci/check-size.sh` and fails when the `.bin` file is larger than 54 KB.

The main stack is 2 KB (`ldscripts/sections.ld`), because the STM32F103 has only 20 KB RAM.
The link fails when less than the full main stack is free.

## Unit tests

The unit tests use doctest and run on the host.
They check the board definitions, the heartbeat, the version, the USB descriptors, the log sink, the line coding map of the UART bridge and the bootloader entry.
The USB tests also run the descriptors and the Versaloon function on the libArT USB device stack, with the fake USB controller of libArT.
The UART bridge tests run the bridge with the CDC-ACM function of libArT, the fake USB controller and a fake UART.
Configure without the toolchain file:

```sh
cmake -B build/tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build/tests -j
ctest --test-dir build/tests --output-on-failure
```

## Code format

`.clang-format` defines the code style. The `clangformat` target formats all sources and tests in place:

```sh
cmake --build build/tests --target clangformat
```

`etc/pre-commit.hook` formats the staged C and C++ files before each commit.
To use it, install it as an executable hook:

```sh
install -m 755 etc/pre-commit.hook "$(git rev-parse --git-path hooks)/pre-commit"
```

## Version

`cmake/generate_version.cmake` writes `Version.h` from `git describe`.
A tag `vMAJOR.MINOR.BUILD` sets the version numbers.
Without such a tag, the version is `0.0.0`, and the `git describe` output is the build revision.

## Continuous integration

`.gitlab-ci.yml` runs these jobs:

- `lint-code`: fails when `clangformat` changes a file.
- `run-unit-tests`: builds and runs the host unit tests.
- `run-tool-tests`: runs the unit tests of the Python scripts in `tools/`.
- `build-mini`, `build-otheruse`, `build-stlinkv2`: release build behind the bootloader, plus the size check.
- `build-mini-debug`: debug build of the Mini behind the bootloader (debug log on the serial port, SWD on), plus the size check.
  The jobs keep the `.hex`, `.bin`, `.elf` and `.map` files as artifacts.

## Branches

- `main`: released, stable firmware.
- `dev`: the next version. Features go into `dev` through merge requests.
