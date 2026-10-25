#pragma once

// Paces the clock edges of the bit-bang engines (swd/SwdBitBang.h,
// jtag/JtagBitBang.h, rvswd/RvSwdEngine.h) on deadlines of the core cycle
// counter. A half period ends at a deadline: the deadline of the last edge
// plus the half period. The code between two edges runs inside the half
// period. Thus the clock rate does not depend on the code, the compiler or
// the flash wait states, while that code is shorter than a half period
// (docs/clock-pacing.md).
//
// Each edge has a wait before it and a call after it:
//
//   pacer.wait(pins, halfCycles);   until the deadline
//   pins.swclk(level);              the edge, and nothing else
//   pacer.edge(pins);
//
// An edge more than lateSlackCycles after its deadline is late: an
// interrupt, or code longer than the half period. The next half period
// then starts at the edge. Thus a late edge makes a half period longer,
// never shorter, and the clock does not catch up. The edges have the
// jitter of the wait loop (some core cycles). The mean rate is exact.
//
// The Pins type has `uint32_t now()`, the free-running core cycle counter.
// The unsigned differences are correct across the wrap of the counter.
// After a pause of more than one wrap, the first wait can be up to one half
// period too long.

#include <cstdint>

namespace pacing {

// The cycles from a deadline to the edge() call: the exit of the wait loop
// and the pin write. A larger value lets an interrupt make the next half
// period shorter by up to this many cycles. A smaller value makes the clock
// slower when the normal delay is longer: the deadline then starts again
// at each edge. Check the rate on the wire after a change of the engines
// or of the core (docs/clock-pacing.md).
inline constexpr uint32_t lateSlackCycles = 16;

class ClockPacer {
   public:
   // A half period starts now, for example after an edge without a wait.
   template <typename Pins>
   [[gnu::always_inline]] void start(Pins &pins) {
      deadline_ = pins.now();
   }

   // Waits until `cycles` after the last deadline.
   template <typename Pins>
   [[gnu::always_inline]] void wait(Pins &pins, uint32_t cycles) {
      next_ = deadline_ + cycles;
      while(pins.now() - deadline_ < cycles) {
      }
   }

   // After the edge that follows wait()
   template <typename Pins>
   [[gnu::always_inline]] void edge(Pins &pins) {
      const uint32_t now = pins.now();
      deadline_ = now - next_ > lateSlackCycles ? now : next_;
   }

   private:
   uint32_t deadline_{ 0 };
   uint32_t next_{ 0 };
};

} // namespace pacing
