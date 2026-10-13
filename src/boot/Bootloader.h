#pragma once

// Start of versaloon-bootloader from the firmware. Target only.
// boot/BootloaderEntry.h has the contract with the bootloader.

namespace boot {

// Writes bootRequest to BKP->DR1 and resets the MCU. The bootloader then
// stays in bootloader mode. Call it only in the app-8k image
// (hasBootloaderEntry in src/main.cpp).
[[noreturn]] void enterBootloader();

} // namespace boot
