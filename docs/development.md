# Development

This document is for developers of the firmware: build outputs, tests, the flash speed benchmark, code format, version, CI and branches.

## Build outputs

The README shows how to build the firmware.
The build writes these files into the build directory:

- `versaloon-next-gen-<image>.elf`, `.hex` and `.bin` for each application image (`standalone`, `app-8k`, `app-10k`).
  The README lists the images and their start addresses.
- Copies of the `.hex` and `.bin` files with the board, the image, the build type and the version in the name.
  An example is `versaloon-next-gen-mini-app-8k-Release-v0.1.0.hex`.
  An image with `VSL_PROTOCOL=cmsis-dap` has `-cmsis-dap` after the board, for example `versaloon-next-gen-mini-cmsis-dap-app-8k-Release-v0.1.0.hex`.
  An image with `VSL_PROTOCOL=dual` has `-dual` after the board, or `-dual-cmsis-dap` with `VSL_DEFAULT_PROTOCOL=cmsis-dap`.
- `versaloon-next-gen-<image>.map`, the linker map of each image.

All images use the same object files.
Only `src/main.cpp` is compiled for each image, because `VSL_HAS_BOOTLOADER_ENTRY` changes it.
One linker script, `ldscripts/mem-f103.ld`, links all images.
CMake gives it the start of the application with `-Wl,--defsym=__vsl_app_offset=<bytes>`.
- `versaloon-bootloader/versaloon-bootloader-<board>.elf`, `.hex` and `.bin`: the bootloader for the board.
- `versaloon-next-gen-with-bootloader.hex` and `.bin`, and copies with the board, the build type and the version in the name: the combined image of the bootloader and the `app-8k` image.
  `tools/vsl_combine_images.py` makes it. The tests are in `tools/test_vsl_combine_images.py`.
- `vsl-images.txt`: each image with its size limit. CI runs `ci/check-size.sh` on each line.

For the Mini and the OtherUse, the build also makes `versaloon-bootloader` for the same board.
The target is `versaloon-bootloader`.
`cmake/Bootloader.cmake` builds the submodule with its own CMake build, as an `ExternalProject`.
Thus the firmware options and the build type do not change the bootloader image.
The bootloader build uses the compiler of the firmware build.
There is no bootloader for the ST-Link/V2, and no bootloader when `VSL_BUILD_BOOTLOADER` is `OFF`.

## Unit tests

The unit tests use doctest and run on the host.
They check the board definitions, the LED patterns (activity, target power, clock fault and the flashes of the personality switch), the version, the USB descriptors, the log sink, the line coding map of the UART bridge, the bootloader entry, the Versaloon command protocol, the `USB_TO_ALL` dispatcher, the `GPIO` block (with the release of SRST and TRST when the host goes away), the `JTAG_RAW` block, the JTAG bit-bang engine (on mock pins), the JTAG fast path (on a mock SPI, compared with the bit-bang engine), the `SWD` block, the SWD request and parity encoding, the SWD bit-bang engine (on mock pins with a mock target), the CMSIS-DAP command processor (on the SWD bit-bang engine, and on the JTAG engines with a mock JTAG-DP, see [cmsis-dap.md](cmsis-dap.md#tests)), the `POWER` block (on a fake power switch and a fake voltage sense), and the Vref scale.
The USB tests also run the descriptors and the Versaloon function on the libArT USB device stack, with the fake USB controller of libArT.
The same tests run the descriptors and the function of the CMSIS-DAP personality, and check that its packet memory fits 512 bytes.
They also check the BOS and the MS OS 2.0 descriptor set, and run the requests of Windows for them.
The UART bridge tests run the bridge with the CDC-ACM function of libArT, the fake USB controller and a fake UART.
Two of these tests run a long JTAG shift on mock pins, with and without the serial service of the engines.
With the service, no UART data is lost.
Other tests check the serial-only mode of the USB event queue, and the calls of the serial service in the JTAG and SWD engines.
Configure without the toolchain file:

```sh
cmake -B build/tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build/tests -j
ctest --test-dir build/tests --output-on-failure
```

### Replay tests

`tests/TestReplay.cpp` replays the USB traffic of OpenOCD sessions through the Versaloon function and the `USB_TO_ALL` dispatcher.
The traces are in `tests/replay/`. There is one trace for JTAG mode and one trace for SWD mode.
A trace has one transfer on each line:

- `> 7f 06 00 7e 03 00` is an OUT transfer of the host.
- `< 00 08 00 ...` is the IN transfer that the firmware must send.
  A `*` at the end matches the remaining bytes of the reply.
- A line that starts with `#` is a comment.

The current traces come from the code of the OpenOCD `vsllink` driver, not from a capture.
The GPIO block is the real block, on fake pins.
The JTAG_RAW block is the real block on the bit-bang engine, with fake pins. The target of the fake pins drives TDO low.
The SWD block is the real block on the bit-bang engine, with the mock pins and the mock target of the engine tests. The mock target replies `OK`, and `0x1BA01477` to each read.

To replace a trace with a capture, do these steps:

1. Build the original Versaloon firmware with `USB_TO_XXX_EN 1` in `app_cfg.h`, and flash it.
2. Record the traffic with `usbmon`, for example with Wireshark on the `usbmonN` interface of the bus.
3. Run an OpenOCD session with `-f interface/vsllink.cfg`, in JTAG mode or in SWD mode.
4. Write the bulk transfers of endpoints `0x03` and `0x82` into the trace format.

## Flash speed benchmark

`tools/flash_benchmark.py` measures the flash speed of the dongle with OpenOCD.
Use it to compare the old firmware and this firmware on the same target (work item #17).
The target is an STM32F1, with JTAG, SWD and SRST connected.

The script writes a test image into the target flash, verifies it and reads it back.
It does these steps in JTAG mode and in SWD mode, at each adapter speed.
Each run is a new OpenOCD process.
The script reads the times from the OpenOCD output, and also measures the time of the full OpenOCD process.
It compares the read-back data with the image.

After reset, the target core runs at 8 MHz (HSI), and JTAG must not be faster than F_CPU / 6.
Thus the target speed limits the measurement.
For this reason, `tools/stm32f1_clock.tcl` sets the target clock before the measurement:

1. OpenOCD connects and halts the target at 1000 kHz.
2. The `stm32f1_clock_max` procedure sets 64 MHz: HSI / 2 × 16 with the PLL, two flash wait states, APB1 at 32 MHz.
   64 MHz allows a TCK of 10.6 MHz, which is more than the 9 MHz of the fast path.
   The procedure does not use the HSE, because the crystal frequency of the target board is not known.
   A wrong PLL factor overclocks the target.
3. OpenOCD sets the adapter speed of the test.
   In JTAG mode, `jtag arp_init` then resets the TAP and the TAP state of OpenOCD.
   This is necessary for the old firmware: it sets up the JTAG pins again on each speed change, and the TAP goes to Test-Logic-Reset.

The results show the target clock.
`--no-clock-setup` keeps the target at 8 MHz.

The test image is pseudo-random data with a fixed seed.
Thus the two firmware versions write the same bytes.
The default image is 32 KiB. This fits into all STM32F103 devices with 32 KiB of flash or more.
Use `--size 65536` for an STM32F103x8 or a larger device.

**Caution:** the script erases the flash of the target.

Do these steps:

1. Write the old firmware into the dongle. Then run:

   ```sh
   python3 tools/flash_benchmark.py run --label old --output old.json
   ```

2. Write this firmware into the dongle. Then run:

   ```sh
   python3 tools/flash_benchmark.py run --label new --output new.json
   ```

3. Make the table for the merge request:

   ```sh
   python3 tools/flash_benchmark.py compare old.json new.json
   ```

Each `run` prints a line for each run, and a Markdown table with the median times.
`compare` prints a Markdown table with the times of the two files and the ratio new / old.
A ratio above 1.00 means that the new firmware is slower.
`compare` lists the steps with a ratio above 1.05 as slower, and then exits with code 1.
Two runs of the same firmware differ by a few percent, so a smaller ratio is not a regression.
It also exits with code 1 when a run in one of the files failed.

The default speeds are 1000, 4500 and 9000 kHz for JTAG, and 250 and 2000 kHz for SWD.
At 1000 kHz, JTAG uses bit-bang. At 4500 and 9000 kHz, JTAG uses the SPI fast path.
The OpenOCD `vsllink` driver has fixed SWD steps: 250 kHz gives 235 kHz SWCLK, and 2000 kHz gives the fastest SWCLK (1.85 MHz in the table).
At this setting, the firmware does not time SWCLK. A logic analyzer measured 1.845 MHz on the Versaloon Mini, see [SWD block](usb-device.md#swd-block).
All requests from 235 to 1849 kHz give 235 kHz.
These options change the run:

- `--transports jtag`, `--jtag-speeds 1000,9000`, `--swd-speeds 2000`: select the transports and the speeds.
- `--repeat 5`: the number of runs for each speed (default 3).
- `--openocd`, `-s`, `--interface`, `--target`: the OpenOCD executable and its scripts.
- `-c 'reset_config srst_only'`: an extra OpenOCD command after the target script.
- `--no-clock-setup`: do not change the target clock.

Write the results into a directory outside the source tree.
`python3 tools/flash_benchmark.py run --help` shows all options.

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
  The jobs for the Mini and the OtherUse also build `versaloon-bootloader` for the board.
- `build-mini-debug`: debug build of the Mini behind the bootloader (debug log on the serial port, SWD on), plus the size check.
- `build-mini-cmsis-dap`, `build-otheruse-cmsis-dap`, `build-stlinkv2-cmsis-dap`: the same release builds with `VSL_PROTOCOL=cmsis-dap`, plus the size check.
- `build-mini-dual`, `build-otheruse-dual`, `build-stlinkv2-dual`: the same release builds with `VSL_PROTOCOL=dual`, plus the size check.
  The limit of the dual image is 53 KB, because the last flash page holds the stored personality.
- `build-mini-dual-cmsis-dap`: the dual release build of the Mini with `VSL_DEFAULT_PROTOCOL=cmsis-dap`, plus the size check.

The build jobs keep the `.hex`, `.bin`, `.elf` and `.map` files and the bootloader images as artifacts.

## Branches

- `main`: released, stable firmware.
- `dev`: the next version. Features go into `dev` through merge requests.
