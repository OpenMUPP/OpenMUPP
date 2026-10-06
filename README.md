# versaloon-next-gen

Firmware for the Versaloon Mini and OtherUse debug adapters (STM32F103C8/CB, 12 MHz crystal, 72 MHz).
The firmware is compatible with the original Versaloon USB protocol, so unmodified OpenOCD can use it.
It also has a USB-CDC (Communication Device Class) serial port, which is bridged to a UART.

The project is at an early stage. The current firmware sets up the board and shows a heartbeat on an LED.
USB is not available yet.
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
3. It puts the board pins into a safe state:
   - All LEDs are off, but the power LED is on.
   - The USB pull-up is off, so the host does not see the device.
   - The target power is off (the switch pin floats).
   - The key pin has a pull resistor to the released level.

The JTAG, SWD, UART and Vref pins stay inputs until their drivers configure them.

The main loop shows a heartbeat on the state LED:

| State LED | Meaning |
|---|---|
| Short flash, one time each second | The firmware runs. The clock comes from the crystal. |
| Fast flash, five times each second | The crystal or the PLL did not start. The clock is HSI (8 MHz) and USB cannot work. |
| On or off all the time | The main loop does not run. |

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
