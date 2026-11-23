// SPDX-License-Identifier: GPL-3.0-or-later
// The order of the shifter direction pins against the MCU pins, and the
// SRST drive with the polarity of srstActive (board/TargetDrive.h).

#include "board/TargetDrive.h"
#include "board/boards/Mini.h"
#include "board/boards/OpenMuppV1.h"
#include "board/boards/YdCh32V307.h"

#include <doctest/doctest.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace {

using board::Pin;
using board::Port;
using board::ShifterGroups;
using board::SrstPinSetting;
using protocol::PinMode;

std::string name(Pin pin) {
   return std::string(1, static_cast<char>('A' + static_cast<int>(pin.port))) + std::to_string(pin.number);
}

// Records the steps of driveTargetOutputs(): "pins" for the MCU pins of the
// engine, "dir <pin>=<level>" for a direction pin, "out <pin>=<level>" and
// "in <pin>" for the other pins of the DIR_OUT group.
struct Recorder {
   std::vector<std::string> steps;

   void direction(Pin pin, bool level) {
      steps.push_back("dir " + name(pin) + (level ? "=1" : "=0"));
   }
   void output(Pin pin, bool level) {
      steps.push_back("out " + name(pin) + (level ? "=1" : "=0"));
   }
   void input(Pin pin) {
      steps.push_back("in " + name(pin));
   }

   void drive(const board::Definition &def, ShifterGroups groups, bool on) {
      board::driveTargetOutputs(def.debug, def.shifters, groups, on, *this, [this] { steps.emplace_back("pins"); });
   }
};

// v1: TRST PB9, TDI PB15, AUX1 PA2, AUX2 PA0
const std::vector<std::string> v1Idle{ "out B9=1", "out B15=1", "out A2=0", "out A0=0" };
const std::vector<std::string> v1Released{ "in B9", "in B15", "in A2", "in A0" };

std::vector<std::string> join(std::initializer_list<std::vector<std::string>> parts) {
   std::vector<std::string> all;
   for(const auto &part : parts) {
      all.insert(all.end(), part.begin(), part.end());
   }
   return all;
}

} // namespace

TEST_CASE("driveTargetOutputs sets all DIR_OUT group pins before DIR_OUT goes high") {
   Recorder r;
   r.drive(board::openMuppV1, ShifterGroups::Out, true);
   CHECK(r.steps == join({ v1Idle, { "pins", "dir B10=1" } }));
}

TEST_CASE("driveTargetOutputs sets both direction pins low before the MCU pins become inputs") {
   // Also after an engine without DIR_SWDIO: a JTAG init before can have
   // set it high (Versaloon has no port owner between its blocks).
   for(const auto groups : { ShifterGroups::Out, ShifterGroups::OutAndSwdio }) {
      Recorder r;
      r.drive(board::openMuppV1, groups, false);
      CHECK(r.steps == join({ { "dir B10=0", "dir C3=0", "pins" }, v1Released }));
   }
}

TEST_CASE("driveTargetOutputs for JTAG also sets DIR_SWDIO high") {
   Recorder r;
   r.drive(board::openMuppV1, ShifterGroups::OutAndSwdio, true);
   CHECK(r.steps == join({ v1Idle, { "pins", "dir B10=1", "dir C3=1" } }));
}

TEST_CASE("driveTargetOutputs without TRST skips it") {
   board::Definition def = board::openMuppV1;
   def.debug.trst = std::nullopt;
   Recorder r;
   r.drive(def, ShifterGroups::Out, true);
   CHECK(r.steps == std::vector<std::string>{ "out B15=1", "out A2=0", "out A0=0", "pins", "dir B10=1" });
}

TEST_CASE("driveTargetOutputs without shifters only sets the MCU pins") {
   REQUIRE_FALSE(board::ydCh32V307.shifters.has_value());
   Recorder r;
   r.drive(board::ydCh32V307, ShifterGroups::OutAndSwdio, true);
   r.drive(board::ydCh32V307, ShifterGroups::OutAndSwdio, false);
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
