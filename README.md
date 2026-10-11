# versaloon-next-gen

Firmware for the Versaloon Mini and OtherUse debug adapters (STM32F103C8/CB, 12 MHz crystal, 72 MHz).
The firmware is compatible with the original Versaloon USB protocol, so unmodified OpenOCD can use it.
It also has a USB-CDC (Communication Device Class) serial port, which is bridged to a UART.
A build option selects a second USB personality, CMSIS-DAP v2, for OpenOCD, pyOCD and probe-rs (see [docs/cmsis-dap.md](docs/cmsis-dap.md)).
A dual image has both personalities and selects one at run time.

## Status

The firmware has these functions:

- It sets up the board and shows the target power, the command activity and the USB state on the LEDs.
- It enumerates as a USB composite device: one debug interface and a CDC-ACM serial port.
- It bridges the serial port to the UART.
  The bridge continues to run during long JTAG and SWD commands.
- It starts the bootloader on request, for a firmware update.

The Versaloon personality has the USB identity of the original Versaloon firmware:

- It receives Versaloon commands and replies to the common commands, for example `GET_INFO`.
- It runs the `USB_TO_ALL` command of OpenOCD, with the `INFO`, `DELAY`, `GPIO`, `JTAG_RAW`, `SWD` and `POWER` blocks. OpenOCD controls SRST and TRST through the `GPIO` block.
- It shifts JTAG bits through the `JTAG_RAW` block, with a fixed TCK frequency or with adaptive clocking (RTCK). The Mini and the OtherUse use SPI and DMA up to 9 MHz TCK. The other bits and speeds use GPIO bit-bang.
- It runs SWD transfers with GPIO bit-bang through the `SWD` block, at the SWCLK frequencies of the OpenOCD `vsllink` driver.
- It switches the 3.3 V target supply through the `POWER` block, and replies the target voltage to `GET_TVCC`. It refuses to switch on a target that has its own supply.

The CMSIS-DAP personality is a CMSIS-DAP v2 probe:

- It runs SWD and JTAG transfers, and `DAP_TransferAbort` during a transfer command.
- It has no SWO and no `DAP_UART_*` commands. Use the CDC-ACM serial port for the UART.

The dual image has both personalities.
A switch command or the key selects the other personality, and a flash page stores it.

Hardware test status:

- The Versaloon personality passed the OpenOCD test in JTAG and SWD mode.
- The CMSIS-DAP personality passed the test on Linux with OpenOCD, pyOCD and probe-rs in SWD mode (see [docs/cmsis-dap.md](docs/cmsis-dap.md#hardware-test)).
- The CMSIS-DAP JTAG mode, Windows and the dual image need a test on the hardware.

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
| `VSL_PROTOCOL` | `versaloon` | USB personality of the image: `versaloon`, `cmsis-dap` or `dual`. A `cmsis-dap` image has the CMSIS-DAP v2 interface, `0403:9b3c`, instead of the Versaloon interface. A `dual` image has both personalities, and one is active after each start (see [docs/usb-device.md](docs/usb-device.md#dual-image)). The serial port and the UART bridge are in all images. See [docs/cmsis-dap.md](docs/cmsis-dap.md). |
| `VSL_DEFAULT_PROTOCOL` | `versaloon` | Only for `VSL_PROTOCOL=dual`: the personality when no personality is stored. `versaloon` or `cmsis-dap`. |
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

- **Debug adapter:** use OpenOCD with `-f interface/vsllink.cfg`, in JTAG or SWD mode.
  On Windows, OpenOCD needs a WinUSB driver for interface 0, for example installed with Zadig.
- **Serial port:** the dongle has a serial port (`/dev/ttyACM*` on Linux, a COM port on Windows).
  The port is bridged to the UART of the board: PA9 (TX) and PA10 (RX) on the Mini and the OtherUse.
  Open it with a terminal program, for example `picocom /dev/ttyACM0`.

A firmware image built with `-DVSL_PROTOCOL=cmsis-dap` shows as a USB device with the ID `0403:9b3c` and the product name `Versaloon CMSIS-DAP`.
The debug tools find it as a CMSIS-DAP v2 probe (see [docs/cmsis-dap.md](docs/cmsis-dap.md#use-the-probe)).
The serial port works as in the default image.

A firmware image built with `-DVSL_PROTOCOL=dual` has both personalities.
To change the personality, send a switch command, or use the key.
`tools/vsl_personality.py` sends the switch command (see [Tools](#tools)).
To use the key, hold it down for 3 s until all LEDs flash.
Then release the key.
The dongle then resets and shows as the other USB device.
The dongle keeps the personality after a power cycle and after a firmware update with `stm32flash`.
See [docs/usb-device.md](docs/usb-device.md#dual-image).

The power LED shows the target power. The state LED flashes for each command of the host. The USB LED shows the USB state.
See [docs/boards.md](docs/boards.md#start-up-and-leds) for the meaning of the LEDs.

## Tools

The Python scripts in `tools/` need Python 3.

| Script | Function | Needs |
|---|---|---|
| `tools/vsl_personality.py` | `show` lists the dongles and their personality. `switch <personality>` switches a dual image to `versaloon` or `cmsis-dap`. | `pyusb` |
| `tools/serial_loopback_test.py` | Tests the UART bridge with a loopback wire from TX to RX (see [docs/uart-bridge.md](docs/uart-bridge.md)). | `pyserial` |
| `tools/flash_benchmark.py` | Measures the flash speed with OpenOCD (see [docs/development.md](docs/development.md#flash-speed-benchmark)). | OpenOCD |

The `test_*.py` files are the unit tests of the scripts. They need no hardware.

## Documentation

- [docs/boards.md](docs/boards.md): board definitions, start-up and LEDs.
- [docs/usb-device.md](docs/usb-device.md): USB descriptors and interfaces, and how to check the enumeration.
- [docs/uart-bridge.md](docs/uart-bridge.md): the USB-to-UART bridge, and the serial port test script.
- [docs/debug-log.md](docs/debug-log.md): the debug log over the serial port in a `Debug` build.
- [docs/cmsis-dap.md](docs/cmsis-dap.md): the CMSIS-DAP personality and its command processor for SWD and JTAG.
- [docs/firmware-update.md](docs/firmware-update.md): the bootloader, the firmware update and the flash layout.
- [docs/development.md](docs/development.md): build outputs, unit tests, the flash speed benchmark, code format, version, CI and branches.
- [docs/plan.md](docs/plan.md): the plan and the work items.
