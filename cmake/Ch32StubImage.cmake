# SPDX-License-Identifier: GPL-3.0-or-later
# Build of a CH32 board (MUPP_MCU_VENDOR CH32, cmake/Boards.cmake) while the
# CH32V307 port is not complete. The image has the start-up code of
# ch32v00x-system, the board start-up of the firmware (clock, SysTick, board
# pins: src/initialize_hardware.cpp) and a stub main() (src/ch32v30x/) that
# flashes the state LED. The other firmware sources need USB and the probe
# engines on the CH32V307, so they are not in this image yet.
# Replace this file with the normal image build when the port is complete.
#
# CMakeLists.txt includes this file and stops: the personality options,
# the bootloader and the image variants of the STM32 boards do not apply.

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

set(CXX_OPTIONS -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wno-volatile)
set(COMMON_OPTIONS -Os -ffunction-sections -fdata-sections -g -fno-common -fmessage-length=0 -fsigned-char -Werror=odr --specs=nano.specs --specs=nosys.specs)
set(C_OPTIONS -std=gnu11)
set(DEBUG_OPTIONS -Wall -Wextra -Og)
# The linker scripts of ch32v00x-system, as its CH32V307 test image uses them
set(MUPP_LDSCRIPT_DIR ${PROJECT_SOURCE_DIR}/dependencies/ch32v00x-system/ldscripts)
set(LINKER_OPTIONS -Wl,-gc-sections -Wl,--no-warn-rwx-segments -L${MUPP_LDSCRIPT_DIR} -T${MUPP_LDSCRIPT} -Tsections.ld -Tnoheap.ld
        -nostartfiles --specs=nano.specs --specs=nosys.specs ${COMMON_OPTIONS})

include("${PROJECT_SOURCE_DIR}/cmake/BuildConfig.cmake")
art_setup_build_config(art-build-config)

if (NOT EXISTS "${PROJECT_SOURCE_DIR}/dependencies/ch32v00x-system/CMakeLists.txt")
    message(FATAL_ERROR "dependencies/ch32v00x-system not found. Run: git submodule update --init dependencies/ch32v00x-system")
endif ()
add_subdirectory("${PROJECT_SOURCE_DIR}/dependencies/ch32v00x-system")

include("${PROJECT_SOURCE_DIR}/cmake/output_files.cmake")

MESSAGE(STATUS "Building openmupp version " ${FIRMWARE_VERSION} " (" ${CMAKE_BUILD_TYPE} ") for board: " ${BOARD} " (stub image, the CH32 port is not complete)")

set(_target openmupp-standalone.elf)
set(_name openmupp-standalone)
add_executable(${_target} src/ch32v30x/StubMain.cpp src/initialize_hardware.cpp src/board/BoardChecks.cpp)
string(TOUPPER "${BOARD}" BOARD_UPPER)
# Selects board::current (src/board/Board.h)
target_compile_definitions(${_target} PRIVATE MUPP_BOARD_${BOARD_UPPER})
target_include_directories(${_target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(${_target} PRIVATE art-build-config ch32v00x-system)
target_link_options(${_target} PRIVATE -Wl,-Map,${_name}.map)
set_target_properties(${_target} PROPERTIES LINK_DEPENDS
        "${MUPP_LDSCRIPT_DIR}/${MUPP_LDSCRIPT};${MUPP_LDSCRIPT_DIR}/sections.ld;${MUPP_LDSCRIPT_DIR}/noheap.ld")
ADD_DUMP_TARGET(${_target})
PRINT_SIZE_OF_TARGETS(${_target})
add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -Oihex $<TARGET_FILE:${_target}> ${_name}.hex
        COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:${_target}> ${_name}.bin)
# The size check of CI (ci/compile-preset.sh): the whole flash, no settings page yet
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mupp-images.txt" "${_name}.bin ${MUPP_FLASH_SIZE}\n")
