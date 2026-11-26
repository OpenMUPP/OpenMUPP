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

// The code in the switch request on EP0 (usb/SwitchRequest.h) that resets
// the probe. The probe starts again with its stored personality. Only the
// switch request takes it: SWITCH_PERSONALITY and ID_DAP_Vendor0 refuse it.
inline constexpr uint8_t resetCode = 0xB1;
static_assert(resetCode >= personality::maxPersonalities && resetCode != bootloaderCode);

// A request that the main loop runs replyDelayMs after it first sees it:
// the bootloader entry, and the reset of the switch request on EP0
// (usb/SwitchRequest.h).
class DelayedRequest {
   public:
   // Time for the host to read the reply before the reset.
   static constexpr uint32_t replyDelayMs = 10;

   // A second request does not move the time.
   void request() {
      requested_ = true;
   }

   [[nodiscard]] bool isRequested() const {
      return requested_;
   }

   // The key must stay up for this time before the reset, as for the
   // personality switch (PersonalitySwitch::releaseDebounceMs): a bounce
   // must not look like a key that is down at the reset.
   static constexpr uint32_t releaseDebounceMs = 50;

   // True when the main loop must run the request at the uptime nowMs.
   // keyDown: the key is down. When the key was down after the request, the
   // key must stay up for releaseDebounceMs, because a key that is down at
   // the reset starts the bootloader.
   [[nodiscard]] bool isDue(uint64_t nowMs, bool keyDown = false) {
      if(!requested_) {
         return false;
      }
      if(!dueMs_) {
         dueMs_ = nowMs + replyDelayMs;
      }
      if(keyDown) {
         keyUpSinceMs_.reset();
         keyWasDown_ = true;
         return false;
      }
      if(keyWasDown_) {
         if(!keyUpSinceMs_) {
            keyUpSinceMs_ = nowMs;
         }
         if(nowMs - *keyUpSinceMs_ < releaseDebounceMs) {
            return false;
         }
      }
      return nowMs >= *dueMs_;
   }

   private:
   bool requested_ = false;
   std::optional<uint64_t> dueMs_;
   bool keyWasDown_ = false;
   std::optional<uint64_t> keyUpSinceMs_;
};

class BootloaderEntry final : public DelayedRequest {};

} // namespace boot
