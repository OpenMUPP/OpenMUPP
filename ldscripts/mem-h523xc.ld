/*
 * Memory Spaces Definitions for STM32H523xC (WeAct STM32H523 core board).
 * As mem-h533.ld, with half the flash.
 *
 * The board has only the standalone image (CMakeLists.txt): no bootloader,
 * __vsl_app_offset is 0. __vsl_flash_slack and __vsl_settings_size work as
 * in mem-f103.ld. A multi image keeps the last 8 KB sector of bank 2 for
 * the stored personality (src/board/SettingsFlashPage.h): __vsl_settings_size
 * is 8 KB. The other images keep 0 bytes.
 *
 * FLASH: 256 KB at 0x08000000 (two banks of 128 KB, 8 KB sectors).
 * RAM: SRAM1 (128 KB), SRAM2 (80 KB) and SRAM3 (64 KB) are contiguous at
 * 0x20000000, 272 KB in total. TrustZone is off, so the non-secure aliases
 * are used. The first 0x400 bytes of RAM are kept free for the copy of the
 * vector table that Interrupt::connectHandler() of system-stm32 makes at
 * SRAM_BASE. The table has 149 entries (596 bytes). VTOR needs the table
 * aligned to the next power of two of its size, which is 1024 bytes.
 */

MEMORY
{
  RAM (xrw) : ORIGIN = 0x20000400, LENGTH = 272K-0x400
  CCMRAM (xrw) : ORIGIN = 0x00000000, LENGTH = 0
  FLASH (rx) : ORIGIN = 0x08000000 + __vsl_app_offset, LENGTH = 256K - __vsl_app_offset - __vsl_settings_size + __vsl_flash_slack
  EEPROM (rx) : ORIGIN = 0x00000000, LENGTH = 0
  FLASHB1 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB0 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB1 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB2 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB3 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  MEMORY_ARRAY (xrw)  : ORIGIN = 0x00000000, LENGTH = 0
}

__vsl_settings_start = 0x08000000 + 256K - __vsl_settings_size;
