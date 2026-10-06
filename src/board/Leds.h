#pragma once

// The LEDs of board::current. A board can have fewer LEDs: the functions do
// nothing for an LED that the board does not have. Target only.

#include "board/Board.h"
#include "board/BoardPins.h"

#include <optional>

namespace board {

enum class Led : uint8_t { Power, State, Usb };

constexpr std::optional<Signal> ledSignal(Led led) {
   switch(led) {
   case Led::Power:
      return current.leds.power;
   case Led::State:
      return current.leds.state;
   case Led::Usb:
      return current.leds.usb;
   }
   return std::nullopt;
}

// Makes all LED pins outputs, with the LEDs off.
inline void initializeLeds() {
   for(Led led : { Led::Power, Led::State, Led::Usb }) {
      if(const auto signal = ledSignal(led)) {
         configureOutput(*signal, false);
      }
   }
}

inline void setLed(Led led, bool on) {
   if(const auto signal = ledSignal(led)) {
      write(*signal, on);
   }
}

} // namespace board
