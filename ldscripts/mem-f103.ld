/*
 * Memory Spaces Definitions for STM32F103x8/xB (Versaloon Mini, OtherUse,
 * ST-Link/V2).
 *
 * Application without a bootloader, at the start of FLASH. Use it for
 * development with a debug probe. The release image must still fit in the
 * 54 KB of mem-f103-bootloader.ld; CI checks that limit.
 *
 * __vsl_settings_size (CMake: -Wl,--defsym) keeps the last bytes of the
 * flash out of the FLASH region. A dual image (VSL_PROTOCOL=dual) keeps one
 * 1 KB page there for the stored personality, at __vsl_settings_start
 * (src/board/SettingsFlashPage.h). The other images keep 0 bytes.
 *
 * RAM: 20 KB at 0x20000000. The first 0x100 bytes of RAM are kept free for
 * the copy of the vector table that Interrupt::connectHandler() of
 * system-stm32 makes at SRAM_BASE. The table has 59 entries (236 bytes).
 * VTOR needs the table aligned to the next power of two of its size, which
 * is 256 bytes.
 *
 * The values below can be addressed in further linker scripts
 * using functions like 'ORIGIN(RAM)' or 'LENGTH(RAM)'.
 */

MEMORY
{
  RAM (xrw) : ORIGIN = 0x20000100, LENGTH = 20K-0x100
  CCMRAM (xrw) : ORIGIN = 0x00000000, LENGTH = 0
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 64K - __vsl_settings_size
  EEPROM (rx) : ORIGIN = 0x00000000, LENGTH = 0
  FLASHB1 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB0 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB1 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB2 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB3 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  MEMORY_ARRAY (xrw)  : ORIGIN = 0x00000000, LENGTH = 0
}

__vsl_settings_start = ORIGIN(FLASH) + LENGTH(FLASH);
