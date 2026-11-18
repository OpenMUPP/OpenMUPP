// SPDX-License-Identifier: GPL-3.0-or-later
// The TAP layer of the AVR JTAG port (avr/AvrJtagTap.h) on the JTAG
// bit-bang engine with mock pins and the AVR TAP model (MockAvrJtag.h).

#include "avr/AvrJtagTap.h"
#include "jtag/JtagBitBang.h"

#include "MockAvrJtag.h"
#include "MockJtagPins.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

using avr::AvrJtagTap;
using mupptest::MockAvrJtag;
using State = MockAvrJtag::State;
namespace ir = avr::tap::ir;

static_assert(avr::tap::isAtmelIdcode(0x4958703F));
static_assert(!avr::tap::isAtmelIdcode(0x4BA00477));
static_assert(!avr::tap::isAtmelIdcode(0xFFFFFFFF));
static_assert(avr::tap::partNumber(0x4958703F) == 0x9587);
static_assert(avr::tap::partNumber(0x5980103F) == 0x9801);

// With RTCK: the TAP must refuse adaptive clocking all the same.
using Pins = mupptest::MockJtagPins<true>;

struct Bench {
   MockAvrJtag target;
   Pins pins{ target };
   jtag::BitBangPort<Pins> port{ pins };
   AvrJtagTap tap{ port };
};

// A port whose shifts fail, as after an RTCK timeout
class FailingPort final : public protocol::JtagPort {
   public:
   void init() override {
   }
   void fini() override {
      ++finis;
   }
   [[nodiscard]] bool setSpeed(uint32_t) override {
      return true;
   }
   [[nodiscard]] bool shift(uint32_t, art::span<const uint8_t>, art::span<const uint8_t>, art::span<uint8_t>) override {
      ++shifts;
      return !fail;
   }
   bool fail{ false };
   int shifts{ 0 };
   int finis{ 0 };
};

} // namespace

TEST_CASE("AvrJtagTap: open drives the pins, sets TCK and resets the TAP") {
   Bench b;
   b.target.state = State::SelectDr;
   b.target.ir = ir::ocdAccess;
   REQUIRE(b.tap.open(200));
   CHECK(b.pins.inits == 1);
   CHECK(b.port.timing().clocking == jtag::Clocking::Timed);
   CHECK(b.port.timing().halfCycles == jtag::halfPeriodCycles(b.pins.coreHz(), 200));
   CHECK(b.target.state == State::Idle);
   CHECK(b.target.ir == ir::idcode);
   CHECK(b.target.resetClocks >= 1);
   CHECK(b.tap.ir() == ir::idcode);
   CHECK(b.target.irScans.empty());
   CHECK(b.target.drScans.empty());
}

TEST_CASE("AvrJtagTap: no adaptive clocking") {
   Bench b;
   CHECK_FALSE(b.tap.open(0));
   CHECK(b.pins.rising.empty());
   // A failed open releases the pins.
   CHECK(b.pins.inits == 1);
   CHECK(b.pins.finis == 1);
   CHECK(b.tap.ir() == AvrJtagTap::noIr);
   CHECK_FALSE(b.tap.setSpeed(0));
   CHECK(b.tap.setSpeed(1000));
   CHECK(b.port.timing().halfCycles == jtag::halfPeriodCycles(b.pins.coreHz(), 1000));
}

TEST_CASE("AvrJtagTap: IDCODE after the reset needs no IR scan") {
   Bench b;
   REQUIRE(b.tap.open(1000));
   uint32_t idcode = 0;
   REQUIRE(b.tap.readIdcode(idcode));
   CHECK(idcode == 0x4958703F);
   CHECK(b.target.irScans.empty());
   REQUIRE(b.target.drScans.size() == 1);
   CHECK(b.target.drScans[0].ir == ir::idcode);
   CHECK(b.target.drScans[0].bits == 32);
   CHECK(b.target.state == State::Idle);
}

TEST_CASE("AvrJtagTap: IDCODE after another instruction") {
   Bench b;
   b.target.idcode = 0x5980103F;
   REQUIRE(b.tap.open(1000));
   REQUIRE(b.tap.setIr(ir::progEnable));
   uint32_t idcode = 0;
   REQUIRE(b.tap.readIdcode(idcode));
   CHECK(idcode == 0x5980103F);
   CHECK(b.target.irScans == std::vector<uint8_t>{ ir::progEnable, ir::idcode });
}

TEST_CASE("AvrJtagTap: setIr keeps the IR, scanIr always scans") {
   Bench b;
   REQUIRE(b.tap.open(1000));
   REQUIRE(b.tap.setIr(ir::ocdAccess));
   REQUIRE(b.tap.setIr(ir::ocdAccess));
   CHECK(b.target.irScans == std::vector<uint8_t>{ ir::ocdAccess });
   uint8_t captured = 0;
   REQUIRE(b.tap.scanIr(ir::forceBreak, &captured));
   CHECK(captured == MockAvrJtag::irCapture);
   REQUIRE(b.tap.scanIr(ir::forceBreak));
   CHECK(b.target.irScans == std::vector<uint8_t>{ ir::ocdAccess, ir::forceBreak, ir::forceBreak });
   CHECK(b.tap.ir() == ir::forceBreak);
   CHECK(b.target.state == State::Idle);

   b.tap.invalidate();
   REQUIRE(b.tap.setIr(ir::forceBreak));
   CHECK(b.target.irScans.size() == 4);
}

TEST_CASE("AvrJtagTap: DR scans of each length") {
   struct Case {
      uint8_t ir;
      uint8_t bits;
   };
   for(const Case c : { Case{ ir::avrReset, 1 }, Case{ ir::progPageLoad, 8 }, Case{ ir::progCommands, 15 },
                        Case{ ir::progEnable, 16 }, Case{ ir::ocdAccess, 21 }, Case{ ir::execute, 38 } }) {
      CAPTURE(c.bits);
      Bench b;
      // The 38-bit instruction execute scan of OCD revision 4
      b.target.ocdRevision = 4;
      const uint64_t mask = (uint64_t{ 1 } << c.bits) - 1u;
      b.target.capture[c.ir] = 0xA5C3'96F0'5A3Cu & mask;
      REQUIRE(b.tap.open(1000));
      uint64_t data = 0x3C5A'0F69'C35Au & mask;
      REQUIRE(b.tap.scanDr(c.ir, data, c.bits));
      CHECK(data == (0xA5C3'96F0'5A3Cu & mask));
      REQUIRE(b.target.drScans.size() == 1);
      CHECK(b.target.drScans[0] == MockAvrJtag::DrScan{ c.ir, c.bits, 0x3C5A'0F69'C35Au & mask });
      CHECK(b.target.state == State::Idle);
      // Scan length: 3 bits to Shift-DR, the data, 2 bits to Run-Test/Idle
      const std::size_t before = b.pins.rising.size();
      REQUIRE(b.tap.scanDr(data, c.bits));
      CHECK(b.pins.rising.size() - before == 3u + c.bits + 2u);
   }
}

TEST_CASE("AvrJtagTap: bits above the DR length are not sent") {
   Bench b;
   REQUIRE(b.tap.open(1000));
   uint64_t data = 0xFFFF'FFFF'FFFF'FFFFu;
   REQUIRE(b.tap.scanDr(ir::ocdAccess, data, 21));
   CHECK(b.target.drScans.back().data == 0x1FFFFF);
   CHECK(data == 0);
}

TEST_CASE("AvrJtagTap: DR scan lengths out of range") {
   Bench b;
   REQUIRE(b.tap.open(1000));
   const std::size_t before = b.pins.rising.size();
   uint64_t data = 0;
   CHECK_FALSE(b.tap.scanDr(data, 0));
   CHECK_FALSE(b.tap.scanDr(data, avr::tap::maxDrBits + 1));
   CHECK(b.pins.rising.size() == before);
   uint64_t all = 0x8000'0000'0000'0001u;
   b.target.capture[ir::bypass] = 0;
   REQUIRE(b.tap.setIr(ir::bypass));
   // BYPASS is 1 bit: 64 bits come back delayed by one bit.
   REQUIRE(b.tap.scanDr(all, avr::tap::maxDrBits));
   CHECK(all == 0x0000'0000'0000'0002u);
}

TEST_CASE("AvrJtagTap: a long DR scan of whole bytes") {
   Bench b;
   REQUIRE(b.tap.open(1000));
   CHECK(b.tap.kHz() == 1000);
   REQUIRE(b.tap.setIr(ir::progPageLoad));
   std::vector<uint8_t> data(20);
   for(std::size_t i = 0; i < data.size(); ++i) {
      data[i] = static_cast<uint8_t>(0x30 + i);
   }
   const std::size_t before = b.pins.rising.size();
   REQUIRE(b.tap.shiftDr(data));
   CHECK(b.pins.rising.size() - before == 3u + 8u * data.size() + 2u);
   CHECK(b.target.state == State::Idle);
   // The 8-bit DR of the model keeps the last byte.
   REQUIRE(b.target.drScans.size() == 1);
   CHECK(b.target.drScans[0] == MockAvrJtag::DrScan{ ir::progPageLoad, 8, 0x43 });
   CHECK_FALSE(b.tap.shiftDr({}));
   CHECK(b.target.drScans.size() == 1);
}

TEST_CASE("AvrJtagTap: a failed shift forgets the IR") {
   FailingPort port;
   AvrJtagTap tap(port);
   REQUIRE(tap.open(1000));
   REQUIRE(tap.setIr(ir::ocdAccess));
   port.fail = true;
   CHECK_FALSE(tap.scanIr(ir::run));
   CHECK(tap.ir() == AvrJtagTap::noIr);
   uint64_t data = 0;
   CHECK_FALSE(tap.scanDr(data, 21));
   uint32_t idcode = 0;
   CHECK_FALSE(tap.readIdcode(idcode));
   CHECK_FALSE(tap.reset());
   port.fail = false;
   const int shifts = port.shifts;
   REQUIRE(tap.setIr(ir::ocdAccess));
   CHECK(port.shifts == shifts + 1);
}

TEST_CASE("AvrJtagTap: a failed reset at open releases the pins") {
   FailingPort port;
   port.fail = true;
   AvrJtagTap tap(port);
   CHECK_FALSE(tap.open(1000));
   CHECK(port.finis == 1);
   CHECK(tap.ir() == AvrJtagTap::noIr);
}

TEST_CASE("AvrJtagTap: close releases the pins and forgets the IR") {
   Bench b;
   REQUIRE(b.tap.open(1000));
   b.tap.close();
   CHECK(b.pins.finis == 1);
   CHECK(b.tap.ir() == AvrJtagTap::noIr);
}
