#pragma once

// Maps the bits of the USB_TO_GPIO block (protocol/GpioBlock.h) to the
// debug pins of a board definition. Hardware-independent: the host unit
// tests check it for every board.
//
// TMS has an output pin and an input pin, which can be different pins (the
// Mini: PB4 out, PB6 in). The output pin drives TMS; the input pin reads it.

#include <cstdint>
#include <optional>

#include "board/BoardDefinition.h"
#include "protocol/GpioBlock.h"

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

} // namespace board
