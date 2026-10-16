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

project(versaloon-ng
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

# Board -> crystal frequency. All boards have an STM32F103x8 or STM32F103xB.
# CMSIS has one define (STM32F103xB) for both parts.
#   mini      Versaloon Mini, 12 MHz crystal
#   otheruse  OtherUse, 12 MHz crystal
#   stlinkv2  ST-Link/V2 clone, 8 MHz crystal (compile-only target)
set(VSL_BOARDS mini otheruse stlinkv2)
# Boards that versaloon-bootloader supports (cmake/Bootloader.cmake)
set(VSL_BOOTLOADER_BOARDS mini otheruse)
set(BOARD "mini" CACHE STRING "Board to build for: ${VSL_BOARDS}")
set_property(CACHE BOARD PROPERTY STRINGS ${VSL_BOARDS})

# Application images. Each image is the same firmware, linked for one start
# address (ldscripts/mem-f103.ld). The value is the space in bytes in front
# of the application:
#   standalone  0       0x08000000, no bootloader: flash with a debug probe
#   app-8k      8192    0x08002000, behind versaloon-bootloader (8 KB)
#   app-10k     10240   0x08002800, behind the legacy Versaloon bootloader
#                       (10 KB) of the existing boards
# The boards in VSL_BOOTLOADER_BOARDS get all images. The other boards have
# no bootloader and get the standalone image only.
set(VSL_APP_OFFSET_standalone 0)
set(VSL_APP_OFFSET_app-8k 8192)
set(VSL_APP_OFFSET_app-10k 10240)
# Build versaloon-bootloader (dependencies/versaloon-bootloader, cmake/Bootloader.cmake)
# for the board. Only for boards that the bootloader supports.
option(VSL_BUILD_BOOTLOADER "Build versaloon-bootloader for the board" ON)

# USB personalities of the firmware image (cmake/Personalities.cmake):
# versaloon, cmsis-dap and wch-link. An image with more than one personality
# has one active personality after each start. A switch command or the key
# selects the next one, and a flash page at the end of the application area
# stores it (src/personality/).
include(Personalities)
# The deprecated VSL_PROTOCOL sets the personalities when it is set. The
# cache entry of VSL_PERSONALITIES then must not exist, so that
# vsl_resolve_personalities() finds an option that is set twice.
if ("${VSL_PROTOCOL}" STREQUAL "")
    set(VSL_PERSONALITIES "versaloon" CACHE STRING
            "USB personalities of the image, a list with ';' or ',': ${VSL_ALL_PERSONALITIES}")
endif ()
set(VSL_DEFAULT_PERSONALITY "" CACHE STRING
        "Personality of a multi image when no personality is stored. Empty: the first one of ${VSL_ALL_PERSONALITIES}")

# The WCH-Link personality uses the USB ID 1a86:8010 of WCH instead of the
# own ID. wlink and minichlink find a probe by this ID. The vendor ID belongs
# to WCH: use it for personal use only. CI does not publish this image.
option(VSL_WCH_LINK_WCH_USB_ID "Use the USB ID of WCH (1a86:8010) in the wch-link personality, for personal use only" OFF)

# Set compile/link variables before creating art-build-config so the interface
# target captures them correctly.  These settings are cross-compile only; the
# native (test) build leaves the variables unset.
if (CMAKE_CROSSCOMPILING)
    if (NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release)
    endif ()
    if (NOT BOARD IN_LIST VSL_BOARDS)
        message(FATAL_ERROR "Unknown BOARD '${BOARD}'. Supported boards: ${VSL_BOARDS}")
    endif ()

    set(STM32_FAMILY "STM32F1" CACHE STRING "STM32 family to build for" FORCE)
    set(STM32_DEVICE "STM32F103xB" CACHE STRING "STM32 device define for CMSIS" FORCE)
    # system-stm32 gives HSE_VALUE to the CMSIS SystemCoreClockUpdate() and the
    # Rcc clock recipes. With a wrong value the core clock and the ADC clock
    # are computed wrong. src/board/BoardChecks.cpp checks that this value is
    # the crystal of the board definition.
    if (BOARD STREQUAL "stlinkv2")
        set(SYSTEM_STM32_HSE_VALUE "8000000" CACHE STRING "HSE crystal frequency in Hz" FORCE)
    else ()
        set(SYSTEM_STM32_HSE_VALUE "12000000" CACHE STRING "HSE crystal frequency in Hz" FORCE)
    endif ()

    if (BOARD IN_LIST VSL_BOOTLOADER_BOARDS)
        set(VSL_APP_IMAGES standalone app-8k app-10k)
    else ()
        set(VSL_APP_IMAGES standalone)
    endif ()

    # Debug log over the CDC-ACM port (src/debug/DebugLog.h). The option
    # disables the UART bridge: the CDC port carries only the log. It is ON by
    # default in a Debug build. A Release build carries no log code.
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_VSL_DEBUG_DEFAULT ON)
    else ()
        set(_VSL_DEBUG_DEFAULT OFF)
    endif ()
    option(VSL_DEBUG_CDC "Send the debug log to the CDC-ACM port instead of the UART bridge" ${_VSL_DEBUG_DEFAULT})
    if (VSL_DEBUG_CDC AND CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "VSL_DEBUG_CDC is for debug builds: a Release build has no debug log. Set -DVSL_DEBUG_CDC=OFF.")
    endif ()

    # Keep the SW-DP of the dongle MCU on (plan section 3): the board drops
    # its LEDs on the SWD pins (board::withSwd() in src/board/BoardDefinition.h).
    option(VSL_DEBUG_SWD "Keep SWD of the dongle MCU on; drop the LEDs on the SWD pins" ${_VSL_DEBUG_DEFAULT})
    if (VSL_DEBUG_SWD AND CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "VSL_DEBUG_SWD is for debug builds: a Release build has all LEDs. Set -DVSL_DEBUG_SWD=OFF.")
    endif ()

    # Checks the personality options. Sets VSL_PERSONALITY_SET, the
    # VSL_WITH_<P> defines (personality/Personality.h), the sources to drop
    # and VSL_IMAGE_TAG.
    vsl_resolve_personalities()
    # The log goes to the CDC port. In a multi image the log is silent while
    # the WCH-Link personality is active.
    if (VSL_PERSONALITY_SET STREQUAL "wch-link" AND VSL_DEBUG_CDC)
        message(FATAL_ERROR "An image with only the wch-link personality has no CDC port for the debug log. Set -DVSL_DEBUG_CDC=OFF.")
    endif ()

    # Selects board::current (src/board/Board.h), e.g. VSL_BOARD_MINI
    string(TOUPPER "${BOARD}" BOARD_UPPER)
    set(PROJECT_DEFINES VSL_BOARD_${BOARD_UPPER} ${VSL_PERSONALITY_DEFINES})
    # An image has the sources of its personalities only: the CMSIS-DAP image
    # does not link the Versaloon command buffers (usb/VersaloonFunction.h).
    # The shared sources (BoardSwd, BoardJtag, BoardGpio) stay: -gc-sections
    # drops what an image does not use.
    set(VSL_FIRMWARE_SOURCES ${PROJECT_SOURCES})
    set(_vsl_dropped_files ${VSL_PERSONALITY_DROPPED_SOURCES})
    # The flash page of the stored personality: only in a multi image. It is
    # one STM32F103 flash page at the end of the application area.
    if (VSL_PERSONALITY_COUNT GREATER 1)
        set(VSL_SETTINGS_PAGE_SIZE 1024)
    else ()
        list(APPEND _vsl_dropped_files "src/board/SettingsFlashPage\\.cpp")
        set(VSL_SETTINGS_PAGE_SIZE 0)
    endif ()
    foreach (_file ${_vsl_dropped_files})
        list(FILTER VSL_FIRMWARE_SOURCES EXCLUDE REGEX "${_file}$")
    endforeach ()
    if (VSL_DEBUG_CDC)
        list(APPEND PROJECT_DEFINES VSL_DEBUG_CDC)
    else ()
        # The libArT LOG_* macros expand to nothing
        list(APPEND PROJECT_DEFINES LOGGING_DISABLED)
    endif ()
    if (VSL_DEBUG_SWD)
        list(APPEND PROJECT_DEFINES VSL_DEBUG_SWD)
    endif ()
    set(CXX_OPTIONS -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wno-volatile)
    set(COMMON_OPTIONS -Os -ffunction-sections -fdata-sections -g -fno-common -fmessage-length=0 -fsigned-char -Werror=odr --specs=nano.specs --specs=nosys.specs)
    set(C_OPTIONS -std=gnu11)
    set(DEBUG_OPTIONS -Wall -Wextra -Og)
    set(LINKER_OPTIONS -Wl,-gc-sections -Wl,--no-warn-rwx-segments -L${CMAKE_CURRENT_SOURCE_DIR}/ldscripts -Wl,--defsym=__vsl_settings_size=${VSL_SETTINGS_PAGE_SIZE} -Tmem-f103.ld -Tlibs.ld -Tsections.ld -nostartfiles --specs=nano.specs --specs=nosys.specs ${COMMON_OPTIONS})
endif ()

# Central interface target to unify compile/link flags and defines across all used libraries and executables
include("${CMAKE_SOURCE_DIR}/cmake/BuildConfig.cmake")
art_setup_build_config(art-build-config)

if (CMAKE_CROSSCOMPILING)
    if (NOT TARGET art::core)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/dependencies/libArT")
    endif ()
    if (NOT TARGET system-stm32)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/dependencies/system-stm32")
    endif ()
    if (NOT TARGET art::stm32)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/dependencies/libart-stm32")
    endif ()

    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/output_files.cmake")

    MESSAGE(STATUS "Building versaloon-ng version " ${FIRMWARE_VERSION} " (" ${CMAKE_BUILD_TYPE} ") for board: " ${BOARD} " (images: " "${VSL_APP_IMAGES}" ", personalities: " "${VSL_PERSONALITY_SET}" ", default: " ${VSL_DEFAULT_PERSONALITY_NAME} ", debug log over CDC: " ${VSL_DEBUG_CDC} ", SWD on: " ${VSL_DEBUG_SWD} ")")

    #----------------------------------------------------------------------------------------------------#
    # Main program
    #----------------------------------------------------------------------------------------------------#
    # All sources except src/main.cpp are compiled one time and linked into
    # each application image. src/main.cpp is compiled per image, because
    # only the app-8k image defines VSL_HAS_BOOTLOADER_ENTRY for it. No other
    # source may use the define.
    set(VSL_SHARED_SOURCES ${VSL_FIRMWARE_SOURCES})
    list(FILTER VSL_SHARED_SOURCES EXCLUDE REGEX "src/main\\.cpp$")
    add_library(versaloon-ng-objects OBJECT ${VSL_SHARED_SOURCES})
    # The JTAG, SWD and RISC-V debug wire engines are instantiated in these
    # files (see board/BoardJtag.h, board/BoardSwd.h and board/BoardRvSwd.h).
    # They use -O2 instead of -Os. It comes after the -Os of
    # COMMON_OPTIONS, so it wins.
    set_source_files_properties(src/board/BoardJtag.cpp src/board/BoardJtagFast.cpp src/board/BoardSwd.cpp
            src/board/BoardRvSwd.cpp PROPERTIES COMPILE_OPTIONS -O2)
    # Apply unified build configuration
    target_link_libraries(versaloon-ng-objects PUBLIC art-build-config art::core art::stm32 system-stm32)
    target_include_directories(versaloon-ng-objects PUBLIC ${PROJECT_INCLUDE_DIRS})

    # The application image header (magic, length, CRC32) of
    # versaloon-bootloader. The bootloader starts only an image with a valid
    # header. The tool is in the bootloader submodule.
    set(_vsl_stamp_cmake "${PROJECT_SOURCE_DIR}/dependencies/versaloon-bootloader/cmake/VslStampImage.cmake")
    if ("app-8k" IN_LIST VSL_APP_IMAGES)
        if (NOT EXISTS "${_vsl_stamp_cmake}")
            message(FATAL_ERROR "${_vsl_stamp_cmake} not found. The images behind a bootloader need it. "
                    "Run: git submodule update --init --recursive")
        endif ()
        include("${_vsl_stamp_cmake}")
    endif ()

    # Links the application image <image> (one of VSL_APP_IMAGES) as the
    # target versaloon-ng-<image>.elf. The build directory gets
    # versaloon-ng-<image>.hex and .bin, and a copy with the board,
    # the build type and the version in the name. An image behind a
    # bootloader gets the application image header.
    function(vsl_add_app_image image)
        set(_target versaloon-ng-${image}.elf)
        set(_name versaloon-ng-${image})
        set(_offset ${VSL_APP_OFFSET_${image}})
        math(EXPR _limit "65536 - ${_offset} - ${VSL_SETTINGS_PAGE_SIZE}")
        set(VSL_APP_LIMIT_${image} ${_limit} PARENT_SCOPE)
        add_executable(${_target} src/main.cpp)
        # system-stm32 is an OBJECT library (start-up code, vector table).
        # CMake adds the objects of an OBJECT library only to the target
        # that links it directly, so each image links the libraries again.
        target_link_libraries(${_target} PUBLIC versaloon-ng-objects art::core art::stm32 system-stm32)
        target_link_options(${_target} PUBLIC -Wl,--defsym=__vsl_app_offset=${_offset} -Wl,-Map,${_name}.map)
        # FW_UPDATE enters the bootloader only behind versaloon-bootloader.
        # The legacy bootloader of app-10k does not know the boot request
        # (hasBootloaderEntry in src/main.cpp).
        if (image STREQUAL "app-8k")
            target_compile_definitions(${_target} PRIVATE VSL_HAS_BOOTLOADER_ENTRY)
        endif ()
        # Relink when a linker script changes
        set_target_properties(${_target} PROPERTIES LINK_DEPENDS
                "${PROJECT_SOURCE_DIR}/ldscripts/mem-f103.ld;${PROJECT_SOURCE_DIR}/ldscripts/libs.ld;${PROJECT_SOURCE_DIR}/ldscripts/sections.ld")

        ADD_DUMP_TARGET(${_target})
        PRINT_SIZE_OF_TARGETS(${_target})

        set(_file versaloon-ng-${BOARD}${VSL_IMAGE_TAG}-${image}-${CMAKE_BUILD_TYPE}-${FIRMWARE_VERSION})
        if (_offset EQUAL 0)
            add_custom_command(TARGET ${_target} POST_BUILD
                    COMMAND ${CMAKE_OBJCOPY} -Oihex $<TARGET_FILE:${_target}> ${_name}.hex
                    COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:${_target}> ${_name}.bin)
        else ()
            # The stamp step writes the header into the ELF file and makes
            # the .hex and .bin files. A separate objcopy step would fill the
            # gaps with 0x00 and give a wrong CRC.
            vsl_stamp_image(${_target} BIN ${_name}.bin HEX ${_name}.hex MAX_LENGTH ${_limit})
        endif ()
        # Post-build steps run in the order of the calls: the copies come
        # after the stamp step.
        add_custom_command(TARGET ${_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy ${_name}.hex ${_file}.hex
                COMMAND ${CMAKE_COMMAND} -E copy ${_name}.bin ${_file}.bin
                COMMENT "Building ${_file}.hex \nBuilding ${_file}.bin")
    endfunction()

    # vsl-images.txt lists each image with its size limit, one line
    # "<bin-file> <limit-in-bytes>". CI runs ci/check-size.sh on each line.
    # The limit of an application image is the FLASH region of
    # ldscripts/mem-f103.ld. The combined image is added below.
    set(_images_list "")
    foreach (_image ${VSL_APP_IMAGES})
        vsl_add_app_image(${_image})
        string(APPEND _images_list "versaloon-ng-${_image}.bin ${VSL_APP_LIMIT_${_image}}\n")
    endforeach ()
    # The firmware must fit behind the legacy bootloader on all boards
    # (docs/firmware-update.md). A board without the app-10k image checks its
    # standalone image against the app-10k limit too.
    if (NOT "app-10k" IN_LIST VSL_APP_IMAGES)
        math(EXPR _limit "65536 - ${VSL_APP_OFFSET_app-10k} - ${VSL_SETTINGS_PAGE_SIZE}")
        string(APPEND _images_list "versaloon-ng-standalone.bin ${_limit}\n")
    endif ()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/vsl-images.txt" "${_images_list}")

    include(Bootloader)
    if (VSL_BUILD_BOOTLOADER AND BOARD IN_LIST VSL_BOOTLOADER_BOARDS)
        vsl_add_bootloader()
        MESSAGE(STATUS "Building versaloon-bootloader for board " ${BOARD} " into " ${VSL_BOOTLOADER_DIR})

        # The combined image: versaloon-bootloader and the app-8k image in
        # one file from 0x08000000, for a new board and a debug probe. The
        # gap between the two is 0xFF (tools/vsl_combine_images.py).
        find_package(Python3 3.7 REQUIRED COMPONENTS Interpreter)
        set(_name versaloon-ng-with-bootloader)
        set(_file versaloon-ng-${BOARD}${VSL_IMAGE_TAG}-with-bootloader-${CMAKE_BUILD_TYPE}-${FIRMWARE_VERSION})
        add_custom_command(
                OUTPUT ${_name}.bin ${_name}.hex
                COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/vsl_combine_images.py"
                    --bootloader "${VSL_BOOTLOADER_DIR}/versaloon-bootloader-${BOARD}.bin"
                    --app versaloon-ng-app-8k.bin --app-offset ${VSL_APP_OFFSET_app-8k}
                    --bin ${_name}.bin --hex ${_name}.hex
                COMMAND ${CMAKE_COMMAND} -E copy ${_name}.hex ${_file}.hex
                COMMAND ${CMAKE_COMMAND} -E copy ${_name}.bin ${_file}.bin
                DEPENDS versaloon-bootloader versaloon-ng-app-8k.elf
                    "${PROJECT_SOURCE_DIR}/tools/vsl_combine_images.py"
                    "${VSL_BOOTLOADER_DIR}/versaloon-bootloader-${BOARD}.bin"
                    "${CMAKE_CURRENT_BINARY_DIR}/versaloon-ng-app-8k.bin"
                BYPRODUCTS ${_file}.hex ${_file}.bin
                COMMENT "Building ${_file}.hex \nBuilding ${_file}.bin"
                VERBATIM)
        add_custom_target(versaloon-ng-with-bootloader ALL DEPENDS ${_name}.bin ${_name}.hex)
        # The combined image ends where the app-8k image ends: its limit is the
        # flash without the settings page.
        math(EXPR _limit "65536 - ${VSL_SETTINGS_PAGE_SIZE}")
        file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/vsl-images.txt" "${_name}.bin ${_limit}\n")
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
