cmake_minimum_required(VERSION 3.20)

# Shared FetchContent source cache — parallel configures are serialized
# with a file lock so the first build downloads and the rest reuse.
set(_FC_SHARED_DIR "${CMAKE_CURRENT_LIST_DIR}/build/_deps")
file(MAKE_DIRECTORY "${_FC_SHARED_DIR}")
file(LOCK "${_FC_SHARED_DIR}/.lock" GUARD PROCESS)

# All downloads land in the shared directory
set(FETCHCONTENT_BASE_DIR "${_FC_SHARED_DIR}" CACHE PATH "")

# For sources already downloaded, set FETCHCONTENT_SOURCE_DIR_<NAME> so
# FetchContent skips the subbuild entirely (avoids generator conflicts)
file(GLOB _fc_src_dirs "${_FC_SHARED_DIR}/*-src")
foreach(_src_dir ${_fc_src_dirs})
    get_filename_component(_dir_name "${_src_dir}" NAME)
    string(REGEX REPLACE "-src$" "" _dep_name "${_dir_name}")
    string(TOUPPER "${_dep_name}" _dep_upper)
    set(FETCHCONTENT_SOURCE_DIR_${_dep_upper} "${_src_dir}")
endforeach()

Include(FetchContent)

# Extract git revision
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_version.cmake")

project(versaloon-next-gen
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

# Link the application behind versaloon-bootloader (0x08002800, 54 KB) or
# at the start of FLASH (0x08000000) for development without a bootloader.
option(VSL_BOOTLOADER "Link the application behind versaloon-bootloader at 0x08002800" ON)
# Build versaloon-bootloader (dependencies/versaloon-bootloader, cmake/Bootloader.cmake)
# for the board. Only for boards that the bootloader supports, and only with VSL_BOOTLOADER.
option(VSL_BUILD_BOOTLOADER "Build versaloon-bootloader for the board" ON)

# USB personality of the firmware image. One image has one personality.
#   versaloon  the Versaloon interface of OpenOCD (default)
#   cmsis-dap  the CMSIS-DAP v2 interface of OpenOCD, pyOCD and probe-rs
set(VSL_PROTOCOLS versaloon cmsis-dap)
set(VSL_PROTOCOL "versaloon" CACHE STRING "USB personality: ${VSL_PROTOCOLS}")
set_property(CACHE VSL_PROTOCOL PROPERTY STRINGS ${VSL_PROTOCOLS})

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

    if (VSL_BOOTLOADER)
        set(VSL_MEMORY_LDSCRIPT mem-f103-bootloader.ld)
    else ()
        set(VSL_MEMORY_LDSCRIPT mem-f103.ld)
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

    if (NOT VSL_PROTOCOL IN_LIST VSL_PROTOCOLS)
        message(FATAL_ERROR "Unknown VSL_PROTOCOL '${VSL_PROTOCOL}'. Supported protocols: ${VSL_PROTOCOLS}")
    endif ()

    # Selects board::current (src/board/Board.h), e.g. VSL_BOARD_MINI
    string(TOUPPER "${BOARD}" BOARD_UPPER)
    set(PROJECT_DEFINES VSL_BOARD_${BOARD_UPPER})
    # FW_UPDATE enters the bootloader only when the firmware is linked behind
    # one and the board has one (src/boot/BootloaderEntry.h)
    if (VSL_BOOTLOADER AND BOARD IN_LIST VSL_BOOTLOADER_BOARDS)
        list(APPEND PROJECT_DEFINES VSL_HAS_BOOTLOADER_ENTRY)
    endif ()
    # An image has the sources of its personality only: the CMSIS-DAP image
    # does not link the Versaloon command buffers (usb/VersaloonFunction.h).
    set(VSL_FIRMWARE_SOURCES ${PROJECT_SOURCES})
    # The flash page of the stored personality: only in a dual image
    set(_vsl_settings_files "src/board/SettingsFlashPage\\.cpp")
    set(VSL_SETTINGS_PAGE_SIZE 0)
    # VSL_WITH_VERSALOON and VSL_WITH_CMSIS_DAP: the personalities in the image
    if (VSL_PROTOCOL STREQUAL "cmsis-dap")
        list(APPEND PROJECT_DEFINES VSL_WITH_CMSIS_DAP)
        set(VSL_IMAGE_TAG "-cmsis-dap")
        set(_vsl_other_protocol_files
                "src/protocol/CommandAssembler\\.cpp" "src/protocol/CommandProcessor\\.cpp"
                "src/protocol/UsbToAll\\.cpp" "src/protocol/UsbToXxx\\.cpp"
                "src/protocol/GpioBlock\\.cpp" "src/protocol/JtagRawBlock\\.cpp"
                "src/protocol/PowerBlock\\.cpp" "src/protocol/SwdBlock\\.cpp"
                "src/usb/VersaloonFunction\\.cpp" "src/board/BoardPower\\.cpp"
                "src/boot/Bootloader\\.cpp")
    else ()
        list(APPEND PROJECT_DEFINES VSL_WITH_VERSALOON)
        set(VSL_IMAGE_TAG "")
        set(_vsl_other_protocol_files "src/protocol/cmsisdap/DapJtag\\.cpp" "src/protocol/cmsisdap/DapProcessor\\.cpp"
                "src/usb/CmsisDapFunction\\.cpp")
    endif ()
    foreach (_file ${_vsl_other_protocol_files} ${_vsl_settings_files})
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
    set(LINKER_OPTIONS -Wl,-gc-sections -Wl,--no-warn-rwx-segments -L${CMAKE_CURRENT_SOURCE_DIR}/ldscripts -Wl,--defsym=__vsl_settings_size=${VSL_SETTINGS_PAGE_SIZE} -T${VSL_MEMORY_LDSCRIPT} -Tlibs.ld -Tsections.ld -nostartfiles --specs=nano.specs --specs=nosys.specs ${COMMON_OPTIONS})
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

    if (VSL_BOOTLOADER)
        set(VSL_LINK_VARIANT bootloader)
    else ()
        set(VSL_LINK_VARIANT direct)
    endif ()
    MESSAGE(STATUS "Building versaloon-next-gen version " ${FIRMWARE_VERSION} " (" ${CMAKE_BUILD_TYPE} ") for board: " ${BOARD} " (" ${VSL_LINK_VARIANT} ", " ${VSL_MEMORY_LDSCRIPT} ", debug log over CDC: " ${VSL_DEBUG_CDC} ", SWD on: " ${VSL_DEBUG_SWD} ")")

    #----------------------------------------------------------------------------------------------------#
    # Main program
    #----------------------------------------------------------------------------------------------------#
    add_executable(versaloon-next-gen.elf ${VSL_FIRMWARE_SOURCES})
    # The JTAG and SWD engines are instantiated in these files (see
    # board/BoardJtag.h and board/BoardSwd.h). They use -O2 instead of
    # -Os. It comes after the -Os of COMMON_OPTIONS, so it wins.
    set_source_files_properties(src/board/BoardJtag.cpp src/board/BoardJtagFast.cpp src/board/BoardSwd.cpp
            PROPERTIES COMPILE_OPTIONS -O2)

    # Apply unified build configuration
    target_link_libraries(versaloon-next-gen.elf PUBLIC art-build-config)
    # Per-target map file name
    target_link_options(versaloon-next-gen.elf PUBLIC -Wl,-Map,versaloon-next-gen.map)
    # Relink when a linker script changes
    set_target_properties(versaloon-next-gen.elf PROPERTIES LINK_DEPENDS
            "${PROJECT_SOURCE_DIR}/ldscripts/${VSL_MEMORY_LDSCRIPT};${PROJECT_SOURCE_DIR}/ldscripts/libs.ld;${PROJECT_SOURCE_DIR}/ldscripts/sections.ld")
    target_include_directories(versaloon-next-gen.elf PUBLIC ${PROJECT_INCLUDE_DIRS})
    target_link_libraries(versaloon-next-gen.elf PUBLIC art::core art::stm32 system-stm32)

    ADD_DUMP_TARGET(versaloon-next-gen.elf)
    PRINT_SIZE_OF_TARGETS(versaloon-next-gen.elf)

    set(HEX_FILE versaloon-next-gen-${BOARD}${VSL_IMAGE_TAG}-${VSL_LINK_VARIANT}-${CMAKE_BUILD_TYPE}-${FIRMWARE_VERSION}.hex)
    set(BIN_FILE versaloon-next-gen-${BOARD}${VSL_IMAGE_TAG}-${VSL_LINK_VARIANT}-${CMAKE_BUILD_TYPE}-${FIRMWARE_VERSION}.bin)

    add_custom_command(TARGET versaloon-next-gen.elf POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -Oihex $<TARGET_FILE:versaloon-next-gen.elf> versaloon-next-gen.hex
            COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:versaloon-next-gen.elf> versaloon-next-gen.bin
            COMMAND ${CMAKE_COMMAND} -E copy versaloon-next-gen.hex ${HEX_FILE}
            COMMAND ${CMAKE_COMMAND} -E copy versaloon-next-gen.bin ${BIN_FILE}
            COMMENT "Building ${HEX_FILE} \nBuilding ${BIN_FILE}")

    include(Bootloader)
    if (VSL_BUILD_BOOTLOADER AND VSL_BOOTLOADER AND BOARD IN_LIST VSL_BOOTLOADER_BOARDS)
        vsl_add_bootloader()
        MESSAGE(STATUS "Building versaloon-bootloader for board " ${BOARD} " into " ${VSL_BOOTLOADER_DIR})
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
