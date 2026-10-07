# versaloon-next-gen

Firmware for the Versaloon Mini and OtherUse debug adapters (STM32F103C8/CB, 12 MHz crystal, 72 MHz).
The firmware is compatible with the original Versaloon USB protocol, so unmodified OpenOCD can use it.
It also has a USB-CDC (Communication Device Class) serial port, which is bridged to a UART.

## Status

The project is at an early stage. The current firmware does these functions:

- It sets up the board and shows a heartbeat on an LED.
- It enumerates as a USB composite device with the USB identity of the original Versaloon firmware.
- It bridges the serial port to the UART.
- It starts the bootloader on request, for a firmware update.
- It receives Versaloon commands and replies to the common commands, for example `GET_INFO`.
- It runs the `USB_TO_ALL` command of OpenOCD, with the `INFO`, `DELAY`, `GPIO`, `JTAG_RAW` and `SWD` blocks. OpenOCD controls SRST and TRST through the `GPIO` block.
- It shifts JTAG bits with GPIO bit-bang through the `JTAG_RAW` block, with a fixed TCK frequency or with adaptive clocking (RTCK).
- It runs SWD transfers with GPIO bit-bang through the `SWD` block, at the SWCLK frequencies of the OpenOCD `vsllink` driver.

The target power block is not available yet.
The JTAG and SWD modes need a test on the hardware.
See [docs/plan.md](docs/plan.md) for the plan and the work items.

## Boards

| `BOARD` | Hardware | Crystal | Status |
|---|---|---|---|
| `mini` | Versaloon Mini | 12 MHz | target |
| `otheruse` | OtherUse | 12 MHz | target |
| `stlinkv2` | ST-Link/V2 clone | 8 MHz | compile-only |

All boards use an STM32F103x8 or STM32F103xB.
See [docs/boards.md](docs/boards.md) for the board definitions and how to add a board.

## Get the source code

The libraries are Git submodules in `dependencies/`.
Clone with the submodules:

```sh
git clone --recurse-submodules ssh://git@gitlab.vandertogt.org:3022/embedded-projects/versaloon-next-gen.git
```

In an existing clone, run `git submodule update --init --recursive`.

The submodules are:

- `libArT`: core library (streams, logging, USB device stack).
- `system-stm32`: start-up code, CMSIS and peripheral drivers. The `dev` branch has the STM32F1 support.
- `libart-stm32`: STM32 adapters for libArT. The `dev` branch has the STM32F1 support.
- `versaloon-bootloader`: the USB bootloader. The `master` branch has the CMake build and the boot request in `BKP->DR1`.

## Requirements

- CMake 3.20 or later. The `versaloon-bootloader` build needs CMake 3.21 or later.
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

- `versaloon-next-gen.elf`, `versaloon-next-gen.hex` and `versaloon-next-gen.bin`: the firmware.
- `versaloon-bootloader/versaloon-bootloader-<board>.hex` (and `.elf`, `.bin`): the bootloader for the board.
  Only the Mini and the OtherUse have a bootloader.

See [docs/development.md](docs/development.md#build-outputs) for all build outputs.

The CMake options are:

| Option | Default | Description |
|---|---|---|
| `BOARD` | `mini` | Board to build for (see [Boards](#boards)). |
| `VSL_BOOTLOADER` | `ON` | `ON`: link at `0x08002800`, behind `versaloon-bootloader`. `OFF`: link at `0x08000000`, for development with a debug probe and no bootloader. The firmware then refuses `VERSALOON_FW_UPDATE`. The ST-Link/V2 has no bootloader: its firmware always refuses `VERSALOON_FW_UPDATE`. |
| `VSL_BUILD_BOOTLOADER` | `ON` | Build `versaloon-bootloader` for the board. Only for the Mini and the OtherUse, and only with `VSL_BOOTLOADER=ON`. |
| `CMAKE_BUILD_TYPE` | `Release` | `Release` or `Debug`. |
| `VSL_DEBUG_SWD` | `ON` in a `Debug` build, else `OFF` | Keep SWD of the dongle MCU on. The board has no LEDs on the SWD pins (see [docs/boards.md](docs/boards.md#board-definitions)). Not allowed in a `Release` build. |
| `VSL_DEBUG_CDC` | `ON` in a `Debug` build, else `OFF` | Send the debug log to the serial port, instead of the UART bridge (see [docs/debug-log.md](docs/debug-log.md)). Not allowed in a `Release` build. |
| `GCC_TOOLCHAIN_PREFIX` | `/usr` | Install directory of the Arm GNU Toolchain. |

## Flash the firmware

The firmware runs behind `versaloon-bootloader`.
A new board needs the bootloader one time.
Write it with a debug probe on the SWD pins of the dongle MCU, for example with OpenOCD:

```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "program versaloon-bootloader/versaloon-bootloader-mini.hex verify reset exit"
```

To write the firmware through the bootloader, do these steps:

1. Start the bootloader. Hold the key (PB9) down and reset the board.
   A new serial port with the ID `0483:5740` shows, for example `/dev/ttyACM0`.
2. Write the `.bin` file with `stm32flash`:

   ```sh
   stm32flash -w versaloon-next-gen.bin -v -S 0x08002800 /dev/ttyACM0
   ```

3. Reset the board. The firmware starts.

See [docs/firmware-update.md](docs/firmware-update.md) for the other ways to start the bootloader, and for the flash layout.

## Use the dongle

Connect the dongle to the host with USB.
It shows as a USB device with the ID `0483:a038` and the product name `Versaloon`.

- **Debug adapter:** use OpenOCD with `-f interface/vsllink.cfg`.
  On Windows, OpenOCD needs a WinUSB driver for interface 0, for example installed with Zadig.
  JTAG and SWD mode are available. They need a test on the hardware (see [Status](#status)).
- **Serial port:** the dongle has a serial port (`/dev/ttyACM*` on Linux, a COM port on Windows).
  The port is bridged to the UART of the board: PA9 (TX) and PA10 (RX) on the Mini and the OtherUse.
  Open it with a terminal program, for example `picocom /dev/ttyACM0`.

The state LED shows a short flash one time each second when the firmware runs.
See [docs/boards.md](docs/boards.md#start-up-and-leds) for the meaning of the LEDs.

## Documentation

- [docs/boards.md](docs/boards.md): board definitions, start-up and LEDs.
- [docs/usb-device.md](docs/usb-device.md): USB descriptors and interfaces, and how to check the enumeration.
- [docs/uart-bridge.md](docs/uart-bridge.md): the USB-to-UART bridge, and the serial port test script.
- [docs/debug-log.md](docs/debug-log.md): the debug log over the serial port in a `Debug` build.
- [docs/firmware-update.md](docs/firmware-update.md): the bootloader, the firmware update and the flash layout.
- [docs/development.md](docs/development.md): build outputs, unit tests, code format, version, CI and branches.
- [docs/plan.md](docs/plan.md): the plan and the work items.
