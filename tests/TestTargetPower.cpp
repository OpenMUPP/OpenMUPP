// SPDX-License-Identifier: GPL-3.0-or-later
// The 3.3 V and 5 V selection of the target supply (protocol/TargetPower.h):
// the change sequence, the 5 V input check and the faults, on a fake board
// with two switches.

#include "FakePower.h"
#include "FakeTargetGate.h"
#include "protocol/TargetPower.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

using protocol::Supply;
using protocol::TargetPower;
using Result = TargetPower::Result;
using Selections = std::vector<Supply>;

struct Bench {
   Bench()
      : delay(hardware)
      , power(hardware, delay) {
      hardware.has5vSwitchPin = true;
   }

   // The sum of the delays with all switches off
   [[nodiscard]] unsigned offMs() const {
      unsigned sum = 0;
      for(const auto &d : delay.delays) {
         sum += d.switchOn ? 0 : d.ms;
      }
      return sum;
   }

   FakePower hardware;
   FakeDelay delay;
   TargetPower power;
};

} // namespace

TEST_CASE("Target supply: 5 V from off checks the target, then switches on and settles") {
   Bench b;
   CHECK(b.power.switchOn(Supply::V5) == Result::Ok);
   CHECK(b.power.supply() == Supply::V5);
   CHECK(b.hardware.selections == Selections{ Supply::V5 });
   REQUIRE(b.delay.delays.size() == 1);
   CHECK(b.delay.delays[0].ms == TargetPower::settleMs);
   CHECK(b.delay.delays[0].switchOn);
}

TEST_CASE("Target supply: 5 V is refused when the 5 V input is low or cannot be measured") {
   Bench b;
   SUBCASE("Low") {
      b.hardware.inputMv = TargetPower::min5vInputMillivolts - 1;
   }
   SUBCASE("No measurement") {
      b.hardware.inputMv = std::nullopt;
   }
   CHECK(b.power.switchOn(Supply::V5) == Result::Refused);
   CHECK(b.hardware.selections.empty());
   CHECK_FALSE(b.power.isOn());
}

TEST_CASE("Target supply: a low 5 V input keeps 3.3 V on") {
   Bench b;
   REQUIRE(b.power.switchOn(Supply::V3v3) == Result::Ok);
   b.hardware.inputMv = 4000;
   CHECK(b.power.switchOn(Supply::V5) == Result::Refused);
   CHECK(b.power.supply() == Supply::V3v3);
   CHECK(b.hardware.selections == Selections{ Supply::V3v3 });
}

TEST_CASE("Target supply: 5 V at the minimum input switches on") {
   Bench b;
   b.hardware.inputMv = TargetPower::min5vInputMillivolts;
   CHECK(b.power.switchOn(Supply::V5) == Result::Ok);
}

TEST_CASE("Target supply: 5 V is refused on a target with its own supply") {
   Bench b;
   b.hardware.externalMv = 3300;
   CHECK(b.power.switchOn(Supply::V5) == Result::Refused);
   CHECK(b.hardware.selections.empty());
}

TEST_CASE("Target supply: 5 V is not supported on a board with a 3.3 V switch only") {
   Bench b;
   b.hardware.has5vSwitchPin = false;
   CHECK(b.power.switchOn(Supply::V5) == Result::NotSupported);
   CHECK(b.hardware.selections.empty());
   CHECK(b.power.switchOn() == Result::Ok);
   CHECK(b.power.supply() == Supply::V3v3);
}

TEST_CASE("Target supply: 3.3 V to 5 V turns both off for 10 ms, without the external check") {
   Bench b;
   REQUIRE(b.power.switchOn(Supply::V3v3) == Result::Ok);
   b.delay.delays.clear();
   CHECK(b.power.switchOn(Supply::V5) == Result::Ok);
   CHECK(b.hardware.selections == Selections{ Supply::V3v3, Supply::Off, Supply::V5 });
   CHECK(b.offMs() == TargetPower::changeOffMs);
   CHECK(b.power.supply() == Supply::V5);
}

TEST_CASE("Target supply: 5 V to 3.3 V waits until the rail is below 3.4 V") {
   Bench b;
   REQUIRE(b.power.switchOn(Supply::V5) == Result::Ok);
   b.delay.delays.clear();
   b.hardware.dischargeReads = 20;
   CHECK(b.power.switchOn(Supply::V3v3) == Result::Ok);
   CHECK(b.hardware.selections == Selections{ Supply::V5, Supply::Off, Supply::V3v3 });
   // 10 ms off, then 1 ms for each of the 20 reads above 3.4 V
   CHECK(b.offMs() == TargetPower::changeOffMs + 20);
   CHECK(b.power.supply() == Supply::V3v3);
}

TEST_CASE("Target supply: 5 V to 3.3 V is refused, and the supply stays off, when the rail does not drop") {
   Bench b;
   REQUIRE(b.power.switchOn(Supply::V5) == Result::Ok);
   b.delay.delays.clear();
   b.hardware.dischargeReads = 10'000;
   CHECK(b.power.switchOn(Supply::V3v3) == Result::Refused);
   CHECK(b.hardware.selections == Selections{ Supply::V5, Supply::Off });
   CHECK_FALSE(b.power.isOn());
   CHECK(b.offMs() == TargetPower::changeOffMs + TargetPower::dischargeTimeoutMs);
}

TEST_CASE("Target supply: the same voltage again changes nothing") {
   Bench b;
   REQUIRE(b.power.switchOn(Supply::V5) == Result::Ok);
   CHECK(b.power.switchOn(Supply::V5) == Result::Ok);
   CHECK(b.hardware.selections == Selections{ Supply::V5 });
}

TEST_CASE("Target supply: a change into a short circuit goes off and is refused") {
   Bench b;
   REQUIRE(b.power.switchOn(Supply::V3v3) == Result::Ok);
   b.hardware.loadedMv = 200;
   CHECK(b.power.switchOn(Supply::V5) == Result::Refused);
   CHECK(b.hardware.selections == Selections{ Supply::V3v3, Supply::Off, Supply::V5, Supply::Off });
   CHECK_FALSE(b.power.isOn());
}

TEST_CASE("Target supply: poll() takes a fault, counts it and reports it once") {
   Bench b;
   REQUIRE(b.power.switchOn(Supply::V5) == Result::Ok);
   b.hardware.fault();
   CHECK(b.power.isOn());
   b.power.poll(0);
   CHECK_FALSE(b.power.isOn());
   CHECK(b.power.faultCount() == 1);
   CHECK(b.power.takeFaultReport());
   CHECK_FALSE(b.power.takeFaultReport());
   b.power.poll(100);
   CHECK(b.power.faultCount() == 1);
}

TEST_CASE("Target supply: a switch-on after a fault does the full sequence") {
   Bench b;
   REQUIRE(b.power.switchOn(Supply::V5) == Result::Ok);
   b.hardware.fault();
   // Not taken by poll() yet: the switch-on takes it.
   b.hardware.externalMv = 0;
   CHECK(b.power.switchOn(Supply::V5) == Result::Ok);
   CHECK(b.power.faultCount() == 1);
   CHECK(b.hardware.selections == Selections{ Supply::V5, Supply::V5 });
}

TEST_CASE("Target supply: switchOn(Off) switches off") {
   Bench b;
   REQUIRE(b.power.switchOn(Supply::V5) == Result::Ok);
   CHECK(b.power.switchOn(Supply::Off) == Result::Ok);
   CHECK_FALSE(b.power.isOn());
   CHECK(b.hardware.selections == Selections{ Supply::V5, Supply::Off });
}

TEST_CASE("Target supply: the target gate refuses the switch-on and switches off") {
   Bench b;
   mupptest::FakeTargetGate gate;
   b.power.setTargetGate(gate);
   CHECK(b.power.switchOn(Supply::V3v3) == Result::Ok);
   CHECK(gate.checks == std::vector{ protocol::TargetProtocol::None });
   gate.allow = false;
   CHECK(b.power.switchOn(Supply::V3v3) == Result::Refused);
   CHECK_FALSE(b.power.isOn());
   CHECK(b.hardware.selections == Selections{ Supply::V3v3, Supply::Off });
   CHECK(b.power.switchOn(Supply::V5) == Result::Refused);
   CHECK(b.hardware.selections.size() == 2);
}
