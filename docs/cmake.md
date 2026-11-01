# CMake

This document describes the CMake options and the CMake presets of the firmware build.
The README shows how to build the firmware. [Development](development.md) describes the build outputs, the tests and CI.

## CMake options

The README shows the presets for the common builds.
These options select the board, the personality and the debug features:

| Option | Default | Description |
|---|---|---|
| `BOARD` | `mini` | Board to build for (see [the README](../README.md#boards)). |
| `MUPP_BUILD_BOOTLOADER` | `ON` | Build `openmupp-bootloader` for the board. Only for the STM32F1 boards. |
| `MUPP_PERSONALITIES` | empty (`versaloon`) | USB personalities of the image: one or more of `versaloon`, `cmsis-dap` and `wch-link`. Separate them with `;` or `,`, for example `versaloon,cmsis-dap`. The order does not matter. The `cmsis-dap` personality has the CMSIS-DAP v2 interface, `0403:9b3c` (see [cmsis-dap.md](cmsis-dap.md)). The `wch-link` personality has the WCH-Link interface, `1209:0001`, and no serial port (see [wch-link.md](wch-link.md)). The other personalities have the serial port and the UART bridge. An image with more than one personality is a multi image: one personality is active after each start (see [usb-device.md](usb-device.md#multi-image)). |
| `MUPP_DEFAULT_PERSONALITY` | empty | Only for a multi image: the personality when no personality is stored. It must be in `MUPP_PERSONALITIES`. Empty: the first one in the order `versaloon`, `cmsis-dap`, `wch-link`. |
| `MUPP_WCH_LINK_WCH_USB_ID` | `OFF` | Only when `MUPP_PERSONALITIES` has `wch-link`: use the USB ID `1a86:8010` of WCH in the WCH-Link personality, so that `wlink` and `minichlink` find the probe. For personal use only: do not publish this image (see [wch-link.md](wch-link.md#usb-id)). |
| `MUPP_CMSIS_DAP_RV_BRIDGE` | `ON` | Add the RISC-V bridge to the CMSIS-DAP personality, also in a multi image. The JTAG port can then be an emulated RISC-V DTM on the WCH debug wire. In the auto mode, the probe finds the target at `DAP_Connect`. The vendor command `ID_DAP_Vendor1` sets the mode and can store it (see [cmsis-dap.md](cmsis-dap.md#risc-v-dtm-bridge)). The stored mode needs the settings page, so an image with the bridge has 1 KB (STM32F103) or 8 KB (STM32H5) less flash. An image without `cmsis-dap` ignores the option. A build directory with `OFF` in its cache keeps `OFF`: use `--fresh` or `-DMUPP_CMSIS_DAP_RV_BRIDGE=ON`. On the STM32H5 boards, only the OpenOCD rates of the bridge are measured (WeAct STM32H523, see [cmsis-dap.md](cmsis-dap.md#ch32v006-rates-on-the-weact-stm32h523)). |
| `CMAKE_BUILD_TYPE` | `Release` | `Release` or `Debug`. |
| `MUPP_LTO` | `ON` in a `Release` build, else `OFF` | Link-time optimisation (LTO) of the firmware and of the libraries. The images are about 13 % smaller. The option sets `CMAKE_INTERPROCEDURAL_OPTIMIZATION`. CMake then makes the libraries with `gcc-ar` and `gcc-ranlib`, because `ar` and `ranlib` give no symbol index for LTO objects. |
| `MUPP_DEBUG_SWD` | `ON` in a `Debug` build, else `OFF` | Keep SWD of the dongle MCU on. The board has no LEDs on the SWD pins (see [boards.md](boards.md#board-definitions)). Not allowed in a `Release` build. |
| `MUPP_DEBUG_CDC` | `ON` in a `Debug` build, else `OFF` | Send the debug log to the serial port, instead of the UART bridge (see [debug-log.md](debug-log.md)). Not allowed in a `Release` build, and not allowed when `wch-link` is the only personality. In a multi image, the log is silent while the WCH-Link personality is active. |
| `MUPP_STRICT_SIZE` | `OFF` (CI: `ON`) | What the build does when an image does not fit its flash limit (see [Images that do not fit](#images-that-do-not-fit)). `OFF`: the build skips the image with a warning and continues. `ON`: the build stops with an error. |
| `MUPP_SKIP_IMAGES` | empty | Application images to leave out of the build: a list of `standalone`, `app-8k` and `app-10k`, separated by `,` or `;`, for example `app-8k,app-10k`. Without `app-8k`, the build makes no `with-bootloader` image. On a board with a bootloader, the standalone image of a build without `app-10k` has no check against the `app-10k` limit. |
| `GCC_TOOLCHAIN_PREFIX` | `/usr` | Install directory of the Arm GNU Toolchain. |

The `MUPP_` options had the `VSL_` prefix before.
The configure step stops with an error if it finds a `VSL_` option.
Rename the option, and use `cmake --fresh` for a build directory from before the rename.

### Image names

The tag after the board in the file name tells the personalities of the image:

- An image with one personality has no tag for `versaloon`.
  For each other personality, the tag is `-` and the personality, for example `-cmsis-dap` or `-wch-link`.
- A multi image has `-` and the short names of its personalities, joined with `+`.
  The default personality comes first, then the other ones in the order `versaloon`, `cmsis-dap`, `wch-link`.
  The short names are `vsl` (`versaloon`), `dap` (`cmsis-dap`) and `wch` (`wch-link`).
- With `MUPP_WCH_LINK_WCH_USB_ID=ON`, the tag ends with `-wch-id`, for example `-wch-link-wch-id`.
- An image with `cmsis-dap` and `MUPP_CMSIS_DAP_RV_BRIDGE=OFF` has a tag that ends with `-no-rv-bridge`, for example `-cmsis-dap-no-rv-bridge` or `-vsl+dap-no-rv-bridge`.

| `MUPP_PERSONALITIES` | `MUPP_DEFAULT_PERSONALITY` | Tag |
|---|---|---|
| `versaloon` | - | none |
| `cmsis-dap` | - | `-cmsis-dap` |
| `wch-link` | - | `-wch-link` |
| `versaloon,cmsis-dap` | `versaloon` | `-vsl+dap` |
| `versaloon,cmsis-dap` | `cmsis-dap` | `-dap+vsl` |
| `versaloon,cmsis-dap,wch-link` | `versaloon` | `-vsl+dap+wch` |
| `cmsis-dap,wch-link` | `cmsis-dap` | `-dap+wch` |
| `cmsis-dap,wch-link` | `wch-link` | `-wch+dap` |

### Images that do not fit

Each image has a flash limit: the flash behind its bootloader, without the settings page of a multi image or an image with the RISC-V bridge.
`mupp-images.txt` lists the limits.
Some sets of personalities do not fit each image, for example the Debug build of `versaloon,cmsis-dap,wch-link`.
The size of an image is only known after the link.
Thus a post-build step, `cmake/ImageSize.cmake`, compares the `.bin` file of each image with its limit.

With `MUPP_STRICT_SIZE=OFF`, the build skips an image that does not fit:

- The FLASH region of the linker script (`__flash_length`) is 64 KB larger (`MUPP_FLASH_SLACK`), so the link does not fail.
- The post-build step removes the `.hex` and `.bin` files of the image and their copies, and shows a CMake warning.
- The build continues and makes the other images.
- When the build skips the `app-8k` image, it also skips the combined image (`cmake/CombinedImage.cmake`).
- The `.elf` and `.map` files of a skipped image stay in the build directory. Do not program them.

An example: the Release build of `versaloon,cmsis-dap,wch-link` for the Mini makes the `standalone`, `app-8k` and combined images.
It skips the `app-10k` image.

With `MUPP_STRICT_SIZE=ON`, the FLASH region has no slack, and the link of an image that does not fit fails.
The post-build step does not run.
The CI build jobs set `MUPP_STRICT_SIZE=ON`, so each image of each CI build must fit.
The presets do not set it: a preset with another `MUPP_PERSONALITIES` skips the images that do not fit.

`mupp-images.txt` also lists a skipped image.
Thus `ci/check-size.sh` fails on a skipped image, because its `.bin` file does not exist.

The host test build runs the post-build steps in CMake script mode on fake files (`tests/cmake/ImageSizeCase.cmake`, tests `image-size.*`).

### Add a personality

`cmake/Personalities.cmake` has one `mupp_personality()` block for each personality.
A block has the code of the personality, the short name, the `MUPP_WITH_<P>` define and the sources that only this personality uses.
The blocks are in code order.
An image without a personality does not compile its sources.
The shared sources (`BoardSwd`, `BoardJtag`, `BoardGpio`) are in each image.
The linker option `-gc-sections` removes the code that an image does not use.
`mupp_resolve_personalities()` checks the options and sets the defines, the sources to remove and the tag.
The host test build runs these checks in CMake script mode (`tests/cmake/ResolvePersonalities.cmake`, tests `personalities.*`).

## CMake presets

`CMakePresets.json` has the configure, build and test presets.
It needs CMake 3.21 or later.
Run `cmake --preset <name>` to configure and `cmake --build --preset <name>` to build.
Each preset has its own build directory, `build/<name>`.
The presets for the firmware use the toolchain file `cmake/gcc_arm.cmake`.
They read `GCC_TOOLCHAIN_PREFIX` from the environment.
The default is `/usr`.

| Preset | Board | Personalities (`MUPP_PERSONALITIES`) | Build type |
|---|---|---|---|
| `mini-cmsis-dap`, `stlinkv2-cmsis-dap`, `stlinkv2miniclone-cmsis-dap` | Mini, ST-Link/V2, ST-Link/V2 mini clone | `cmsis-dap` | Release |
| `mini-cmsis-dap-no-rv-bridge` | Mini | `cmsis-dap`, with `MUPP_CMSIS_DAP_RV_BRIDGE=OFF` (tag `-cmsis-dap-no-rv-bridge`) | Release |
| `mini-dual`, `stlinkv2-dual`, `stlinkv2miniclone-dual` | Mini, ST-Link/V2, ST-Link/V2 mini clone | `cmsis-dap,wch-link` (tag `-dap+wch`) | Release |
| `mini-debug` | Mini | `cmsis-dap`, with `MUPP_CMSIS_DAP_RV_BRIDGE=OFF`: with the bridge, the `app-8k` and `app-10k` images do not fit (tag `-cmsis-dap-no-rv-bridge`) | Debug |
| `mini-wch-link`, `stlinkv2-wch-link`, `stlinkv2miniclone-wch-link` | Mini, ST-Link/V2, ST-Link/V2 mini clone | `wch-link` | Release |
| `mini-wch-link-wch-id` | Mini | `wch-link`, with `MUPP_WCH_LINK_WCH_USB_ID=ON` | Release |
| `mini`, `stlinkv2`, `stlinkv2miniclone` | Mini, ST-Link/V2, ST-Link/V2 mini clone | `versaloon` | Release |
| `weacth523-cmsis-dap` | WeAct STM32H523 core board | `cmsis-dap` | Release |
| `weacth523-dual` | WeAct STM32H523 core board | `cmsis-dap,wch-link` (tag `-dap+wch`) | Release |
| `weacth523-debug` | WeAct STM32H523 core board | `cmsis-dap` | Debug |
| `weacth523-wch-link` | WeAct STM32H523 core board | `wch-link` | Release |
| `weacth523` | WeAct STM32H523 core board | `versaloon` | Release |
| `host-tests` | Host | - | Debug |

The firmware presets all set `CMAKE_EXPORT_COMPILE_COMMANDS`.
Each firmware preset inherits a hidden `board-<board>` preset.
A preset with a suffix also inherits the hidden `personalities-<suffix>` preset.
The `personalities-*` presets set `MUPP_PERSONALITIES` and `MUPP_DEFAULT_PERSONALITY`.
They are for the common sets of personalities.
For another set, add `-DMUPP_PERSONALITIES=<list>` to a preset.
The `-dual` presets build `cmsis-dap,wch-link`.
The `-cmsis-dap-wch-link` presets do not exist: use the `-dual` presets.
For a dual image with `versaloon,cmsis-dap`, add `-DMUPP_PERSONALITIES=versaloon,cmsis-dap` to a preset.
The `host-tests` preset has no toolchain file.
It has a build preset that builds the `mupp-tests` target, and a test preset that runs `ctest` with `--output-on-failure`.
CI builds the `-cmsis-dap`, `-dual` and `-debug` presets, and `mini-cmsis-dap-no-rv-bridge`.
The other presets are for local builds only.
CI uses these presets (see [Continuous integration](development.md#continuous-integration)).

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
