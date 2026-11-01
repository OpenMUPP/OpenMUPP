// SPDX-License-Identifier: GPL-3.0-or-later

#include "ErrorLed.h"

#include <doctest/doctest.h>

TEST_CASE("The error LED is off when no error occurs") {
   ErrorLed led;

   for(uint64_t t = 0; t < 5000; t += 10) {
      led.update(t, 0);
      CHECK_FALSE(led.isActive());
      CHECK_FALSE(led.isOn());
   }
}

TEST_CASE("An error gives a double flash in each period") {
   ErrorLed led;
   constexpr uint64_t start = 1000;
   led.update(start, 1);
   REQUIRE(led.isActive());

   for(uint64_t period = 0; period < 3; ++period) {
      const uint64_t base = start + period * ErrorLed::periodMs;
      CAPTURE(period);
      led.update(base, 1);
      CHECK(led.isOn());
      led.update(base + ErrorLed::flashMs - 1, 1);
      CHECK(led.isOn());
      led.update(base + ErrorLed::flashMs, 1);
      CHECK_FALSE(led.isOn());
      led.update(base + 2 * ErrorLed::flashMs, 1);
      CHECK(led.isOn());
      led.update(base + 3 * ErrorLed::flashMs - 1, 1);
      CHECK(led.isOn());
      led.update(base + 3 * ErrorLed::flashMs, 1);
      CHECK_FALSE(led.isOn());
      led.update(base + ErrorLed::periodMs - 1, 1);
      CHECK_FALSE(led.isOn());
   }
}

TEST_CASE("The error pattern stops holdMs after the last error") {
   ErrorLed led;
   led.update(0, 1);
   led.update(ErrorLed::holdMs - 1, 1);
   CHECK(led.isActive());
   led.update(ErrorLed::holdMs, 1);
   CHECK_FALSE(led.isActive());
   CHECK_FALSE(led.isOn());
}

TEST_CASE("A new error restarts the hold time, but not the pattern") {
   ErrorLed led;
   led.update(0, 1);
   // The second error comes in the off time of the first period.
   constexpr uint64_t second = 1500;
   led.update(second, 2);
   CHECK(led.isActive());
   // The pattern continues from the first error: 1500 % 600 = 300 is off.
   CHECK_FALSE(led.isOn());
   // 1802 % 600 = 2 is in the first flash of the fourth period.
   led.update(3 * ErrorLed::periodMs + 2, 2);
   CHECK(led.isOn());
   led.update(second + ErrorLed::holdMs - 1, 2);
   CHECK(led.isActive());
   led.update(second + ErrorLed::holdMs, 2);
   CHECK_FALSE(led.isActive());
}

TEST_CASE("An error after the end of the pattern starts it again") {
   ErrorLed led;
   led.update(0, 1);
   led.update(ErrorLed::holdMs, 1);
   REQUIRE_FALSE(led.isActive());
   // Starts at the error, not in step with the first pattern
   constexpr uint64_t again = ErrorLed::holdMs + 250;
   led.update(again, 2);
   CHECK(led.isActive());
   CHECK(led.isOn());
}

TEST_CASE("The error LED handles a wrap of the counter and a long uptime") {
   ErrorLed led;
   constexpr uint64_t start = 5'000'000'000'000;
   led.update(start, 0xFFFFFFFF);
   REQUIRE(led.isActive());
   led.update(start + ErrorLed::holdMs, 0xFFFFFFFF);
   REQUIRE_FALSE(led.isActive());
   led.update(start + ErrorLed::holdMs + 1, 0);
   CHECK(led.isActive());
   CHECK(led.isOn());
}
