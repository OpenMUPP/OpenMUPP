// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A fake power switch and voltage sense, for the target power tests.

#include "protocol/Delay.h"
#include "protocol/PowerHardware.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <vector>

// The rail of the target. With the switches off, the rail is at externalMv
// (the supply of the target). With a switch on, it is at 3300 mV or
// 5000 mV, or at loadedMv when the target pulls it down. After a switch-off
// from 5 V, the rail stays at 5000 mV for dischargeReads measurements.
class FakePower final : public protocol::PowerHardware {
   public:
   [[nodiscard]] bool hasSwitch() const override {
      return hasSwitchPin;
   }

   [[nodiscard]] bool has5vSwitch() const override {
      return has5vSwitchPin;
   }

   void select(protocol::Supply supply) override {
      REQUIRE(hasSwitchPin);
      REQUIRE((supply != protocol::Supply::V5 || has5vSwitchPin));
      if(supply == protocol::Supply::Off && selected == protocol::Supply::V5) {
         charged = dischargeReads;
      }
      selected = supply;
      isOn = supply != protocol::Supply::Off;
      switches.push_back(isOn);
      selections.push_back(supply);
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
      if(!isOn && charged > 0) {
         --charged;
         return 5000;
      }
      if(!isOn) {
         return externalMv;
      }
      return loadedMv.value_or(selected == protocol::Supply::V5 ? 5000 : 3300);
   }

   [[nodiscard]] std::optional<uint16_t> fiveVoltInputMillivolts() override {
      REQUIRE(has5vSwitchPin);
      return inputMv;
   }

   // The fault interrupt: all switches off
   void fault() {
      isOn = false;
      selected = protocol::Supply::Off;
      pendingFault = true;
   }

   [[nodiscard]] bool takeFault() override {
      const bool taken = pendingFault;
      pendingFault = false;
      return taken;
   }

   bool hasSwitchPin{ true };
   bool hasSensePin{ true };
   bool has5vSwitchPin{ false };
   bool isOn{ false };
   protocol::Supply selected{ protocol::Supply::Off };
   bool failSense{ false };
   uint16_t externalMv{ 0 };
   std::optional<uint16_t> loadedMv;
   std::optional<uint16_t> inputMv{ 5000 };
   unsigned dischargeReads{ 0 };
   unsigned charged{ 0 };
   bool pendingFault{ false };
   // On or off at each select()
   std::vector<bool> switches;
   std::vector<protocol::Supply> selections;
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
