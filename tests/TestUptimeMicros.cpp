// SPDX-License-Identifier: GPL-3.0-or-later

#include "UptimeMicros.h"

#include <doctest/doctest.h>

namespace {
// 72 MHz and 1 ms per tick
constexpr uint32_t reload = 72'000;
} // namespace

TEST_CASE("The uptime has the microseconds of the current tick") {
   // The counter has just reloaded
   CHECK(uptimeMicroseconds(5, reload, reload, false) == 5000);
   CHECK(uptimeMicroseconds(5, reload, reload - reload / 2, false) == 5500);
   CHECK(uptimeMicroseconds(5, reload, 0, false) == 6000);
}

TEST_CASE("A wrap that the tick count misses does not make the uptime go back") {
   // Just before the wrap
   const uint32_t before = uptimeMicroseconds(5, reload, 1, false);
   // After the wrap, before the interrupt runs: the tick count is old
   const uint32_t pending = uptimeMicroseconds(5, reload, reload - 10, true);
   // After the interrupt
   const uint32_t after = uptimeMicroseconds(6, reload, reload - 20, false);

   CHECK(pending >= before);
   CHECK(after >= pending);
   CHECK(pending == 6000);
}

TEST_CASE("A wrap after the counter read keeps the uptime") {
   // The counter read is just before the wrap, the pending bit is set
   // after it. The elapsed time is nearly one tick.
   const uint32_t value = uptimeMicroseconds(5, reload, 1, true);
   CHECK(value >= 5999);
   CHECK(value <= 6000);
   // The next read counts the tick
   CHECK(uptimeMicroseconds(6, reload, reload - 1, false) >= value);
}

TEST_CASE("The uptime never goes back over a sweep of the counter") {
   uint32_t last = 0;
   for(uint64_t tick = 0; tick < 3; ++tick) {
      for(uint32_t value = reload; value > 7; value -= 7) {
         // After the last step of a tick the wrap is pending, the
         // interrupt has not run yet
         const uint32_t now = uptimeMicroseconds(tick, reload, value, false);
         CHECK(now >= last);
         last = now;
      }
      const uint32_t pending = uptimeMicroseconds(tick, reload, reload - 3, true);
      CHECK(pending >= last);
      last = pending;
   }
}

TEST_CASE("The uptime wraps at 2^32 microseconds") {
   // 2^32 us is 4294967.296 ms
   CHECK(uptimeMicroseconds(4'294'968, reload, reload, false) == 4'294'968'000u - 4'294'967'296u);
}
