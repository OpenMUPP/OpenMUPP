// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ID_DAP_Vendor5 of the CMSIS-DAP processor (DapProcessor.h): the VPP
// control (protocol/Vpp.h) for the hardware test.
//
// [0x85][op u8] -> [0x85][status u8][level mV u16][sense mV u16][sense2 mV
// u16]: op 0 off, 2 only read, and [0x85][1][mV u16] on at mV. The status
// is 0 (done), 1 (refused: the level, a limit or the gate, or the other
// processor owns the target pins), 2 (VPP_SENSE did not show the level),
// 3 (a short or an overload on the target pin), or 0xFF for another op.
// After status 2 and 3, VPP is off. level is the VPP level after the
// operation, 0 for off. sense and sense2 are VPP_SENSE (the boost output)
// and VPP_SENSE2 (the target pin), 0 when the measurement fails. Op 1 can
// take up to 2 s.

#include "protocol/Vpp.h"
#include "protocol/cmsisdap/DapProcessor.h"

namespace cmsisdap {

class DapVpp final : public VppCommand {
   public:
   explicit DapVpp(protocol::Vpp &vpp)
      : vpp_(vpp) {
   }

   std::size_t command(art::span<const uint8_t> fields, art::span<uint8_t> response, bool pinsFree) override;
   // Only a VPP that is on: a switch-off sets VPP_PD high, which holds
   // the target in reset.
   void hostGone() override {
      if(vpp_.isOn()) {
         vpp_.switchOff();
      }
   }

   private:
   protocol::Vpp &vpp_;
};

} // namespace cmsisdap
