#include "TargetPowerLed.h"

#include <doctest/doctest.h>

TEST_CASE("The target power LED is on before the first measurement") {
   TargetPowerLed led;

   CHECK(led.isOn());
   CHECK(led.isDue(0));
}

TEST_CASE("The target power LED follows the target voltage") {
   TargetPowerLed led;

   led.update(0, 0);
   CHECK_FALSE(led.isOn());
   led.update(100, 3300);
   CHECK(led.isOn());
   led.update(200, 0);
   CHECK_FALSE(led.isOn());
}

TEST_CASE("The target power LED has a hysteresis") {
   TargetPowerLed led;
   led.update(0, 0);

   // Between the two levels the state stays
   led.update(100, 1600);
   CHECK_FALSE(led.isOn());
   led.update(200, TargetPowerLed::onAboveMillivolts);
   CHECK_FALSE(led.isOn());
   led.update(300, TargetPowerLed::onAboveMillivolts + 1);
   CHECK(led.isOn());
   led.update(400, 1600);
   CHECK(led.isOn());
   led.update(500, TargetPowerLed::offBelowMillivolts);
   CHECK(led.isOn());
   led.update(600, TargetPowerLed::offBelowMillivolts - 1);
   CHECK_FALSE(led.isOn());
}

TEST_CASE("A failed measurement keeps the state") {
   TargetPowerLed led;
   led.update(0, 3300);
   led.update(100, std::nullopt);
   CHECK(led.isOn());
   led.update(200, 0);
   led.update(300, std::nullopt);
   CHECK_FALSE(led.isOn());
}

TEST_CASE("The target power LED measures each interval") {
   TargetPowerLed led;
   led.update(1000, 3300);

   CHECK_FALSE(led.isDue(1000));
   CHECK_FALSE(led.isDue(1000 + TargetPowerLed::intervalMs - 1));
   CHECK(led.isDue(1000 + TargetPowerLed::intervalMs));
}
