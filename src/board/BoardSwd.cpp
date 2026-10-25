#include "board/BoardSwd.h"

// The SWD engine, compiled with -O2 (CMakeLists.txt).
template class swd::BitBangPort<board::SwdPins>;

namespace board {

namespace {

using hw::GpioPin;

// The output register first, so the pin starts at its level.
void configureOutput(Pin pin, bool level) {
   const GpioPin gpio = gpioPin(pin);
   gpio.write(level);
   gpio.configure(GpioPin::Mode::Output, GpioPin::OutputType::PushPull, GpioPin::Pull::None, GpioPin::Speed::VeryHigh);
}

} // namespace

void SwdPins::init() {
   const DebugPins &pins = current.debug;
   // An open SWDIO reads 1: no target gives the ACK 0b111.
   if(pins.tmsIn != pins.tmsOut) {
      gpioPin(pins.tmsIn).configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);
   }
   configureOutput(pins.tck, true);
   configureOutput(pins.tmsOut, true);
#if !defined(STM32F1)
   // The pull-up of swdioRelease() (board/BoardSwd.h). A pull-up on a
   // push-pull output has no effect.
   gpioPin(pins.tmsOut).configure(GpioPin::Mode::Output, GpioPin::OutputType::PushPull, GpioPin::Pull::Up, GpioPin::Speed::VeryHigh);
#endif
}

void SwdPins::fini() {
   const DebugPins &pins = current.debug;
   for(const Pin pin : { pins.tck, pins.tmsOut }) {
      gpioPin(pin).configure(GpioPin::Mode::Input);
   }
}

} // namespace board
