// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The target power switches and the Vref sense of board::current as
// PowerHardware (protocol/PowerHardware.h). Target only.
//
// A board with a PowerSwitch has 3.3 V only: the switch pin floats when the
// supply is off, and drives the active level of PowerSwitch::on when it is
// on. A board with a TargetSupply has a 3.3 V and a 5 V switch: one enable
// pin is high, or both are low. A falling edge on a fault input switches
// both off in the interrupt (CH32V307 only). The senses (Vref, 5 V input)
// read with ADC1. A board without a switch or without a sense reports that
// to TargetPower.

#include <optional>

#include "board/BoardDefinition.h"
#include "protocol/PowerHardware.h"

namespace board {

class BoardPower final : public protocol::PowerHardware {
   public:
   // Floats the switch pin (supply off), and starts and calibrates ADC1 for
   // the Vref pin. Call it once before the first use.
   void init();

   [[nodiscard]] bool hasSwitch() const override;
   [[nodiscard]] bool has5vSwitch() const override;
   void select(protocol::Supply supply) override;
   [[nodiscard]] bool hasSense() const override;
   [[nodiscard]] std::optional<uint16_t> senseMillivolts() override;
   [[nodiscard]] std::optional<uint16_t> fiveVoltInputMillivolts() override;
   [[nodiscard]] bool takeFault() override;

   private:
   // The mean of some conversions of the sense input, in mV at the divider
   // input
   [[nodiscard]] std::optional<uint16_t> measure(const VrefSense &sense) const;

   bool adcReady_{ false };
};

} // namespace board
