#pragma once

// Heartbeat LED pattern: a short flash once per period, so the LED shows
// that the main loop runs. The pattern is a function of the uptime only,
// so it has no state and no counter that can wrap.
// Hardware-independent: main() gives the uptime and drives the LED.

#include <cstdint>

class Heartbeat {
   public:
   struct Pattern {
      uint32_t periodMs;
      uint32_t onMs;
   };

   // The system clock runs from the crystal at 72 MHz.
   static constexpr Pattern normal{ .periodMs = 1000, .onMs = 100 };
   // The crystal or the PLL did not start: the system clock runs from HSI
   // (8 MHz), and USB cannot work. The LED flashes fast.
   static constexpr Pattern clockFault{ .periodMs = 200, .onMs = 100 };

   explicit constexpr Heartbeat(Pattern pattern)
      : pattern(pattern) {
   }

   // True when the LED is on at the uptime nowMs.
   [[nodiscard]] constexpr bool isOn(uint64_t nowMs) const {
      return nowMs % pattern.periodMs < pattern.onMs;
   }

   private:
   Pattern pattern;
};
