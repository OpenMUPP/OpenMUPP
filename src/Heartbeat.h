#pragma once

// Heartbeat LED pattern: a short flash once per period. The state LED
// shows it for the clock fault; the activity LED (ActivityLed.h) shows the
// normal state. The pattern is a function of the uptime only,
// so it has no state and no counter that can wrap.
// Hardware-independent: main() gives the uptime and drives the LED.

#include <cstdint>

#include "IntDiv.h"

class Heartbeat {
   public:
   struct Pattern {
      // 16 bits: intdiv::mod() needs no 64-bit division
      uint16_t periodMs;
      uint32_t onMs;
   };

   // The crystal or the PLL did not start: the system clock runs from HSI
   // (8 MHz), and USB cannot work. The LED flashes fast.
   static constexpr Pattern clockFault{ .periodMs = 200, .onMs = 100 };

   explicit constexpr Heartbeat(Pattern pattern)
      : pattern(pattern) {
   }

   // True when the LED is on at the uptime nowMs.
   [[nodiscard]] constexpr bool isOn(uint64_t nowMs) const {
      return intdiv::mod(nowMs, pattern.periodMs) < pattern.onMs;
   }

   private:
   Pattern pattern;
};
