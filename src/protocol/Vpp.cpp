// SPDX-License-Identifier: GPL-3.0-or-later

#include "protocol/Vpp.h"

#include <algorithm>

namespace protocol {

Vpp::Vpp(VppHardware &hardware, Delay &delay, VppGate &gate)
   : hardware_(hardware)
   , delay_(delay)
   , gate_(gate) {
}

Vpp::Result Vpp::switchOn(uint16_t millivolts, uint16_t familyLimit) {
   const uint16_t limit = std::min(familyLimit, hardwareMaxMillivolts);
   if(millivolts < minMillivolts || millivolts > limit) {
      return Result::Refused;
   }
   const auto gateLimit = gate_.check();
   if(!gateLimit || millivolts > *gateLimit) {
      switchOff();
      return Result::Refused;
   }
   // From VPP to 0 V, never to the run level
   hardware_.setPullDown(true);
   hardware_.setOn(false);
   level_ = 0;
   if(!reachLevel(millivolts, std::min(limit, *gateLimit))) {
      switchOff();
      return Result::NoLevel;
   }
   // From 0 V to VPP (VPP_PD is high), with no step at the run level
   hardware_.setOn(true);
   delay_.microseconds(switchUs);
   hardware_.setPullDown(false);
   delay_.milliseconds(sense2SettleMs);
   const auto boost = hardware_.senseMillivolts();
   const auto pin = hardware_.sense2Millivolts();
   if(!boost || !pin || *pin + maxDropMillivolts < *boost) {
      switchOff();
      return Result::TargetFault;
   }
   level_ = millivolts;
   return Result::Ok;
}

bool Vpp::reachLevel(uint16_t millivolts, uint16_t limit) {
   const int32_t target = millivolts;
   const int32_t low = target - toleranceMillivolts;
   const int32_t high = std::min<int32_t>(target + toleranceMillivolts, limit);
   int32_t code = vppDacCode(millivolts);
   hardware_.setDac(static_cast<uint16_t>(code));
   hardware_.setEnable(true);
   std::optional<int32_t> last;
   unsigned steadyInWindow = 0;
   for(unsigned ms = 0; ms < levelTimeoutMs; ms += correctIntervalMs) {
      delay_.milliseconds(correctIntervalMs);
      const auto read = hardware_.senseMillivolts();
      if(!read) {
         return false;
      }
      const int32_t now = *read;
      const bool steady = last && now - *last <= steadyMillivolts && *last - now <= steadyMillivolts;
      last = now;
      if(now >= low && now <= high) {
         steadyInWindow = steady ? steadyInWindow + 1 : 0;
         if(steadyInWindow >= settleReads) {
            return true;
         }
         continue;
      }
      steadyInWindow = 0;
      if(!steady) {
         continue;
      }
      // Half of the error, at least one step. A higher code gives a lower VPP.
      int32_t step = (now - target) * static_cast<int32_t>(vppDacPerMvNum) / static_cast<int32_t>(vppDacPerMvDen) / 2;
      if(step == 0) {
         step = now > target ? 1 : -1;
      }
      code = std::clamp<int32_t>(code + step, 0, 4095);
      hardware_.setDac(static_cast<uint16_t>(code));
   }
   return false;
}

void Vpp::switchOff() {
   hardware_.setPullDown(true);
   hardware_.setOn(false);
   hardware_.setEnable(false);
   hardware_.dacOff();
   level_ = 0;
}

void Vpp::poll() {
   if(isOn() && !gate_.isAllowed()) {
      switchOff();
   }
}

} // namespace protocol
