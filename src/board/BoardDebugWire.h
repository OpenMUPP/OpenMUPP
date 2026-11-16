// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The debugWIRE engine of board::current (avr/DebugWireEngine.h). Target
// only, CH32V307 only (BoardDebugWireV30x.cpp).
//
// DebugWirePins: the drive pin is SRST, with the polarity of
// DebugPins::srstActive (YD: open-drain, low pulls the line low; v1:
// push-pull into a transistor, high pulls the line low). The read-back pin
// is DebugPins::srstSense on USART3 RX (PB11), with the receiver only: the
// USART3 TX pin (PB10) stays free. sense() reads the input register of
// PB11, which the USART does not change. now() is the cycle counter
// (board/CycleCounter.h). The interrupt lock is mstatus.MIE, as in
// board/BoardRvSwd.h.

#include <cmsis_device.h>
#include <cstdint>

#include "avr/DebugWireEngine.h"
#include "avr/DebugWireTarget.h"
#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/CycleCounter.h"

namespace board {

class DebugWirePins {
   public:
   void init();
   void fini();
   [[nodiscard]] bool rxEnable(uint32_t baud);
   void rxDisable();
   int32_t rxRead();

   [[gnu::always_inline]] void begin() {
      startCycleCounter();
   }

   [[gnu::always_inline]] void drive(bool low) {
      Drive::write(resetSignal.levelFor(low));
   }
   [[gnu::always_inline]] bool sense() {
      return Sense::read();
   }
   [[gnu::always_inline]] uint32_t now() {
      return cycleCount();
   }
   [[gnu::always_inline]] uint32_t coreHz() {
      return SystemCoreClock;
   }

   // QingKe: mstatus.MIE (bit 3) is the global interrupt enable. One csrrc
   // reads and clears it.
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
   static constexpr Signal resetSignal{ current.debug.srst, current.debug.srstActive };
   static_assert(current.debug.srstSense.has_value(), "debugWIRE needs DebugPins::srstSense");
   using Drive = FastPinOf<current.debug.srst>;
   using Sense = FastPinOf<*current.debug.srstSense>;
};

using BoardDebugWire = avr::DebugWireEngine<DebugWirePins>;
using BoardDebugWireTarget = avr::DebugWireTarget<BoardDebugWire>;

} // namespace board

// BoardDebugWireV30x.cpp instantiates the engine and the target
// operations, with -O2 (CMakeLists.txt).
#if defined(CH32V30x_D8C)
extern template class avr::DebugWireEngine<board::DebugWirePins>;
extern template class avr::DebugWireTarget<board::BoardDebugWire>;
#endif
