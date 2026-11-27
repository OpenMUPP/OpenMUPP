// SPDX-License-Identifier: GPL-3.0-or-later

#include "boot/Bootloader.h"

#include <Bkp.h>
#include <cmsis_device.h>

#include "boot/BootloaderEntry.h"

// openmupp-bootloader on the CH32V307 (OpenMUPP v1, app-8k image). The
// contract is the one of the STM32F1: bootRequest in the first backup data
// register (BKP->DATAR1).
[[noreturn]] void boot::enterBootloader() {
   hw::Bkp::enable();
   hw::Bkp::writeDR1(bootRequest);
   // The reset puts USBHS and its D+ pull-up into the reset state, so the
   // host sees a disconnect.
   NVIC_SystemReset();
   __builtin_unreachable();
}
