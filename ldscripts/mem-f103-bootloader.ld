/*
 * Memory Spaces Definitions for STM32F103x8/xB (Versaloon Mini, OtherUse,
 * ST-Link/V2).
 *
 * Application behind versaloon-bootloader. The bootloader uses the first
 * 10 KB of FLASH (0x08000000-0x080027FF). The application starts at
 * 0x08002800. The FLASH region is 54 KB (64 KB STM32F103C8 minus the
 * bootloader), so the link fails when the application does not fit.
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
  FLASH (rx) : ORIGIN = 0x08002800, LENGTH = 54K
  EEPROM (rx) : ORIGIN = 0x00000000, LENGTH = 0
  FLASHB1 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB0 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB1 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB2 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  EXTMEMB3 (rx) : ORIGIN = 0x00000000, LENGTH = 0
  MEMORY_ARRAY (xrw)  : ORIGIN = 0x00000000, LENGTH = 0
}
