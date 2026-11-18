// SPDX-License-Identifier: GPL-3.0-or-later
// AVR JTAG debug mode (avr/AvrJtagDebug.h): attach, halt, run, step,
// status and reset with the sequences of docs/avr-jtag-ocd.md, on the
// JTAG bit-bang engine with mock pins and the OCD model of MockAvrJtag.h.

#include "avr/AvrJtagDebug.h"
#include "avr/AvrJtagOcd.h"
#include "avr/AvrJtagTap.h"
#include "jtag/JtagBitBang.h"

#include "MockAvrJtag.h"
#include "MockJtagPins.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using avr::JtagDebugStatus;
using mupptest::MockAvrJtag;
namespace ir = avr::tap::ir;
namespace bsr = avr::jtagOcd::bsr;

using Pins = mupptest::MockJtagPins<false>;

struct Bench {
   explicit Bench(uint8_t revision = 3) {
      target.ocdRevision = revision;
      ocd.setRevision(revision);
      REQUIRE(tap.open(1000));
   }

   // The OCD register writes (bit 20 set) since the last call
   std::vector<uint32_t> writes() {
      std::vector<uint32_t> result;
      for(std::size_t i = seen_; i < target.drScans.size(); ++i) {
         const auto &scan = target.drScans[i];
         if(scan.ir == ir::ocdAccess && (scan.data & avr::jtagOcd::registerWrite) != 0) {
            result.push_back(static_cast<uint32_t>(scan.data));
         }
      }
      seen_ = target.drScans.size();
      return result;
   }

   [[nodiscard]] bool hasIr(uint8_t instruction) const {
      return std::find(target.irScans.begin(), target.irScans.end(), instruction) != target.irScans.end();
   }

   MockAvrJtag target;
   Pins pins{ target };
   jtag::BitBangPort<Pins> port{ pins };
   avr::AvrJtagTap tap{ port };
   avr::AvrJtagOcd ocd{ tap };
   avr::AvrJtagDebug debug{ tap, ocd };

   private:
   std::size_t seen_{ 0 };
};

} // namespace

TEST_CASE("AvrJtagDebug: attach halts the core with the reference sequence") {
   Bench b;
   b.target.pc = 0x123;
   b.target.stopDelay = 3;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   CHECK(b.debug.ocdAnswers());
   CHECK(b.debug.isStopped());
   CHECK(b.target.stopped);
   CHECK(b.target.pc == 0x123);
   CHECK(b.target.bsr == bsr::forceBreak);
   // BCR 0: (0 & 0x9FFC) | 0x4000, & ~0x03E0, then 0xC000
   CHECK(b.writes() == std::vector<uint32_t>{ 0x184000, 0x184000, 0x18C000 });
   // IDCODE after the TAP reset: no IR scan, one 32-bit DR scan
   CHECK(b.target.drScans.front().ir == ir::idcode);
   CHECK(b.hasIr(ir::forceBreak));
   CHECK(b.target.ocdRegs[8] == 0xC000);
}

TEST_CASE("AvrJtagDebug: halt keeps BCR bit 15 and the reg 3 mode, and clears bits 9 to 5") {
   Bench b;
   b.target.ocdRegs[8] = 0x83FF;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   // 0x83FF & 0x9FFC | 0x4000 = 0xC3FC, then & ~0x03E0 = 0xC01C
   CHECK(b.writes() == std::vector<uint32_t>{ 0x18C3FC, 0x18C01C, 0x18C000 });
}

TEST_CASE("AvrJtagDebug: attach succeeds without OCDEN, OCD commands give NoOcd") {
   Bench b;
   b.target.ocden = false;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   CHECK_FALSE(b.debug.ocdAnswers());
   CHECK_FALSE(b.debug.isStopped());
   CHECK(b.debug.halt() == JtagDebugStatus::NoOcd);
   CHECK(b.debug.run() == JtagDebugStatus::NoOcd);
   CHECK(b.debug.step() == JtagDebugStatus::NoOcd);
   CHECK(b.debug.reset() == JtagDebugStatus::NoOcd);
   bool stopped = false;
   CHECK(b.debug.status(stopped) == JtagDebugStatus::NoOcd);
   CHECK_FALSE(b.hasIr(ir::forceBreak));
   CHECK_FALSE(b.hasIr(ir::run));

   // OCDEN programmed later (Bloom manage_ocden_fuse_bit)
   b.target.ocden = true;
   CHECK(b.debug.halt() == JtagDebugStatus::Ok);
   CHECK(b.debug.ocdAnswers());
   CHECK(b.target.stopped);
}

TEST_CASE("AvrJtagDebug: attach fails without an Atmel IDCODE") {
   Bench b;
   b.target.idcode = 0x4BA00477;
   CHECK(b.debug.attach() == JtagDebugStatus::NoTap);
   CHECK_FALSE(b.hasIr(ir::forceBreak));
}

TEST_CASE("AvrJtagDebug: halt times out when the core does not stop") {
   Bench b;
   b.target.stopDelay = 100'000;
   CHECK(b.debug.attach() == JtagDebugStatus::Timeout);
   CHECK_FALSE(b.debug.isStopped());
}

TEST_CASE("AvrJtagDebug: step executes one instruction") {
   Bench b;
   b.target.pc = 0x40;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   b.writes();
   REQUIRE(b.debug.step() == JtagDebugStatus::Ok);
   CHECK(b.debug.isStopped());
   CHECK(b.target.pc == 0x41);
   CHECK(b.target.bsr == bsr::step);
   // (0xC000 & 0x9FFC) | 0x2000, then after the stop (0xA000 & 0x9FFC) |
   // 0x4000 and the BCR read back with bit 2 clear
   CHECK(b.writes() == std::vector<uint32_t>{ 0x18A000, 0x18C000, 0x18C000 });
   CHECK(b.target.irScans.back() == ir::ocdAccess);
}

TEST_CASE("AvrJtagDebug: run, status and the stop at a breakpoint") {
   Bench b;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   b.writes();
   REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
   CHECK_FALSE(b.debug.isStopped());
   CHECK_FALSE(b.target.stopped);
   CHECK(b.writes().empty());

   bool stopped = true;
   REQUIRE(b.debug.status(stopped) == JtagDebugStatus::Ok);
   CHECK_FALSE(stopped);
   CHECK(b.writes().empty());

   b.target.stop(0x50, bsr::break1);
   REQUIRE(b.debug.status(stopped) == JtagDebugStatus::Ok);
   CHECK(stopped);
   CHECK(b.debug.isStopped());
   CHECK(b.writes() == std::vector<uint32_t>{ 0x18C000, 0x18C000 });

   // A second status of the same stop writes nothing.
   REQUIRE(b.debug.status(stopped) == JtagDebugStatus::Ok);
   CHECK(stopped);
   CHECK(b.writes().empty());
}

TEST_CASE("AvrJtagDebug: reset stops at the reset vector") {
   for(const uint8_t revision : { uint8_t{ 3 }, uint8_t{ 4 } }) {
      CAPTURE(revision);
      Bench b{ revision };
      b.target.resetVector = 0x3800;
      b.target.pc = 0x40;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      b.writes();
      REQUIRE(b.debug.reset() == JtagDebugStatus::Ok);
      CHECK(b.debug.isStopped());
      CHECK(b.target.stopped);
      CHECK_FALSE(b.target.inReset);
      CHECK(b.target.pc == 0x3800);
      CHECK(b.target.bsr == bsr::break3);
      CHECK(b.target.ocdRegs[8] == 0xC000);
      std::vector<uint32_t> expected{ 0x18C000, 0x18C298, 0x120000, 0x130000 };
      if(revision >= 4) {
         expected.insert(expected.end(), { 0x160000, 0x170000 });
      }
      expected.push_back(0x18C000);
      CHECK(b.writes() == expected);
      // AVR_RESET 1 after the first BCR write, 0 after the mask
      std::vector<uint64_t> resets;
      for(const auto &scan : b.target.drScans) {
         if(scan.ir == ir::avrReset) {
            resets.push_back(scan.data);
         }
      }
      CHECK(resets == std::vector<uint64_t>{ 1, 0 });
   }
}

TEST_CASE("AvrJtagDebug: reset of a running core") {
   Bench b;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
   REQUIRE(b.debug.reset() == JtagDebugStatus::Ok);
   CHECK(b.target.stopped);
   CHECK(b.target.pc == 0);
}

TEST_CASE("AvrJtagDebug: a reset timeout removes the match-all breakpoint") {
   Bench b;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   b.target.stopDelay = 100'000;
   CHECK(b.debug.reset() == JtagDebugStatus::Timeout);
   CHECK(b.target.ocdRegs[8] == 0xC000);
}
