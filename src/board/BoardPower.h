#pragma once

// The target power switch and the Vref sense of board::current as
// PowerHardware (protocol/PowerHardware.h). Target only.
//
// The switch pin floats when the supply is off, and drives the active level
// of PowerSwitch::on when it is on. The sense reads the Vref pin with ADC1.
// A board without a switch or without a sense reports that to the block.

#include <optional>

#include "protocol/PowerHardware.h"

namespace board {

class BoardPower final : public protocol::PowerHardware {
   public:
   // Floats the switch pin (supply off), and starts and calibrates ADC1 for
   // the Vref pin. Call it once before the first use.
   void init();

   [[nodiscard]] bool hasSwitch() const override;
   void setSwitch(bool on) override;
   [[nodiscard]] bool hasSense() const override;
   [[nodiscard]] std::optional<uint16_t> senseMillivolts() override;

   private:
   bool adcReady_{ false };
};

} // namespace board
