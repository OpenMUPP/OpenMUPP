#pragma once

// The debug connector pins of board::current for the USB_TO_GPIO block
// (protocol/GpioBlock.h). Target only.

#include "protocol/GpioBlock.h"

namespace board {

class BoardGpio final : public protocol::GpioPins {
   public:
   [[nodiscard]] uint16_t available() const override;
   void configure(uint16_t bit, protocol::PinMode mode, bool level) override;
   void write(uint16_t mask, uint16_t value) override;
   [[nodiscard]] uint16_t read(uint16_t mask) const override;
};

} // namespace board
