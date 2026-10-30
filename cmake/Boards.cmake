# The boards of the firmware. BOARD selects one board, and the board selects
# its MCU family. CMakeLists.txt includes this file before project(): the
# toolchain file (cmake/gcc_arm.cmake) takes the CPU flags from STM32_FAMILY.
#
# Each board has one list VSL_BOARD_<board>:
#   family    STM32_FAMILY of system-stm32. cmake/gcc_arm.cmake has the CPU
#             flags of each family.
#   device    STM32_DEVICE, the CMSIS device define
#   crystal   HSE crystal in Hz, 0 for a board without a crystal
#   ldscript  memory linker script in ldscripts/ of system-stm32
#   flash     flash size in bytes. The size limit of the application images
#             comes from it (CMakeLists.txt).
# The family and the crystal must be the same as in the board definition
# (src/board/boards/). src/board/BoardChecks.cpp checks this.
#
#   mini      Versaloon Mini
#   otheruse  OtherUse
#   stlinkv2  ST-Link/V2, pin map of the official design (compile-only target)
#   stlinkv2miniclone
#             ST-Link/V2 mini clone, USB stick dongle
#             (docs/stlinkv2-mini-clone.md)
#   weacth523 WeAct STM32H523 core board (STM32H523CC)
# The STM32F1 boards have an STM32F103x8 or an STM32F103xB. CMSIS has one
# define (STM32F103xB) for both parts. The firmware uses only the 64 KB of
# the STM32F103x8.
set(VSL_BOARDS mini otheruse stlinkv2 stlinkv2miniclone weacth523)
#                         family  device      crystal  ldscript      flash
set(VSL_BOARD_mini        STM32F1 STM32F103xB 12000000 mem-F103x8.ld 65536)
set(VSL_BOARD_otheruse    STM32F1 STM32F103xB 12000000 mem-F103x8.ld 65536)
set(VSL_BOARD_stlinkv2    STM32F1 STM32F103xB 8000000  mem-F103x8.ld 65536)
set(VSL_BOARD_stlinkv2miniclone STM32F1 STM32F103xB 8000000  mem-F103x8.ld 65536)
set(VSL_BOARD_weacth523   STM32H5 STM32H523xx 8000000  mem-H523xC.ld 262144)
# Boards that openmupp-bootloader supports (cmake/Bootloader.cmake)
set(VSL_BOOTLOADER_BOARDS mini otheruse stlinkv2 stlinkv2miniclone)
# Boards that shipped with the legacy Versaloon bootloader (10 KB). Only
# they get the app-10k image.
set(VSL_LEGACY_BOOTLOADER_BOARDS mini otheruse)
set(BOARD "mini" CACHE STRING "Board to build for: ${VSL_BOARDS}")
set_property(CACHE BOARD PROPERTY STRINGS ${VSL_BOARDS})

# The host test build (no toolchain file) does not use the board.
if (CMAKE_TOOLCHAIN_FILE AND NOT BOARD IN_LIST VSL_BOARDS)
    message(FATAL_ERROR "Unknown BOARD '${BOARD}'. Supported boards: ${VSL_BOARDS}")
endif ()
if (BOARD IN_LIST VSL_BOARDS)
    list(GET VSL_BOARD_${BOARD} 0 _vsl_family)
    list(GET VSL_BOARD_${BOARD} 1 _vsl_device)
    list(GET VSL_BOARD_${BOARD} 2 _vsl_crystal)
    list(GET VSL_BOARD_${BOARD} 3 VSL_LDSCRIPT)
    list(GET VSL_BOARD_${BOARD} 4 VSL_FLASH_SIZE)
    set(STM32_FAMILY "${_vsl_family}" CACHE STRING "STM32 family to build for" FORCE)
    set(STM32_DEVICE "${_vsl_device}" CACHE STRING "STM32 device define for CMSIS" FORCE)
    # system-stm32 gives HSE_VALUE to the CMSIS SystemCoreClockUpdate() and
    # the Rcc clock recipes. With a wrong value the core clock and the ADC
    # clock are computed wrong. Empty: system-stm32 uses its own default.
    if (_vsl_crystal EQUAL 0)
        set(_vsl_crystal "")
    endif ()
    set(SYSTEM_STM32_HSE_VALUE "${_vsl_crystal}" CACHE STRING "HSE crystal frequency in Hz" FORCE)
endif ()
