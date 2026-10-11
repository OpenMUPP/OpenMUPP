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
//
// The key gesture asks for a longer delay, flashDelayMs. During that time
// indication() tells the main loop to flash all LEDs, so the user sees
// that the switch starts. The key is already released at that time.

#include <cstdint>
#include <optional>

#include "personality/Personality.h"

namespace personality {

class PersonalitySwitch {
   public:
   // Time for the host to read the reply of the command before the reset.
   static constexpr uint32_t replyDelayMs = 10;
   // Delay of the key gesture: three flashes of all LEDs
   static constexpr uint32_t flashPeriodMs = 200;
   static constexpr uint32_t flashDelayMs = 3 * flashPeriodMs;

   // active: the personality of this start
   explicit constexpr PersonalitySwitch(Personality active)
      : active_(active) {
   }

   [[nodiscard]] Personality active() const {
      return active_;
   }

   // Requests a personality. A request for the active personality cancels
   // a pending request, so the firmware does not reset. A second request
   // for the same personality does not move the time. delayMs: the time
   // from the first isDue() call to the reset. It must be at least
   // replyDelayMs. A delay above replyDelayMs gives an indication().
   void request(Personality personality, uint32_t delayMs = replyDelayMs) {
      if(personality == active_) {
         requested_.reset();
         dueMs_.reset();
         return;
      }
      requested_ = personality;
      if(!dueMs_) {
         delayMs_ = delayMs;
      }
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
         dueMs_ = nowMs + delayMs_;
      }
      return nowMs >= *dueMs_;
   }

   // The state of the indication LEDs at the uptime nowMs: on or off. No
   // value when the main loop must not override the LEDs. Call it after
   // isDue(). The flashes start on, with one flash each flashPeriodMs.
   [[nodiscard]] std::optional<bool> indication(uint64_t nowMs) const {
      if(!requested_ || !dueMs_ || delayMs_ <= replyDelayMs || nowMs >= *dueMs_) {
         return std::nullopt;
      }
      const uint64_t elapsed = nowMs - (*dueMs_ - delayMs_);
      return elapsed % flashPeriodMs < flashPeriodMs / 2;
   }

   private:
   Personality active_;
   std::optional<Personality> requested_;
   std::optional<uint64_t> dueMs_;
   uint32_t delayMs_{ replyDelayMs };
};

} // namespace personality
