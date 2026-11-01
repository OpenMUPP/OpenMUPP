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

void TargetPower::poll(uint64_t nowMs) {
   if(!on_ || !hardware_.hasSense() || nowMs - lastCheckMs_ < pollIntervalMs) {
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

TargetPower::Result TargetPower::switchOn() {
   if(!hardware_.hasSwitch()) {
      return Result::NotSupported;
   }
   if(on_) {
      return Result::Ok;
   }
   if(!hardware_.hasSense()) {
      hardware_.setSwitch(true);
      on_ = true;
      return Result::Ok;
   }
   // The voltage at the Vref pin is the target supply, as long as the
   // switch is off.
   const auto external = isLive();
   if(!external || *external) {
      return Result::Refused;
   }
   hardware_.setSwitch(true);
   on_ = true;
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
   if(on_) {
      hardware_.setSwitch(false);
      on_ = false;
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
