#pragma once

// The pins of board::current for the RISC-V debug wire engine
// (rvswd/RvSwdEngine.h). Target only. The pins of the SWD engine
// (board/BoardSwd.h): SWCLK is the TCK pin, SWDIO the TMS pins. The
// one-wire interface of the CH32V00x uses SWDIO only. The idle level of
// SWCLK is low, and the interrupt lock is PRIMASK. A one-wire frame runs
// on the timer of board::oneWireTimerOf() with two DMA channels
// (BoardRvSwd.cpp on the STM32F1, BoardRvSwdH5.cpp on the STM32H5).

#include <cmsis_device.h>
#include <cstdint>

#include "board/BoardSwd.h"
#include "rvswd/DebugModule.h"
#include "rvswd/RvSwdEngine.h"

namespace board {

class RvSwdPins : public SwdPins {
   public:
   // SWCLK output low, SWDIO driven high, the one-wire timer and DMA set up
   void init();
   // SWCLK and SWDIO inputs, the one-wire timer reset
   void fini();

   // See rvswd/RvSwdEngine.h. Call it with the interrupts locked.
   bool oneWireFrame(uint64_t bits, uint32_t count, uint32_t readFrom, uint32_t &samples);

   [[gnu::always_inline]] uint32_t lockIrq() {
      const uint32_t primask = __get_PRIMASK();
      __disable_irq();
      return primask;
   }
   [[gnu::always_inline]] void unlockIrq(uint32_t primask) {
      __set_PRIMASK(primask);
   }
};

using BoardRvSwd = rvswd::RvSwdEngine<RvSwdPins>;
using BoardDebugModule = rvswd::DebugModule<BoardRvSwd>;

} // namespace board

// BoardRvSwd.cpp instantiates the engine, with -O2 (CMakeLists.txt).
extern template class rvswd::RvSwdEngine<board::RvSwdPins>;
// BoardDebugModule.cpp instantiates the debug module layer.
extern template class rvswd::DebugModule<board::BoardRvSwd>;
