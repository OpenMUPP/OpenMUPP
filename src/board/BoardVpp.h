// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The VPP pins, the DAC and the VPP senses of board::current (the vpp
// block) as VppHardware (protocol/VppHardware.h). Target only. Only the
// CH32V307 has the DAC code: a board with a vpp block on another family
// does not build. The senses read with ADC1 of BoardPower.

#include "board/BoardPower.h"
#include "protocol/VppHardware.h"

namespace board {

class BoardVpp final : public protocol::VppHardware {
   public:
   explicit BoardVpp(const BoardPower &power);

   void setDac(uint16_t code) override;
   void dacOff() override;
   void setEnable(bool on) override;
   void setOn(bool on) override;
   void setPullDown(bool on) override;
   [[nodiscard]] std::optional<uint16_t> senseMillivolts() override;
   [[nodiscard]] std::optional<uint16_t> sense2Millivolts() override;

   private:
   [[nodiscard]] std::optional<uint16_t> measure(Pin pin) const;

   const BoardPower &power_;
};

} // namespace board
