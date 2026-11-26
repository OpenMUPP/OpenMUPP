// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The pins, the DAC and the senses of the VPP boost: the interface of the
// board VPP (board/BoardVpp.h). The VPP driver (protocol/Vpp.h) uses it,
// and the host unit tests use a fake one.

#include <cstdint>
#include <optional>

namespace protocol {

class VppHardware {
   public:
   // Sets the DAC output (0 to 4095, 0 V to VDDA). A higher value gives a
   // lower VPP.
   virtual void setDac(uint16_t code) = 0;
   // Turns the DAC off: its pin is high-impedance.
   virtual void dacOff() = 0;
   // VPP_EN: the boost
   virtual void setEnable(bool on) = 0;
   // VPP_ON: the output switch to the target pin
   virtual void setOn(bool on) = 0;
   // VPP_PD: high pulls the target pin to 0 V
   virtual void setPullDown(bool on) = 0;
   // VPP_SENSE in mV at the divider input: the boost output, before the
   // output switch. No value when the measurement fails.
   [[nodiscard]] virtual std::optional<uint16_t> senseMillivolts() = 0;
   // VPP_SENSE2 in mV at the divider input: the target pin
   [[nodiscard]] virtual std::optional<uint16_t> sense2Millivolts() = 0;

   protected:
   ~VppHardware() = default;
};

} // namespace protocol
