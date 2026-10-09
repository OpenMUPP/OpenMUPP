#include "board/BoardJtag.h"

// The JTAG engines, compiled with -O2 (CMakeLists.txt).
template class jtag::BitBangPort<board::JtagPins>;
template class jtag::BitBangPort<board::JtagPins, jtag::TckIdle::High>;
template class jtag::FastPort<board::JtagPins, board::JtagSpi>;

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

void JtagPins::init(bool tckLevel) {
   const DebugPins &pins = current.debug;
   // TMS and TDI high: extra TCK pulses keep the TAP in Test-Logic-Reset.
   // They go first: a rising edge on TCK then samples them high.
   configureOutput(pins.tmsOut, true);
   configureOutput(pins.tdi, true);
   configureOutput(pins.tck, tckLevel);
   // An open TDO reads 1: OpenOCD then sees no device instead of a chain
   // of zeros.
   gpioPin(pins.tdo).configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);
   // An open RTCK stays low: adaptive clocking then gives a timeout.
   if(pins.rtck) {
      gpioPin(*pins.rtck).configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Down);
   }
   if(pins.tmsIn != pins.tmsOut) {
      gpioPin(pins.tmsIn).configure(GpioPin::Mode::Input);
   }
}

void JtagPins::fini() {
   const DebugPins &pins = current.debug;
   for(const Pin pin : { pins.tck, pins.tmsOut, pins.tdi }) {
      gpioPin(pin).configure(GpioPin::Mode::Input);
   }
}

} // namespace board
