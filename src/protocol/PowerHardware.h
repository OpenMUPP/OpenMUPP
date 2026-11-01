// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The target power switch and the target voltage sense: the interface of
// the board power (board/BoardPower.h). The Versaloon power block
// (protocol/versaloon/PowerBlock.h) uses it.

#include <cstdint>
#include <optional>

namespace protocol {

// The power switch and the voltage sense of the board.
class PowerHardware {
   public:
   [[nodiscard]] virtual bool hasSwitch() const = 0;
   // Switches the 3.3 V supply of the target on or off. Only for a board
   // with a switch.
   virtual void setSwitch(bool on) = 0;

   [[nodiscard]] virtual bool hasSense() const = 0;
   // Measures the target voltage in mV. No value when the measurement
   // fails. Only for a board with a sense.
   [[nodiscard]] virtual std::optional<uint16_t> senseMillivolts() = 0;

   protected:
   ~PowerHardware() = default;
};

} // namespace protocol
