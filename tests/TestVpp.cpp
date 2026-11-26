// SPDX-License-Identifier: GPL-3.0-or-later
// The VPP driver (protocol/Vpp.h) on fake pins, a fake DAC and a fake ADC.

#include "FakeVpp.h"
#include "protocol/Vpp.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

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
