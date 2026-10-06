#include "Heartbeat.h"

#include <doctest/doctest.h>

TEST_CASE("Heartbeat flashes at the start of each period") {
   constexpr Heartbeat heartbeat{ Heartbeat::normal };

   CHECK(heartbeat.isOn(0));
   CHECK(heartbeat.isOn(99));
   CHECK_FALSE(heartbeat.isOn(100));
   CHECK_FALSE(heartbeat.isOn(999));
   CHECK(heartbeat.isOn(1000));
   CHECK(heartbeat.isOn(1099));
   CHECK_FALSE(heartbeat.isOn(1100));
}

TEST_CASE("Heartbeat keeps the pattern after a long uptime") {
   constexpr Heartbeat heartbeat{ Heartbeat::normal };
   // More than 2^32 ms (about 50 days) of uptime
   constexpr uint64_t start = 5'000'000'000'000;

   CHECK(heartbeat.isOn(start));
   CHECK_FALSE(heartbeat.isOn(start + 100));
   CHECK(heartbeat.isOn(start + 1000));
}

TEST_CASE("The clock fault pattern flashes faster than the normal pattern") {
   static_assert(Heartbeat::clockFault.periodMs < Heartbeat::normal.periodMs);
   constexpr Heartbeat heartbeat{ Heartbeat::clockFault };

   CHECK(heartbeat.isOn(0));
   CHECK_FALSE(heartbeat.isOn(100));
   CHECK(heartbeat.isOn(200));
}
