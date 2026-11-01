// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The debug connector pins as GPIO pins: the interface of the board GPIO
// (board/BoardGpio.h). The Versaloon GPIO block
// (protocol/versaloon/GpioBlock.h) and the CMSIS-DAP processor use it. One
// bit addresses each pin. The bits are those of the Versaloon USB_TO_GPIO
// block.

#include <cstdint>

namespace protocol {

// The bit of each pin
namespace gpio {
inline constexpr uint16_t srst = 0x0001;
inline constexpr uint16_t trst = 0x0002;
inline constexpr uint16_t usr1 = 0x0004;
inline constexpr uint16_t usr2 = 0x0008;
inline constexpr uint16_t tck = 0x0010;
inline constexpr uint16_t tdo = 0x0020;
inline constexpr uint16_t tdi = 0x0040;
inline constexpr uint16_t rtck = 0x0080;
inline constexpr uint16_t tms = 0x0100;

inline constexpr uint16_t all = 0x01FF;
// The pins that can only be inputs
inline constexpr uint16_t inputOnly = tdo | rtck;
} // namespace gpio

enum class PinMode : uint8_t {
   Input,
   InputPullUp,
   InputPullDown,
   PushPull,
   OpenDrain,
};

// The debug connector pins, addressed by one gpio bit each.
class GpioPins {
   public:
   // The gpio bits of the pins that the board has
   [[nodiscard]] virtual uint16_t available() const = 0;
   // Sets the mode of one pin (bit: one gpio bit of available()). For an
   // output, level is the output level; the pin does not glitch to the
   // other level.
   virtual void configure(uint16_t bit, PinMode mode, bool level) = 0;
   // Writes the output register of the pins of mask (bits of available()).
   virtual void write(uint16_t mask, uint16_t value) = 0;
   // Reads the level of the pins of mask (bits of available()).
   [[nodiscard]] virtual uint16_t read(uint16_t mask) const = 0;

   protected:
   ~GpioPins() = default;
};

} // namespace protocol
