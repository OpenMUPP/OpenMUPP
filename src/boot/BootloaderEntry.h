// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Entry into the bootloader from the running firmware: openmupp-bootloader
// on the STM32F1, the ROM bootloader (USB DFU) on the STM32H5.
// Hardware-independent: the host unit tests check it.
//
// These commands call request(): VERSALOON_FW_UPDATE, and the personality
// switch commands with bootloaderCode instead of a personality code
// (SWITCH_PERSONALITY, ID_DAP_Vendor0 and the switch request on EP0, see
// personality/PersonalitySwitch.h). The protocol calls request() after it has queued the reply. The main loop
// calls isDue() with the uptime. isDue() becomes true replyDelayMs after
// the first isDue() call that sees the request, so the host has time to read
// the reply. Then the main loop calls boot::enterBootloader()
// (boot/Bootloader.h), which writes bootRequest to a backup register and
// resets.
//
// Contract with openmupp-bootloader (dependencies/openmupp-bootloader, README
// "Start the bootloader from the application"): after a reset with
// bootRequest in BKP->DR1, the bootloader clears the register and stays in
// bootloader mode. The next reset starts the application again.
//
// On the STM32H5, the firmware itself reads bootRequest in TAMP->BKP0R
// after the reset, clears it and jumps to the ROM bootloader
// (boot::startRomBootloaderOnRequest()).

#include <cstdint>
#include <optional>

#include "personality/Personality.h"

namespace boot {

// The value in BKP->DR1 that keeps the bootloader in bootloader mode. The
// STM32H5 uses the same value in TAMP->BKP0R.
// The bootloader defines the same value (boot::bootRequest in
// src/boot/BootDecision.h of openmupp-bootloader).
inline constexpr uint16_t bootRequest = 0xB007;

// Only the app-8k image of the STM32F1 and the standalone image of the
// STM32H5 enter the bootloader (hasBootloaderEntry in src/main.cpp). The
// other images refuse FW_UPDATE and bootloaderCode.

// The code in the personality switch commands that starts the bootloader
// instead of a personality. It is outside the personality codes.
inline constexpr uint8_t bootloaderCode = 0xB0;
static_assert(bootloaderCode >= personality::maxPersonalities);

class BootloaderEntry {
   public:
   // Time for the host to read the reply before the reset.
   static constexpr uint32_t replyDelayMs = 10;

   // Requests the bootloader. A second request does not move the time.
   void request() {
      requested_ = true;
   }

   [[nodiscard]] bool isRequested() const {
      return requested_;
   }

   // True when the main loop must enter the bootloader at the uptime nowMs.
   [[nodiscard]] bool isDue(uint64_t nowMs) {
      if(!requested_) {
         return false;
      }
      if(!dueMs_) {
         dueMs_ = nowMs + replyDelayMs;
      }
      return nowMs >= *dueMs_;
   }

   private:
   bool requested_ = false;
   std::optional<uint64_t> dueMs_;
};

} // namespace boot
