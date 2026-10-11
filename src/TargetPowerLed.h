#pragma once

// Target power LED: on when the Vref pin has a voltage, so the target has
// power (from the dongle or from its own supply). The LED has a
// hysteresis, so a voltage near the threshold does not make it flicker.
// A board without a Vref sense, or a failed measurement, keeps the last
// state. The state before the first measurement is on, as the power LED
// of the start-up code.
// Hardware-independent: main() gives the measured voltage and drives the LED.

#include <cstdint>
#include <optional>

class TargetPowerLed {
   public:
   // The same level as protocol::PowerBlock::externalSupplyMillivolts: a
   // voltage above it is a live supply.
   static constexpr uint16_t onAboveMillivolts = 1800;
   static constexpr uint16_t offBelowMillivolts = 1500;
   // Time between two measurements
   static constexpr uint32_t intervalMs = 100;

   // True when the LED must be measured at the uptime nowMs.
   [[nodiscard]] constexpr bool isDue(uint64_t nowMs) const {
      return !measured || nowMs >= lastMs + intervalMs;
   }

   // Gives the result of a measurement at nowMs. No value: it failed.
   constexpr void update(uint64_t nowMs, std::optional<uint16_t> millivolts) {
      measured = true;
      lastMs = nowMs;
      if(!millivolts) {
         return;
      }
      if(*millivolts > onAboveMillivolts) {
         on = true;
      } else if(*millivolts < offBelowMillivolts) {
         on = false;
      }
   }

   [[nodiscard]] constexpr bool isOn() const {
      return on;
   }

   private:
   uint64_t lastMs{ 0 };
   bool measured{ false };
   bool on{ true };
};
