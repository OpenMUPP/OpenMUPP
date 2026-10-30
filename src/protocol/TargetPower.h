#pragma once

// The 3.3 V target supply with its protections, for every personality that
// switches it: the Versaloon POWER block (protocol/versaloon/PowerBlock.h)
// and ID_DAP_Vendor2 of CMSIS-DAP (protocol/cmsisdap/DapProcessor.h).
// Hardware-independent: it runs on a PowerHardware object, and the host
// unit tests use a fake one.
//
// switchOn() refuses, and the supply stays off, when:
//
//   - the target voltage is above externalSupplyMillivolts: the target
//     has its own supply, and two supplies on one rail can do damage;
//   - the target voltage cannot be measured on a board with a sense;
//   - the voltage is not above that level settleMs after the switch-on:
//     the rail has a short circuit or an overload. The supply then goes
//     off again.
//
// A board without a Vref sense switches on without the checks above, and
// never switches off by itself.
//
// poll() switches the supply off when the voltage stays at or below
// externalSupplyMillivolts, as the original firmware does: the target
// pulls the rail down, or an external supply with a lower voltage
// back-feeds it.

#include <cstdint>
#include <optional>

#include "protocol/Delay.h"
#include "protocol/PowerHardware.h"

namespace protocol {

class TargetPower {
   public:
   // A target voltage above this level is a live supply
   static constexpr uint16_t externalSupplyMillivolts = 1800;
   // Time from the switch-on to the first check of the voltage
   static constexpr uint16_t settleMs = 10;
   // Time between two checks in poll()
   static constexpr uint32_t pollIntervalMs = 10;
   // Consecutive low checks in poll() that switch the supply off
   static constexpr unsigned lowChecksToOff = 3;

   enum class Result : uint8_t {
      Ok,
      // A protection refused the switch-on
      Refused,
      // The board has no power switch
      NotSupported,
   };

   TargetPower(PowerHardware &hardware, Delay &delay);

   [[nodiscard]] Result switchOn();
   void switchOff();

   // Call it in the main loop. nowMs is a millisecond counter. It checks
   // the voltage every pollIntervalMs while the supply is on.
   void poll(uint64_t nowMs);

   // True when the supply is on.
   [[nodiscard]] bool isOn() const {
      return on_;
   }

   // The target voltage in mV, 0 without a sense or when it fails.
   [[nodiscard]] uint16_t millivolts();

   [[nodiscard]] PowerHardware &hardware() {
      return hardware_;
   }

   private:
   // True when the voltage of the sense is above externalSupplyMillivolts.
   // No value when the measurement fails.
   [[nodiscard]] std::optional<bool> isLive();

   PowerHardware &hardware_;
   Delay &delay_;
   bool on_{ false };
   unsigned lowChecks_{ 0 };
   uint64_t lastCheckMs_{ 0 };
};

} // namespace protocol
