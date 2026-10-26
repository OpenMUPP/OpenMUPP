# versaloon-NG

versaloon-NG is firmware for debug probes with an STM32F103 chip: the Versaloon Mini and the OtherUse.
A build for the ST-Link/V2 mini clone also exists, with a hardware test of the USB personalities and the LEDs. A build for the ST-Link/V2 exists, but it has no hardware test (see [Boards](#boards)).
The port to the STM32H5 (the WeAct STM32H523 core board) has the Versaloon and the CMSIS-DAP personalities.
Each firmware image has one of these personalities:

| Personality | USB ID | Debug tools | Target chips |
|---|---|---|---|
| Versaloon (original protocol, the default) | `0483:a038` | Unmodified OpenOCD (`interface/vsllink.cfg`) | Arm Cortex-M, SWD and JTAG |
| CMSIS-DAP v2 | `0403:9b3c` | OpenOCD, pyOCD, probe-rs | Arm Cortex-M, SWD and JTAG (see [docs/cmsis-dap.md](docs/cmsis-dap.md)) |
| WCH-Link | `1209:0001` | `wlink`, `minichlink` | WCH RISC-V chips (CH32V), see [docs/wch-link.md](docs/wch-link.md) |

`wlink` and `minichlink` find only the WCH ID `1a86:8010`.
Build the WCH-Link image with `VSL_WCH_LINK_WCH_USB_ID` for personal use only (see [docs/wch-link.md](docs/wch-link.md#usb-id)).

The Versaloon and CMSIS-DAP personalities also have a USB-CDC serial port.
The firmware bridges it to a UART of the board.
The WCH-Link personality has no serial port.
A multi image has more than one personality, and you select one at run time.
The firmware drives the LEDs for the target power, the command activity and the USB state.
The key starts the bootloader, and the Versaloon personality also starts it on request (see [Flash the firmware](#flash-the-firmware)).

The [status](docs/status.md) document lists what the firmware does and what the hardware tests cover.

## Boards

| `BOARD` | Hardware | Crystal | Status |
|---|---|---|---|
| `mini` | Versaloon Mini | 12 MHz | target |
| `otheruse` | OtherUse | 12 MHz | target |
| `stlinkv2` | ST-Link/V2, pin map of the official design | 8 MHz | compile-only |
| `stlinkv2miniclone` | ST-Link/V2 mini clone (USB stick), see [docs/stlinkv2-mini-clone.md](docs/stlinkv2-mini-clone.md) | 8 MHz | target, no target transfers tested |
| `weacth523` | WeAct STM32H523 core board | 8 MHz | target, 3.3 V targets only |

The Mini, the OtherUse and the ST-Link/V2 use an STM32F103x8 or STM32F103xB. The ST-Link/V2 mini clone uses an STM32F103 compatible MCU.
The WeAct STM32H523 core board uses an STM32H523CC.
On the WeAct STM32H523 core board, the WCH-Link personality needs an external pull-up on SWDIO for one-wire targets.
See [WeAct STM32H523 core board](docs/boards.md#weact-stm32h523-core-board) in docs/boards.md for its limits and the wiring to the target.
See [docs/boards.md](docs/boards.md) for the board definitions and how to add a board.

## Get the source code

The libraries are Git submodules in `dependencies/`.
Clone with the submodules:

```sh
git clone --recurse-submodules ssh://git@gitlab.vandertogt.org:3022/embedded-projects/versaloon-ng.git
```

In an existing clone, run `git submodule update --init --recursive`.

The submodules are:

- `libArT`: core library (streams, logging, USB device stack).
- `system-stm32`: start-up code, CMSIS and peripheral drivers. The `dev` branch has the STM32F1 and the STM32H5 support.
- `libart-stm32`: STM32 adapters for libArT. The `dev` branch has the STM32F1 and the STM32H5 support.
- `versaloon-bootloader`: the USB bootloader. The `master` branch has the CMake build and the boot request in `BKP->DR1`.

## Requirements

- CMake 3.20 or later. The `versaloon-bootloader` build and the CMake presets (`CMakePresets.json`) need CMake 3.21 or later.
- The Arm GNU Toolchain (`arm-none-eabi`), for example 14.3.rel1 as in CI.
- A host C++ compiler with C++23 support, for the unit tests.
- `clang-format`, for the `clangformat` target.
- Internet access at configure time: CMake FetchContent downloads CMSIS, ETL and doctest.

## Build the firmware

Select a preset, then configure and build:

```sh
export GCC_TOOLCHAIN_PREFIX=/opt/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi
cmake --preset mini
cmake --build --preset mini -j
```

The build directory is `build/<preset>`, here `build/mini`.
The presets `mini`, `otheruse`, `stlinkv2` and `stlinkv2miniclone` build the Release images of one board.
A preset without a suffix builds the Versaloon personality.
Add `-cmsis-dap`, `-dual`, `-wch-link` or `-triple` for the other personalities, for example `mini-cmsis-dap`.
A `-dual` preset builds the multi image with the Versaloon and the CMSIS-DAP personalities.
A `-triple` preset builds the multi image with the Versaloon, the CMSIS-DAP and the WCH-Link personalities.
For another set of personalities, add `-DVSL_PERSONALITIES=<list>`, for example `-DVSL_PERSONALITIES=versaloon,wch-link` (see [CMake options](docs/development.md#cmake-options)).
Some sets of personalities do not fit each image.
The build skips an image that does not fit, shows a warning and makes the other images.
With `-DVSL_STRICT_SIZE=ON`, the build stops with an error instead (see [Images that do not fit](docs/development.md#images-that-do-not-fit)).
`mini-debug` is the Debug build of the Mini.
The presets `weacth523`, `weacth523-cmsis-dap`, `weacth523-dual`, `weacth523-wch-link` and `weacth523-debug` build the WeAct STM32H523 core board.
`cmake --list-presets` shows all presets.
[docs/development.md](docs/development.md#cmake-presets) describes them.
The presets read `GCC_TOOLCHAIN_PREFIX` from the environment.
If `arm-none-eabi-gcc` is in `/usr/bin`, you can omit it.
To set local paths or options, add your own presets in `CMakeUserPresets.json`.
Git ignores this file.

The alternative is to give the options on the command line:

```sh
cmake -B build/mini -DCMAKE_TOOLCHAIN_FILE=cmake/gcc_arm.cmake -DBOARD=mini \
      -DGCC_TOOLCHAIN_PREFIX=/opt/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi
cmake --build build/mini -j
```

Use `-DBOARD=otheruse`, `-DBOARD=stlinkv2`, `-DBOARD=stlinkv2miniclone` or `-DBOARD=weacth523` for the other boards.
If `arm-none-eabi-gcc` is in `/usr/bin`, you can omit `GCC_TOOLCHAIN_PREFIX`.

The build links the same firmware into one or more application images.
Each image has a different start address:

| Image | Start address | Flash for the application | Use |
|---|---|---|---|
| `standalone` | `0x08000000` | 64 KB | No bootloader. Write it with a debug probe. |
| `app-8k` | `0x08002000` | 56 KB | Behind the new 8 KB `versaloon-bootloader`. |
| `app-10k` | `0x08002800` | 54 KB | Behind the legacy 10 KB Versaloon bootloader of the existing boards. |

The Mini and the OtherUse get all three images.
The ST-Link/V2 and the ST-Link/V2 mini clone get the `standalone` and the `app-8k` image. They never had the legacy bootloader, so they get no `app-10k` image.
The WeAct STM32H523 core board gets the `standalone` image only, with 256 KB flash.
A multi image has less flash, because the end of the flash holds the stored personality.
This is 1 KB (one flash page) on the STM32F103 boards and 8 KB (one flash sector) on the STM32H5 board.

The build writes these files into the build directory:

- `versaloon-ng-<image>.elf`, `.hex` and `.bin` for each image, for example `versaloon-ng-app-10k.bin`.
- `versaloon-bootloader/versaloon-bootloader-<board>.hex` (and `.elf`, `.bin`): the bootloader for the board.
  Only the STM32F1 boards have a bootloader: the Mini, the OtherUse, the ST-Link/V2 and the ST-Link/V2 mini clone.
- `versaloon-ng-with-bootloader.hex` and `.bin`: the combined image of a board with a bootloader.
  It has `versaloon-bootloader` at `0x08000000` and the `app-8k` image at `0x08002000`.
  The `.bin` file has `0xFF` between the two.

See [docs/development.md](docs/development.md#build-outputs) for all build outputs.

See [docs/development.md](docs/development.md#cmake-options) for all CMake options.

## Flash the firmware

Select the image for the bootloader on the board:

| Board | Bootloader | Image | Start address |
|---|---|---|---|
| Existing Mini or OtherUse | Legacy Versaloon bootloader, 10 KB | `app-10k` | `0x08002800` |
| New Mini or OtherUse | `versaloon-bootloader`, 8 KB | `app-8k` | `0x08002000` |
| Any board, debug probe | none | `standalone` | `0x08000000` |

The images behind a bootloader have an application image header with a CRC32 (see [docs/firmware-update.md](docs/firmware-update.md#application-image-header)).
`versaloon-bootloader` starts only an image with a valid header.

### Board with the legacy bootloader

An existing board keeps its legacy 10 KB bootloader.
To write the firmware, do these steps:

1. Start the bootloader. Hold the key (PB9) down while you connect the board to USB.
   A new serial port shows, for example `/dev/ttyACM0`.
2. Write the `.bin` file of the `app-10k` image with `stm32flash`:

   ```sh
   stm32flash -w versaloon-ng-app-10k.bin -v -S 0x08002800 /dev/ttyACM0
   ```

3. Disconnect the board and connect it again. The firmware starts.

The `app-10k` image refuses `VERSALOON_FW_UPDATE`, because the legacy bootloader does not know the boot request.

### Board with versaloon-bootloader

A new board needs `versaloon-bootloader` one time.
An existing board can also change to it.
Write the combined image with a debug probe on the SWD pins of the dongle MCU.
It has the bootloader and the firmware, so one step is sufficient.
For example, with OpenOCD:

```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "program versaloon-ng-with-bootloader.hex verify reset exit"
```

To write only the bootloader, use `versaloon-bootloader/versaloon-bootloader-<board>.hex` in the same command.

`versaloon-bootloader` write-protects its 8 KB area.
To write a new bootloader over it, remove the write protection first (see [Install the bootloader](docs/firmware-update.md#install-the-bootloader)).

To write the firmware through the bootloader, do these steps:

1. Start the bootloader. Hold the key (PB9) down and reset the board, or send `VERSALOON_FW_UPDATE` or a switch command with the bootloader code `0xB0` (see [Bootloader code](docs/firmware-update.md#bootloader-code)).
   A new serial port with the ID `0483:5740` shows, for example `/dev/ttyACM0`.
2. Write the `.bin` file of the `app-8k` image with `stm32flash`, and start it:

   ```sh
   stm32flash -w versaloon-ng-app-8k.bin -v -g 0x08002000 -S 0x08002000 /dev/ttyACM0
   ```

### Board without a bootloader

Write the `standalone` image with a debug probe, for example with OpenOCD:

```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "program versaloon-ng-standalone.hex verify reset exit"
```

On the STM32F1 boards, the `standalone` image refuses `VERSALOON_FW_UPDATE`.
On the WeAct STM32H523 core board, it starts the ROM USB DFU, and `dfu-util` writes the next firmware (see [STM32H5: ROM USB DFU](docs/firmware-update.md#stm32h5-rom-usb-dfu)).

See [docs/firmware-update.md](docs/firmware-update.md) for the other ways to start the bootloader, and for the flash layout.

## Use the dongle

Connect the dongle to the host with USB.
It shows as a USB device with the ID `0483:a038` and the product name `Versaloon`.

- **Debug adapter:** use OpenOCD with `-f interface/vsllink.cfg`, in JTAG or SWD mode.
  On Windows, OpenOCD needs a WinUSB driver for interface 0, for example installed with Zadig.
- **Serial port:** the dongle has a serial port (`/dev/ttyACM*` on Linux, a COM port on Windows).
  The port is bridged to the UART of the board: PA9 (TX) and PA10 (RX) on the Mini and the OtherUse.
  Open it with a terminal program, for example `picocom /dev/ttyACM0`.

A firmware image built with `-DVSL_PERSONALITIES=cmsis-dap` shows as a USB device with the ID `0403:9b3c` and the product name `Versaloon CMSIS-DAP`.
The debug tools find it as a CMSIS-DAP v2 probe (see [docs/cmsis-dap.md](docs/cmsis-dap.md#use-the-probe)).
The serial port works as in the default image.

A firmware image built with `-DVSL_PERSONALITIES=wch-link` shows as a USB device with the ID `1209:0001` and the product name `Versaloon WCH-Link`.
The host tools `wlink` and `minichlink` find the probe if it has the ID `1a86:8010` (see [docs/wch-link.md](docs/wch-link.md#use-the-probe)).
This image has no serial port.

A firmware image built with more than one personality is a multi image, for example `-DVSL_PERSONALITIES=versaloon,cmsis-dap,wch-link`.
`VSL_DEFAULT_PERSONALITY` selects the personality when no personality is stored.
The deprecated option `VSL_PROTOCOL=dual` builds the image with the Versaloon and the CMSIS-DAP personalities.
To change the personality, send a switch command, or use the key.
`tools/vsl_personality.py` sends the switch request on EP0 (see [Tools](#tools)).
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
| `tools/vsl_personality.py` | `show` lists the dongles and their personality. `switch <personality>` switches a multi image to `versaloon`, `cmsis-dap` or `wch-link`. | `pyusb` |
| `tools/vsl_rv_bridge.py` | `on` and `off` switch the RISC-V bridge of a CMSIS-DAP image with `VSL_CMSIS_DAP_RV_BRIDGE`, for example before probe-rs starts (see [docs/cmsis-dap.md](docs/cmsis-dap.md#bridge-switch)). | `pyusb` |
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
- [docs/development.md](docs/development.md): build outputs, unit tests, the flash speed benchmark, code format, version, CI and branches.
- [docs/status.md](docs/status.md): the functions of the firmware and the hardware test status.
- [docs/plan.md](docs/plan.md): the plan and the work items.
