#include "board/BoardRvSwd.h"

// The RISC-V debug wire engine, compiled with -O2 (CMakeLists.txt).
template class rvswd::RvSwdEngine<board::RvSwdPins>;

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

void RvSwdPins::init() {
   const DebugPins &pins = current.debug;
   // An open SWDIO reads 1.
   if(pins.tmsIn != pins.tmsOut) {
      gpioPin(pins.tmsIn).configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);
   }
   configureOutput(pins.tck, false);
   configureOutput(pins.tmsOut, true);
}

} // namespace board
