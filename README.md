# versaloon-next-gen

Firmware for the Versaloon Mini and OtherUse debug adapters (STM32F103C8/CB, 12 MHz crystal, 72 MHz).
The firmware is compatible with the original Versaloon USB protocol, so unmodified OpenOCD can use it.
It also has a USB-CDC (Communication Device Class) serial port, which is bridged to a UART.

The project is at an early stage. The current firmware sets up the board, shows a heartbeat on an LED and enumerates as a USB composite device.
The Versaloon protocol and the UART bridge are not available yet (see [USB device](#usb-device)).
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
The main loop runs the USB device. It shows a heartbeat on the state LED:

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

The current firmware has placeholders for the two functions:

- Interface 0 sends each OUT packet back without change, with the same size (`src/usb/VersaloonFunction.cpp`).
  The Versaloon command protocol replaces this echo in a later version.
- The serial port sends the received data back.
  The UART bridge replaces this loopback in a later version.
  In a debug build with `VSL_DEBUG_CDC`, the serial port carries the debug log instead (see [Debug log](#debug-log-over-the-serial-port)).

### Check the enumeration

The host unit tests check the descriptors.
To check the device on the hardware, connect the dongle and do these steps.

On Linux:

1. Run `lsusb -v -d 0483:a038`. Make sure that the output shows the interfaces and endpoints of the tables above, and the IAD.
2. Make sure that the kernel log (`dmesg`) shows a new `/dev/ttyACM*` port.
3. Open the port in a terminal program, for example `picocom /dev/ttyACM0`. Make sure that the typed characters come back.
4. Run OpenOCD with `-f interface/vsllink.cfg`. OpenOCD finds the device, but fails at the protocol, because the protocol is not available yet.

On Windows 10 or later:

1. Make sure that the Device Manager shows a new COM port under "Ports (COM & LPT)", with the built-in CDC driver (`usbser.sys`).
2. Make sure that interface 0 shows as a separate device ("Versaloon").
   OpenOCD needs a WinUSB driver for interface 0, for example installed with Zadig.

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
- Internet access at configure time: CMake FetchContent downloads CMSIS and doctest.

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
| `VSL_BOOTLOADER` | `ON` | `ON`: link at `0x08002800`, behind `versaloon-bootloader`. `OFF`: link at `0x08000000`, for development with a debug probe and no bootloader. |
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
They check the board definitions, the heartbeat, the version, the USB descriptors and the log sink.
The USB tests also run the descriptors and the Versaloon function on the libArT USB device stack, with the fake USB controller of libArT.
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
- `build-mini`, `build-otheruse`, `build-stlinkv2`: release build behind the bootloader, plus the size check.
  The jobs keep the `.hex`, `.bin`, `.elf` and `.map` files as artifacts.

## Branches

- `main`: released, stable firmware.
- `dev`: the next version. Features go into `dev` through merge requests.
