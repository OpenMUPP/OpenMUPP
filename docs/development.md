# Development

This document is for developers of the firmware: build outputs, CMake options, CMake presets, tests, the flash speed benchmark, code format, version, CI and branches.

## Build outputs

The README shows how to build the firmware.
The build writes these files into the build directory:

- `versaloon-ng-<image>.elf`, `.hex` and `.bin` for each application image (`standalone`, `app-8k`, `app-10k`).
  The README lists the images and their start addresses.
- Copies of the `.hex` and `.bin` files with the board, the image, the build type and the version in the name.
  An example is `versaloon-ng-mini-app-8k-Release-v0.1.0.hex`.
  The tag after the board tells the personalities of the image (see [Image names](#image-names)).
  An example is `versaloon-ng-mini-cmsis-dap-app-8k-Release-v0.1.0.hex`.
- `versaloon-ng-<image>.map`, the linker map of each image.
- `versaloon-bootloader/versaloon-bootloader-<board>.elf`, `.hex` and `.bin`: the bootloader for the board.
- `versaloon-ng-with-bootloader.hex` and `.bin`, and copies with the board, the build type and the version in the name: the combined image of the bootloader and the `app-8k` image.
  `tools/vsl_combine_images.py` makes it. The tests are in `tools/test_vsl_combine_images.py`.
- `vsl-images.txt`: each image with its size limit. CI runs `ci/check-size.sh` on each line.

An image that does not fit its limit has no `.hex` and `.bin` files, and no copies (see [Images that do not fit](#images-that-do-not-fit)).

All images use the same object files.
Only `src/main.cpp` is compiled for each image, because `VSL_HAS_BOOTLOADER_ENTRY` changes it.
One linker script of the board links all images. The STM32F1 boards use `ldscripts/mem-f103.ld` (see [MCU family](boards.md#mcu-family)).
CMake gives it the start of the application with `-Wl,--defsym=__vsl_app_offset=<bytes>`.

For the Mini and the OtherUse, the build also makes `versaloon-bootloader` for the same board.
The target is `versaloon-bootloader`.
`cmake/Bootloader.cmake` builds the submodule with its own CMake build, as an `ExternalProject`.
Thus the firmware options and the build type do not change the bootloader image.
The bootloader build uses the compiler of the firmware build.
There is no bootloader for the ST-Link/V2, and no bootloader when `VSL_BUILD_BOOTLOADER` is `OFF`.

## CMake options

The README shows the presets for the common builds.
These options select the board, the personality and the debug features:

| Option | Default | Description |
|---|---|---|
| `BOARD` | `mini` | Board to build for (see [the README](../README.md#boards)). |
| `VSL_BUILD_BOOTLOADER` | `ON` | Build `versaloon-bootloader` for the board. Only for the Mini and the OtherUse. |
| `VSL_PERSONALITIES` | empty (`versaloon`) | USB personalities of the image: one or more of `versaloon`, `cmsis-dap` and `wch-link`. Separate them with `;` or `,`, for example `versaloon,cmsis-dap`. The order does not matter. The `cmsis-dap` personality has the CMSIS-DAP v2 interface, `0403:9b3c` (see [cmsis-dap.md](cmsis-dap.md)). The `wch-link` personality has the WCH-Link interface, `1209:0001`, and no serial port (see [wch-link.md](wch-link.md)). The other personalities have the serial port and the UART bridge. An image with more than one personality is a multi image: one personality is active after each start (see [usb-device.md](usb-device.md#multi-image)). |
| `VSL_DEFAULT_PERSONALITY` | empty | Only for a multi image: the personality when no personality is stored. It must be in `VSL_PERSONALITIES`. Empty: the first one in the order `versaloon`, `cmsis-dap`, `wch-link`. |
| `VSL_WCH_LINK_WCH_USB_ID` | `OFF` | Only when `VSL_PERSONALITIES` has `wch-link`: use the USB ID `1a86:8010` of WCH in the WCH-Link personality, so that `wlink` and `minichlink` find the probe. For personal use only: do not publish this image (see [wch-link.md](wch-link.md#usb-id)). |
| `VSL_CMSIS_DAP_RV_BRIDGE` | `OFF` | Only with `VSL_PERSONALITIES=cmsis-dap`: add the RISC-V bridge to the CMSIS-DAP personality. The vendor command `ID_DAP_Vendor1` then switches the JTAG port to an emulated RISC-V DTM on the WCH debug wire (see [cmsis-dap.md](cmsis-dap.md#risc-v-dtm-bridge)). The bridge is not tested on the STM32H5 boards. |
| `VSL_PROTOCOL`, `VSL_DEFAULT_PROTOCOL` | not set | Deprecated. Use `VSL_PERSONALITIES` and `VSL_DEFAULT_PERSONALITY`. The build changes `VSL_PROTOCOL=dual` to `VSL_PERSONALITIES=versaloon,cmsis-dap`, and each other value to the personality with the same name. Then it shows a deprecation warning. With `VSL_PROTOCOL=dual` only, `VSL_DEFAULT_PROTOCOL` becomes `VSL_DEFAULT_PERSONALITY`. With the other values of `VSL_PROTOCOL`, the build ignores `VSL_DEFAULT_PROTOCOL`. Without `VSL_PROTOCOL`, the build ignores `VSL_DEFAULT_PROTOCOL` and shows a deprecation warning. Do not set `VSL_PROTOCOL` and `VSL_PERSONALITIES` together. To remove the old options from the cache of a build directory, use `-UVSL_PROTOCOL -UVSL_DEFAULT_PROTOCOL`. The presets now set `VSL_PERSONALITIES`, so an old build directory of a preset can stop with an error. Then configure it again with `cmake --preset <name> --fresh`. |
| `CMAKE_BUILD_TYPE` | `Release` | `Release` or `Debug`. |
| `VSL_LTO` | `ON` in a `Release` build, else `OFF` | Link-time optimisation (LTO) of the firmware and of the libraries. The images are about 13 % smaller. The option sets `CMAKE_INTERPROCEDURAL_OPTIMIZATION`. CMake then makes the libraries with `gcc-ar` and `gcc-ranlib`, because `ar` and `ranlib` give no symbol index for LTO objects. |
| `VSL_DEBUG_SWD` | `ON` in a `Debug` build, else `OFF` | Keep SWD of the dongle MCU on. The board has no LEDs on the SWD pins (see [boards.md](boards.md#board-definitions)). Not allowed in a `Release` build. |
| `VSL_DEBUG_CDC` | `ON` in a `Debug` build, else `OFF` | Send the debug log to the serial port, instead of the UART bridge (see [debug-log.md](debug-log.md)). Not allowed in a `Release` build, and not allowed when `wch-link` is the only personality. In a multi image, the log is silent while the WCH-Link personality is active. |
| `VSL_STRICT_SIZE` | `OFF` (CI: `ON`) | What the build does when an image does not fit its flash limit (see [Images that do not fit](#images-that-do-not-fit)). `OFF`: the build skips the image with a warning and continues. `ON`: the build stops with an error. |
| `VSL_SKIP_IMAGES` | empty | Application images to leave out of the build: a list of `standalone`, `app-8k` and `app-10k`, separated by `,` or `;`, for example `app-8k,app-10k`. Without `app-8k`, the build makes no `with-bootloader` image. On a board with a bootloader, the standalone image of a build without `app-10k` has no check against the `app-10k` limit. |
| `GCC_TOOLCHAIN_PREFIX` | `/usr` | Install directory of the Arm GNU Toolchain. |

### Image names

The tag after the board in the file name tells the personalities of the image:

- An image with one personality has no tag for `versaloon`.
  For each other personality, the tag is `-` and the personality, for example `-cmsis-dap` or `-wch-link`.
- A multi image has `-` and the short names of its personalities, joined with `+`.
  The default personality comes first, then the other ones in the order `versaloon`, `cmsis-dap`, `wch-link`.
  The short names are `vsl` (`versaloon`), `dap` (`cmsis-dap`) and `wch` (`wch-link`).
- With `VSL_WCH_LINK_WCH_USB_ID=ON`, the tag ends with `-wch-id`, for example `-wch-link-wch-id`.
- With `VSL_CMSIS_DAP_RV_BRIDGE=ON`, the tag ends with `-rv-bridge`: `-cmsis-dap-rv-bridge`.

| `VSL_PERSONALITIES` | `VSL_DEFAULT_PERSONALITY` | Tag | Old tag (`VSL_PROTOCOL`) |
|---|---|---|---|
| `versaloon` | - | none | none |
| `cmsis-dap` | - | `-cmsis-dap` | `-cmsis-dap` |
| `wch-link` | - | `-wch-link` | `-wch-link` |
| `versaloon,cmsis-dap` | `versaloon` | `-vsl+dap` | `-dual` |
| `versaloon,cmsis-dap` | `cmsis-dap` | `-dap+vsl` | `-dual-cmsis-dap` |
| `versaloon,cmsis-dap,wch-link` | `versaloon` | `-vsl+dap+wch` | - |
| `cmsis-dap,wch-link` | `wch-link` | `-wch+dap` | - |

The tags of the dual images changed: `-dual` is now `-vsl+dap`, and `-dual-cmsis-dap` is now `-dap+vsl`.
This is also true when you build with the deprecated `VSL_PROTOCOL=dual`.

### Images that do not fit

Each image has a flash limit: the flash behind its bootloader, without the settings page of a multi image.
`vsl-images.txt` lists the limits.
Some sets of personalities do not fit each image, for example the Debug build of `versaloon,cmsis-dap,wch-link`.
The size of an image is only known after the link.
Thus a post-build step, `cmake/ImageSize.cmake`, compares the `.bin` file of each image with its limit.

With `VSL_STRICT_SIZE=OFF`, the build skips an image that does not fit:

- The FLASH region of the linker script is 64 KB larger (`__vsl_flash_slack`), so the link does not fail.
- The post-build step removes the `.hex` and `.bin` files of the image and their copies, and shows a CMake warning.
- The build continues and makes the other images.
- When the build skips the `app-8k` image, it also skips the combined image (`cmake/CombinedImage.cmake`).
- The `.elf` and `.map` files of a skipped image stay in the build directory. Do not program them.

An example: the Release build of `versaloon,cmsis-dap,wch-link` for the Mini makes the `standalone`, `app-8k` and combined images.
It skips the `app-10k` image.

With `VSL_STRICT_SIZE=ON`, the FLASH region has no slack, and the link of an image that does not fit fails.
The post-build step does not run.
The CI build jobs set `VSL_STRICT_SIZE=ON`, so each image of each CI build must fit.
The presets do not set it: a preset with another `VSL_PERSONALITIES` skips the images that do not fit.

`vsl-images.txt` also lists a skipped image.
Thus `ci/check-size.sh` fails on a skipped image, because its `.bin` file does not exist.

The host test build runs the post-build steps in CMake script mode on fake files (`tests/cmake/ImageSizeCase.cmake`, tests `image-size.*`).

### Add a personality

`cmake/Personalities.cmake` has one `vsl_personality()` block for each personality.
A block has the code of the personality, the short name, the `VSL_WITH_<P>` define and the sources that only this personality uses.
The blocks are in code order.
An image without a personality does not compile its sources.
The shared sources (`BoardSwd`, `BoardJtag`, `BoardGpio`) are in each image.
The linker option `-gc-sections` removes the code that an image does not use.
`vsl_resolve_personalities()` checks the options and sets the defines, the sources to remove and the tag.
The host test build runs these checks in CMake script mode (`tests/cmake/ResolvePersonalities.cmake`, tests `personalities.*`).

## CMake presets

`CMakePresets.json` has the configure, build and test presets.
It needs CMake 3.21 or later.
Run `cmake --preset <name>` to configure and `cmake --build --preset <name>` to build.
Each preset has its own build directory, `build/<name>`.
The presets for the firmware use the toolchain file `cmake/gcc_arm.cmake`.
They read `GCC_TOOLCHAIN_PREFIX` from the environment.
The default is `/usr`.

| Preset | Board | Personalities (`VSL_PERSONALITIES`) | Build type |
|---|---|---|---|
| `mini`, `otheruse`, `stlinkv2` | Mini, OtherUse, ST-Link/V2 | `versaloon` | Release |
| `mini-cmsis-dap`, `otheruse-cmsis-dap`, `stlinkv2-cmsis-dap` | Mini, OtherUse, ST-Link/V2 | `cmsis-dap` | Release |
| `mini-cmsis-dap-rv-bridge`, `otheruse-cmsis-dap-rv-bridge` | Mini, OtherUse | `cmsis-dap`, with `VSL_CMSIS_DAP_RV_BRIDGE=ON` | Release |
| `mini-dual`, `otheruse-dual`, `stlinkv2-dual` | Mini, OtherUse, ST-Link/V2 | `versaloon,cmsis-dap` (tag `-vsl+dap`) | Release |
| `mini-dual-cmsis-dap` | Mini | `versaloon,cmsis-dap`, with `VSL_DEFAULT_PERSONALITY=cmsis-dap` (tag `-dap+vsl`) | Release |
| `mini-wch-link`, `otheruse-wch-link`, `stlinkv2-wch-link` | Mini, OtherUse, ST-Link/V2 | `wch-link` | Release |
| `mini-wch-link-wch-id` | Mini | `wch-link`, with `VSL_WCH_LINK_WCH_USB_ID=ON` | Release |
| `mini-triple`, `otheruse-triple`, `stlinkv2-triple` | Mini, OtherUse, ST-Link/V2 | `versaloon,cmsis-dap,wch-link` (tag `-vsl+dap+wch`) | Release |
| `mini-cmsis-dap-wch-link` | Mini | `cmsis-dap,wch-link` (tag `-dap+wch`) | Release |
| `mini-debug` | Mini | `versaloon` | Debug |
| `weacth523` | WeAct STM32H523 core board | `versaloon` | Release |
| `weacth523-cmsis-dap` | WeAct STM32H523 core board | `cmsis-dap` | Release |
| `weacth523-dual` | WeAct STM32H523 core board | `versaloon,cmsis-dap` (tag `-vsl+dap`) | Release |
| `weacth523-wch-link` | WeAct STM32H523 core board | `wch-link` | Release |
| `weacth523-debug` | WeAct STM32H523 core board | `versaloon` | Debug |
| `host-tests` | Host | - | Debug |

The firmware presets all set `CMAKE_EXPORT_COMPILE_COMMANDS`.
Each firmware preset inherits a hidden `board-<board>` preset.
A preset with a suffix also inherits the hidden `personalities-<suffix>` preset.
The `personalities-*` presets set `VSL_PERSONALITIES` and `VSL_DEFAULT_PERSONALITY`.
They are for the sets of personalities that CI builds.
For another set, add `-DVSL_PERSONALITIES=<list>` to a preset.
The `host-tests` preset has no toolchain file.
It has a build preset that builds the `vsl-tests` target, and a test preset that runs `ctest` with `--output-on-failure`.
CI builds each firmware preset, except `mini-wch-link-wch-id` and `otheruse-cmsis-dap-rv-bridge`.
CI uses these presets (see [Continuous integration](#continuous-integration)).

Add a preset for a local setup in `CMakeUserPresets.json`.
Git ignores this file.
This example inherits the `mini` preset and sets another toolchain:

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "mini-local",
      "inherits": "mini",
      "cacheVariables": { "GCC_TOOLCHAIN_PREFIX": "/opt/arm-gnu-toolchain" }
    }
  ]
}
```

## Unit tests

The unit tests use doctest and run on the host.
They check the board definitions, the LED patterns (activity, probe error, target power, clock fault and the flashes of the personality switch), the version, the USB descriptors, the log sink, the line coding map of the UART bridge, the bootloader entry, the Versaloon command protocol, the `USB_TO_ALL` dispatcher, the `GPIO` block (with the release of SRST and TRST when the host goes away), the `JTAG_RAW` block, the JTAG bit-bang engine (on mock pins), the JTAG fast path (on a mock SPI, compared with the bit-bang engine), the `SWD` block, the SWD request and parity encoding, the SWD bit-bang engine (on mock pins with a mock target), the CMSIS-DAP command processor (on the SWD bit-bang engine, on the JTAG engines with a mock JTAG-DP, and on the emulated TAP of the RISC-V bridge, see [cmsis-dap.md](cmsis-dap.md#tests)), the `POWER` block (on a fake power switch and a fake voltage sense), and the Vref scale.
The USB tests also run the descriptors and the Versaloon function on the libArT USB device stack, with the fake USB controller of libArT.
The same tests run the descriptors and the function of the CMSIS-DAP personality, and check that its packet memory fits 512 bytes.
They also check the BOS and the MS OS 2.0 descriptor set, and run the requests of Windows for them.
The UART bridge tests run the bridge with the CDC-ACM function of libArT, the fake USB controller and a fake UART.
Two of these tests run a long JTAG shift on mock pins, with and without the serial service of the engines.
With the service, no UART data is lost.
Other tests check the serial-only mode of the USB event queue, and the calls of the serial service in the JTAG and SWD engines.
Use the `host-tests` presets.
They configure without the toolchain file, in the Debug build type:

```sh
cmake --preset host-tests
cmake --build --preset host-tests -j
ctest --preset host-tests
```

The alternative is the plain commands:

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
At this setting, the firmware does not time SWCLK. A logic analyzer measured 2.7 MHz on the OtherUse, see [Clock pacing](clock-pacing.md).
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
cmake --build --preset host-tests --target clangformat
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
- `run-unit-tests`: builds and runs the host unit tests with the `host-tests` presets.
- `run-tool-tests`: runs the unit tests of the Python scripts in `tools/`.
- `build-mini`: release build of all images of the Mini with the `mini` preset, plus the size check of each image in `vsl-images.txt`.
  The build jobs configure with `-DVSL_STRICT_SIZE=ON`: a build job fails when an image does not fit.
  The jobs for the Mini and the OtherUse also build `versaloon-bootloader` and the combined image for the board.
- `build-mini-debug`: debug build of the Mini (debug log on the serial port, SWD on), plus the size check.
- `build`: a `parallel:matrix` job with one release build and size check for each board and set of personalities (decision Q6 of #79).
  GitLab gives each build the name `build: [<values>]`, with the values of its matrix entry in order, for example `build: [mini, -triple, -vsl+dap+wch]` or `build: [otheruse]`.
  It builds the preset `<board><suffix>` (see [CMake presets](#cmake-presets)).

  | Personalities | Suffix | Tag | Boards |
  |---|---|---|---|
  | `versaloon` | - | - | OtherUse, ST-Link/V2 |
  | `cmsis-dap` | `-cmsis-dap` | `-cmsis-dap` | Mini, OtherUse, ST-Link/V2 |
  | `wch-link` | `-wch-link` | `-wch-link` | Mini, OtherUse, ST-Link/V2 |
  | `versaloon,cmsis-dap` | `-dual` | `-vsl+dap` | Mini, OtherUse, ST-Link/V2 |
  | `versaloon,cmsis-dap`, default `cmsis-dap` | `-dual-cmsis-dap` | `-dap+vsl` | Mini |
  | `versaloon,cmsis-dap,wch-link` | `-triple` | `-vsl+dap+wch` | Mini, OtherUse, ST-Link/V2 |

  Each limit of a multi image is 1 KB smaller, because the last flash page holds the stored personality.
- `build-mini-cmsis-dap-wch-link`: release build of the Mini with `VSL_PERSONALITIES=cmsis-dap,wch-link`, plus the size check.
  This job keeps no artifacts.
- `build-mini-cmsis-dap-rv-bridge`: release build of the Mini with `VSL_PERSONALITIES=cmsis-dap` and `VSL_CMSIS_DAP_RV_BRIDGE=ON`, plus the size check.
  This job keeps no artifacts.
- `build-weacth523`: a `parallel:matrix` job for the WeAct STM32H523 core board, with the presets `weacth523`, `weacth523-cmsis-dap`, `weacth523-dual`, `weacth523-wch-link` and `weacth523-debug`, plus the size check.
  The limit of the dual image is 8 KB smaller, because the last flash sector holds the stored personality.

The build jobs of the Mini and the WeAct STM32H523 core board run in each pipeline.
The build jobs of the OtherUse and the ST-Link/V2 are manual: start them in the pipeline view of GitLab.
A manual job can fail and not block the pipeline or the merge request (MR).
On a tag, all build jobs run.

The CI cache keeps the FetchContent sources in `build/_deps` between pipelines.
`build-mini`, `build-weacth523` and `run-unit-tests` update the cache, and the other jobs only read it.
`build-weacth523` has its own cache key, because the STM32H5 needs other CMSIS packages.
In CI, CMake checks each cached source against the declared tag, and fetches it again when the tag changes.
Outside CI, CMake uses a source in `build/_deps` as it is, and does not check the tag.
After you change the tag of a dependency, remove `build/_deps/<name>-src`.
The build jobs keep only the `.hex` and `.bin` files from `build/<preset>` as artifacts, with the board, the image, the build type and the version in the name.
The Mini and the OtherUse have the `standalone`, `app-8k`, `app-10k` and `with-bootloader` images.
The ST-Link/V2 has the `standalone` image only.
The artifacts have no `.elf` or `.map` files, and no bootloader image alone.
The tag of a multi image has a `+`, also in the name of the artifact archive and of the matrix job.
In a URL, encode the full job name, for example in a link to `/-/jobs/artifacts/<ref>/download?job=<job>`.
Encode `+` as `%2B`, the space as `%20`, `,` as `%2C`, `[` as `%5B` and `]` as `%5D`.

## Branches

- `main`: released, stable firmware.
- `dev`: the next version. Features go into `dev` through merge requests.
