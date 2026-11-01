// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Start of the bootloader from the firmware. Target only.
// boot/BootloaderEntry.h has the contract with the bootloader.
//
// STM32F1 (boot/Bootloader.cpp): openmupp-bootloader, app-8k image only.
// STM32H5 (boot/RomBootloader.cpp): the USB DFU of the ROM bootloader in
// system memory (ST AN2606), standalone image.

namespace boot {

// Writes bootRequest to a backup register and resets the MCU: BKP->DR1 on
// the STM32F1, TAMP->BKP0R on the STM32H5. Call it only in an image with
// the bootloader entry (hasBootloaderEntry in src/main.cpp).
[[noreturn]] void enterBootloader();

#if defined(STM32H5)
// Jumps to the ROM bootloader when enterBootloader() requested it before
// the reset, and clears the request. Else it returns. Call it first in
// os_startup_initialize_hardware(): the jump needs the clocks, the PLL,
// the ICACHE and the interrupts in their reset state.
void startRomBootloaderOnRequest();
#endif

} // namespace boot
