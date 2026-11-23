// SPDX-License-Identifier: GPL-3.0-or-later
// The order of the shifter direction pins against the MCU pins, and the
// SRST drive with the polarity of srstActive (board/TargetDrive.h).

#include "board/TargetDrive.h"
#include "board/boards/Mini.h"
#include "board/boards/OpenMuppV1.h"
#include "board/boards/YdCh32V307.h"

#include <doctest/doctest.h>

#include <initializer_list>
#include <map>
#include <optional>
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
// The self-test steps (write, read, settle) are not in steps: levels holds
// the pin levels, and the read-back inputs of v1 read TCK and TDI, or
// stuckTdi.
struct Recorder {
   std::vector<std::string> steps;
   board::DirOutUsers users;
   std::map<std::string, bool> levels;
   std::optional<bool> stuckTdi;
   int settles = 0;
   bool result = true;

   void direction(Pin pin, bool level) {
      steps.push_back("dir " + name(pin) + (level ? "=1" : "=0"));
   }
   void output(Pin pin, bool level) {
      steps.push_back("out " + name(pin) + (level ? "=1" : "=0"));
      levels[name(pin)] = level;
   }
   void write(Pin pin, bool level) {
      levels[name(pin)] = level;
   }
   bool read(Pin pin) {
      const auto &shifters = *board::openMuppV1.shifters;
      if(pin == shifters.tckReadBack) {
         return levels[name(board::openMuppV1.debug.tck)];
      }
      if(pin == shifters.tdiReadBack) {
         return stuckTdi.value_or(levels[name(board::openMuppV1.debug.tdi)]);
      }
      return levels[name(pin)];
   }
   void settle() {
      ++settles;
   }
   void input(Pin pin) {
      steps.push_back("in " + name(pin));
   }

   void drive(const board::Definition &def, ShifterGroups groups, bool on) {
      result =
      board::driveTargetOutputs(def.debug, def.shifters, users, groups, on, *this, [this] { steps.emplace_back("pins"); });
   }
   // A gpio request: "pin" for setPin()
   void gpio(const board::Definition &def, uint16_t bit, bool output) {
      board::driveGpioPin(def.debug, def.shifters, users, bit, output, *this, [this] { steps.emplace_back("pin"); });
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

namespace gpio = protocol::gpio;

TEST_CASE("gpio output: the other DIR_OUT group pins get a level, then the pin, then DIR_OUT high") {
   Recorder r;
   r.gpio(board::openMuppV1, gpio::trst, true);
   // TCK PB13 low, TDI, AUX1, AUX2; not TRST itself
   CHECK(r.steps == std::vector<std::string>{ "out B13=0", "out B15=1", "out A2=0", "out A0=0", "pin", "dir B10=1" });
   r.steps.clear();
   // DIR_OUT is high: only the pin changes.
   r.gpio(board::openMuppV1, gpio::tck, true);
   CHECK(r.steps == std::vector<std::string>{ "pin" });
}

TEST_CASE("gpio input: DIR_OUT goes low with the last gpio output, before the pins become inputs") {
   Recorder r;
   r.gpio(board::openMuppV1, gpio::trst, true);
   r.gpio(board::openMuppV1, gpio::tck, true);
   r.steps.clear();
   // TCK keeps DIR_OUT high: TRST cannot be an input and gets its released level.
   r.gpio(board::openMuppV1, gpio::trst, false);
   CHECK(r.steps == std::vector<std::string>{ "out B9=1" });
   r.steps.clear();
   r.gpio(board::openMuppV1, gpio::tck, false);
   CHECK(r.steps == std::vector<std::string>{ "dir B10=0", "in B9", "in B15", "in A2", "in A0", "pin" });
   r.steps.clear();
   // DIR_OUT is low: only the pin changes.
   r.gpio(board::openMuppV1, gpio::tdi, false);
   CHECK(r.steps == std::vector<std::string>{ "pin" });
}

TEST_CASE("gpio input while an engine runs: the pin gets its released level") {
   Recorder r;
   r.drive(board::openMuppV1, ShifterGroups::OutAndSwdio, true);
   r.steps.clear();
   // OpenOCD (vsllink) releases TRST with an input request during JTAG.
   r.gpio(board::openMuppV1, gpio::trst, true);
   r.gpio(board::openMuppV1, gpio::trst, false);
   r.gpio(board::openMuppV1, gpio::tck, false);
   CHECK(r.steps == std::vector<std::string>{ "pin", "out B9=1", "out B13=0" });
}

TEST_CASE("an engine start keeps a TRST that the gpio asserted") {
   Recorder r;
   r.gpio(board::openMuppV1, gpio::trst, true);
   r.steps.clear();
   r.drive(board::openMuppV1, ShifterGroups::OutAndSwdio, true);
   CHECK(r.steps == std::vector<std::string>{ "pins", "dir C3=1" });
   r.steps.clear();
   // The engine end releases the gpio outputs too, TCK included.
   r.drive(board::openMuppV1, ShifterGroups::OutAndSwdio, false);
   CHECK(r.steps == join({ { "dir B10=0", "dir C3=0", "pins" }, v1Released }));
   CHECK_FALSE(r.users.any());
}

TEST_CASE("gpio TCK output: the engine end also makes TCK an input") {
   Recorder r;
   r.gpio(board::openMuppV1, gpio::tck, true);
   r.drive(board::openMuppV1, ShifterGroups::Out, true);
   r.steps.clear();
   r.drive(board::openMuppV1, ShifterGroups::Out, false);
   CHECK(r.steps == join({ { "dir B10=0", "dir C3=0", "pins", "in B13" }, v1Released }));
}

TEST_CASE("gpio TMS: DIR_SWDIO high after an output, low before an input") {
   Recorder r;
   r.gpio(board::openMuppV1, gpio::tms, true);
   r.gpio(board::openMuppV1, gpio::tms, false);
   CHECK(r.steps == std::vector<std::string>{ "pin", "dir C3=1", "dir C3=0", "pin" });
   CHECK_FALSE(r.users.any());
}

TEST_CASE("gpio pins without a shifter direction and boards without shifters: only the pin") {
   Recorder r;
   for(const uint16_t bit : { gpio::srst, gpio::tdo, gpio::rtck }) {
      r.gpio(board::openMuppV1, bit, true);
      r.gpio(board::openMuppV1, bit, false);
   }
   for(const uint16_t bit : { gpio::tck, gpio::tdi, gpio::trst, gpio::tms }) {
      r.gpio(board::ydCh32V307, bit, true);
      r.gpio(board::ydCh32V307, bit, false);
   }
   CHECK(r.steps == std::vector<std::string>(14, "pin"));
   CHECK_FALSE(r.users.any());
}

TEST_CASE("gpio open-drain output behind a shifter is push-pull") {
   const auto &v1 = board::openMuppV1.shifters;
   for(const uint16_t bit : { gpio::tck, gpio::tdi, gpio::trst, gpio::tms }) {
      CHECK(board::gpioPinMode(v1, bit, PinMode::OpenDrain) == PinMode::PushPull);
      CHECK(board::gpioPinMode(v1, bit, PinMode::InputPullUp) == PinMode::InputPullUp);
      CHECK(board::gpioPinMode(board::ydCh32V307.shifters, bit, PinMode::OpenDrain) == PinMode::OpenDrain);
   }
   // SRST has its own rule (srstPinSetting()).
   CHECK(board::gpioPinMode(v1, gpio::srst, PinMode::OpenDrain) == PinMode::OpenDrain);
}

TEST_CASE("connect self-test: TCK and TDI go to both levels, and end at the engine levels") {
   Recorder r;
   r.levels["B13"] = true; // the engine sets TCK high in setMcuPins()
   r.drive(board::openMuppV1, ShifterGroups::Out, true);
   CHECK(r.result);
   CHECK(r.settles == 4);
   CHECK(r.levels["B13"]);
   CHECK(r.levels["B15"]);
}

TEST_CASE("connect self-test: a read-back mismatch fails") {
   for(const bool stuck : { false, true }) {
      Recorder r;
      r.stuckTdi = stuck;
      r.drive(board::openMuppV1, ShifterGroups::OutAndSwdio, true);
      CHECK_FALSE(r.result);
      // The pins stay driven: the host sees the error in the transfers.
      CHECK(r.users.engine);
   }
}

TEST_CASE("connect self-test: at each engine start, also after a gpio output, and not without shifters") {
   Recorder r;
   // OpenOCD releases TRST with DAP_SWJ_Pins before the first transfer.
   r.gpio(board::openMuppV1, gpio::trst, true);
   r.drive(board::openMuppV1, ShifterGroups::Out, true);
   CHECK(r.result);
   CHECK(r.settles == 4);
   // A second engine start without an end (Versaloon blocks): no test.
   r.stuckTdi = false;
   r.drive(board::openMuppV1, ShifterGroups::Out, true);
   CHECK(r.result);
   CHECK(r.settles == 4);
   // A mismatch after a gpio output fails too.
   Recorder fail;
   fail.gpio(board::openMuppV1, gpio::trst, true);
   fail.stuckTdi = false;
   fail.drive(board::openMuppV1, ShifterGroups::Out, true);
   CHECK_FALSE(fail.result);
   Recorder yd;
   yd.drive(board::ydCh32V307, ShifterGroups::Out, true);
   CHECK(yd.result);
   CHECK(yd.settles == 0);
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
