# SPDX-License-Identifier: GPL-3.0-or-later
# The boards of the firmware. BOARD selects one board, and the board selects
# its MCU family. CMakeLists.txt includes this file before project(): the
# toolchain file takes the CPU flags from the family: cmake/gcc_arm.cmake
# from STM32_FAMILY, cmake/gcc_riscv.cmake from CH32_FAMILY.
#
# Each board has one list MUPP_BOARD_<board>:
#   family    STM32_FAMILY of system-stm32 (STM32F1, STM32H5), or CH32_FAMILY
#             of ch32v00x-system (CH32V30X). The toolchain file has the CPU
#             flags of each family.
#   device    STM32_DEVICE, the CMSIS device define, or CH32_DEVICE
#   crystal   HSE crystal in Hz, 0 for a board without a crystal
#   ldscript  memory linker script in ldscripts/ of system-stm32 or of
#             ch32v00x-system
#   flash     flash size in bytes. The size limit of the application images
#             comes from it (CMakeLists.txt).
# The family and the crystal must be the same as in the board definition
# (src/board/boards/). src/board/BoardChecks.cpp checks this.
#
#   mini      Versaloon Mini
#   otheruse  OtherUse
#   stlinkv21 ST-Link/V2-1 of the Nucleo-64 boards (not tested on hardware)
#   stlinkv2miniclone
#             ST-Link/V2 mini clone, USB stick dongle
#             (docs/stlinkv2-mini-clone.md)
#   weacth523 WeAct STM32H523 core board (STM32H523CC)
#   ydch32v307
#             YD-CH32V307VCT6 development board. Build only: the image is a
#             stub (src/ch32v30x/), the port is not complete.
# The STM32F1 boards have an STM32F103x8 or an STM32F103xB. CMSIS has one
# define (STM32F103xB) for both parts. The firmware uses only the 64 KB of
# the STM32F103x8.
set(MUPP_BOARDS mini otheruse stlinkv21 stlinkv2miniclone weacth523 ydch32v307)
#                         family  device      crystal  ldscript      flash
set(MUPP_BOARD_mini        STM32F1 STM32F103xB 12000000 mem-F103x8.ld 65536)
set(MUPP_BOARD_otheruse    STM32F1 STM32F103xB 12000000 mem-F103x8.ld 65536)
set(MUPP_BOARD_stlinkv21   STM32F1 STM32F103xB 8000000  mem-F103x8.ld 65536)
set(MUPP_BOARD_stlinkv2miniclone STM32F1 STM32F103xB 8000000  mem-F103x8.ld 65536)
set(MUPP_BOARD_weacth523   STM32H5 STM32H523xx 8000000  mem-H523xC.ld 262144)
set(MUPP_BOARD_ydch32v307  CH32V30X CH32V30x_D8C 8000000 mem-ch32v307.ld 262144)
# Boards that openmupp-bootloader supports (cmake/Bootloader.cmake)
set(MUPP_BOOTLOADER_BOARDS mini otheruse stlinkv21 stlinkv2miniclone)
# Boards that also ship with the 32 KB Versaloon bootloader: the newer
# Versaloon Mini, with an STM32F103 with 128 KB flash or more. They get the
# app-32k image. MUPP_BOOTLOADER_32K_<board> gives the memory linker script
# and the flash size in bytes of that part, because the board entry above
# uses only 64 KB. The flash size must be 128 KB or more.
set(MUPP_BOOTLOADER_32K_BOARDS mini)
set(MUPP_BOOTLOADER_32K_mini mem-F103xB.ld 131072)
set(BOARD "mini" CACHE STRING "Board to build for: ${MUPP_BOARDS}")
set_property(CACHE BOARD PROPERTY STRINGS ${MUPP_BOARDS})

# The host test build (no toolchain file) does not use the board.
if (CMAKE_TOOLCHAIN_FILE AND NOT BOARD IN_LIST MUPP_BOARDS)
    message(FATAL_ERROR "Unknown BOARD '${BOARD}'. Supported boards: ${MUPP_BOARDS}")
endif ()
if (BOARD IN_LIST MUPP_BOARDS)
    list(GET MUPP_BOARD_${BOARD} 0 _mupp_family)
    list(GET MUPP_BOARD_${BOARD} 1 _mupp_device)
    list(GET MUPP_BOARD_${BOARD} 2 _mupp_crystal)
    list(GET MUPP_BOARD_${BOARD} 3 MUPP_LDSCRIPT)
    list(GET MUPP_BOARD_${BOARD} 4 MUPP_FLASH_SIZE)
    # MUPP_MCU_VENDOR selects the system library: system-stm32 or ch32v00x-system.
    if (_mupp_family MATCHES "^CH32")
        set(MUPP_MCU_VENDOR CH32)
        set(_mupp_toolchain gcc_riscv.cmake)
        set(CH32_FAMILY "${_mupp_family}" CACHE STRING "CH32 family to build for" FORCE)
        set(CH32_DEVICE "${_mupp_device}" CACHE STRING "CH32 device macro" FORCE)
        if (_mupp_crystal EQUAL 0)
            set(_mupp_crystal "")
        endif ()
        set(CH32V00X_SYSTEM_HSE_VALUE "${_mupp_crystal}" CACHE STRING "HSE crystal frequency in Hz" FORCE)
    else ()
        set(MUPP_MCU_VENDOR STM32)
        set(_mupp_toolchain gcc_arm.cmake)
        set(STM32_FAMILY "${_mupp_family}" CACHE STRING "STM32 family to build for" FORCE)
        set(STM32_DEVICE "${_mupp_device}" CACHE STRING "STM32 device define for CMSIS" FORCE)
        # system-stm32 gives HSE_VALUE to the CMSIS SystemCoreClockUpdate() and
        # the Rcc clock recipes. With a wrong value the core clock and the ADC
        # clock are computed wrong. Empty: system-stm32 uses its own default.
        if (_mupp_crystal EQUAL 0)
            set(_mupp_crystal "")
        endif ()
        set(SYSTEM_STM32_HSE_VALUE "${_mupp_crystal}" CACHE STRING "HSE crystal frequency in Hz" FORCE)
    endif ()
    # A board needs the toolchain file of its architecture. Without this
    # check, the wrong one stops with an unclear error about the family.
    if (CMAKE_TOOLCHAIN_FILE)
        cmake_path(GET CMAKE_TOOLCHAIN_FILE FILENAME _mupp_toolchain_name)
        if (NOT _mupp_toolchain_name STREQUAL _mupp_toolchain)
            message(FATAL_ERROR "BOARD '${BOARD}' needs the toolchain file cmake/${_mupp_toolchain}, not ${_mupp_toolchain_name}")
        endif ()
    endif ()
endif ()
