// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A fake VPP boost and gate, for the VPP tests.

#include "protocol/Vpp.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A boost with the scale of OpenMUPP v1. The output rises at once to the
// level of the DAC, and falls by dischargePerMs (the dividers discharge
// the capacitors). offsetMv is the tolerance error of the board. The
// delay advances the time of the model.
class FakeVppBoard final : public protocol::VppHardware, public protocol::Delay {
   public:
   void setDac(uint16_t code) override {
      dac = code;
      events.push_back("dac " + std::to_string(code));
   }
   void dacOff() override {
      dac.reset();
      events.push_back("dac off");
   }
   void setEnable(bool value) override {
      enable = value;
      events.push_back(std::string("en ") + (value ? "1" : "0"));
   }
   void setOn(bool value) override {
      on = value;
      events.push_back(std::string("on ") + (value ? "1" : "0"));
   }
   void setPullDown(bool value) override {
      pullDown = value;
      events.push_back(std::string("pd ") + (value ? "1" : "0"));
   }
   [[nodiscard]] std::optional<uint16_t> senseMillivolts() override {
      if(failSense) {
         return std::nullopt;
      }
      return static_cast<uint16_t>(outputMv);
   }
   [[nodiscard]] std::optional<uint16_t> sense2Millivolts() override {
      if(pullDown || !on) {
         return 0;
      }
      return static_cast<uint16_t>(shortedPin ? 1000 : outputMv - 30);
   }
   void microseconds(uint16_t /*us*/) override {
   }
   void milliseconds(uint16_t ms) override {
      for(uint16_t i = 0; i < ms; ++i) {
         ++nowMs;
         const int32_t level = setpoint();
         outputMv = outputMv < level ? level : std::max(level, outputMv - dischargePerMs);
      }
   }

   // The output level of the boost now
   [[nodiscard]] int32_t setpoint() const {
      if(!enable) {
         return vinMv;
      }
      const int32_t dacMv = dac ? *dac * 3300 / 4095 : 600;
      return std::max<int32_t>(vinMv, 14100 - dacMv * 9 / 2 + offsetMv);
   }

   std::optional<uint16_t> dac;
   bool enable{ false };
   bool on{ false };
   bool pullDown{ false };
   bool failSense{ false };
   bool shortedPin{ false };
   int32_t offsetMv{ 0 };
   int32_t vinMv{ 4600 };
   int32_t outputMv{ 4600 };
   int32_t dischargePerMs{ 13 };
   uint32_t nowMs{ 0 };
   std::vector<std::string> events;
};

class FakeVppGate final : public protocol::VppGate {
   public:
   [[nodiscard]] std::optional<uint16_t> check() override {
      ++checks;
      return limit;
   }
   [[nodiscard]] bool isAllowed() const override {
      return limit.has_value();
   }

   std::optional<uint16_t> limit{ protocol::Vpp::hardwareMaxMillivolts };
   int checks{ 0 };
};
