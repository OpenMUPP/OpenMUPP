#pragma once

// The switch to the other personality of a dual image. Hardware-
// independent: the host unit tests check it.
//
// The switch commands call request(): SWITCH_PERSONALITY of the Versaloon
// protocol (protocol/CommandProcessor.h) and ID_DAP_Vendor0 of CMSIS-DAP
// (protocol/cmsisdap/DapProcessor.h). The key gesture
// (personality/KeyGesture.h) requests the other personality. The command
// queues its reply before the request takes effect. The main loop calls
// isDue() with the uptime. isDue() becomes true replyDelayMs after the
// first isDue() call that sees the request, so the host has time to read
// the reply. Then the main loop stores the requested personality
// (personality/PersonalityStore.h) and resets. After the reset, the USB
// device enumerates with the descriptors of the new personality.

#include <cstdint>
#include <optional>

#include "personality/Personality.h"

namespace personality {

class PersonalitySwitch {
   public:
   // Time for the host to read the reply of the command before the reset.
   static constexpr uint32_t replyDelayMs = 10;

   // active: the personality of this start
   explicit constexpr PersonalitySwitch(Personality active)
      : active_(active) {
   }

   [[nodiscard]] Personality active() const {
      return active_;
   }

   // Requests a personality. A request for the active personality cancels
   // a pending request, so the firmware does not reset. A second request
   // for the same personality does not move the time.
   void request(Personality personality) {
      if(personality == active_) {
         requested_.reset();
         dueMs_.reset();
         return;
      }
      requested_ = personality;
   }

   // The requested personality, empty when there is no request.
   [[nodiscard]] std::optional<Personality> requested() const {
      return requested_;
   }

   // True when the main loop must store requested() and reset at the
   // uptime nowMs.
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
   Personality active_;
   std::optional<Personality> requested_;
   std::optional<uint64_t> dueMs_;
};

} // namespace personality
