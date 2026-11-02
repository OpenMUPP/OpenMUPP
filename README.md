# OpenMUPP

OpenMUPP (Open Multi-architecture Universal Programmer & Probe) is the firmware of the OpenMUPP v1 probe.
OpenMUPP v1 is an open hardware USB debug probe with an STM32H523 MCU.
It has level shifters for targets from 1.2 V to 5 V, a target power supply and a high-voltage (VPP) supply.
The schematics are in the repository [openmupp-v1](https://github.com/OpenMUPP/openmupp-v1).
The firmware debugs and programs target chips of more than one architecture.
Today it supports Arm Cortex-M (SWD and JTAG) and WCH RISC-V (CH32V).
The name OpenMUPP stands for more than these two: the goal is one probe for many architectures (see [Planned targets](#planned-targets)).

The OpenMUPP v1 probe is the main target of this project.
The firmware has no board definition for it yet (see [Boards](#boards)).
Until then, the WeAct STM32H523 core board is the test board for the same MCU.
The firmware also builds for older probe hardware: the Versaloon Mini, the ST-Link/V2-1 of the Nucleo-64 boards and the ST-Link/V2 mini clone (STM32F103 or compatible chip).
These boards are secondary targets. They help to develop and test the code, and you can use them as probes.
The ST-Link/V2 mini clone has a hardware test of the USB personalities, the LEDs and SWD to a target.
The ST-Link/V2-1 has a hardware test of the WCH-Link personality on one wire (CH32V006) only.
Each firmware image has one of these personalities:

| Personality | USB ID | Debug tools | Target chips |
|---|---|---|---|
| Versaloon (original protocol) | `0483:a038` | Unmodified OpenOCD (`interface/vsllink.cfg`) | Arm Cortex-M, SWD and JTAG |
| CMSIS-DAP v2 (the default) | `1209:0001` | OpenOCD, pyOCD, probe-rs | Arm Cortex-M, SWD and JTAG (see [docs/cmsis-dap.md](docs/cmsis-dap.md)) |
| WCH-Link | `1209:0001` | `wlink`, `minichlink` | WCH RISC-V chips (CH32V), see [docs/wch-link.md](docs/wch-link.md) |

The Versaloon personality is an independent implementation of the Versaloon USB protocol. It contains no code of the original Versaloon project. OpenMUPP is not related to that project or its author.

`wlink` and `minichlink` find only the WCH ID `1a86:8010`.
Build the WCH-Link image with `MUPP_WCH_LINK_WCH_USB_ID` for personal use only (see [docs/wch-link.md](docs/wch-link.md#usb-id)).

The Versaloon and CMSIS-DAP personalities also have a USB-CDC serial port.
The firmware bridges it to a UART of the board.
The WCH-Link personality has no serial port.
A multi image has more than one personality, and you select one at run time.
The firmware drives the LEDs for the target power, the command activity and the USB state.
The key starts the bootloader, and the Versaloon personality also starts it on request (see [Flash the firmware](#flash-the-firmware)).

The [status](docs/status.md) document lists what the firmware does and what the hardware tests cover.

## Planned targets

OpenMUPP v1 has hardware for more protocols than the firmware supports today.
These targets are planned. None of them works yet.

| Target | Protocols | State |
|---|---|---|
| AVR | ISP (SPI) programming and debugWIRE debug, with the EDBG protocol of Microchip. The first release does not cover high-voltage programming, PDI, TPI, UPDI and aWire. High-voltage programming will use the VPP supply of the board. | Design work started. The firmware does not exist. |
| PIC | Programming. High-voltage programming uses the VPP supply of the board. | Planned. No design yet. |
| STM8 | SWIM (single-wire debug and programming interface) | Planned. No design yet. |

The AVR personality will run at the same time as the CMSIS-DAP personality, in one image for the STM32H523 boards.
The planned protocols need the OpenMUPP v1 hardware: the level shifters, the split nRESET path and the VPP supply.
An adapter board maps the target connector to the pinout of each target.
All adapters are passive, also the high-voltage adapters. The VPP supply is on the OpenMUPP v1 board.
Each adapter has an ID resistor. The firmware applies VPP only when the ID shows a high-voltage adapter (see [VPP sequence](docs/boards.md#vpp-sequence)).

## Boards

OpenMUPP v1 is the main target. The other boards are for development, tests and use as a probe.

| `BOARD` | Hardware | Crystal | Role | Status |
|---|---|---|---|---|
| (none yet) | OpenMUPP v1 probe board, STM32H523CC | 24 MHz | Main target | No board definition yet |
| `weacth523` | WeAct STM32H523 core board | 8 MHz | Test board for the OpenMUPP v1 MCU | target, 3.3 V targets only |
| `mini` | Versaloon Mini | 12 MHz | Secondary | target |
| `stlinkv21` | ST-Link/V2-1 of the Nucleo-64 boards (MB1136, MB1360), see [docs/boards.md](docs/boards.md#st-linkv2-1) | 8 MHz | Secondary | WCH-Link one wire tested, SWD only (no JTAG pins) |
| `stlinkv2miniclone` | ST-Link/V2 mini clone (USB stick), see [docs/stlinkv2-mini-clone.md](docs/stlinkv2-mini-clone.md) | 8 MHz | Secondary | target, SWD only (no JTAG pins) |

[docs/boards.md](docs/boards.md#openmupp-v1-probe-board) lists the pins of OpenMUPP v1 and the rules for its power functions.
The board layer needs the direction pins, the split nRESET path and the power functions before the firmware can drive the board.
The WeAct STM32H523 core board uses an STM32H523CC, as the OpenMUPP v1 probe does.
On the WeAct STM32H523 core board, the WCH-Link personality needs an external pull-up on SWDIO for one-wire targets.
See [WeAct STM32H523 core board](docs/boards.md#weact-stm32h523-core-board) in docs/boards.md for its limits and the wiring to the target.
The Mini and the ST-Link/V2-1 use an STM32F103x8 or STM32F103xB. The ST-Link/V2 mini clone uses an STM32F103 compatible MCU.
See [docs/boards.md](docs/boards.md) for the board definitions and how to add a board.

## Get the source code

The libraries are Git submodules in `dependencies/`.
Clone with the submodules:

```sh
git clone --recurse-submodules https://github.com/OpenMUPP/OpenMUPP.git
```

In an existing clone, run `git submodule update --init --recursive`.

The submodules are:

- `libArT`: core library (streams, logging, USB device stack).
- `system-stm32`: start-up code, CMSIS and peripheral drivers. Its `main` branch has the STM32F1 and the STM32H5 support. The primary repository is https://github.com/armint/system-stm32.
- `libart-stm32`: STM32 adapters for libArT. Its `main` branch has the STM32F1 and the STM32H5 support. The primary repository is https://github.com/armint/libart-stm32.
- `openmupp-bootloader`: the USB bootloader. The `master` branch has the CMake build and the boot request in `BKP->DR1`.

## Requirements

- CMake 3.20 or later. The `openmupp-bootloader` build and the CMake presets (`CMakePresets.json`) need CMake 3.21 or later.
- The Arm GNU Toolchain (`arm-none-eabi`), for example 14.3.rel1 as in CI.
- A host C++ compiler with C++23 support, for the unit tests.
- `clang-format`, for the `clangformat` target.
- Internet access at configure time: CMake FetchContent downloads CMSIS, ETL and doctest.

## Build the firmware

Select a preset, then configure and build:

```sh
export GCC_TOOLCHAIN_PREFIX=/opt/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi
cmake --preset mini-cmsis-dap
cmake --build --preset mini-cmsis-dap -j
```

The build directory is `build/<preset>`, here `build/mini-cmsis-dap`.
The presets `mini`, `stlinkv21` and `stlinkv2miniclone` build the Release images of one board.
The suffix of the preset selects the personalities:

- `-cmsis-dap`: the CMSIS-DAP personality, for example `mini-cmsis-dap`.
- `-dual`: the multi image with the CMSIS-DAP and the WCH-Link personalities. CMSIS-DAP is the default personality.
- `-wch-link`: the WCH-Link personality.
- `mini-versaloon`: the Versaloon personality, for the Mini.
- No suffix: the default personality, CMSIS-DAP. This is the same image as the `-cmsis-dap` preset.

The CI builds only the `-cmsis-dap` and the `-dual` images.
The Versaloon personality is for old tools only: use CMSIS-DAP for a new setup.
For the Versaloon personality or another set of personalities, add `-DMUPP_PERSONALITIES=<list>`, for example `-DMUPP_PERSONALITIES=versaloon` for the Versaloon personality, or `-DMUPP_PERSONALITIES=versaloon,cmsis-dap` for a dual image with Versaloon and CMSIS-DAP (see [CMake options](docs/cmake.md#cmake-options)).
Before this change, an empty `MUPP_PERSONALITIES` built the Versaloon personality, and a multi image with Versaloon and CMSIS-DAP started with Versaloon.
To keep the old image in an existing build directory, add `-DMUPP_PERSONALITIES=versaloon`, or `-DMUPP_DEFAULT_PERSONALITY=versaloon` for the multi image.
Some sets of personalities do not fit each image.
The build skips an image that does not fit, shows a warning and makes the other images.
With `-DMUPP_STRICT_SIZE=ON`, the build stops with an error instead (see [Images that do not fit](docs/cmake.md#images-that-do-not-fit)).
`mini-debug` is the Debug build of the Mini, with the CMSIS-DAP personality and without the RISC-V bridge. With the bridge, the Debug image does not fit behind a bootloader.
The presets `weacth523`, `weacth523-cmsis-dap`, `weacth523-dual`, `weacth523-wch-link` and `weacth523-debug` build the WeAct STM32H523 core board.
`cmake --list-presets` shows all presets.
[docs/cmake.md](docs/cmake.md#cmake-presets) describes them.
The presets read `GCC_TOOLCHAIN_PREFIX` from the environment.
If `arm-none-eabi-gcc` is in `/usr/bin`, you can omit it.
To set local paths or options, add your own presets in `CMakeUserPresets.json`.
Git ignores this file.

The alternative is to give the options on the command line:

```sh
cmake -B build/mini -DCMAKE_TOOLCHAIN_FILE=cmake/gcc_arm.cmake -DBOARD=mini \
      -DMUPP_PERSONALITIES=cmsis-dap \
      -DGCC_TOOLCHAIN_PREFIX=/opt/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi
cmake --build build/mini -j
```

Use `-DBOARD=stlinkv21`, `-DBOARD=stlinkv2miniclone` or `-DBOARD=weacth523` for the other boards.
If `arm-none-eabi-gcc` is in `/usr/bin`, you can omit `GCC_TOOLCHAIN_PREFIX`.

The build links the same firmware into one or more application images.
Each image has a different start address:

| Image | Start address | Flash for the application | Use |
|---|---|---|---|
| `standalone` | `0x08000000` | 64 KB | No bootloader. Write it with a debug probe. |
| `app-8k` | `0x08002000` | 56 KB | Behind the new 8 KB `openmupp-bootloader`. |
| `app-10k` | `0x08002800` | 54 KB | Behind the legacy 10 KB Versaloon bootloader of the existing boards. |

The Mini gets all three images.
The ST-Link/V2-1 and the ST-Link/V2 mini clone get the `standalone` and the `app-8k` image. They never had the legacy bootloader, so they get no `app-10k` image.
The WeAct STM32H523 core board gets the `standalone` image only, with 256 KB flash.
A multi image or an image with the RISC-V bridge has less flash, because the end of the flash holds the stored personality and the bridge mode.
This is 1 KB (one flash page) on the STM32F103 boards and 8 KB (one flash sector) on the STM32H5 board.

The build writes these files into the build directory:

- `openmupp-<image>.elf`, `.hex` and `.bin` for each image, for example `openmupp-app-10k.bin`.
- `openmupp-bootloader/openmupp-bootloader-<board>.hex` (and `.elf`, `.bin`): the bootloader for the board.
  Only the STM32F1 boards have a bootloader: the Mini, the ST-Link/V2-1 and the ST-Link/V2 mini clone.
- `openmupp-with-bootloader.hex` and `.bin`: the combined image of a board with a bootloader.
  It has `openmupp-bootloader` at `0x08000000` and the `app-8k` image at `0x08002000`.
  The `.bin` file has `0xFF` between the two.

See [docs/development.md](docs/development.md#build-outputs) for all build outputs.

See [docs/cmake.md](docs/cmake.md#cmake-options) for all CMake options.

## Flash the firmware

Select the image for the bootloader on the board:

| Board | Bootloader | Image | Start address |
|---|---|---|---|
| Existing Mini | Legacy Versaloon bootloader, 10 KB | `app-10k` | `0x08002800` |
| New Mini | `openmupp-bootloader`, 8 KB | `app-8k` | `0x08002000` |
| ST-Link/V2-1 or ST-Link/V2 mini clone | `openmupp-bootloader`, 8 KB | `app-8k` | `0x08002000` |
| Any board, debug probe | none | `standalone` | `0x08000000` |

The images behind a bootloader have an application image header with a CRC32 (see [docs/firmware-update.md](docs/firmware-update.md#application-image-header)).
`openmupp-bootloader` starts only an image with a valid header.

### Board with the legacy bootloader

An existing board keeps its legacy 10 KB bootloader.
To write the firmware, do these steps:

1. Start the bootloader. Hold the key (PB9) down while you connect the board to USB.
   A new serial port shows, for example `/dev/ttyACM0`.
2. Write the `.bin` file of the `app-10k` image with `stm32flash`:

   ```sh
   stm32flash -w openmupp-app-10k.bin -v -S 0x08002800 /dev/ttyACM0
   ```

3. Disconnect the board and connect it again. The firmware starts.

The `app-10k` image refuses `VERSALOON_FW_UPDATE`, because the legacy bootloader does not know the boot request.

### Board with openmupp-bootloader

A new board needs `openmupp-bootloader` one time.
An existing board can also change to it.
Write the combined image with a debug probe on the SWD pins of the dongle MCU.
It has the bootloader and the firmware, so one step is sufficient.
On an ST-Link/V2-1 or a mini clone with its original firmware, remove the read protection first (see [Install the bootloader](docs/firmware-update.md#install-the-bootloader)).
For example, with OpenOCD:

```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "program openmupp-with-bootloader.hex verify reset exit"
```

To write only the bootloader, use `openmupp-bootloader/openmupp-bootloader-<board>.hex` in the same command.

`openmupp-bootloader` write-protects its 8 KB area.
To write a new bootloader over it, remove the write protection first (see [Install the bootloader](docs/firmware-update.md#install-the-bootloader)).

To write the firmware through the bootloader, do these steps:

1. Start the bootloader. Send `VERSALOON_FW_UPDATE` or a switch command with the bootloader code `0xB0` (see [Bootloader code](docs/firmware-update.md#bootloader-code)), or use the key of the board (see [Start the bootloader](docs/firmware-update.md#start-the-bootloader)).
   A new serial port with the ID `0483:5740` shows, for example `/dev/ttyACM0`.
2. Write the `.bin` file of the `app-8k` image with `stm32flash`, and start it:

   ```sh
   stm32flash -w openmupp-app-8k.bin -v -g 0x08002000 -S 0x08002000 /dev/ttyACM0
   ```

### Board without a bootloader

Write the `standalone` image with a debug probe, for example with OpenOCD:

```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "program openmupp-standalone.hex verify reset exit"
```

On the STM32F1 boards, the `standalone` image refuses `VERSALOON_FW_UPDATE`.
On the WeAct STM32H523 core board, it starts the ROM USB DFU, and `dfu-util` writes the next firmware (see [STM32H5: ROM USB DFU](docs/firmware-update.md#stm32h5-rom-usb-dfu)).

See [docs/firmware-update.md](docs/firmware-update.md) for the other ways to start the bootloader, and for the flash layout.

## Use the dongle

Connect the dongle to the host with USB.
The default image shows as a USB device with the ID `1209:0001` and the product name `OpenMUPP CMSIS-DAP`.
The debug tools find it as a CMSIS-DAP v2 probe (see [docs/cmsis-dap.md](docs/cmsis-dap.md#use-the-probe)).

- **Serial port:** the dongle has a serial port (`/dev/ttyACM*` on Linux, a COM port on Windows).
  The port is bridged to the UART of the board: PA9 (TX) and PA10 (RX) on the Mini.
  Open it with a terminal program, for example `picocom /dev/ttyACM0`.

A firmware image built with `-DMUPP_PERSONALITIES=versaloon` shows as a USB device with the ID `0483:a038` and the product name `Versaloon`.
Use OpenOCD with `-f interface/vsllink.cfg`, in JTAG or SWD mode.
On Windows, OpenOCD needs a WinUSB driver for interface 0, for example installed with Zadig.
The serial port works as in the default image.

A firmware image built with `-DMUPP_PERSONALITIES=wch-link` shows as a USB device with the ID `1209:0001` and the product name `OpenMUPP WCH-Link`.
The host tools `wlink` and `minichlink` find the probe if it has the ID `1a86:8010` (see [docs/wch-link.md](docs/wch-link.md#use-the-probe)).
This image has no serial port.

A firmware image built with more than one personality is a multi image, for example `-DMUPP_PERSONALITIES=cmsis-dap,wch-link`.
`MUPP_DEFAULT_PERSONALITY` selects the personality when no personality is stored.
To change the personality, send a switch command, or use the key.
`tools/mupp_probe.py` sends the switch request on EP0 (see [Tools](#tools)).
To use the key, hold it down for 3 s until all LEDs flash.
Then release the key.
The dongle then resets and shows as the USB device of the next personality.
The dongle keeps the personality after a power cycle and after a firmware update with `stm32flash`.
The WCH-Link personality has no switch command: use the switch request on EP0 or the key (see [docs/usb-device.md](docs/usb-device.md#switch-request-on-ep0)).
See [docs/usb-device.md](docs/usb-device.md#multi-image).

The power LED shows the target power. The state LED flashes for each command of the host, and gives a double flash after a JTAG or SWD error. The USB LED shows the USB state.
See [docs/boards.md](docs/boards.md#start-up-and-leds) for the meaning of the LEDs.

## Tools

The Python scripts in `tools/` need Python 3.

| Script | Function | Needs |
|---|---|---|
| `tools/mupp_probe.py` | `show` lists the dongles and their personality. `switch <personality>` switches a multi image to `versaloon`, `cmsis-dap` or `wch-link`. `bridge off`, `on`, `auto` and `stored` set the mode of the RISC-V bridge of a CMSIS-DAP image. `bridge --store` also stores the mode: the mode after a USB reset and at power-up (see [docs/cmsis-dap.md](docs/cmsis-dap.md#bridge-switch)). `power on`, `off` and `status` switch the 3.3 V target supply of a CMSIS-DAP image and show the target voltage (see [docs/cmsis-dap.md](docs/cmsis-dap.md#target-power)). `--id VID:PID` selects a dongle by its USB ID. | `pyusb` |
| `tools/serial_loopback_test.py` | Tests the UART bridge with a loopback wire from TX to RX (see [docs/uart-bridge.md](docs/uart-bridge.md)). | `pyserial` |
| `tools/dap_uart_loopback_test.py` | Tests the `DAP_UART_*` commands of the CMSIS-DAP personality with a loopback wire from TX to RX (see [docs/cmsis-dap.md](docs/cmsis-dap.md#uart-hardware-test)). | `pyusb`, `pyserial` for `--serial-port` |
| `tools/flash_benchmark.py` | Measures the flash speed with OpenOCD (see [docs/development.md](docs/development.md#flash-speed-benchmark)). | OpenOCD |
| `tools/pyocd_jtag_test.py` | Runs the pyOCD JTAG test on an STM32F1 target with the CMSIS-DAP personality (see [docs/cmsis-dap.md](docs/cmsis-dap.md#jtag-hardware-test)). `--protocol swd` runs the same checks on SWD. `--target` selects the pyOCD target. It erases the flash of the target. | pyOCD |
| `tools/probe_hw_test.py` | Runs the hardware test of a multi image on an STM32F1 target and a UART loopback. It tests the Versaloon personality with OpenOCD (SWD and JTAG). It then switches to the CMSIS-DAP personality and tests it with OpenOCD, pyOCD and probe-rs. It also runs the UART loopback tests. It erases the flash of the target. | `pyusb`, `pyserial`, OpenOCD, pyOCD, probe-rs (`--skip` for a missing tool) |
| `tools/wch_link_frames_2wire.py`, `tools/wch_link_frames_1wire.py` | Decode the wire frames of a WCH-LinkE from a logic analyzer CSV file: 54-clock frames on two wires, 41-pulse frames on one wire (see [docs/wch-link-protocol.md](docs/wch-link-protocol.md#wire-frame-format)). | none |
| `tools/clock_rate.py` | Measures the clock rate of a bit-bang engine from a logic analyzer CSV file (see [docs/clock-pacing.md](docs/clock-pacing.md#measurements)). | none |

The `test_*.py` files are the unit tests of the scripts. They need no hardware.

## Documentation

- [docs/boards.md](docs/boards.md): board definitions, start-up and LEDs.
- [docs/usb-device.md](docs/usb-device.md): USB descriptors and interfaces, and how to check the enumeration.
- [docs/uart-bridge.md](docs/uart-bridge.md): the USB-to-UART bridge, and the serial port test script.
- [docs/debug-log.md](docs/debug-log.md): the debug log over the serial port in a `Debug` build.
- [docs/cmsis-dap.md](docs/cmsis-dap.md): the CMSIS-DAP personality and its command processor for SWD and JTAG.
- [docs/clock-pacing.md](docs/clock-pacing.md): how the SWD, JTAG and RVSWD engines time their clock, with measurements on the wire.
- [docs/firmware-update.md](docs/firmware-update.md): the bootloader, the firmware update and the flash layout.
- [docs/wch-link.md](docs/wch-link.md): the WCH-Link personality: build, USB ID, target pins, udev rule, size, implementation and hardware test.
- [docs/wch-link-protocol.md](docs/wch-link-protocol.md): the USB protocol and the wire frames of the WCH-LinkE, from captures ([docs/wch-link-captures/](docs/wch-link-captures/README.md)).
- [docs/cmake.md](docs/cmake.md): the CMake options and the CMake presets.
- [docs/development.md](docs/development.md): build outputs, unit tests, the flash speed benchmark, code format, version, CI and branches.
- [docs/status.md](docs/status.md): the functions of the firmware and the hardware test status.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Copyright (C) 2026 Armin van der Togt <armin@vandertogt.org>

OpenMUPP is free software. You can redistribute it and modify it under the terms of the GNU General Public License, version 3 or any later version. See [LICENSE](LICENSE).
The program has no warranty.

Some parts have a different license:

- `cmake/ClangFormat.cmake` uses the Boost Software License 1.0. See [LICENSES/BSL-1.0.txt](LICENSES/BSL-1.0.txt).
- The OpenOCD configuration files in `etc/openocd/` and `tools/stm32f1_clock.tcl` use GPL-2.0-or-later.
- The submodules `libArT`, `libart-stm32` and `system-stm32` use the MIT license. Each submodule has its own license file.
- The build downloads other libraries with CMake `FetchContent`, for example the CMSIS headers of Arm and ST (Apache-2.0). These libraries keep their own licenses.

The hardware design in the repository [openmupp-v1](https://github.com/OpenMUPP/openmupp-v1) has its own license.
