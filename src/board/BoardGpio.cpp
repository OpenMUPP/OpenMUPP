#include "board/BoardGpio.h"

#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/DebugGpio.h"

namespace board {

namespace {

using hw::GpioPin;
using protocol::PinMode;

constexpr uint16_t availableBits = gpioAvailable(current.debug);

void configurePin(Pin pin, PinMode mode, bool level) {
   const GpioPin gpio = gpioPin(pin);
   switch(mode) {
   case PinMode::Input:
      gpio.configure(GpioPin::Mode::Input);
      break;
   case PinMode::InputPullUp:
      gpio.configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);
      break;
   case PinMode::InputPullDown:
      gpio.configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Down);
      break;
   case PinMode::PushPull:
   case PinMode::OpenDrain:
      // The output register first, so the pin starts at its level. An
      // input becomes a push-pull output in setMode() and open-drain in
      // setOutputType() a few cycles later: an open-drain pin at the high
      // level drives high for these cycles.
      gpio.write(level);
      gpio.configure(GpioPin::Mode::Output,
                     mode == PinMode::OpenDrain ? GpioPin::OutputType::OpenDrain : GpioPin::OutputType::PushPull,
                     GpioPin::Pull::None, GpioPin::Speed::High);
      break;
   }
}

} // namespace

uint16_t BoardGpio::available() const {
   return availableBits;
}

void BoardGpio::configure(uint16_t bit, PinMode mode, bool level) {
   const auto out = gpioOutputPin(current.debug, bit);
   if(!out) {
      return;
   }
   const auto in = gpioInputPin(current.debug, bit);
   const bool output = mode == PinMode::PushPull || mode == PinMode::OpenDrain;
   if(in && *in != *out) {
      // TMS with two pins: the input pin stays an input. The output pin
      // releases the line when TMS is an input.
      configurePin(output ? *out : *in, mode, level);
      if(!output) {
         configurePin(*out, PinMode::Input, false);
      }
      return;
   }
   configurePin(*out, mode, level);
}

void BoardGpio::write(uint16_t mask, uint16_t value) {
   for(uint16_t bit = 1; bit <= protocol::gpio::tms; bit = static_cast<uint16_t>(bit << 1)) {
      if((mask & bit) == 0) {
         continue;
      }
      if(const auto pin = gpioOutputPin(current.debug, bit)) {
         gpioPin(*pin).write((value & bit) != 0);
      }
   }
}

uint16_t BoardGpio::read(uint16_t mask) const {
   uint16_t value = 0;
   for(uint16_t bit = 1; bit <= protocol::gpio::tms; bit = static_cast<uint16_t>(bit << 1)) {
      if((mask & bit) == 0) {
         continue;
      }
      const auto pin = gpioInputPin(current.debug, bit);
      if(pin && gpioPin(*pin).read()) {
         value = static_cast<uint16_t>(value | bit);
      }
   }
   return value;
}

} // namespace board
