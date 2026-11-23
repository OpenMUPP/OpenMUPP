// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The target power switches and the target voltage sense: the interface of
// the board power (board/BoardPower.h). The target supply
// (protocol/TargetPower.h) uses it.

#include <cstdint>
#include <optional>

namespace protocol {

// The supply that the switches give to the target
enum class Supply : uint8_t { Off, V3v3, V5 };

// The power switches and the voltage sense of the board.
class PowerHardware {
   public:
   // True when the board has a 3.3 V switch.
   [[nodiscard]] virtual bool hasSwitch() const = 0;
   // True when the board also has a 5 V switch on the same rail.
   [[nodiscard]] virtual bool has5vSwitch() const = 0;
   // Switches one supply on, or all off. Only a supply that the board has.
   // The hardware does not do the change sequence: TargetPower does it.
   virtual void select(Supply supply) = 0;

   [[nodiscard]] virtual bool hasSense() const = 0;
   // Measures the target voltage in mV. No value when the measurement
   // fails. Only for a board with a sense.
   [[nodiscard]] virtual std::optional<uint16_t> senseMillivolts() = 0;

   // Measures the input of the 5 V switch in mV. No value when the
   // measurement fails. Only for a board with a 5 V switch.
   [[nodiscard]] virtual std::optional<uint16_t> fiveVoltInputMillivolts() = 0;

   // True once after a fault input of a switch switched all supplies off.
   // The call clears the fault.
   [[nodiscard]] virtual bool takeFault() = 0;

   protected:
   ~PowerHardware() = default;
};

} // namespace protocol
