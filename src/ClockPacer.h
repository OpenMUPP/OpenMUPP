// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Paces the clock edges of the bit-bang engines (swd/SwdBitBang.h,
// jtag/JtagBitBang.h, rvswd/RvSwdEngine.h) on deadlines of the core cycle
// counter. A half period ends at a deadline: the last deadline plus the
// half period. The code between two edges runs inside the half period.
// Thus the clock rate does not depend on the code, the compiler or the
// flash wait states, while that code is shorter than a half period
// (docs/clock-pacing.md).
//
// Each edge has a wait before it and a call after it:
//
//   pacer.wait(pins);    until the deadline
//   pins.swclk(level);   the edge
//   pacer.edge(pins);
//
// An edge more than the slack after its deadline is late: the code took
// the whole half period, or an interrupt came between the wait and the
// edge. The next half period then starts at the edge. Thus a late edge
// makes its own half period longer, never the next one shorter, and the
// clock does not catch up. An edge within lateSlackCycles makes the next
// half period shorter by up to that many cycles. The edges have the jitter
// of the wait loop: up to one pass of it. The mean rate is exact.
//
// The Pins type has `uint32_t now()`, the free-running core cycle counter.
// The unsigned differences are correct across the wrap of the counter.
// After a pause of more than one wrap, the first wait can be up to one half
// period too long.

#include <cstdint>

namespace pacing {

// The cycles from a deadline to the edge() call: up to one pass of the wait
// loop, the pin write, a pin read before it (SWD), and the read of the
// counter. On the STM32F103 at 72 MHz, this is up to about 20 cycles. An
// interrupt adds at least its entry and exit (24 cycles on the Cortex-M3),
// so the pacer sees each interrupt between the wait and the edge. A smaller
// value makes the clock slower when the normal delay is longer: the
// deadline then starts again at each edge. A half period half of this
// value or less has no margin: an interrupt within the slack can make the
// next half period very short. Check the rate on the wire after a change
// of the engines or of the core (docs/clock-pacing.md).
//
// On the STM32H5 at 250 MHz, 32 cycles are a full half period at 4 MHz:
// where the JTAG code is longer than the half period, the next half
// period was then as short as 60 ns. With 16 cycles, the same edges start
// the deadlines again (docs/clock-pacing.md).
#if defined(STM32H5)
inline constexpr uint32_t lateSlackCycles = 16;
#else
inline constexpr uint32_t lateSlackCycles = 32;
#endif

class ClockPacer {
   public:
   void setHalfCycles(uint32_t cycles) {
      half_ = cycles;
   }

   // A half period starts now, for example after an edge without a wait.
   template <typename Pins>
   [[gnu::always_inline]] void start(Pins &pins) {
      deadline_ = pins.now();
   }

   // Waits until the half period after the last deadline. An edge and
   // edge() follow, or the next wait (a half period without an edge).
   template <typename Pins>
   [[gnu::always_inline]] void wait(Pins &pins) {
      const uint32_t deadline = deadline_;
      const uint32_t half = half_;
      // A do-while gives one branch per pass: less jitter
      uint32_t now;
      do {
         now = pins.now();
      } while(now - deadline < half);
      deadline_ = deadline + half;
   }

   // After the edge that follows wait()
   template <typename Pins>
   [[gnu::always_inline]] void edge(Pins &pins) {
      const uint32_t now = pins.now();
      if(now - deadline_ > lateSlackCycles) {
         deadline_ = now;
      }
   }

   private:
   uint32_t deadline_{ 0 };
   uint32_t half_{ 0 };
};

} // namespace pacing
