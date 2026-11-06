# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20)

# Shared FetchContent source cache — parallel configures are serialized
# with a file lock so the first build downloads and the rest reuse.
set(_FC_SHARED_DIR "${CMAKE_CURRENT_LIST_DIR}/build/_deps")
file(MAKE_DIRECTORY "${_FC_SHARED_DIR}")
file(LOCK "${_FC_SHARED_DIR}/.lock" GUARD PROCESS)

# All downloads land in the shared directory
set(FETCHCONTENT_BASE_DIR "${_FC_SHARED_DIR}" CACHE PATH "")

# For sources already downloaded, set FETCHCONTENT_SOURCE_DIR_<NAME> so
# FetchContent skips the subbuild entirely (avoids generator conflicts).
# Not in CI: there build/_deps comes from the CI cache, and the subbuild
# must check that each cached source is still at the declared tag.
if(NOT DEFINED ENV{CI})
    file(GLOB _fc_src_dirs "${_FC_SHARED_DIR}/*-src")
    foreach(_src_dir ${_fc_src_dirs})
        get_filename_component(_dir_name "${_src_dir}" NAME)
        string(REGEX REPLACE "-src$" "" _dep_name "${_dir_name}")
        string(TOUPPER "${_dep_name}" _dep_upper)
        set(FETCHCONTENT_SOURCE_DIR_${_dep_upper} "${_src_dir}")
    endforeach()
endif()

Include(FetchContent)

# Extract git revision
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_version.cmake")

# BOARD and its MCU family, CMSIS device, crystal and linker script. Before
# project(): the toolchain file takes the CPU flags from STM32_FAMILY.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Boards.cmake")

project(openmupp
        LANGUAGES C CXX ASM
        VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_BUILD}"
)

# Add our CMAKE modules folder
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

# Compiler settings
set(CMAKE_CXX_STANDARD 23)
set(CXX_DIALECT gnu)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)
set(CMAKE_C_DIALECT gnu)

file(GLOB_RECURSE PROJECT_SOURCES
        "${PROJECT_SOURCE_DIR}/src/*.c"
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
)

file(GLOB_RECURSE PROJECT_HEADERS
        "src/*.h"
        "src/*.hpp"
)

file(GLOB_RECURSE TEST_SOURCES
        "tests/*.h"
        "tests/*.cpp"
)

set(PROJECT_INCLUDE_DIRS
        ${CMAKE_CURRENT_BINARY_DIR}
        ${PROJECT_SOURCE_DIR}/src
)

# Application images. Each image is the same firmware, linked for one start
# address (the system-stm32 memory script of the board, mem-F103x8.ld for
# the STM32F1 boards). The value is the space in bytes in front
# of the application:
#   standalone  0       0x08000000, no bootloader: flash with a debug probe
#   app-8k      8192    0x08002000, behind openmupp-bootloader (8 KB)
#   app-10k     10240   0x08002800, behind the legacy Versaloon bootloader
#                       (10 KB) of the existing boards
# The boards in MUPP_BOOTLOADER_BOARDS get standalone and app-8k, and the
# boards in MUPP_LEGACY_BOOTLOADER_BOARDS also app-10k. The other boards have
# no bootloader and get the standalone image only.
set(MUPP_APP_OFFSET_standalone 0)
set(MUPP_APP_OFFSET_app-8k 8192)
set(MUPP_APP_OFFSET_app-10k 10240)
# Build openmupp-bootloader (dependencies/openmupp-bootloader, cmake/Bootloader.cmake)
# for the board. Only for boards that the bootloader supports.
option(MUPP_BUILD_BOOTLOADER "Build openmupp-bootloader for the board" ON)
# An image that does not fit its flash limit: OFF skips the image with a
# warning (cmake/ImageSize.cmake), ON stops the build. The size is only
# known after the link. With OFF, the FLASH region of the linker script is
# 64 KB larger (MUPP_FLASH_SLACK), so that the link does not fail. The
# CI build jobs set ON: each image of each CI build must fit.
option(MUPP_STRICT_SIZE "Stop the build when an image does not fit its flash limit, instead of skipping the image" OFF)
# Application images to leave out, for example app-10k: CI leaves it out of
# a strict build of a set of personalities that does not fit it.
# A CMake list or a comma-separated list, as MUPP_PERSONALITIES.
set(MUPP_SKIP_IMAGES "" CACHE STRING "Application images to leave out of the build (standalone, app-8k, app-10k)")

# USB personalities of the firmware image (cmake/Personalities.cmake):
# versaloon, cmsis-dap and wch-link. An image with more than one personality
# has one active personality after each start. A switch command or the key
# selects the next one, and a flash page at the end of the application area
# stores it (src/personality/).
include(Personalities)
mupp_personality_options()

# The WCH-Link personality uses the USB ID 1a86:8010 of WCH instead of the
# own ID. wlink and minichlink find a probe by this ID. The vendor ID belongs
# to WCH: use it for personal use only. CI does not publish this image.
option(MUPP_WCH_LINK_WCH_USB_ID "Use the USB ID of WCH (1a86:8010) in the wch-link personality, for personal use only" OFF)

# The RISC-V bridge of the CMSIS-DAP personality (docs/cmsis-dap.md, "RISC-V
# DTM bridge"): the JTAG port can be an emulated RISC-V DTM on the WCH debug
# wire. In each image with cmsis-dap, also in a multi image. An image without
# cmsis-dap ignores the option.
option(MUPP_CMSIS_DAP_RV_BRIDGE "Add the RISC-V DTM bridge to the cmsis-dap personality" ON)

# A CH32 board builds the stub image of the CH32V307 port and stops here
# (cmake/Ch32StubImage.cmake), until the port is complete. OFF builds the
# normal firmware image for the CH32 board.
option(MUPP_CH32_STUB_IMAGE "Build the stub image for a CH32 board instead of the firmware" ON)
if (CMAKE_CROSSCOMPILING AND MUPP_MCU_VENDOR STREQUAL "CH32" AND MUPP_CH32_STUB_IMAGE)
    include(Ch32StubImage)
    return()
endif ()

# Set compile/link variables before creating art-build-config so the interface
# target captures them correctly.  These settings are cross-compile only; the
# native (test) build leaves the variables unset.
if (CMAKE_CROSSCOMPILING)
    if (NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release)
    endif ()

    if (BOARD IN_LIST MUPP_LEGACY_BOOTLOADER_BOARDS)
        set(MUPP_APP_IMAGES standalone app-8k app-10k)
    elseif (BOARD IN_LIST MUPP_BOOTLOADER_BOARDS)
        set(MUPP_APP_IMAGES standalone app-8k)
    else ()
        set(MUPP_APP_IMAGES standalone)
    endif ()
    string(REPLACE "," ";" _mupp_skip_images "${MUPP_SKIP_IMAGES}")
    foreach (_image ${_mupp_skip_images})
        if (NOT _image MATCHES "^(standalone|app-8k|app-10k)$")
            message(FATAL_ERROR "MUPP_SKIP_IMAGES: unknown image '${_image}', use standalone, app-8k or app-10k")
        endif ()
    endforeach ()
    if (_mupp_skip_images)
        list(REMOVE_ITEM MUPP_APP_IMAGES ${_mupp_skip_images})
    endif ()
    if (NOT MUPP_APP_IMAGES)
        message(FATAL_ERROR "MUPP_SKIP_IMAGES leaves no image for board ${BOARD}")
    endif ()

    # Debug log over the CDC-ACM port (src/debug/DebugLog.h). The option
    # disables the UART bridge: the CDC port carries only the log. It is ON by
    # default in a Debug build. A Release build carries no log code.
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_MUPP_DEBUG_DEFAULT ON)
    else ()
        set(_MUPP_DEBUG_DEFAULT OFF)
    endif ()
    option(MUPP_DEBUG_CDC "Send the debug log to the CDC-ACM port instead of the UART bridge" ${_MUPP_DEBUG_DEFAULT})
    if (MUPP_DEBUG_CDC AND CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "MUPP_DEBUG_CDC is for debug builds: a Release build has no debug log. Set -DMUPP_DEBUG_CDC=OFF.")
    endif ()

    # Keep the SW-DP of the dongle MCU on (plan section 3): the board drops
    # its LEDs on the SWD pins (board::withSwd() in src/board/BoardDefinition.h).
    option(MUPP_DEBUG_SWD "Keep SWD of the dongle MCU on; drop the LEDs on the SWD pins" ${_MUPP_DEBUG_DEFAULT})
    if (MUPP_DEBUG_SWD AND CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "MUPP_DEBUG_SWD is for debug builds: a Release build has all LEDs. Set -DMUPP_DEBUG_SWD=OFF.")
    endif ()

    # Checks the personality options and MUPP_DEBUG_CDC. Sets
    # MUPP_PERSONALITY_SET, the MUPP_WITH_<P> defines (personality/Personality.h), the sources to drop
    # and MUPP_IMAGE_TAG.
    mupp_resolve_personalities()

    # Selects board::current (src/board/Board.h), e.g. MUPP_BOARD_MINI
    string(TOUPPER "${BOARD}" BOARD_UPPER)
    set(PROJECT_DEFINES MUPP_BOARD_${BOARD_UPPER} ${MUPP_PERSONALITY_DEFINES})
    # An image has the sources of its personalities only: the CMSIS-DAP image
    # does not link the Versaloon command buffers (usb/VersaloonFunction.h).
    # The shared sources (BoardSwd, BoardJtag, BoardGpio) stay: -gc-sections
    # drops what an image does not use.
    set(MUPP_FIRMWARE_SOURCES ${PROJECT_SOURCES})
    # src/ch32v30x/ is the stub image of the CH32 boards (cmake/Ch32StubImage.cmake)
    list(FILTER MUPP_FIRMWARE_SOURCES EXCLUDE REGEX "/src/ch32v30x/")
    set(_mupp_dropped_files ${MUPP_PERSONALITY_DROPPED_SOURCES})
    # The settings page: the stored personality of a multi image and the
    # stored mode of the RISC-V bridge. Only in a multi image and in an image
    # with the bridge. It is at the end of the application area: one 1 KB
    # flash page on the STM32F103, one 8 KB flash sector on the STM32H5.
    if (MUPP_PERSONALITY_COUNT GREATER 1 OR MUPP_RV_BRIDGE)
        set(_mupp_settings ON)
    else ()
        set(_mupp_settings OFF)
    endif ()
    if (_mupp_settings AND STM32_FAMILY STREQUAL "STM32F1")
        set(MUPP_SETTINGS_PAGE_SIZE 1024)
    elseif (_mupp_settings AND STM32_FAMILY STREQUAL "STM32H5")
        set(MUPP_SETTINGS_PAGE_SIZE 8192)
    elseif (_mupp_settings AND MUPP_MCU_VENDOR STREQUAL "CH32")
        message(FATAL_ERROR "Board ${BOARD}: the flash settings page of the CH32V307 is not ported yet (P6), so only a single personality without the RISC-V bridge builds (MUPP_PERSONALITIES=${MUPP_PERSONALITY_SET}, MUPP_CMSIS_DAP_RV_BRIDGE=${MUPP_RV_BRIDGE}).")
    elseif (_mupp_settings)
        message(FATAL_ERROR "Board ${BOARD}: a multi image or the RISC-V bridge (MUPP_PERSONALITIES=${MUPP_PERSONALITY_SET}) needs a settings page, which the ${STM32_FAMILY} does not have.")
    else ()
        list(APPEND _mupp_dropped_files "src/board/SettingsFlashPage\\.cpp")
        set(MUPP_SETTINGS_PAGE_SIZE 0)
    endif ()
    # STM32F1 only: the entry into openmupp-bootloader (BKP registers,
    # app-8k image), the JTAG fast path on SPI v1 and DMA1, and the one-wire
    # engine of WCH-Link on the timers and DMA1. STM32H5 only: the entry into
    # the ROM bootloader, the JTAG fast path on SPI v2 and GPDMA, and the
    # one-wire engine on TIM3 and GPDMA.
    if (NOT STM32_FAMILY STREQUAL "STM32F1")
        list(APPEND _mupp_dropped_files "src/boot/Bootloader\\.cpp" "src/board/BoardJtagFast\\.cpp" "src/board/BoardRvSwd\\.cpp")
    endif ()
    if (NOT STM32_FAMILY STREQUAL "STM32H5")
        list(APPEND _mupp_dropped_files "src/boot/RomBootloader\\.cpp" "src/board/BoardJtagFastH5\\.cpp" "src/board/BoardRvSwdH5\\.cpp")
    endif ()
    foreach (_file ${_mupp_dropped_files})
        list(FILTER MUPP_FIRMWARE_SOURCES EXCLUDE REGEX "${_file}$")
    endforeach ()
    if (MUPP_DEBUG_CDC)
        list(APPEND PROJECT_DEFINES MUPP_DEBUG_CDC)
    else ()
        # The libArT LOG_* macros expand to nothing
        list(APPEND PROJECT_DEFINES LOGGING_DISABLED)
    endif ()
    if (MUPP_DEBUG_SWD)
        list(APPEND PROJECT_DEFINES MUPP_DEBUG_SWD)
    endif ()
    # Link-time optimisation of the firmware and of all libraries (#75): the
    # images are about 15 % smaller. It is ON by default in a Release build.
    if (CMAKE_BUILD_TYPE STREQUAL "Release")
        set(_MUPP_LTO_DEFAULT ON)
    else ()
        set(_MUPP_LTO_DEFAULT OFF)
    endif ()
    option(MUPP_LTO "Link-time optimisation (-flto)" ${_MUPP_LTO_DEFAULT})
    # For the targets below and in the dependencies. CMake adds -flto to
    # the compile and the link, and makes the archives with gcc-ar and
    # gcc-ranlib: with plain ar and ranlib, an archive of LTO objects has
    # no symbol index, and the link fails.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${MUPP_LTO})
    if (MUPP_STRICT_SIZE)
        set(MUPP_FLASH_SLACK 0)
    else ()
        set(MUPP_FLASH_SLACK 65536)
    endif ()
    set(CXX_OPTIONS -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wno-volatile)
    set(COMMON_OPTIONS -Os -ffunction-sections -fdata-sections -g -fno-common -fmessage-length=0 -fsigned-char -Werror=odr --specs=nano.specs --specs=nosys.specs)
    set(C_OPTIONS -std=gnu11)
    set(DEBUG_OPTIONS -Wall -Wextra -Og)
    # The linker scripts of system-stm32 (docs/building.md there). The
    # settings page is the reserved flash at the end (__flash_reserved_start).
    # The main stack is 2 KB, because the STM32F103 has only 20 KB RAM, and
    # the link fails when less than the full stack is free. Each image sets
    # __flash_offset and __flash_length (mupp_add_app_image).
    if (MUPP_MCU_VENDOR STREQUAL "CH32")
        # The scripts of ch32v00x-system (no settings page, no flash offset yet)
        set(MUPP_LDSCRIPT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ch32v00x-system/ldscripts)
        set(MUPP_LIBS_LD "")
        set(LINKER_OPTIONS -Wl,-gc-sections -Wl,--no-warn-rwx-segments -L${MUPP_LDSCRIPT_DIR} -T${MUPP_LDSCRIPT} -Tsections.ld -Tnoheap.ld -nostartfiles --specs=nano.specs --specs=nosys.specs ${COMMON_OPTIONS})
    else ()
        set(MUPP_LDSCRIPT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/system-stm32/ldscripts)
        set(MUPP_LIBS_LD "${MUPP_LDSCRIPT_DIR}/libs.ld")
        set(LINKER_OPTIONS -Wl,-gc-sections -Wl,--no-warn-rwx-segments -L${MUPP_LDSCRIPT_DIR} -Wl,--defsym=__flash_reserved_end=${MUPP_SETTINGS_PAGE_SIZE} -Wl,--defsym=__main_stack_size=2048 -Wl,--defsym=__minimum_stack_size=2048 -T${MUPP_LDSCRIPT} -Tlibs.ld -Tsections.ld -nostartfiles --specs=nano.specs --specs=nosys.specs ${COMMON_OPTIONS})
    endif ()
endif ()

# Central interface target to unify compile/link flags and defines across all used libraries and executables
include("${CMAKE_SOURCE_DIR}/cmake/BuildConfig.cmake")
art_setup_build_config(art-build-config)

if (CMAKE_CROSSCOMPILING)
    if (NOT TARGET art::core)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/dependencies/libArT")
    endif ()
    # The system library and the libArT port of the MCU vendor
    if (MUPP_MCU_VENDOR STREQUAL "CH32")
        set(MUPP_VENDOR_LIBS ch32v00x-art ch32v00x-system)
        if (NOT TARGET ch32v00x-system)
            add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ch32v00x-system")
        endif ()
        if (NOT TARGET ch32v00x-art)
            add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ch32v00x-art")
        endif ()
    else ()
        set(MUPP_VENDOR_LIBS art::stm32 system-stm32)
        if (NOT TARGET system-stm32)
            add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/dependencies/system-stm32")
        endif ()
        if (NOT TARGET art::stm32)
            add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/dependencies/libart-stm32")
        endif ()
    endif ()

    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/output_files.cmake")

    MESSAGE(STATUS "Building openmupp version " ${FIRMWARE_VERSION} " (" ${CMAKE_BUILD_TYPE} ") for board: " ${BOARD} " (images: " "${MUPP_APP_IMAGES}" ", personalities: " "${MUPP_PERSONALITY_SET}" ", default: " ${MUPP_DEFAULT_PERSONALITY_NAME} ", debug log over CDC: " ${MUPP_DEBUG_CDC} ", SWD on: " ${MUPP_DEBUG_SWD} ", RISC-V bridge: " ${MUPP_RV_BRIDGE} ", strict size: " ${MUPP_STRICT_SIZE} ", LTO: " ${MUPP_LTO} ")")

    #----------------------------------------------------------------------------------------------------#
    # Main program
    #----------------------------------------------------------------------------------------------------#
    # All sources except src/main.cpp are compiled one time and linked into
    # each application image. src/main.cpp is compiled per image, because
    # only the images with the bootloader entry define MUPP_HAS_BOOTLOADER_ENTRY
    # for it. No other source may use the define.
    set(MUPP_SHARED_SOURCES ${MUPP_FIRMWARE_SOURCES})
    list(FILTER MUPP_SHARED_SOURCES EXCLUDE REGEX "src/main\\.cpp$")
    add_library(openmupp-objects OBJECT ${MUPP_SHARED_SOURCES})
    # The JTAG, SWD and RISC-V debug wire engines are instantiated in these
    # files (see board/BoardJtag.h, board/BoardSwd.h and board/BoardRvSwd.h).
    # They use -O2 instead of -Os. It comes after the -Os of
    # COMMON_OPTIONS, so it wins.
    set_source_files_properties(src/board/BoardJtag.cpp src/board/BoardJtagFast.cpp src/board/BoardJtagFastH5.cpp
            src/board/BoardSwd.cpp src/board/BoardRvSwd.cpp src/board/BoardRvSwdH5.cpp PROPERTIES COMPILE_OPTIONS -O2)
    # Apply unified build configuration
    target_link_libraries(openmupp-objects PUBLIC art-build-config art::core ${MUPP_VENDOR_LIBS})
    target_include_directories(openmupp-objects PUBLIC ${PROJECT_INCLUDE_DIRS})

    # The application image header (magic, length, CRC32) of
    # openmupp-bootloader. The bootloader starts only an image with a valid
    # header. The tool is in the bootloader submodule.
    set(_mupp_stamp_cmake "${PROJECT_SOURCE_DIR}/dependencies/openmupp-bootloader/cmake/MuppStampImage.cmake")
    if ("app-8k" IN_LIST MUPP_APP_IMAGES OR "app-10k" IN_LIST MUPP_APP_IMAGES)
        if (NOT EXISTS "${_mupp_stamp_cmake}")
            message(FATAL_ERROR "${_mupp_stamp_cmake} not found. The images behind a bootloader need it. "
                    "Run: git submodule update --init --recursive")
        endif ()
        include("${_mupp_stamp_cmake}")
    endif ()

    # Links the application image <image> (one of MUPP_APP_IMAGES) as the
    # target openmupp-<image>.elf. The build directory gets
    # openmupp-<image>.hex and .bin, and a copy with the board,
    # the build type and the version in the name. An image behind a
    # bootloader gets the application image header.
    function(mupp_add_app_image image)
        set(_target openmupp-${image}.elf)
        set(_name openmupp-${image})
        set(_offset ${MUPP_APP_OFFSET_${image}})
        math(EXPR _limit "${MUPP_FLASH_SIZE} - ${_offset} - ${MUPP_SETTINGS_PAGE_SIZE}")
        set(MUPP_APP_LIMIT_${image} ${_limit} PARENT_SCOPE)
        # The FLASH region of the link, with the slack: the size check
        # below compares the image with the real limit.
        math(EXPR _link_limit "${_limit} + ${MUPP_FLASH_SLACK}")
        add_executable(${_target} src/main.cpp)
        # system-stm32 is an OBJECT library (start-up code, vector table).
        # CMake adds the objects of an OBJECT library only to the target
        # that links it directly, so each image links the libraries again.
        target_link_libraries(${_target} PUBLIC openmupp-objects art::core ${MUPP_VENDOR_LIBS})
        target_link_options(${_target} PUBLIC -Wl,-Map,${_name}.map)
        if (NOT MUPP_MCU_VENDOR STREQUAL "CH32")
            target_link_options(${_target} PUBLIC -Wl,--defsym=__flash_offset=${_offset} -Wl,--defsym=__flash_length=${_link_limit})
        endif ()
        # FW_UPDATE enters the bootloader only behind openmupp-bootloader.
        # The legacy bootloader of app-10k does not know the boot request
        # (hasBootloaderEntry in src/main.cpp). On the STM32H5, the
        # standalone image enters the ROM bootloader.
        if (image STREQUAL "app-8k" OR (image STREQUAL "standalone" AND STM32_FAMILY STREQUAL "STM32H5"))
            target_compile_definitions(${_target} PRIVATE MUPP_HAS_BOOTLOADER_ENTRY)
        endif ()
        # Relink when a linker script or the size check changes
        set_target_properties(${_target} PROPERTIES LINK_DEPENDS
                "${MUPP_LDSCRIPT_DIR}/${MUPP_LDSCRIPT};${MUPP_LIBS_LD};${MUPP_LDSCRIPT_DIR}/sections.ld;${PROJECT_SOURCE_DIR}/cmake/ImageSize.cmake")

        ADD_DUMP_TARGET(${_target})
        PRINT_SIZE_OF_TARGETS(${_target})

        set(_file openmupp-${BOARD}${MUPP_IMAGE_TAG}-${image}-${CMAKE_BUILD_TYPE}-${FIRMWARE_VERSION})
        if (_offset EQUAL 0)
            add_custom_command(TARGET ${_target} POST_BUILD
                    COMMAND ${CMAKE_OBJCOPY} -Oihex $<TARGET_FILE:${_target}> ${_name}.hex
                    COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:${_target}> ${_name}.bin)
        else ()
            # The stamp step writes the header into the ELF file and makes
            # the .hex and .bin files. A separate objcopy step would fill the
            # gaps with 0x00 and give a wrong CRC.
            mupp_stamp_image(${_target} BIN ${_name}.bin HEX ${_name}.hex MAX_LENGTH ${_link_limit})
        endif ()
        # Post-build steps run in the order of the calls: the size check
        # comes after the stamp step. It makes the copies when the image
        # fits, and skips the image when it does not fit.
        add_custom_command(TARGET ${_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -DIMAGE=${_name} -DLIMIT=${_limit} -DCOPY=${_file}
                    -DSTRICT=${MUPP_STRICT_SIZE} -P ${PROJECT_SOURCE_DIR}/cmake/ImageSize.cmake
                COMMENT "Checking the size of ${_name}, building ${_file}.hex and ${_file}.bin"
                VERBATIM)
    endfunction()

    # mupp-images.txt lists each image with its size limit, one line
    # "<bin-file> <limit-in-bytes>". CI runs ci/check-size.sh on each line.
    # The limit of an application image is the FLASH region of
    # the board linker script (MUPP_LDSCRIPT). The combined image is added below.
    set(_images_list "")
    foreach (_image ${MUPP_APP_IMAGES})
        mupp_add_app_image(${_image})
        string(APPEND _images_list "openmupp-${_image}.bin ${MUPP_APP_LIMIT_${_image}}\n")
    endforeach ()
    # The firmware must fit behind the legacy bootloader on all STM32F1 boards
    # (docs/firmware-update.md). A board without the app-10k image checks its
    # standalone image against the app-10k limit too. MUPP_SKIP_IMAGES=app-10k
    # on a board with the legacy bootloader (MUPP_LEGACY_BOOTLOADER_BOARDS)
    # turns this check off. The legacy bootloader is STM32F1 only.
    if (NOT BOARD IN_LIST MUPP_LEGACY_BOOTLOADER_BOARDS AND "standalone" IN_LIST MUPP_APP_IMAGES AND STM32_FAMILY STREQUAL "STM32F1")
        math(EXPR _limit "${MUPP_FLASH_SIZE} - ${MUPP_APP_OFFSET_app-10k} - ${MUPP_SETTINGS_PAGE_SIZE}")
        string(APPEND _images_list "openmupp-standalone.bin ${_limit}\n")
    endif ()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mupp-images.txt" "${_images_list}")

    include(Bootloader)
    if (MUPP_BUILD_BOOTLOADER AND BOARD IN_LIST MUPP_BOOTLOADER_BOARDS)
        mupp_add_bootloader()
        MESSAGE(STATUS "Building openmupp-bootloader for board " ${BOARD} " into " ${MUPP_BOOTLOADER_DIR})
    endif ()
    if (MUPP_BUILD_BOOTLOADER AND BOARD IN_LIST MUPP_BOOTLOADER_BOARDS AND "app-8k" IN_LIST MUPP_APP_IMAGES)

        # The combined image: openmupp-bootloader and the app-8k image in
        # one file from 0x08000000, for a new board and a debug probe. The
        # gap between the two is 0xFF (tools/mupp_combine_images.py).
        find_package(Python3 3.7 REQUIRED COMPONENTS Interpreter)
        set(_name openmupp-with-bootloader)
        set(_file openmupp-${BOARD}${MUPP_IMAGE_TAG}-with-bootloader-${CMAKE_BUILD_TYPE}-${FIRMWARE_VERSION})
        # cmake/CombinedImage.cmake skips the combined image when the build
        # skipped the app-8k image. The app-8k .bin file is not in DEPENDS:
        # it can be missing. The .elf file of the app-8k image is.
        add_custom_command(
                OUTPUT ${_name}.bin ${_name}.hex
                COMMAND ${CMAKE_COMMAND} -DPYTHON=${Python3_EXECUTABLE}
                    -DTOOL=${PROJECT_SOURCE_DIR}/tools/mupp_combine_images.py
                    -DBOOTLOADER=${MUPP_BOOTLOADER_DIR}/openmupp-bootloader-${BOARD}.bin
                    -DAPP=openmupp-app-8k.bin -DAPP_OFFSET=${MUPP_APP_OFFSET_app-8k}
                    -DIMAGE=${_name} -DCOPY=${_file}
                    -P ${PROJECT_SOURCE_DIR}/cmake/CombinedImage.cmake
                DEPENDS openmupp-bootloader openmupp-app-8k.elf
                    "${PROJECT_SOURCE_DIR}/tools/mupp_combine_images.py"
                    "${PROJECT_SOURCE_DIR}/cmake/CombinedImage.cmake"
                    "${MUPP_BOOTLOADER_DIR}/openmupp-bootloader-${BOARD}.bin"
                BYPRODUCTS ${_file}.hex ${_file}.bin
                COMMENT "Building ${_file}.hex \nBuilding ${_file}.bin"
                VERBATIM)
        add_custom_target(openmupp-with-bootloader ALL DEPENDS ${_name}.bin ${_name}.hex)
        # The combined image ends where the app-8k image ends: its limit is the
        # flash without the settings page.
        math(EXPR _limit "${MUPP_FLASH_SIZE} - ${MUPP_SETTINGS_PAGE_SIZE}")
        file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/mupp-images.txt" "${_name}.bin ${_limit}\n")
    endif ()
else ()
    enable_testing()
    add_subdirectory(tests)
endif ()

### Clang-format related stuff ###
include(ClangFormat)

# Format the whole project
clangformat_setup(
        ${PROJECT_SOURCES}
        ${PROJECT_HEADERS}
        ${TEST_SOURCES}
)
