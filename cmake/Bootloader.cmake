# versaloon-bootloader (dependencies/versaloon-bootloader) for the board of
# the build. It uses the first 8 KB of the flash. The app-8k image is linked
# behind it (CMakeLists.txt, VSL_APP_IMAGES).
#
# The bootloader has its own CMake build. It runs as an ExternalProject, so
# the compile flags of the firmware (cmake/gcc_arm.cmake, CMAKE_BUILD_TYPE)
# do not change the bootloader image. The bootloader uses its own toolchain
# file with the compiler of this build, and the system-stm32 submodule of
# this project, so both images use the same library commit. The install
# step copies versaloon-bootloader-<board>.elf, .hex and .bin to
# VSL_BOOTLOADER_DIR.

include(ExternalProject)

# VSL_BOOTLOADER_BOARDS (boards that versaloon-bootloader supports) is set in
# the top-level CMakeLists.txt.
# Output directory of the bootloader images
set(VSL_BOOTLOADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/versaloon-bootloader")

# Adds the target versaloon-bootloader, which builds the bootloader for
# BOARD into VSL_BOOTLOADER_DIR.
function(vsl_add_bootloader)
    set(_src_dir "${PROJECT_SOURCE_DIR}/dependencies/versaloon-bootloader")
    if (NOT EXISTS "${_src_dir}/CMakeLists.txt")
        message(FATAL_ERROR "versaloon-bootloader not found in ${_src_dir}. Run: git submodule update --init --recursive, "
                "or set -DVSL_BUILD_BOOTLOADER=OFF")
    endif ()

    # .../bin/arm-none-eabi-gcc -> ... (the root directory of the toolchain)
    get_filename_component(_toolchain_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_toolchain_root "${_toolchain_bin}" DIRECTORY)

    set(_out "${VSL_BOOTLOADER_DIR}/versaloon-bootloader-${BOARD}")

    # The bootloader build finds the changed sources itself, so it runs on
    # each build.
    ExternalProject_Add(versaloon-bootloader
            SOURCE_DIR "${_src_dir}"
            BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/versaloon-bootloader-build"
            INSTALL_DIR "${VSL_BOOTLOADER_DIR}"
            CMAKE_ARGS
                -DCMAKE_TOOLCHAIN_FILE=${_src_dir}/cmake/gcc_arm.cmake
                -DGCC_TOOLCHAIN_PREFIX=${_toolchain_root}
                -DVERSALOON_BOOTLOADER_BOARDS=${BOARD}
                -DVSL_SYSTEM_STM32_DIR=${PROJECT_SOURCE_DIR}/dependencies/system-stm32
                -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
            BUILD_ALWAYS ON
            BUILD_BYPRODUCTS "${_out}.elf" "${_out}.hex" "${_out}.bin"
    )
endfunction()
