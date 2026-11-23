// SPDX-License-Identifier: GPL-3.0-or-later

#include "protocol/TargetPower.h"

namespace protocol {

TargetPower::TargetPower(PowerHardware &hardware, Delay &delay)
   : hardware_(hardware)
   , delay_(delay) {
}

uint16_t TargetPower::millivolts() {
   if(!hardware_.hasSense()) {
      return 0;
   }
   return hardware_.senseMillivolts().value_or(0);
}

bool TargetPower::takeFaultReport() {
   takeHardwareFault();
   const bool report = faultReport_;
   faultReport_ = false;
   return report;
}

void TargetPower::takeHardwareFault() {
   if(!hardware_.takeFault()) {
      return;
   }
   // The interrupt switched the hardware off already.
   supply_ = Supply::Off;
   ++faults_;
   faultReport_ = true;
}

void TargetPower::poll(uint64_t nowMs) {
   takeHardwareFault();
   if(!isOn() || !hardware_.hasSense() || nowMs - lastCheckMs_ < pollIntervalMs) {
      return;
   }
   lastCheckMs_ = nowMs;
   const auto live = isLive();
   // A failed measurement says nothing about the rail.
   if(!live) {
      return;
   }
   lowChecks_ = *live ? 0 : lowChecks_ + 1;
   if(lowChecks_ >= lowChecksToOff) {
      switchOff();
   }
}

TargetPower::Result TargetPower::switchOn(Supply supply) {
   if(supply == Supply::Off) {
      switchOff();
      return Result::Ok;
   }
   if(!hardware_.hasSwitch() || (supply == Supply::V5 && !hardware_.has5vSwitch())) {
      return Result::NotSupported;
   }
   takeHardwareFault();
   if(supply_ == supply) {
      return Result::Ok;
   }
   if(supply == Supply::V5) {
      const auto input = hardware_.fiveVoltInputMillivolts();
      if(!input || *input < min5vInputMillivolts) {
         return Result::Refused;
      }
   }
   if(!hardware_.hasSense()) {
      if(isOn()) {
         hardware_.select(Supply::Off);
         delay_.milliseconds(changeOffMs);
      }
      hardware_.select(supply);
      supply_ = supply;
      return Result::Ok;
   }
   if(isOn()) {
      // A change: the rail has the voltage of this supply, not of the target.
      const Supply from = supply_;
      switchOff();
      delay_.milliseconds(changeOffMs);
      if(from == Supply::V5 && !waitBelow3v3()) {
         return Result::Refused;
      }
   } else {
      // The voltage at the Vref pin is the target supply, as long as the
      // switches are off.
      const auto external = isLive();
      if(!external || *external) {
         return Result::Refused;
      }
   }
   hardware_.select(supply);
   supply_ = supply;
   lowChecks_ = 0;
   delay_.milliseconds(settleMs);
   const auto live = isLive();
   if(!live || !*live) {
      switchOff();
      return Result::Refused;
   }
   return Result::Ok;
}

void TargetPower::switchOff() {
   if(isOn()) {
      hardware_.select(Supply::Off);
      supply_ = Supply::Off;
   }
}

bool TargetPower::waitBelow3v3() {
   for(uint16_t waitedMs = 0;; ++waitedMs) {
      const auto value = hardware_.senseMillivolts();
      if(value && *value < max3v3Millivolts) {
         return true;
      }
      if(waitedMs >= dischargeTimeoutMs) {
         return false;
      }
      delay_.milliseconds(1);
   }
}

std::optional<bool> TargetPower::isLive() {
   const auto value = hardware_.senseMillivolts();
   if(!value) {
      return std::nullopt;
   }
   return *value > externalSupplyMillivolts;
}

} // namespace protocol
