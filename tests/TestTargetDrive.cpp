// SPDX-License-Identifier: GPL-3.0-or-later
// The order of the shifter direction pins against the MCU pins, and the
// SRST drive with the polarity of srstActive (board/TargetDrive.h).

#include "board/TargetDrive.h"
#include "board/boards/OpenMuppV1.h"
#include "board/boards/YdCh32V307.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

using board::Pin;
using board::Port;
using board::ShifterGroups;

// Records the steps of driveTargetOutputs(): "pins" for the MCU pins,
// "<pin>=<level>" for a direction pin.
struct Recorder {
   std::vector<std::string> steps;

   void drive(const std::optional<board::Shifters> &shifters, ShifterGroups groups, bool on) {
      board::driveTargetOutputs(
      shifters, groups, on,
      [this](Pin pin, bool level) {
         steps.push_back(std::string(1, static_cast<char>('A' + static_cast<int>(pin.port))) +
                         std::to_string(pin.number) + (level ? "=1" : "=0"));
      },
      [this] { steps.emplace_back("pins"); });
   }
};

} // namespace

TEST_CASE("driveTargetOutputs sets the MCU pins before DIR_OUT goes high") {
   Recorder r;
   r.drive(board::openMuppV1.shifters, ShifterGroups::Out, true);
   CHECK(r.steps == std::vector<std::string>{ "pins", "B10=1" });
}

TEST_CASE("driveTargetOutputs sets DIR_OUT low before the MCU pins become inputs") {
   Recorder r;
   r.drive(board::openMuppV1.shifters, ShifterGroups::Out, false);
   CHECK(r.steps == std::vector<std::string>{ "B10=0", "pins" });
}

TEST_CASE("driveTargetOutputs for JTAG also sets DIR_SWDIO") {
   Recorder r;
   r.drive(board::openMuppV1.shifters, ShifterGroups::OutAndSwdio, true);
   r.drive(board::openMuppV1.shifters, ShifterGroups::OutAndSwdio, false);
   CHECK(r.steps == std::vector<std::string>{ "pins", "B10=1", "C3=1", "B10=0", "C3=0", "pins" });
}

TEST_CASE("driveTargetOutputs without shifters only sets the MCU pins") {
   REQUIRE_FALSE(board::ydCh32V307.shifters.has_value());
   Recorder r;
   r.drive(board::ydCh32V307.shifters, ShifterGroups::OutAndSwdio, true);
   r.drive(board::ydCh32V307.shifters, ShifterGroups::OutAndSwdio, false);
   CHECK(r.steps == std::vector<std::string>{ "pins", "pins" });
}
