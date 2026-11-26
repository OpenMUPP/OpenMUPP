// SPDX-License-Identifier: GPL-3.0-or-later
// The VPP driver (protocol/Vpp.h) on fake pins, a fake DAC and a fake ADC.

#include "protocol/Vpp.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

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

struct Bench {
   FakeVppBoard board;
   FakeVppGate gate;
   protocol::Vpp vpp{ board, board, gate };
};

using Result = protocol::Vpp::Result;

bool near(int32_t value, int32_t level) {
   return value >= level - protocol::Vpp::toleranceMillivolts && value <= level + protocol::Vpp::toleranceMillivolts;
}

} // namespace

TEST_CASE("VPP: switch-on follows the sequence and reaches the level") {
   Bench b;
   CHECK(b.vpp.switchOn(12000) == Result::Ok);
   CHECK(b.vpp.isOn());
   CHECK(b.vpp.level() == 12000);
   CHECK(b.gate.checks == 1);
   CHECK(near(b.board.outputMv, 12000));
   CHECK(b.board.enable);
   CHECK(b.board.on);
   CHECK_FALSE(b.board.pullDown);
   // VPP_ON low, the DAC, VPP_EN high, then VPP_PD high before VPP_ON high
   const auto &e = b.board.events;
   REQUIRE(e.size() >= 6);
   CHECK(e[0] == "on 0");
   CHECK(e[1] == "dac " + std::to_string(protocol::vppDacCode(12000)));
   CHECK(e[2] == "en 1");
   CHECK(e[e.size() - 3] == "pd 1");
   CHECK(e[e.size() - 2] == "on 1");
   CHECK(e[e.size() - 1] == "pd 0");
}

TEST_CASE("VPP: the driver corrects the DAC for the board tolerance") {
   for(const int32_t offset : { -500, 500 }) {
      CAPTURE(offset);
      Bench b;
      b.board.offsetMv = offset;
      CHECK(b.vpp.switchOn(12000) == Result::Ok);
      CHECK(near(b.board.outputMv, 12000));
   }
}

TEST_CASE("VPP: a step down waits for the discharge and keeps VPP_ON low") {
   Bench b;
   REQUIRE(b.vpp.switchOn(14000) == Result::Ok);
   b.board.events.clear();
   const uint32_t start = b.board.nowMs;
   CHECK(b.vpp.switchOn(5000) == Result::Ok);
   CHECK(near(b.board.outputMv, 5000));
   // 9 V at 13 mV/ms
   CHECK(b.board.nowMs - start > 600);
   CHECK(b.board.events.front() == "on 0");
   // VPP_ON goes high only after the level (pd 1, on 1, pd 0 at the end)
   CHECK(std::count(b.board.events.begin(), b.board.events.end(), "on 1") == 1);
   CHECK(b.board.events[b.board.events.size() - 2] == "on 1");
}

TEST_CASE("VPP: the levels 5 V, 12 V and 14 V") {
   for(const uint16_t mv : { 5000, 12000, 14000 }) {
      CAPTURE(mv);
      Bench b;
      CHECK(b.vpp.switchOn(mv) == Result::Ok);
      CHECK(near(b.board.outputMv, mv));
   }
}

TEST_CASE("VPP: a level out of range or above a limit is refused, VPP stays as it was") {
   Bench b;
   REQUIRE(b.vpp.switchOn(12000) == Result::Ok);
   b.board.events.clear();
   CHECK(b.vpp.switchOn(4999) == Result::Refused);
   CHECK(b.vpp.switchOn(14701) == Result::Refused);
   CHECK(b.vpp.switchOn(13001, 13000) == Result::Refused);
   CHECK(b.board.events.empty());
   CHECK(b.vpp.level() == 12000);
   CHECK(b.gate.checks == 1);
   CHECK(b.vpp.switchOn(13000, 13000) == Result::Ok);
   CHECK(b.board.outputMv <= 13000);
}

TEST_CASE("VPP: a refusal of the gate switches VPP off") {
   Bench b;
   REQUIRE(b.vpp.switchOn(12000) == Result::Ok);
   b.gate.limit.reset();
   CHECK(b.vpp.switchOn(12000) == Result::Refused);
   CHECK_FALSE(b.vpp.isOn());
   CHECK_FALSE(b.board.on);
   CHECK_FALSE(b.board.enable);
   CHECK(b.board.pullDown);
}

TEST_CASE("VPP: the limit of the gate refuses a higher level") {
   Bench b;
   b.gate.limit = 13000;
   CHECK(b.vpp.switchOn(13500) == Result::Refused);
   CHECK_FALSE(b.board.enable);
   CHECK(b.vpp.switchOn(12000) == Result::Ok);
}

TEST_CASE("VPP: no level in 2 s switches VPP off") {
   Bench b;
   // The board makes 13.6 V at most
   b.board.offsetMv = -500;
   const uint32_t start = b.board.nowMs;
   CHECK(b.vpp.switchOn(14500) == Result::NoLevel);
   CHECK(b.board.nowMs - start == protocol::Vpp::levelTimeoutMs);
   CHECK_FALSE(b.vpp.isOn());
   CHECK_FALSE(b.board.enable);
   CHECK(b.board.pullDown);
   CHECK(std::count(b.board.events.begin(), b.board.events.end(), "on 1") == 0);
}

TEST_CASE("VPP: a failed measurement switches VPP off") {
   Bench b;
   b.board.failSense = true;
   CHECK(b.vpp.switchOn(12000) == Result::NoLevel);
   CHECK_FALSE(b.board.enable);
   CHECK(std::count(b.board.events.begin(), b.board.events.end(), "on 1") == 0);
}

TEST_CASE("VPP: a short on the target pin (VPP_SENSE2) switches VPP off") {
   Bench b;
   b.board.shortedPin = true;
   CHECK(b.vpp.switchOn(12000) == Result::TargetFault);
   CHECK_FALSE(b.vpp.isOn());
   CHECK_FALSE(b.board.on);
   CHECK_FALSE(b.board.enable);
   CHECK(b.board.pullDown);
}

TEST_CASE("VPP: switch-off sets VPP_ON low, VPP_EN low, then VPP_PD high") {
   Bench b;
   REQUIRE(b.vpp.switchOn(12000) == Result::Ok);
   b.board.events.clear();
   b.vpp.switchOff();
   CHECK(b.board.events == std::vector<std::string>{ "on 0", "en 0", "pd 1", "dac off" });
   CHECK_FALSE(b.vpp.isOn());
   CHECK(b.vpp.level() == 0);
}

TEST_CASE("VPP: poll switches VPP off when the gate does not allow it any more") {
   Bench b;
   REQUIRE(b.vpp.switchOn(12000) == Result::Ok);
   b.vpp.poll();
   CHECK(b.vpp.isOn());
   b.gate.limit.reset();
   b.vpp.poll();
   CHECK_FALSE(b.vpp.isOn());
   CHECK_FALSE(b.board.enable);
}
