// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Activity LED pattern: a flash for the commands of the host, and off when
// the dongle is idle. Each flash is onMs long. After a flash the LED stays
// off for at least offMs, so a continuous command stream shows as a steady
// flicker and not as a LED that is on all the time.
// Hardware-independent: main() gives the uptime and a counter of the
// commands, and drives the LED.

#include <cstdint>

class ActivityLed {
   public:
   static constexpr uint32_t onMs = 40;
   static constexpr uint32_t offMs = 40;

   // Call it in each main loop iteration. activityCount increases for each
   // command. It can wrap: only a difference to the last value counts.
   void update(uint64_t nowMs, uint32_t activityCount) {
      if(activityCount != lastCount) {
         lastCount = activityCount;
         pending = true;
      }
      // A flash ends, and the off time ends offMs later.
      if(flashing && nowMs >= flashEndMs) {
         flashing = false;
      }
      if(!flashing && pending && nowMs >= nextFlashMs) {
         flashing = true;
         pending = false;
         flashEndMs = nowMs + onMs;
         nextFlashMs = flashEndMs + offMs;
      }
   }

   // True when the LED is on, as of the last update().
   [[nodiscard]] constexpr bool isOn() const {
      return flashing;
   }

   private:
   uint32_t lastCount{ 0 };
   uint64_t flashEndMs{ 0 };
   uint64_t nextFlashMs{ 0 };
   bool flashing{ false };
   bool pending{ false };
};
