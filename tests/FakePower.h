// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A fake power switch and voltage sense, for the target power tests.

#include "protocol/Delay.h"
#include "protocol/PowerHardware.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <vector>

// The rail of the target. With the switch off, the rail is at externalMv
// (the supply of the target). With the switch on, it is at 3300 mV, or at
// loadedMv when the target pulls it down.
class FakePower final : public protocol::PowerHardware {
   public:
   [[nodiscard]] bool hasSwitch() const override {
      return hasSwitchPin;
   }

   void setSwitch(bool on) override {
      REQUIRE(hasSwitchPin);
      isOn = on;
      switches.push_back(on);
   }

   [[nodiscard]] bool hasSense() const override {
      return hasSensePin;
   }

   [[nodiscard]] std::optional<uint16_t> senseMillivolts() override {
      REQUIRE(hasSensePin);
      ++senses;
      if(failSense) {
         return std::nullopt;
      }
      return isOn ? loadedMv.value_or(3300) : externalMv;
   }

   bool hasSwitchPin{ true };
   bool hasSensePin{ true };
   bool isOn{ false };
   bool failSense{ false };
   uint16_t externalMv{ 0 };
   std::optional<uint16_t> loadedMv;
   std::vector<bool> switches;
   unsigned senses{ 0 };
};

// Records the delays and the state of the switch at each delay.
class FakeDelay final : public protocol::Delay {
   public:
   explicit FakeDelay(const FakePower &power)
      : power_(power) {
   }

   void microseconds(uint16_t) override {
   }

   void milliseconds(uint16_t ms) override {
      delays.push_back({ ms, power_.isOn });
   }

   struct Delayed {
      uint16_t ms;
      bool switchOn;
   };
   std::vector<Delayed> delays;

   private:
   const FakePower &power_;
};
