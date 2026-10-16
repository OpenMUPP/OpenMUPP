#pragma once

// Maps the bits of the USB_TO_GPIO block (protocol/versaloon/GpioBlock.h) to the
// debug pins of a board definition. Hardware-independent: the host unit
// tests check it for every board.
//
// TMS has an output pin and an input pin, which can be different pins (the
// Mini: PB4 out, PB6 in). The output pin drives TMS; the input pin reads it.

#include <array>
#include <cstdint>
#include <optional>

#include "board/BoardDefinition.h"
#include "protocol/versaloon/GpioBlock.h"

namespace board {

// The pin that drives the bit (output, and the output register), if the
// board has it.
constexpr std::optional<Pin> gpioOutputPin(const DebugPins &pins, uint16_t bit) {
   namespace gpio = protocol::gpio;
   switch(bit) {
   case gpio::srst:
      return pins.srst;
   case gpio::trst:
      return pins.trst;
   case gpio::tck:
      return pins.tck;
   case gpio::tdo:
      return pins.tdo;
   case gpio::tdi:
      return pins.tdi;
   case gpio::rtck:
      return pins.rtck;
   case gpio::tms:
      return pins.tmsOut;
   default:
      // USR1 and USR2: no board has them.
      return std::nullopt;
   }
}

// The pin that reads the bit, if the board has it.
constexpr std::optional<Pin> gpioInputPin(const DebugPins &pins, uint16_t bit) {
   if(bit == protocol::gpio::tms) {
      return pins.tmsIn;
   }
   return gpioOutputPin(pins, bit);
}

// The bits of the pins that the board has.
constexpr uint16_t gpioAvailable(const DebugPins &pins) {
   uint16_t mask = 0;
   for(uint16_t bit = 1; bit <= protocol::gpio::tms; bit = static_cast<uint16_t>(bit << 1)) {
      if(gpioOutputPin(pins, bit)) {
         mask = static_cast<uint16_t>(mask | bit);
      }
   }
   return mask;
}

// One pin and its mode
struct GpioPinSetting {
   Pin pin;
   protocol::PinMode mode;

   constexpr bool operator==(const GpioPinSetting &) const = default;
};

// The pins that CONFIG of one bit changes, in this order. Empty when the
// board does not have the pin. A TMS output sets the output pin. A TMS
// input with two pins sets the input pin to the mode and releases the
// output pin (floating input).
constexpr std::array<std::optional<GpioPinSetting>, 2> gpioPinSettings(const DebugPins &pins, uint16_t bit, protocol::PinMode mode) {
   const auto out = gpioOutputPin(pins, bit);
   if(!out) {
      return {};
   }
   const auto in = gpioInputPin(pins, bit);
   const bool output = mode == protocol::PinMode::PushPull || mode == protocol::PinMode::OpenDrain;
   if(output || !in || *in == *out) {
      return { GpioPinSetting{ *out, mode }, std::nullopt };
   }
   return { GpioPinSetting{ *in, mode }, GpioPinSetting{ *out, protocol::PinMode::Input } };
}

} // namespace board
