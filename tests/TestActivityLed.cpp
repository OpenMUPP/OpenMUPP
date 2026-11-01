// SPDX-License-Identifier: GPL-3.0-or-later

#include "ActivityLed.h"

#include <doctest/doctest.h>

TEST_CASE("The activity LED is off when no command runs") {
   ActivityLed led;

   for(uint64_t t = 0; t < 1000; t += 10) {
      led.update(t, 0);
      CHECK_FALSE(led.isOn());
   }
}

TEST_CASE("A command gives one flash of onMs") {
   ActivityLed led;
   led.update(100, 0);
   led.update(110, 1);
   CHECK(led.isOn());
   led.update(110 + ActivityLed::onMs - 1, 1);
   CHECK(led.isOn());
   led.update(110 + ActivityLed::onMs, 1);
   CHECK_FALSE(led.isOn());
   led.update(1000, 1);
   CHECK_FALSE(led.isOn());
}

TEST_CASE("A continuous command stream flickers") {
   ActivityLed led;
   unsigned flashes = 0;
   bool wasOn = false;
   uint32_t count = 0;

   for(uint64_t t = 0; t < 1000; ++t) {
      led.update(t, ++count);
      if(led.isOn() && !wasOn) {
         ++flashes;
      }
      wasOn = led.isOn();
   }
   // One flash each onMs + offMs
   CHECK(flashes == 1000 / (ActivityLed::onMs + ActivityLed::offMs) + 1);
}

TEST_CASE("A command in the off time flashes after the off time") {
   ActivityLed led;
   led.update(0, 1);
   REQUIRE(led.isOn());
   led.update(ActivityLed::onMs, 1);
   REQUIRE_FALSE(led.isOn());
   led.update(ActivityLed::onMs + 5, 2);
   CHECK_FALSE(led.isOn());
   led.update(ActivityLed::onMs + ActivityLed::offMs, 2);
   CHECK(led.isOn());
}

TEST_CASE("The activity LED handles a wrap of the counter and a long uptime") {
   ActivityLed led;
   constexpr uint64_t start = 5'000'000'000'000;
   led.update(start, 0xFFFFFFFF);
   REQUIRE(led.isOn());
   led.update(start + 1000, 0xFFFFFFFF);
   REQUIRE_FALSE(led.isOn());
   led.update(start + 1001, 0);
   CHECK(led.isOn());
}
