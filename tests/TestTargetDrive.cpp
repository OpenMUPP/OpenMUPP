// SPDX-License-Identifier: GPL-3.0-or-later
// The order of the shifter direction pins against the MCU pins, and the
// SRST drive with the polarity of srstActive (board/TargetDrive.h).

#include "board/TargetDrive.h"
#include "board/boards/Mini.h"
#include "board/boards/OpenMuppV1.h"
#include "board/boards/YdCh32V307.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

using board::Pin;
using board::Port;
using board::ShifterGroups;
using board::SrstPinSetting;
using protocol::PinMode;

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

TEST_CASE("SRST active low: the request goes to the pin unchanged") {
   using board::Active;
   for(const PinMode mode : { PinMode::Input, PinMode::InputPullUp, PinMode::InputPullDown, PinMode::PushPull, PinMode::OpenDrain }) {
      for(const bool level : { false, true }) {
         CHECK(board::srstPinSetting(Active::Low, mode, level) == SrstPinSetting{ mode, level });
      }
   }
}

TEST_CASE("SRST active high: push-pull with the inverse level, released is a low output") {
   const auto active = board::openMuppV1.debug.srstActive;
   REQUIRE(active == board::Active::High);
   // DAP_ResetTarget: assert, then release (open-drain high)
   CHECK(board::srstPinSetting(active, PinMode::OpenDrain, false) == SrstPinSetting{ PinMode::PushPull, true });
   CHECK(board::srstPinSetting(active, PinMode::OpenDrain, true) == SrstPinSetting{ PinMode::PushPull, false });
   // The Versaloon GPIO block can ask for a push-pull output.
   CHECK(board::srstPinSetting(active, PinMode::PushPull, false) == SrstPinSetting{ PinMode::PushPull, true });
   CHECK(board::srstPinSetting(active, PinMode::PushPull, true) == SrstPinSetting{ PinMode::PushPull, false });
   // DAP_Disconnect and the GPIO block release SRST as an input. A write
   // after that (BoardGpio::write() keeps the mode) does not assert it.
   for(const PinMode mode : { PinMode::Input, PinMode::InputPullUp, PinMode::InputPullDown }) {
      for(const bool level : { false, true }) {
         CHECK(board::srstPinSetting(active, mode, level) == SrstPinSetting{ PinMode::PushPull, false });
      }
   }
}

TEST_CASE("SRST read: the read-back input on OpenMUPP v1, the SRST pin elsewhere") {
   const auto &v1 = board::openMuppV1.debug;
   CHECK(board::srstReadPin(v1) == Pin{ Port::B, 11 });
   CHECK(board::srstLineLevel(v1, true));
   CHECK_FALSE(board::srstLineLevel(v1, false));
   // The YD board keeps the SRST pin: PB11 needs a wire for debugWIRE only.
   CHECK(board::srstReadPin(board::ydCh32V307.debug) == Pin{ Port::B, 12 });
   CHECK(board::srstReadPin(board::mini.debug) == board::mini.debug.srst);
   CHECK(board::srstLineLevel(board::mini.debug, true));
   // Active high without a read-back input: the pin level is inverse.
   board::DebugPins noSense = v1;
   noSense.srstSense = std::nullopt;
   CHECK(board::srstReadPin(noSense) == Pin{ Port::B, 12 });
   CHECK_FALSE(board::srstLineLevel(noSense, true));
   CHECK(board::srstLineLevel(noSense, false));
}
