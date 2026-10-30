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

// Makes all LED pins outputs, with the LEDs off. A two-colour state LED
// shows idle.
inline void initializeLeds() {
   for(Led led : { Led::Power, Led::State, Led::Usb }) {
      if(const auto signal = ledSignal(led)) {
         configureOutput(*signal, led == Led::State && current.leds.stateTwoColour);
      }
   }
}

inline void setLed(Led led, bool on) {
   if(const auto signal = ledSignal(led)) {
      write(*signal, on);
   }
}

// Drives the state LED (stateDriveOf()).
inline void showState(StateShow show) {
   if constexpr(current.leds.state.has_value()) {
      const StateDrive drive = stateDriveOf(*current.leds.state, current.leds.stateTwoColour, show);
      const hw::GpioPin pin = gpioPin(current.leds.state->pin);
      if constexpr(!current.leds.stateTwoColour) {
         pin.write(drive.level);
      } else {
         // Change the pin mode only on a change: the main loop calls this
         // each pass.
         static StateDrive shown{ false, current.leds.state->levelFor(true) }; // initializeLeds()
         if(drive != shown) {
            shown = drive;
            pin.write(drive.level);
            pin.configure(drive.off ? hw::GpioPin::Mode::Analog : hw::GpioPin::Mode::Output);
         }
      }
   }
}

} // namespace board
