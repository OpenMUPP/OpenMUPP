/*
 * Memory Spaces Definitions for STM32F103x8/xB (Versaloon Mini, OtherUse,
 * ST-Link/V2).
 *
 * The same script links all application images. CMake gives the start of
 * the application with -Wl,--defsym:
 *
 * __vsl_app_offset is the space in front of the application, from the start
 * of the 64 KB flash (CMakeLists.txt, VSL_APP_IMAGES):
 *   0      standalone: no bootloader, the application starts at 0x08000000
 *   8 KB   app-8k: behind versaloon-bootloader, at 0x08002000
 *   10 KB  app-10k: behind the legacy Versaloon bootloader, at 0x08002800
 * The FLASH region is the rest of the 64 KB of an STM32F103C8, so the link
 * fails when the application does not fit behind the bootloader.
 *
 * __vsl_flash_slack (CMake: -Wl,--defsym) makes the FLASH region larger.
 * With VSL_STRICT_SIZE=OFF it is 64 KB: an image that does not fit still
 * links, and the post-build step cmake/ImageSize.cmake skips it. With
 * VSL_STRICT_SIZE=ON it is 0, and the link fails.
 *
 * __vsl_settings_size (CMake: -Wl,--defsym) keeps the last bytes of the
 * flash out of the FLASH region. A multi image (more than one personality
 * in VSL_PERSONALITIES) keeps one 1 KB page there for the stored
 * personality, at __vsl_settings_start (src/board/SettingsFlashPage.h).
 * The other images keep 0 bytes.
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
  FLASH (rx) : ORIGIN = 0x08000000 + __vsl_app_offset, LENGTH = 64K - __vsl_app_offset - __vsl_settings_size + __vsl_flash_slack
  EEPROM (rx) : ORIGIN = 0x00000000, LENGTH = 0
  FLASHB1 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB0 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB1 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB2 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB3 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  MEMORY_ARRAY (xrw)  : ORIGIN = 0x00000000, LENGTH = 0
}

/* At the end of the 64 KB, also when __vsl_flash_slack is not 0 */
__vsl_settings_start = 0x08000000 + 64K - __vsl_settings_size;
