#pragma once

// Maps board::Pin to the system-stm32 GPIO drivers. Target only: needs CMSIS.

#include "board/BoardDefinition.h"

#include <Afio.h>
#include <cmsis_device.h>
#include <cstdint>
#include <peripherals/FastPin.h>
#include <peripherals/GpioPin.h>

namespace board {

constexpr uintptr_t portBase(Port port) {
   switch(port) {
   case Port::A:
      return GPIOA_BASE;
   case Port::B:
      return GPIOB_BASE;
   case Port::C:
      return GPIOC_BASE;
   case Port::D:
      return GPIOD_BASE;
   }
   return GPIOA_BASE;
}

// Compile-time pin for the bit-bang engines, e.g. FastPinOf<current.debug.tck>.
template <Pin P>
using FastPinOf = hw::FastPin<portBase(P.port), P.number>;

inline hw::GpioPin gpioPin(Pin pin) {
   return { reinterpret_cast<GPIO_TypeDef *>(portBase(pin.port)), pin.number };
}

// Makes the pin of the signal a push-pull output in the given state. The
// output register is written first, so the pin does not glitch.
inline void configureOutput(Signal signal, bool asserted) {
   const hw::GpioPin pin = gpioPin(signal.pin);
   pin.write(signal.levelFor(asserted));
   pin.configure(hw::GpioPin::Mode::Output);
}

inline void write(Signal signal, bool asserted) {
   gpioPin(signal.pin).write(signal.levelFor(asserted));
}

constexpr hw::Afio::SwjConfig afioSwjConfig(SwjConfig config) {
   switch(config) {
   case SwjConfig::Full:
      return hw::Afio::SwjConfig::Full;
   case SwjConfig::NoJtag:
      return hw::Afio::SwjConfig::SwdOnly;
   case SwjConfig::Disabled:
      return hw::Afio::SwjConfig::Disabled;
   }
   return hw::Afio::SwjConfig::Disabled;
}

} // namespace board
