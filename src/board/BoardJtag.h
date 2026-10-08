#pragma once

// The JTAG pins of board::current for the bit-bang engine
// (jtag/JtagBitBang.h). Target only. Each pin access is one store to BSRR
// or one load from IDR (hw::FastPin). now() is the DWT cycle counter.

#include <cmsis_device.h>
#include <cstdint>

#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/CycleCounter.h"
#include "jtag/JtagBitBang.h"

namespace board {

class JtagPins {
   public:
   static constexpr bool hasRtck = current.debug.rtck.has_value();

   void init(bool tckLevel);
   void fini();

   [[gnu::always_inline]] void begin() {
      startCycleCounter();
   }

   [[gnu::always_inline]] void tck(bool level) {
      Tck::write(level);
   }
   [[gnu::always_inline]] void tms(bool level) {
      Tms::write(level);
   }
   [[gnu::always_inline]] void tdi(bool level) {
      Tdi::write(level);
   }
   [[gnu::always_inline]] bool tdo() {
      return Tdo::read();
   }
   [[gnu::always_inline]] bool rtck() {
      return Rtck::read();
   }
   [[gnu::always_inline]] uint32_t now() {
      return cycleCount();
   }
   [[gnu::always_inline]] uint32_t coreHz() {
      return SystemCoreClock;
   }

   private:
   using Tck = FastPinOf<current.debug.tck>;
   using Tms = FastPinOf<current.debug.tmsOut>;
   using Tdi = FastPinOf<current.debug.tdi>;
   using Tdo = FastPinOf<current.debug.tdo>;
   // Without RTCK, the engine never reads it (hasRtck is false).
   using Rtck = FastPinOf<current.debug.rtck.value_or(current.debug.tdo)>;
};

using BoardJtag = jtag::BitBangPort<JtagPins>;

} // namespace board
