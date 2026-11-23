// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The target supply with its protections, for every personality that
// switches it: the Versaloon POWER block (protocol/versaloon/PowerBlock.h),
// ID_DAP_Vendor2 of CMSIS-DAP (protocol/cmsisdap/DapProcessor.h) and the
// EDBG control scope (protocol/avr/EdbgTransport.h). Hardware-independent:
// it runs on a PowerHardware object, and the host unit tests use a fake one.
//
// switchOn() selects 3.3 V or 5 V. It refuses, and the supply stays as it
// was, when:
//
//   - the supply is off and the target voltage is above
//     externalSupplyMillivolts: the target has its own supply, and two
//     supplies on one rail can do damage;
//   - the supply is off and the target voltage cannot be measured on a
//     board with a sense;
//   - 5 V is selected and the input of the 5 V switch is below
//     min5vInputMillivolts, or cannot be measured: the USB supply is weak.
//
// A change between 3.3 V and 5 V uses the sequence of docs/boards.md
// ("Target power sequence"): both switches off, wait changeOffMs, for 5 V
// to 3.3 V also wait until the voltage is below max3v3Millivolts. It
// refuses, and the supply stays off, when the voltage does not drop in
// dischargeTimeoutMs.
//
// After the switch-on, the voltage must be above externalSupplyMillivolts
// settleMs later. Else the rail has a short circuit or an overload: the
// supply goes off again, and switchOn() refuses.
//
// A board without a Vref sense switches on without the voltage checks, and
// never switches off by itself.
//
// poll() switches the supply off when the voltage stays at or below
// externalSupplyMillivolts, as the original firmware does: the target
// pulls the rail down, or an external supply with a lower voltage
// back-feeds it. poll() and switchOn() also take a fault of the hardware:
// the fault interrupt switched the supply off, and TargetPower counts it.

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
   // Time with both switches off in a change of the voltage
   static constexpr uint16_t changeOffMs = 10;
   // In a change from 5 V to 3.3 V, the 3.3 V switch goes on only below
   // this voltage (docs/boards.md)
   static constexpr uint16_t max3v3Millivolts = 3400;
   // The longest wait for max3v3Millivolts. The Vref divider alone
   // (200 kOhm) discharges a target capacitance of some uF in this time.
   static constexpr uint16_t dischargeTimeoutMs = 500;
   // The lowest input of the 5 V switch for a switch-on. 4.4 V is the USB
   // minimum at a low-power device.
   static constexpr uint16_t min5vInputMillivolts = 4400;

   enum class Result : uint8_t {
      Ok,
      // A protection refused the switch-on
      Refused,
      // The board has no switch for this supply
      NotSupported,
   };

   TargetPower(PowerHardware &hardware, Delay &delay);

   // Switches the supply on at 3.3 V or 5 V, or changes the voltage.
   // Supply::Off is the same as switchOff().
   [[nodiscard]] Result switchOn(Supply supply = Supply::V3v3);
   void switchOff();

   // Call it in the main loop. nowMs is a millisecond counter. It checks
   // the voltage every pollIntervalMs while the supply is on.
   void poll(uint64_t nowMs);

   // True when the supply is on.
   [[nodiscard]] bool isOn() const {
      return supply_ != Supply::Off;
   }

   [[nodiscard]] Supply supply() const {
      return supply_;
   }

   // Number of faults that switched the supply off. It wraps at 2^32.
   [[nodiscard]] uint32_t faultCount() const {
      return faults_;
   }

   // True once after a fault, for the report to the host. The call clears
   // the report.
   [[nodiscard]] bool takeFaultReport();

   // The target voltage in mV, 0 without a sense or when it fails.
   [[nodiscard]] uint16_t millivolts();

   [[nodiscard]] PowerHardware &hardware() {
      return hardware_;
   }

   private:
   // True when the voltage of the sense is above externalSupplyMillivolts.
   // No value when the measurement fails.
   [[nodiscard]] std::optional<bool> isLive();
   // True when the voltage drops below max3v3Millivolts in
   // dischargeTimeoutMs.
   [[nodiscard]] bool waitBelow3v3();
   // Takes a fault of the hardware: the supply is then off.
   void takeHardwareFault();

   PowerHardware &hardware_;
   Delay &delay_;
   Supply supply_{ Supply::Off };
   unsigned lowChecks_{ 0 };
   uint64_t lastCheckMs_{ 0 };
   uint32_t faults_{ 0 };
   bool faultReport_{ false };
};

} // namespace protocol
