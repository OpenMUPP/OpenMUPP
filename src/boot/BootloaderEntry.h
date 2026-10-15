#pragma once

// Entry into versaloon-bootloader from the running firmware
// (VERSALOON_FW_UPDATE). Hardware-independent: the host unit tests check it.
//
// The protocol calls request() after it has queued the reply. The main loop
// calls isDue() with the uptime. isDue() becomes true replyDelayMs after
// the first isDue() call that sees the request, so the host has time to read
// the reply. Then the main loop calls boot::enterBootloader()
// (boot/Bootloader.h), which writes bootRequest to BKP->DR1 and resets.
//
// Contract with versaloon-bootloader (tools/versaloon-bootloader, README
// "Start the bootloader from the application"): after a reset with
// bootRequest in BKP->DR1, the bootloader clears the register and stays in
// bootloader mode. The next reset starts the application again.

#include <cstdint>
#include <optional>

namespace boot {

// The value in BKP->DR1 that keeps the bootloader in bootloader mode.
// The bootloader defines the same value (boot::bootRequest in
// src/boot/BootDecision.h of versaloon-bootloader).
inline constexpr uint16_t bootRequest = 0xB007;

// Only the app-8k image enters the bootloader (hasBootloaderEntry in
// src/main.cpp). The other images refuse FW_UPDATE.

class BootloaderEntry {
   public:
   // Time for the host to read the reply of FW_UPDATE before the reset.
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
