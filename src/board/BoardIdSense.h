// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The target gate (protocol/TargetGate.h) of a board with an adapter ID and
// a board ID (board::IdSense, OpenMUPP v1). Target only.
//
// init() reads the board ID once: the board does not change. check() reads
// the adapter ID again at each connect, because a user can change the
// adapter while the probe runs. The gate refuses (decision D2 and the
// answers on issue #129):
//
// - a board ID that is not a known revision (board/IdSense.h), or an ADC
//   that fails;
// - an adapter fault: pin 16 shorted to GND, for example an adapter that is
//   rotated by 180 degrees.
//
// Else the protocol starts, also on a wrong adapter. The debug log then
// has one warning: a wrong adapter for the protocol, no adapter, or an
// unknown (free) slot. A refusal is an error in the log. The vendor
// string of DAP_Info (vendorText()) shows the adapter of the last reading.

#include "board/BoardPower.h"
#include "board/IdSense.h"
#include "protocol/TargetGate.h"

namespace board {

class BoardTargetGate final : public protocol::TargetGate {
   public:
   explicit BoardTargetGate(const BoardPower &power);

   // Reads the board ID and the adapter ID. Call it once, after
   // BoardPower::init().
   void init();

   bool check(protocol::TargetProtocol protocol) override;
   [[nodiscard]] bool isAllowed() const override {
      return allowed_;
   }

   // The last reading of the adapter ID
   [[nodiscard]] IdReading adapter() const {
      return adapter_;
   }

   // "OpenMUPP" with the adapter of the last reading. The text stays at
   // this address.
   [[nodiscard]] const char *vendorText() const {
      return vendorText_;
   }

   private:
   [[nodiscard]] bool readAdapter();

   const BoardPower &power_;
   bool boardKnown_{ false };
   bool allowed_{ false };
   IdReading adapter_{ IdKind::None, 0 };
   char vendorText_[32] = "OpenMUPP";
};

} // namespace board
