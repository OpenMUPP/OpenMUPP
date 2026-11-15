// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The SWD pins of board::current for the bit-bang engine
// (swd/SwdBitBang.h). Target only. SWCLK is the TCK pin. SWDIO is driven
// on the TMS output pin and read on the TMS input pin; they can be one pin
// (see board::DebugPins). Each pin access is one store to BSRR or one load
// from IDR, and a direction change is four bit-band stores (hw::FastPin).
// On the CH32V307, a direction change is one store of a word from begin().

#include <cstdint>

#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/CycleCounter.h"
#include "swd/SwdBitBang.h"

namespace board {

class SwdPins {
   public:
#if defined(__riscv)
   // The timed code makes 1.85 MHz at 144 MHz (swd/SwdBitBang.h).
   static constexpr bool timedAtDelayZero = true;
   static_assert(swd::halfPeriodCycles(144'000'000, 0) >= swd::timedCodeCycles);
   // The NOP pads of the untimed engine without extra NOPs (pad 0, requests
   // from 11.08 MHz). Pad 0 gives 9.6 MHz (median). Without a low-half pad,
   // the SWDIO setup time is 18 ns at minimum. The unrolled bits have less
   // code in the high half, so a high-half pad of 2 keeps the target time to
   // drive SWDIO at 48 ns at minimum (docs/clock-pacing.md).
   static constexpr uint32_t untimedHighPadNops = 2;
   static constexpr uint32_t untimedLowPadNops = 0;
   static constexpr bool unrolledBits = true;
   // The requests with a half period below 36 cycles (above 2 MHz) run
   // untimed with NOP pads (swd::untimedPadFor()). Near its code limit,
   // the timed engine makes single periods up to 11 % short. The shortest
   // untimed period without extra NOPs is 13 cycles (90 ns), and the jump
   // into the NOP sled and back adds 8 cycles to a half period
   // (docs/clock-pacing.md).
   static constexpr uint32_t untimedBelowHalfCycles = 36;
   static constexpr uint32_t untimedMinPeriodCycles = 13;
   // The padded engine (pads from 1) has the bit loops: their code is in
   // the high half, so it needs no high-half pad. Pad 1 has no extra NOPs
   // (swd::extraNopsOfPad()). The shortest period is 14 + 2 * pad cycles:
   // 16 cycles (9 MHz) at pad 1 (docs/clock-pacing.md).
   static constexpr uint32_t paddedHighPadNops = 0;
   static constexpr uint32_t paddedMinPeriodCycles = 14;
   static constexpr uint32_t padSledCycles = 8;
   // The NOPs of the sled (BoardSwd.cpp)
   static constexpr uint32_t padSledNops = 32;
#endif

   void init();
   void fini();

   [[gnu::always_inline]] void begin() {
      startCycleCounter();
#if defined(__riscv)
      // The two CNF/MODE words of SWDIO, from the other pins of the
      // register now. During an operation, no code changes the other pins
      // of the register (no interrupt handler configures a GPIO), so each
      // direction change is one store, without a read-modify-write.
      constexpr uint32_t shift = hw::gpio_f1::configShift(swdioPin);
      const uint32_t others = swdioConfigRegister() & ~(0xFUL << shift);
      swdioOutputWord_ = others | (static_cast<uint32_t>(SwdioOut::OutputConfig) << shift);
      swdioInputWord_ = others | (static_cast<uint32_t>(pullUpInput) << shift);
#endif
   }

   [[gnu::always_inline]] void swclk(bool level) {
      Swclk::write(level);
   }
   // The output register first, so the pin starts at its level.
   [[gnu::always_inline]] void swdioDrive(bool level) {
      SwdioOut::write(level);
#if defined(__riscv)
      swdioConfigRegister() = swdioOutputWord_;
#else
      SwdioOut::setOutput();
#endif
   }
   // Input first, then the pull-up: SWDIO does not glitch. On the STM32H5,
   // PUPDR keeps the pull-up of init() in both directions, so only the mode
   // changes.
   [[gnu::always_inline]] void swdioRelease() {
#if defined(STM32F1) || defined(__riscv)
      // F1-style GPIO (also CH32): the pull-up is a pull input with ODR high.
#if defined(__riscv)
      swdioConfigRegister() = swdioInputWord_;
#else
      SwdioOut::setConfig<pullUpInput>();
#endif
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

#if defined(__riscv)
   // The NOP sled of the untimed engine (BoardSwd.cpp): setPadSled() sets
   // the entry for nopCount NOPs before the return, padSled() runs them.
   void setPadSled(uint32_t nopCount);
   [[gnu::always_inline]] void padSled() {
      asm volatile("jalr %0" : : "r"(padSledEntry_) : "ra");
   }
#endif

#if defined(__riscv)
   protected:
   // The direction change with a read-modify-write of hw::FastPin, without
   // the words of begin(): RvSwdPins uses it.
   [[gnu::always_inline]] static void swdioDriveReadModifyWrite(bool level) {
      SwdioOut::write(level);
      SwdioOut::setOutput();
   }
   [[gnu::always_inline]] static void swdioReleaseReadModifyWrite() {
      SwdioOut::setConfig<pullUpInput>();
      SwdioOut::set();
   }
#endif

   private:
#if defined(STM32F1) || defined(__riscv)
   // Input with pull: ODR high selects the pull-up.
   static constexpr uint8_t pullUpInput =
   hw::gpio_f1::pinConfig(hw::GpioPin::Mode::Input, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::Up);
#endif

   using Swclk = FastPinOf<current.debug.tck>;
   using SwdioOut = FastPinOf<current.debug.tmsOut>;
   using SwdioIn = FastPinOf<current.debug.tmsIn>;

#if defined(__riscv)
   static constexpr uint8_t swdioPin = current.debug.tmsOut.number;

   // CFGLR or CFGHR of SWDIO
   [[gnu::always_inline]] static volatile uint32_t &swdioConfigRegister() {
      return (&SwdioOut::port()->CFGLR)[hw::gpio_f1::configIndex(swdioPin)];
   }

   // Set by begin()
   uint32_t swdioOutputWord_{ 0 };
   uint32_t swdioInputWord_{ 0 };
   // Set by setPadSled()
   uintptr_t padSledEntry_{ 0 };
#endif
};

#if defined(__riscv)
// The largest pad needs its extra NOPs - padSledCycles NOPs of the sled.
static_assert(swd::extraNopsOfPad<SwdPins>(swd::untimedPadCount<SwdPins>() - 1) - SwdPins::padSledCycles <= SwdPins::padSledNops);
#endif

using BoardSwd = swd::BitBangPort<SwdPins>;

} // namespace board

// BoardSwd.cpp instantiates the engine, with -O2 (CMakeLists.txt).
extern template class swd::BitBangPort<board::SwdPins>;
