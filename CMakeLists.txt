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
set(BOARD "mini" CACHE STRING "Board to build for: ${VSL_BOARDS}")
set_property(CACHE BOARD PROPERTY STRINGS ${VSL_BOARDS})

# Link the application behind versaloon-bootloader (0x08002800, 54 KB) or
# at the start of FLASH (0x08000000) for development without a bootloader.
option(VSL_BOOTLOADER "Link the application behind versaloon-bootloader at 0x08002800" ON)

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

    # Selects board::current (src/board/Board.h), e.g. VSL_BOARD_MINI
    string(TOUPPER "${BOARD}" BOARD_UPPER)
    set(PROJECT_DEFINES VSL_BOARD_${BOARD_UPPER})
    set(CXX_OPTIONS -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wno-volatile)
    set(COMMON_OPTIONS -Os -ffunction-sections -fdata-sections -g -fno-common -fmessage-length=0 -fsigned-char -Werror=odr --specs=nano.specs --specs=nosys.specs)
    set(C_OPTIONS -std=gnu11)
    set(DEBUG_OPTIONS -Wall -Wextra -Og)
    set(LINKER_OPTIONS -Wl,-gc-sections -Wl,--no-warn-rwx-segments -L${CMAKE_CURRENT_SOURCE_DIR}/ldscripts -T${VSL_MEMORY_LDSCRIPT} -Tlibs.ld -Tsections.ld -nostartfiles --specs=nano.specs --specs=nosys.specs ${COMMON_OPTIONS})
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
    # libart-stm32 does not build for STM32F1 yet (embedded-libraries/libart-stm32#14).
    # Configure it, but keep it out of the default build and do not link it
    # until that issue is closed.
    if (NOT TARGET art::stm32)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/dependencies/libart-stm32" EXCLUDE_FROM_ALL)
    endif ()

    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/output_files.cmake")

    if (VSL_BOOTLOADER)
        set(VSL_LINK_VARIANT bootloader)
    else ()
        set(VSL_LINK_VARIANT direct)
    endif ()
    MESSAGE(STATUS "Building versaloon-next-gen version " ${FIRMWARE_VERSION} " (" ${CMAKE_BUILD_TYPE} ") for board: " ${BOARD} " (" ${VSL_LINK_VARIANT} ", " ${VSL_MEMORY_LDSCRIPT} ")")

    #----------------------------------------------------------------------------------------------------#
    # Main program
    #----------------------------------------------------------------------------------------------------#
    add_executable(versaloon-next-gen.elf ${PROJECT_SOURCES})

    # Apply unified build configuration
    target_link_libraries(versaloon-next-gen.elf PUBLIC art-build-config)
    # Per-target map file name
    target_link_options(versaloon-next-gen.elf PUBLIC -Wl,-Map,versaloon-next-gen.map)
    # Relink when a linker script changes
    set_target_properties(versaloon-next-gen.elf PROPERTIES LINK_DEPENDS
            "${PROJECT_SOURCE_DIR}/ldscripts/${VSL_MEMORY_LDSCRIPT};${PROJECT_SOURCE_DIR}/ldscripts/libs.ld;${PROJECT_SOURCE_DIR}/ldscripts/sections.ld")
    target_include_directories(versaloon-next-gen.elf PUBLIC ${PROJECT_INCLUDE_DIRS})
    target_link_libraries(versaloon-next-gen.elf PUBLIC art::core system-stm32)

    ADD_DUMP_TARGET(versaloon-next-gen.elf)
    PRINT_SIZE_OF_TARGETS(versaloon-next-gen.elf)

    set(HEX_FILE versaloon-next-gen-${BOARD}-${VSL_LINK_VARIANT}-${CMAKE_BUILD_TYPE}-${FIRMWARE_VERSION}.hex)
    set(BIN_FILE versaloon-next-gen-${BOARD}-${VSL_LINK_VARIANT}-${CMAKE_BUILD_TYPE}-${FIRMWARE_VERSION}.bin)

    add_custom_command(TARGET versaloon-next-gen.elf POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -Oihex $<TARGET_FILE:versaloon-next-gen.elf> versaloon-next-gen.hex
            COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:versaloon-next-gen.elf> versaloon-next-gen.bin
            COMMAND ${CMAKE_COMMAND} -E copy versaloon-next-gen.hex ${HEX_FILE}
            COMMAND ${CMAKE_COMMAND} -E copy versaloon-next-gen.bin ${BIN_FILE}
            COMMENT "Building ${HEX_FILE} \nBuilding ${BIN_FILE}")
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
