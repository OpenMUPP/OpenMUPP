// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The uptime in microseconds from the SysTick count and the SysTick
// counter, as a 32-bit value that wraps at 2^32. Hardware-independent:
// main() reads the registers.
//
// SysTick counts down from the reload value to 0 and then reloads. The
// SysTick interrupt adds 1 to the tick count. The interrupt cannot run
// while main() reads the registers with interrupts off, but the counter
// can wrap at that time. Then the tick count misses one tick, and the
// counter value is small again: the uptime would jump back by about 1 ms.
// The pending bit of the SysTick interrupt shows this case.

#include <cstdint>

#include "IntDiv.h"

// ticks: the tick count (1 tick is 1 ms). reload: the SysTick LOAD value
// plus 1, the counter clocks of one tick. value: the SysTick counter value
// (VAL), read after the tick count. tickPending: the pending bit of the
// SysTick interrupt, read after the counter value. value is less than
// reload.
constexpr uint32_t uptimeMicroseconds(uint64_t ticks, uint32_t reload, uint32_t value, bool tickPending) {
   const uint32_t elapsed = reload - value;
   // A wrap that happens after the counter was read leaves a large
   // elapsed value. The next read then counts the tick. Thus only a small
   // elapsed value shows a tick that was not counted.
   if(tickPending && elapsed <= reload / 2) {
      ++ticks;
   }
   // The result wraps at 2^32: the low 32 bits of the tick count are
   // sufficient.
   return static_cast<uint32_t>(ticks) * 1000u + intdiv::mulDiv(elapsed, 1000u, reload);
}
