// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The pins of board::current for the RISC-V debug wire engine
// (rvswd/RvSwdEngine.h). Target only. The pins of the SWD engine
// (board/BoardSwd.h): SWCLK is the TCK pin, SWDIO the TMS pins. The
// one-wire interface of the CH32V00x uses SWDIO only. The idle level of
// SWCLK is low, and the interrupt lock is PRIMASK (mstatus.MIE on the
// CH32V307). A one-wire frame runs on the timer of board::oneWireTimerOf()
// with two DMA channels (BoardRvSwd.cpp on the STM32F1, BoardRvSwdH5.cpp on the STM32H5,
// BoardRvSwdV30x.cpp on the CH32V307).

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

#if defined(__riscv)
   // QingKe: mstatus.MIE (bit 3) is the global interrupt enable. One csrrc
   // reads and clears it. __disable_irq() of ch32v00x-system also clears
   // MPIE, so the lock does not use it.
   [[gnu::always_inline]] uint32_t lockIrq() {
      uint32_t mstatus = 0;
      asm volatile("csrrc %0, mstatus, %1" : "=r"(mstatus) : "r"(mstatusMie) : "memory");
      return mstatus & mstatusMie;
   }
   [[gnu::always_inline]] void unlockIrq(uint32_t mie) {
      if(mie != 0) {
         asm volatile("csrs mstatus, %0" : : "r"(mstatusMie) : "memory");
      }
   }

   private:
   static constexpr uint32_t mstatusMie = 1u << 3;
#else
   [[gnu::always_inline]] uint32_t lockIrq() {
      const uint32_t primask = __get_PRIMASK();
      __disable_irq();
      return primask;
   }
   [[gnu::always_inline]] void unlockIrq(uint32_t primask) {
      __set_PRIMASK(primask);
   }
#endif
};

using BoardRvSwd = rvswd::RvSwdEngine<RvSwdPins>;
using BoardDebugModule = rvswd::DebugModule<BoardRvSwd>;

} // namespace board

// BoardRvSwd.cpp instantiates the engine, with -O2 (CMakeLists.txt).
extern template class rvswd::RvSwdEngine<board::RvSwdPins>;
// BoardDebugModule.cpp instantiates the debug module layer.
extern template class rvswd::DebugModule<board::BoardRvSwd>;
