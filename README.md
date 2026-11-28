# OpenMUPP

OpenMUPP (Open Multi-architecture Universal Programmer & Probe) is the firmware for the OpenMUPP v1 probe.
OpenMUPP v1 is an open hardware USB debug probe with a CH32V307 MCU.
It has level shifters for targets from 1.2 V to 5 V, a target power supply of 3.3 V or 5 V, and a high-voltage (VPP) supply to program old MCUs, for example AVR and PIC.
The schematics are in the repository [openmupp-v1](https://github.com/OpenMUPP/openmupp-v1).

The probe can debug and program target chips of more than one architecture.
Today it supports Arm Cortex-M (SWD and JTAG), WCH RISC-V (CH32V) and AVR (ISP, debugWIRE and JTAG).
The goal is one probe for many architectures (see [Planned targets](#planned-targets)).

The [status](docs/status.md) document lists what the firmware does and what the hardware tests cover.

## Personalities

Each firmware image has one or more personalities.
A personality is the USB interface that a set of host tools uses:

| Personality | USB ID | Host tools | Target chips |
|---|---|---|---|
| CMSIS-DAP v2 (the default) | `1209:0001` | OpenOCD, pyOCD, probe-rs | Arm Cortex-M (SWD and JTAG), WCH RISC-V through the RISC-V bridge (see [docs/cmsis-dap.md](docs/cmsis-dap.md)) |
| AVR (EDBG), in the CMSIS-DAP image | `1209:0001` | avrdude, Bloom and AVaRICE with avr-gdb | AVR: ISP programming, debugWIRE programming and debug, JTAG programming and debug (see [docs/avr.md](docs/avr.md)) |
| WCH-Link | `1209:0001` | `wlink`, `minichlink` | WCH RISC-V chips (CH32V), see [docs/wch-link.md](docs/wch-link.md) |
| Versaloon (original protocol) | `0483:a038` | OpenOCD (`interface/vsllink.cfg`) | Arm Cortex-M (SWD and JTAG) |

The AVR personality is part of the CMSIS-DAP image.
It is the command set of the Microchip EDBG tools on the CMSIS-DAP v1 (HID) interface of that image.
On OpenMUPP v1, each image with `cmsis-dap` has it by default (`MUPP_CMSIS_DAP_AVR`, see [docs/cmsis-dap.md](docs/cmsis-dap.md#avr-commands)).
Thus the CMSIS-DAP and the AVR personalities run at the same time.

`wlink` and `minichlink` find only the WCH ID `1a86:8010`.
For personal use only, build the WCH-Link image with `MUPP_WCH_LINK_WCH_USB_ID` (see [docs/wch-link.md](docs/wch-link.md#usb-id)).

The Versaloon personality is for old tools only. For a new setup, use CMSIS-DAP.
The Versaloon personality is an independent implementation of the Versaloon USB protocol. It contains no code of the original Versaloon project. OpenMUPP is not related to that project or its author.

The CMSIS-DAP and the Versaloon images also have a USB-CDC serial port.
The firmware bridges it to a UART of the board.
A multi image has more than one personality, and you can select one at run time (see [Use the probe](#use-the-probe)).

## Planned targets

OpenMUPP v1 has hardware for more protocols than the firmware supports today.
These targets are planned. None of them works yet.

| Target | Protocols | State |
|---|---|---|
| AVR | High-voltage programming, PDI, TPI, UPDI and aWire. High-voltage programming will use the VPP supply of the board. | Planned. ISP, debugWIRE and JTAG work today (see [docs/avr.md](docs/avr.md)). |
| PIC | Programming. High-voltage programming will use the VPP supply of the board. | Planned. No design yet. |
| STM8 | SWIM (single-wire debug and programming interface) | Planned. No design yet. |

The planned protocols need the OpenMUPP v1 hardware: the level shifters, the split nRESET path and the VPP supply.
An adapter maps the target connector to the pinout of each target.
All adapters are passive, also the high-voltage adapters. The VPP supply is on the OpenMUPP v1 board.
Each adapter has an ID resistor. The firmware applies VPP only when the ID shows a high-voltage adapter (see [VPP sequence](docs/boards.md#vpp-sequence)).

## OpenMUPP v1 probe board

The board name in the build is `openmuppv1`.
The board has a CH32V307RCT6 and an 8 MHz crystal.
The firmware on OpenMUPP v1 has these functions:

- The debug engines and the UART bridge reach the target through the level shifters.
- The target supply switches on at 3.3 V or 5 V. The firmware reacts to the power fault inputs.
- The firmware reads the adapter ID and the board ID. It refuses the target on an adapter fault or an unknown board revision.
- USB runs at high speed (480 Mbit/s) in the CMSIS-DAP image.
- The DFU bootloader `openmupp-bootloader` updates the firmware over USB (see [Flash the firmware](#flash-the-firmware)).

The AVR and PIC high-voltage protocols do not use VPP yet. A host control sets VPP for the hardware test.
[docs/boards.md](docs/boards.md#openmupp-v1-probe-board) lists the pins of OpenMUPP v1 and the rules for its power functions.

## Get the source code

The libraries are Git submodules in `dependencies/`.
Clone with the submodules:

```sh
git clone --recurse-submodules https://github.com/OpenMUPP/OpenMUPP.git
```

In an existing clone, run `git submodule update --init --recursive`.

The submodules are:

- `libArT`: core library (streams, logging, USB device stack).
- `ch32v00x-system`: start-up code, device headers and peripheral drivers of the WCH CH32V00x, CH32V20x and CH32V30x. Only the CH32 boards (`openmuppv1`, `ydch32v307`) use it. Its `main` branch has the CH32V30x support. The primary repository is https://github.com/armint/ch32v00x-system.
- `ch32v00x-art`: the libArT adapters for the CH32V30x (the USB device controller). Only the CH32 boards use it. Its default branch is `dev`, not `main`. The primary repository is https://github.com/armint/ch32v00x-art.
- `openmupp-bootloader`: the USB DFU bootloader (https://github.com/OpenMUPP/openmupp-bootloader). Its `master` branch has the CMake build.
- `system-stm32`: start-up code, CMSIS and peripheral drivers of the STM32. Only the STM32 boards use it. The primary repository is https://github.com/armint/system-stm32.
- `libart-stm32`: the libArT adapters for the STM32. Only the STM32 boards use it. The primary repository is https://github.com/armint/libart-stm32.

## Requirements

- CMake 3.21 or later.
- The xPack RISC-V GCC (`riscv-none-elf`), for example 15.2.0-1 as in CI.
- For the Versaloon Mini and the ST-Link boards only: the Arm GNU Toolchain (`arm-none-eabi`), for example 15.3.rel2 as in CI.
- A host C++ compiler with C++23 support, for the unit tests.
- `clang-format`, for the `clangformat` target.
- Internet access at configure time: CMake FetchContent downloads CMSIS, ETL and doctest.

## Build the firmware

Select a preset. Give the install directory of the toolchain with `-DGCC_TOOLCHAIN_PREFIX`. Then configure and build:

```sh
cmake --preset openmuppv1-cmsis-dap \
      -DGCC_TOOLCHAIN_PREFIX=/opt/xpack-riscv-none-elf-gcc-15.2.0-1
cmake --build --preset openmuppv1-cmsis-dap -j
```

The build directory is `build/<preset>`, here `build/openmuppv1-cmsis-dap`.
If the compiler of the toolchain is in `/usr/bin`, you can omit `GCC_TOOLCHAIN_PREFIX`.
To keep local paths or options, add your own presets in `CMakeUserPresets.json`.
Git ignores this file.

These presets build OpenMUPP v1:

- `openmuppv1-cmsis-dap`: the CMSIS-DAP personality with the CMSIS-DAP v1 (HID) interface and the AVR personality (`MUPP_CMSIS_DAP_HID`, `MUPP_CMSIS_DAP_AVR`).
- `openmuppv1-debug`: the same personalities, as a Debug build with the debug log on the serial port and on UART5 (see [docs/debug-log.md](docs/debug-log.md)).

All personalities build for OpenMUPP v1.
For another set of personalities, add `-DMUPP_PERSONALITIES=<list>` (see [CMake options](docs/cmake.md#cmake-options)).
For example, `-DMUPP_PERSONALITIES=cmsis-dap,wch-link` builds a multi image with the CMSIS-DAP and the WCH-Link personalities.
`MUPP_DEFAULT_PERSONALITY` selects the personality of a multi image when no personality is stored. The default is `cmsis-dap`.

Some sets of personalities do not fit each image.
The build skips an image that does not fit, shows a warning and makes the other images.
With `-DMUPP_STRICT_SIZE=ON`, the build stops with an error instead (see [Images that do not fit](docs/cmake.md#images-that-do-not-fit)).
`cmake --list-presets` shows all presets.
[docs/cmake.md](docs/cmake.md#cmake-presets) describes them, and [docs/cmake.md](docs/cmake.md#cmake-options) describes all CMake options.

The build of OpenMUPP v1 writes these files into the build directory:

- `openmupp-app-8k.elf`, `.hex` and `.bin`: the firmware for the DFU bootloader. It starts at `0x08002000`, and it can be up to 248 KB.
- `openmupp-standalone.elf`, `.hex` and `.bin`: the firmware without a bootloader, at `0x08000000`. Write it with a debug probe.
- `openmupp-bootloader/openmupp-bootloader-openmuppv1.bin` (and `.elf`, `.hex`): the DFU bootloader alone.
- `openmupp-with-bootloader.bin`: the bootloader at `0x08000000` and the `app-8k` image at `0x08002000` in one file, for a new board.

See [docs/development.md](docs/development.md#build-outputs) for all build outputs, and [Flash layout and size limit](docs/firmware-update.md#flash-layout-and-size-limit) for the limits.

## Flash the firmware

`openmupp-bootloader` is a USB DFU 1.1 device (Device Firmware Upgrade) with the ID `1209:0001` and the product string `OpenMUPP Bootloader`.
It runs the `app-8k` image at `0x08002000`.
The bootloader checks the application image header (see [docs/firmware-update.md](docs/firmware-update.md#application-image-header)) and starts only a valid image.

### Write the bootloader on a new board

Write `openmupp-with-bootloader.bin` from `0x08000000` with a debug probe.
This file has the bootloader and the firmware, so one step is sufficient.
To write only the bootloader, use `openmupp-bootloader/openmupp-bootloader-openmuppv1.bin`.

The key of OpenMUPP v1 is on BOOT0.
A key that is down at a reset starts the ROM ISP bootloader of WCH (`4348:55e0`, `wchisp`), not `openmupp-bootloader`.
Use the ROM bootloader when `openmupp-bootloader` is damaged.

### Write the firmware with dfu-util

To write the firmware, do these steps:

1. Start the bootloader:

   ```sh
   tools/mupp_probe.py switch bootloader
   ```

   The firmware resets into the bootloader, and the tool waits for it.
2. Write the `app-8k` image with `dfu-util`:

   ```sh
   dfu-util -d 1209:0001 -D openmupp-app-8k.bin
   ```

The bootloader writes the image from `0x08002000`. The host cannot select an address.
After the download, the bootloader checks the image and starts the firmware.
With an image that is not valid, it reports `errVERIFY` and stays in DFU mode.
A download keeps the stored personality and the bridge mode.
On Linux, `dfu-util` and `mupp_probe.py` need access to the device: install `etc/70-openmupp-bootloader.rules`.

See [docs/firmware-update.md](docs/firmware-update.md) for the other ways to start the bootloader, and for the flash layout.

## Use the probe

Connect the probe to the host with USB.
The default image shows as a USB device with the ID `1209:0001` and the product name `OpenMUPP CMSIS-DAP`.
The debug tools find it as a CMSIS-DAP v2 probe (see [docs/cmsis-dap.md](docs/cmsis-dap.md#use-the-probe)).
On Linux, the udev rules in `etc/` give access to the probe.

- **AVR:** avrdude, Bloom and AVaRICE find the probe through the HID interface. See [docs/avr.md](docs/avr.md) for the wiring, the avrdude commands and the Bloom setup.
- **Serial port:** the probe has a serial port (`/dev/ttyACM*` on Linux, a COM port on Windows).
  On OpenMUPP v1, the port is bridged to UART7: PA6 (TX) and PA7 (RX), through the level shifters.
  Open it with a terminal program, for example `picocom /dev/ttyACM0`.

A firmware image built with `-DMUPP_PERSONALITIES=wch-link` shows as a USB device with the ID `1209:0001` and the product name `OpenMUPP WCH-Link`.
The host tools `wlink` and `minichlink` find the probe only if it has the ID `1a86:8010` (see [docs/wch-link.md](docs/wch-link.md#use-the-probe)).
This image has no serial port.

A firmware image built with `-DMUPP_PERSONALITIES=versaloon` shows as a USB device with the ID `0483:a038` and the product name `Versaloon`.
Use OpenOCD with `-f interface/vsllink.cfg`, in JTAG or SWD mode.
On Windows, OpenOCD needs a WinUSB driver for interface 0, for example installed with Zadig.

To change the personality of a multi image, send a switch command or use the key.
`tools/mupp_probe.py switch <personality>` sends the switch request on EP0 (see [Tools](#tools)).
To use the key, hold it down for 3 s until all LEDs flash.
Then release the key.
The probe then resets and shows as the USB device of the next personality.
The probe keeps the personality after a power cycle and after a firmware update.
The WCH-Link personality has no switch command: use the switch request on EP0 or the key (see [docs/usb-device.md](docs/usb-device.md#switch-request-on-ep0)).
See [docs/usb-device.md](docs/usb-device.md#multi-image).

The power LED shows the target power. The state LED flashes for each command of the host, and gives a double flash after a JTAG or SWD error. On OpenMUPP v1, the state LED is continuously on when the firmware refuses the target, for example on an adapter fault. The USB LED shows the USB state.
See [docs/boards.md](docs/boards.md#start-up-and-leds) for the meaning of the LEDs.

## Other boards

OpenMUPP v1 is the main target.
The firmware also builds for these boards. They help to develop and test the code. The Versaloon Mini and the ST-Link boards can also be a probe with fewer functions.

| `BOARD` | Hardware | MCU | Use |
|---|---|---|---|
| `ydch32v307` | YD-CH32V307VCT6 development board, see [docs/boards.md](docs/boards.md#yd-ch32v307vct6-development-board) | CH32V307VCT6 | Development board with the MCU family of OpenMUPP v1. It was used for development before OpenMUPP v1 was available. No level shifters, 3.3 V targets only. |
| `mini` | Versaloon Mini | STM32F103x8 or STM32F103xB | Probe with fewer functions. |
| `stlinkv21` | ST-Link/V2-1 of the Nucleo-64 boards (MB1136, MB1360), see [docs/boards.md](docs/boards.md#st-linkv2-1) | STM32F103xB | Probe with fewer functions. SWD only (no JTAG pins). |
| `stlinkv2miniclone` | ST-Link/V2 mini clone (USB stick), see [docs/stlinkv2-mini-clone.md](docs/stlinkv2-mini-clone.md) | STM32F103 compatible | Probe with fewer functions. SWD only (no JTAG pins). |

The AVR personality, SWO and the CMSIS-DAP v1 (HID) interface are only on the CH32V307 boards.
The presets `ydch32v307`, `ydch32v307-cmsis-dap`, `ydch32v307-dual`, `ydch32v307-all` and `ydch32v307-wch-link` build the YD-CH32V307VCT6 board with the RISC-V GCC.

The STM32F1 boards (`mini`, `stlinkv21`, `stlinkv2miniclone`) need the Arm GNU Toolchain:

```sh
cmake --preset mini-cmsis-dap \
      -DGCC_TOOLCHAIN_PREFIX=/opt/arm-gnu-toolchain-15.3.rel2-x86_64-arm-none-eabi
cmake --build --preset mini-cmsis-dap -j
```

The suffix of the preset selects the personalities: `-cmsis-dap`, `-dual` (CMSIS-DAP and WCH-Link), `-wch-link`, and `mini-versaloon` for the Versaloon personality.
`mini-debug` is the Debug build of the Mini, without the RISC-V bridge.
With the bridge, the Debug image does not fit behind a bootloader.

On the STM32F1 boards, the flash is small, so the bootloader and the image sizes are different:

| Image | Start address | Flash for the application | Use |
|---|---|---|---|
| `standalone` | `0x08000000` | 64 KB | No bootloader. Write it with a debug probe. |
| `app-8k` | `0x08002000` | 56 KB | Behind the 8 KB `openmupp-bootloader` or the 8 KB Versaloon bootloader. |
| `app-32k` | `0x08008000` | 96 KB | Mini only: behind the 32 KB Versaloon bootloader of the newer Mini, with 128 KB flash. |

A multi image or an image with the RISC-V bridge has 1 KB (one flash page) less, because the end of the flash holds the stored personality and the bridge mode.
Write `openmupp-with-bootloader.hex` with a debug probe on the SWD pins of the board MCU, for example with OpenOCD.
On an ST-Link/V2-1 or a mini clone with its original firmware, remove the read protection first (see [Install the bootloader](docs/firmware-update.md#install-the-bootloader)).
Then write the `app-8k` image with `dfu-util`, as on OpenMUPP v1.
A Mini can also keep its Versaloon bootloader: hold the key (PB9) down while you connect the board to USB, and write the image with `stm32flash` (see [docs/firmware-update.md](docs/firmware-update.md#write-the-firmware)).
On the Mini, the serial port is bridged to PA9 (TX) and PA10 (RX).

See [docs/boards.md](docs/boards.md) for the board definitions and how to add a board.

## Tools

The host tools in `tools/` need Python 3.
Each script is executable, for example `tools/mupp_probe.py show`. On Windows, start it with `py`.

| Script | Function | Needs |
|---|---|---|
| `tools/mupp_probe.py` | `show` lists the probes and their personality. `switch <personality>` switches a multi image to `versaloon`, `cmsis-dap` or `wch-link`. `switch bootloader` starts the bootloader. `bridge off`, `on`, `auto` and `stored` set the mode of the RISC-V bridge of a CMSIS-DAP image. `bridge --store` also stores the mode: the mode after a USB reset and at power-up (see [docs/cmsis-dap.md](docs/cmsis-dap.md#bridge-switch)). `power on`, `off` and `status` switch the target supply of a CMSIS-DAP image and show the target voltage (see [docs/cmsis-dap.md](docs/cmsis-dap.md#target-power)). `uart normal`, `single` and `status` set or show the wire mode of the UART on OpenMUPP v1 (see [docs/cmsis-dap.md](docs/cmsis-dap.md#uart-wire-mode)). `adapter` shows the firmware version, the board revision and the adapter of a CMSIS-DAP image (see [docs/cmsis-dap.md](docs/cmsis-dap.md#board-id-and-adapter-id)). `reset` resets the probe in each personality (see [docs/usb-device.md](docs/usb-device.md#switch-request-on-ep0)). `--id VID:PID` selects a probe by its USB ID. | `pyusb` |
| `tools/mupp_combine_images.py` | Combines the bootloader and an application image into one flash image from `0x08000000`, for a new board. The build uses it to make `openmupp-with-bootloader.bin`. | none |

[docs/test-tools.md](docs/test-tools.md) describes the hardware tests, the benchmarks and the decoders in `tools/hwtest/` and `tools/analysis/`.

## Documentation

- [docs/status.md](docs/status.md): the functions of the firmware and the hardware test status.
- [docs/boards.md](docs/boards.md): board definitions, start-up and LEDs, and the OpenMUPP v1 power functions.
- [docs/avr.md](docs/avr.md): AVR programming and debug: USB ID, Bloom, udev rule, wiring, avrdude commands, debugWIRE, JTAG and low-clock targets.
- [docs/avr-jtag-ocd.md](docs/avr-jtag-ocd.md): the AVR JTAG on-chip debug protocol.
- [docs/cmsis-dap.md](docs/cmsis-dap.md): the CMSIS-DAP personality and its command processor for SWD and JTAG.
- [docs/wch-link.md](docs/wch-link.md): the WCH-Link personality: build, USB ID, target pins, udev rule, size, implementation and hardware test.
- [docs/wch-link-protocol.md](docs/wch-link-protocol.md): the USB protocol and the wire frames of the WCH-LinkE, from captures ([docs/wch-link-captures/](docs/wch-link-captures/README.md)).
- [docs/usb-device.md](docs/usb-device.md): USB descriptors and interfaces, and how to check the enumeration.
- [docs/uart-bridge.md](docs/uart-bridge.md): the USB-to-UART bridge, and the serial port test script.
- [docs/debug-log.md](docs/debug-log.md): the debug log over the serial port in a `Debug` build.
- [docs/clock-pacing.md](docs/clock-pacing.md): how the SWD, JTAG and RVSWD engines time their clock, with measurements on the wire.
- [docs/firmware-update.md](docs/firmware-update.md): the bootloader, the firmware update and the flash layout.
- [docs/cmake.md](docs/cmake.md): the CMake options and the CMake presets.
- [docs/test-tools.md](docs/test-tools.md): the hardware test scripts, the benchmarks and the decoders of captures.
- [docs/development.md](docs/development.md): build outputs, unit tests, the flash speed benchmark, code format, version, CI and branches.
- [docs/stlinkv2-mini-clone.md](docs/stlinkv2-mini-clone.md): the ST-Link/V2 mini clone.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Copyright (C) 2026 Armin van der Togt <armin@vandertogt.org>

OpenMUPP is free software. You can redistribute it and modify it under the terms of the GNU General Public License, version 3 or any later version. See [LICENSE](LICENSE).
The program has no warranty.

Some parts have a different license:

- `cmake/ClangFormat.cmake` uses the Boost Software License 1.0. See [LICENSES/BSL-1.0.txt](LICENSES/BSL-1.0.txt).
- The OpenOCD configuration files in `etc/openocd/` and `tools/hwtest/stm32f1_clock.tcl` use GPL-2.0-or-later.
- The submodules `libArT`, `libart-stm32` and `system-stm32` use the MIT license. Each submodule has its own license file.
- The submodule `ch32v00x-system` uses the MIT license. Its WCH device files and SVD files keep the WCH terms (see its README). Only the CH32 boards use this submodule.
- The build downloads other libraries with CMake `FetchContent`, for example the CMSIS headers of Arm and ST (Apache-2.0). These libraries keep their own licenses.

The hardware design in the repository [openmupp-v1](https://github.com/OpenMUPP/openmupp-v1) has its own license.
