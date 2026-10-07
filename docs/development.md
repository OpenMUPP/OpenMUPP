# Development

This document is for developers of the firmware: build outputs, tests, code format, version, CI and branches.

## Build outputs

The README shows how to build the firmware.
The build writes these files into the build directory:

- `versaloon-next-gen.elf`, `versaloon-next-gen.hex` and `versaloon-next-gen.bin`.
- Copies of the `.hex` and `.bin` files with the board, the link variant, the build type and the version in the name.
  An example is `versaloon-next-gen-mini-bootloader-Release-v0.1.0.hex`.
- `versaloon-next-gen.map`, the linker map.
- `versaloon-bootloader/versaloon-bootloader-<board>.elf`, `.hex` and `.bin`: the bootloader for the board.

For the Mini and the OtherUse, the build also makes `versaloon-bootloader` for the same board.
The target is `versaloon-bootloader`.
`cmake/Bootloader.cmake` builds the submodule with its own CMake build, as an `ExternalProject`.
Thus the firmware options and the build type do not change the bootloader image.
The bootloader build uses the compiler of the firmware build.
There is no bootloader for the ST-Link/V2, and no bootloader when `VSL_BOOTLOADER` or `VSL_BUILD_BOOTLOADER` is `OFF`.

## Unit tests

The unit tests use doctest and run on the host.
They check the board definitions, the heartbeat, the version, the USB descriptors, the log sink, the line coding map of the UART bridge and the bootloader entry.
The USB tests also run the descriptors and the Versaloon function on the libArT USB device stack, with the fake USB controller of libArT.
The UART bridge tests run the bridge with the CDC-ACM function of libArT, the fake USB controller and a fake UART.
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
- `run-tool-tests`: runs the unit tests of the Python scripts in `tools/`.
- `build-mini`, `build-otheruse`, `build-stlinkv2`: release build behind the bootloader, plus the size check.
  The jobs for the Mini and the OtherUse also build `versaloon-bootloader` for the board.
- `build-mini-debug`: debug build of the Mini behind the bootloader (debug log on the serial port, SWD on), plus the size check.
  The jobs keep the `.hex`, `.bin`, `.elf` and `.map` files and the bootloader images as artifacts.

## Branches

- `main`: released, stable firmware.
- `dev`: the next version. Features go into `dev` through merge requests.
