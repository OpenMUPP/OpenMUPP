#pragma once

// The key gesture of a dual image: hold the key for holdMs to switch to
// the other personality. Hardware-independent: the host unit tests check
// it. The main loop gives the key state and the uptime, and requests the
// other personality (personality/PersonalitySwitch.h) when update()
// returns true. This is the moment that the key has been down long
// enough: the LEDs flash then, and the user can release the key.
//
// The reset waits for the release of the key (PersonalitySwitch::isDue()).
// The key at reset is the key of versaloon-bootloader: if the key is still
// down at the reset of the switch, the bootloader stays in bootloader
// mode, and the new personality does not start. The gesture counts only
// after the firmware saw the key released. Thus a key that is down at the
// start, or that sticks, does not switch the personality after each reset.

#include <cstdint>
#include <optional>

namespace personality {

class KeyGesture {
   public:
   static constexpr uint32_t holdMs = 3000;

   // pressed: the key state at the uptime nowMs. True once for each
   // press, when the key has been down for holdMs.
   [[nodiscard]] bool update(bool pressed, uint64_t nowMs) {
      if(pressed) {
         if(!armed_) {
            return false;
         }
         if(!pressedSinceMs_) {
            pressedSinceMs_ = nowMs;
         }
         if(!fired_ && nowMs - *pressedSinceMs_ >= holdMs) {
            fired_ = true;
            return true;
         }
         return false;
      }
      armed_ = true;
      fired_ = false;
      pressedSinceMs_.reset();
      return false;
   }

   private:
   bool armed_ = false;
   bool fired_ = false;
   std::optional<uint64_t> pressedSinceMs_;
};

} // namespace personality
