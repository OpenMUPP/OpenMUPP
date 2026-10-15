#pragma once

// Probe error LED pattern: a double flash after a JTAG or SWD error. Each
// period of periodMs starts with two flashes of flashMs, with flashMs off
// between them. The pattern starts at the first error and stops holdMs
// after the last error. Each new error restarts the hold time, but not the
// pattern. While the pattern shows, it replaces the activity flashes
// (ActivityLed.h) on the state LED.
// Hardware-independent: main() gives the uptime and a counter of the
// errors, and drives the LED.

#include <cstdint>

class ErrorLed {
   public:
   static constexpr uint32_t flashMs = 60;
   static constexpr uint32_t periodMs = 600;
   static constexpr uint32_t holdMs = 2000;

   // Call it in each main loop iteration. errorCount increases for each
   // error. It can wrap: only a difference to the last value counts.
   void update(uint64_t nowMs, uint32_t errorCount) {
      if(errorCount != lastCount) {
         lastCount = errorCount;
         if(!active) {
            active = true;
            startMs = nowMs;
         }
         endMs = nowMs + holdMs;
      }
      if(active && nowMs >= endMs) {
         active = false;
      }
      const uint64_t phase = active ? (nowMs - startMs) % periodMs : periodMs;
      on = phase < flashMs || (phase >= 2 * flashMs && phase < 3 * flashMs);
   }

   // True while the pattern shows, as of the last update().
   [[nodiscard]] constexpr bool isActive() const {
      return active;
   }

   // True when the LED is on, as of the last update().
   [[nodiscard]] constexpr bool isOn() const {
      return on;
   }

   private:
   uint32_t lastCount{ 0 };
   uint64_t startMs{ 0 };
   uint64_t endMs{ 0 };
   bool active{ false };
   bool on{ false };
};
