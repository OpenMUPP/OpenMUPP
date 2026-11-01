// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The SWD pins of board::current for the bit-bang engine
// (swd/SwdBitBang.h). Target only. SWCLK is the TCK pin. SWDIO is driven
// on the TMS output pin and read on the TMS input pin; they can be one pin
// (see board::DebugPins). Each pin access is one store to BSRR or one load
// from IDR, and a direction change is four bit-band stores (hw::FastPin).

#include <cstdint>

#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/CycleCounter.h"
#include "swd/SwdBitBang.h"

namespace board {

class SwdPins {
   public:
   void init();
   void fini();

   [[gnu::always_inline]] void begin() {
      startCycleCounter();
   }

   [[gnu::always_inline]] void swclk(bool level) {
      Swclk::write(level);
   }
   // The output register first, so the pin starts at its level.
   [[gnu::always_inline]] void swdioDrive(bool level) {
      SwdioOut::write(level);
      SwdioOut::setOutput();
   }
   // Input first, then the pull-up: SWDIO does not glitch. On the STM32H5,
   // PUPDR keeps the pull-up of init() in both directions, so only the mode
   // changes.
   [[gnu::always_inline]] void swdioRelease() {
#if defined(STM32F1)
      SwdioOut::setConfig<pullUpInput>();
      SwdioOut::set();
#else
      SwdioOut::setInput();
#endif
   }
   [[gnu::always_inline]] void swdioWrite(bool level) {
      SwdioOut::write(level);
   }
   [[gnu::always_inline]] bool swdio() {
      return SwdioIn::read();
   }
   [[gnu::always_inline]] uint32_t now() {
      return cycleCount();
   }
   [[gnu::always_inline]] uint32_t coreHz() {
      return SystemCoreClock;
   }

   private:
#if defined(STM32F1)
   // Input with pull: ODR high selects the pull-up.
   static constexpr uint8_t pullUpInput =
   hw::gpio_f1::pinConfig(hw::GpioPin::Mode::Input, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::Up);
#endif

   using Swclk = FastPinOf<current.debug.tck>;
   using SwdioOut = FastPinOf<current.debug.tmsOut>;
   using SwdioIn = FastPinOf<current.debug.tmsIn>;
};

using BoardSwd = swd::BitBangPort<SwdPins>;

} // namespace board

// BoardSwd.cpp instantiates the engine, with -O2 (CMakeLists.txt).
extern template class swd::BitBangPort<board::SwdPins>;
