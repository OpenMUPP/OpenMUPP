# SPDX-License-Identifier: GPL-3.0-or-later
# openmupp-bootloader (dependencies/openmupp-bootloader) for the board of
# the build. It uses the first 8 KB of the flash. The app-8k image is linked
# behind it (CMakeLists.txt, MUPP_APP_IMAGES).
#
# The bootloader has its own CMake build. It runs as an ExternalProject, so
# the compile flags of the firmware (cmake/gcc_arm.cmake, CMAKE_BUILD_TYPE)
# do not change the bootloader image. The bootloader uses its own toolchain
# file with the compiler of this build, and the system-stm32 (STM32) or
# ch32v00x-system (CH32V307) submodule of this project, so both images use
# the same library commit. The install
# step copies openmupp-bootloader-<board>.elf, .hex and .bin to
# MUPP_BOOTLOADER_DIR.

include(ExternalProject)

# MUPP_BOOTLOADER_BOARDS (boards that openmupp-bootloader supports) is set in
# the top-level CMakeLists.txt.
# Output directory of the bootloader images
set(MUPP_BOOTLOADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/openmupp-bootloader")

# Adds the target openmupp-bootloader, which builds the bootloader for
# BOARD into MUPP_BOOTLOADER_DIR.
function(mupp_add_bootloader)
    set(_src_dir "${PROJECT_SOURCE_DIR}/dependencies/openmupp-bootloader")
    if (NOT EXISTS "${_src_dir}/CMakeLists.txt")
        message(FATAL_ERROR "openmupp-bootloader not found in ${_src_dir}. Run: git submodule update --init --recursive, "
                "or set -DMUPP_BUILD_BOOTLOADER=OFF")
    endif ()

    # .../bin/arm-none-eabi-gcc -> ... (the root directory of the toolchain)
    get_filename_component(_toolchain_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_toolchain_root "${_toolchain_bin}" DIRECTORY)

    set(_out "${MUPP_BOOTLOADER_DIR}/openmupp-bootloader-${BOARD}")

    if (MUPP_MCU_VENDOR STREQUAL "CH32")
        # The DFU bootloader of the CH32V307 selects its build with BOARD.
        set(_board_args
                -DCMAKE_TOOLCHAIN_FILE=${_src_dir}/cmake/gcc_riscv.cmake
                -DBOARD=${BOARD}
                -DMUPP_CH32V00X_SYSTEM_DIR=${PROJECT_SOURCE_DIR}/dependencies/ch32v00x-system)
    else ()
        set(_board_args
                -DCMAKE_TOOLCHAIN_FILE=${_src_dir}/cmake/gcc_arm.cmake
                -DOPENMUPP_BOOTLOADER_BOARDS=${BOARD}
                -DMUPP_SYSTEM_STM32_DIR=${PROJECT_SOURCE_DIR}/dependencies/system-stm32)
    endif ()

    # The bootloader build finds the changed sources itself, so it runs on
    # each build.
    ExternalProject_Add(openmupp-bootloader
            SOURCE_DIR "${_src_dir}"
            BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/openmupp-bootloader-build"
            INSTALL_DIR "${MUPP_BOOTLOADER_DIR}"
            CMAKE_ARGS
                ${_board_args}
                -DGCC_TOOLCHAIN_PREFIX=${_toolchain_root}
                # The bootloader area ends where the app-8k image starts. One
                # value sets both, so they cannot disagree.
                -DOPENMUPP_BOOTLOADER_AREA_SIZE=${MUPP_APP_OFFSET_app-8k}
                -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
            BUILD_ALWAYS ON
            BUILD_BYPRODUCTS "${_out}.elf" "${_out}.hex" "${_out}.bin"
    )
endfunction()
