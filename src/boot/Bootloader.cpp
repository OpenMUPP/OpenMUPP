// SPDX-License-Identifier: GPL-3.0-or-later

#include "boot/Bootloader.h"

#include <Bkp.h>
#include <cmsis_device.h>

#include "boot/BootloaderEntry.h"

[[noreturn]] void boot::enterBootloader() {
   // PWR and BKP clocks on, PWR_CR.DBP set: BKP->DR1 is writable.
   hw::Bkp::enable();
   hw::Bkp::writeDR1(bootRequest);
   // The reset puts the USB peripheral and the D+ pull-up pin into their
   // reset state, so the host sees a disconnect.
   NVIC_SystemReset();
}
