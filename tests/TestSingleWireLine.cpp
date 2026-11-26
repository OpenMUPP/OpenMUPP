// SPDX-License-Identifier: GPL-3.0-or-later
// The direction logic of the single-wire UART (uart/SingleWireLine.h), on
// fake pins that record each change in order.

#include "uart/SingleWireLine.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

using Log = std::vector<std::string>;

// The state after the start-up: the shifter drives the target, the TX pin
// is the UART output, the receiver is on.
struct FakePins {
   void driveTarget(bool on) {
      drive = on;
      log.push_back(on ? "drive" : "release");
      contended = contended || contention();
   }
   void connectTx(bool on) {
      tx = on;
      log.push_back(on ? "tx-uart" : "tx-input");
      contended = contended || contention();
   }
   void enableReceiver(bool on) {
      receiver = on;
      log.push_back(on ? "rx-on" : "rx-off");
   }

   // The shifter and the TX pin of the MCU drive against each other.
   [[nodiscard]] bool contention() const {
      return !drive && tx;
   }

   bool drive = true;
   bool tx = true;
   bool receiver = true;
   // Set when a change gave contention, also for a short time
   bool contended = false;
   Log log;
};

struct Line {
   FakePins pins;
   uart::SingleWireLine<FakePins> line{ pins };
};

} // namespace

TEST_CASE("Single-wire line: normal mode changes no pin") {
   Line l;
   l.line.txStart();
   CHECK_FALSE(l.line.sending());
   l.line.txComplete();
   CHECK(l.pins.log.empty());
   CHECK_FALSE(l.line.singleWire());
}

TEST_CASE("Single-wire line: the switch releases the target, TX pin first") {
   Line l;
   l.line.setSingleWire(true, false);
   CHECK(l.line.singleWire());
   CHECK_FALSE(l.line.sending());
   CHECK(l.pins.log == Log{ "tx-input", "release", "rx-on" });
   CHECK_FALSE(l.pins.contention());
}

TEST_CASE("Single-wire line: a transmission drives the line with the receiver off") {
   Line l;
   l.line.setSingleWire(true, false);
   l.pins.log.clear();

   l.line.txStart();
   CHECK(l.line.sending());
   CHECK(l.pins.log == Log{ "rx-off", "drive", "tx-uart" });
   CHECK(l.pins.drive);
   CHECK_FALSE(l.pins.receiver);

   // The next block of the same transmission changes nothing.
   l.pins.log.clear();
   l.line.txStart();
   CHECK(l.pins.log.empty());

   l.line.txComplete();
   CHECK_FALSE(l.line.sending());
   CHECK(l.pins.log == Log{ "tx-input", "release", "rx-on" });
   CHECK_FALSE(l.pins.drive);
   CHECK(l.pins.receiver);

   // A second TC (no transmission) changes nothing.
   l.pins.log.clear();
   l.line.txComplete();
   CHECK(l.pins.log.empty());
}

TEST_CASE("Single-wire line: no pin change gives contention") {
   Line l;
   l.line.setSingleWire(true, false);
   l.line.txStart();
   l.line.txComplete();
   l.line.setSingleWire(false, false);
   l.line.setSingleWire(true, true);
   l.line.txComplete();
   l.line.txStart();
   l.line.setSingleWire(false, true);
   l.line.txComplete();
   CHECK_FALSE(l.pins.contended);
}

TEST_CASE("Single-wire line: the switch back to normal drives the target") {
   Line l;
   l.line.setSingleWire(true, false);
   l.pins.log.clear();
   l.line.setSingleWire(false, false);
   CHECK_FALSE(l.line.singleWire());
   CHECK(l.pins.log == Log{ "drive", "tx-uart" });
   CHECK(l.pins.drive);
   CHECK(l.pins.tx);
   CHECK(l.pins.receiver);
}

TEST_CASE("Single-wire line: the same mode again changes nothing") {
   Line l;
   l.line.setSingleWire(false, false);
   CHECK(l.pins.log.empty());
   l.line.setSingleWire(true, false);
   l.pins.log.clear();
   l.line.setSingleWire(true, true);
   CHECK(l.pins.log.empty());
   CHECK_FALSE(l.line.sending());
}

TEST_CASE("Single-wire line: a switch while the UART sends waits for the end") {
   Line l;
   l.line.setSingleWire(true, true);
   CHECK(l.line.singleWire());
   CHECK(l.line.sending());
   CHECK(l.pins.log.empty());
   l.line.txComplete();
   CHECK(l.pins.log == Log{ "tx-input", "release", "rx-on" });
}

TEST_CASE("Single-wire line: a switch to normal while the line sends turns the receiver on at the end") {
   Line l;
   l.line.setSingleWire(true, false);
   l.line.txStart();
   l.pins.log.clear();
   l.line.setSingleWire(false, true);
   CHECK(l.pins.log.empty());
   CHECK(l.line.sending());
   l.line.txComplete();
   CHECK(l.pins.log == Log{ "rx-on" });
   CHECK(l.pins.drive);
   CHECK(l.pins.tx);
   CHECK(l.pins.receiver);
   // Normal mode again: a transmission changes no pin.
   l.pins.log.clear();
   l.line.txStart();
   l.line.txComplete();
   CHECK(l.pins.log.empty());
}

TEST_CASE("Single-wire line: off and on again while the line sends releases it at the end") {
   Line l;
   l.line.setSingleWire(true, false);
   l.line.txStart();
   l.line.setSingleWire(false, true);
   l.line.setSingleWire(true, true);
   l.pins.log.clear();
   l.line.txComplete();
   CHECK(l.pins.log == Log{ "tx-input", "release", "rx-on" });
}
