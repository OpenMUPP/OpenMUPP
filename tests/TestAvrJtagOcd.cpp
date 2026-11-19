// SPDX-License-Identifier: GPL-3.0-or-later
// The low level of the AVR JTAG OCD (avr/AvrJtagOcd.h) on the TAP layer,
// the JTAG bit-bang engine with mock pins and the OCD model of
// MockAvrJtag.h.

#include "avr/AvrJtagOcd.h"
#include "avr/AvrJtagTap.h"
#include "jtag/JtagBitBang.h"

#include "MockAvrJtag.h"
#include "MockJtagPins.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

using avr::AvrJtagOcd;
using mupptest::MockAvrJtag;
namespace ir = avr::tap::ir;
namespace reg = avr::jtagOcd::reg;
namespace bcr = avr::jtagOcd::bcr;
namespace bsr = avr::jtagOcd::bsr;
namespace control = avr::jtagOcd::control;

using Pins = mupptest::MockJtagPins<false>;

// AVR instructions
constexpr uint16_t ldi(uint8_t d, uint8_t k) {
   return static_cast<uint16_t>(0xE000u | ((k & 0xF0u) << 4) | ((d - 16u) << 4) | (k & 0x0Fu));
}

constexpr uint16_t mov(uint8_t d, uint8_t r) {
   return static_cast<uint16_t>(0x2C00u | ((r & 0x10u) << 5) | (d << 4) | (r & 0x0Fu));
}

constexpr uint16_t in(uint8_t d, uint8_t a) {
   return static_cast<uint16_t>(0xB000u | ((a & 0x30u) << 5) | (d << 4) | (a & 0x0Fu));
}

constexpr uint16_t out(uint8_t a, uint8_t r) {
   return static_cast<uint16_t>(0xB800u | ((a & 0x30u) << 5) | (r << 4) | (a & 0x0Fu));
}

constexpr uint16_t ldZ(uint8_t d) {
   return static_cast<uint16_t>(0x8000u | (d << 4));
}

constexpr uint16_t stY(uint8_t r) {
   return static_cast<uint16_t>(0x8208u | (r << 4));
}

constexpr uint16_t eijmp = 0x9419;
constexpr uint16_t spm = 0x95E8;
constexpr uint8_t ocdDataReg = 0x31;

static_assert(ldi(31, 0x02) == 0xE0F2);

struct Bench {
   explicit Bench(uint8_t revision = 3) {
      target.ocdRevision = revision;
      ocd.setRevision(revision);
      REQUIRE(tap.open(1000));
      // Out of Run-Test/Idle, as after the attach of the probe: an idle
      // TCK cycle moves the PC of a stopped core.
      REQUIRE(tap.scanIr(ir::idcode, nullptr, avr::tap::End::Update));
   }

   // A scan that executes the instruction of the last execute()
   void flush() {
      uint16_t value = 0;
      REQUIRE(ocd.readRegister(reg::bsr, value));
   }

   // The OCD scans after the open
   std::vector<MockAvrJtag::DrScan> ocdScans() const {
      std::vector<MockAvrJtag::DrScan> scans;
      for(const auto &scan : target.drScans) {
         if(scan.ir == ir::ocdAccess || scan.ir == ir::execute) {
            scans.push_back(scan);
         }
      }
      return scans;
   }

   // Reads r through the OCDR, as docs/avr-jtag-ocd.md
   uint8_t readCpuRegister(uint8_t r) {
      REQUIRE(ocd.execute(out(ocdDataReg, r)));
      uint16_t value = 0;
      REQUIRE(ocd.readRegister(reg::control, value));
      REQUIRE((value & control::ocdrFull) != 0);
      REQUIRE(ocd.readRegister(reg::ocdr, value));
      return static_cast<uint8_t>(value >> 8);
   }

   MockAvrJtag target;
   Pins pins{ target };
   jtag::BitBangPort<Pins> port{ pins };
   avr::AvrJtagTap tap{ port };
   AvrJtagOcd ocd{ tap };
};

} // namespace

TEST_CASE("AvrJtagOcd: a register write is one scan") {
   Bench b;
   REQUIRE(b.ocd.writeRegister(reg::break1, 0x00CC));
   CHECK(b.ocdScans() == std::vector<MockAvrJtag::DrScan>{ { ir::ocdAccess, 21, 0x1100CC } });
   CHECK(b.target.ocdRegs[1] == 0x00CC);
}

TEST_CASE("AvrJtagOcd: a register read is two scans of the same read") {
   Bench b;
   REQUIRE(b.ocd.writeRegister(reg::bcr, 0xC000));
   uint16_t value = 0;
   REQUIRE(b.ocd.readRegister(reg::bcr, value));
   CHECK(value == 0xC000);
   CHECK(b.ocdScans() == std::vector<MockAvrJtag::DrScan>{
                         { ir::ocdAccess, 21, 0x18C000 }, { ir::ocdAccess, 21, 0x080000 }, { ir::ocdAccess, 21, 0x080000 } });
   // One IR scan for all three, after the IDCODE scan of the bench
   CHECK(b.target.irScans == std::vector<uint8_t>{ ir::idcode, ir::ocdAccess });
}

TEST_CASE("AvrJtagOcd: register numbers out of range") {
   Bench b;
   const std::size_t edges = b.pins.rising.size();
   uint16_t value = 0;
   CHECK_FALSE(b.ocd.writeRegister(16, 0));
   CHECK_FALSE(b.ocd.readRegister(16, value));
   CHECK(b.pins.rising.size() == edges);
}

TEST_CASE("AvrJtagOcd: a read fails when the result is for another register") {
   Bench b;
   uint16_t value = 0x1234;
   SUBCASE("no OCD answer (OCDEN not programmed)") {
      b.target.ocden = false;
      CHECK_FALSE(b.ocd.readRegister(reg::control, value));
   }
   SUBCASE("other register") {
      b.target.capture[ir::ocdAccess] = 0x090002;
      CHECK_FALSE(b.ocd.readRegister(reg::bcr, value));
   }
   SUBCASE("a write command") {
      b.target.capture[ir::ocdAccess] = 0x18C000;
      CHECK_FALSE(b.ocd.readRegister(reg::bcr, value));
   }
   CHECK(value == 0x1234);
}

TEST_CASE("AvrJtagOcd: reg 4 to 7 keep the low byte on revision 4 only") {
   for(const uint8_t revision : { uint8_t{ 3 }, uint8_t{ 4 } }) {
      CAPTURE(revision);
      Bench b(revision);
      REQUIRE(b.ocd.writeRegister(reg::breakHigh0 + 2, 0xFFFF));
      uint16_t value = 0;
      REQUIRE(b.ocd.readRegister(reg::breakHigh0 + 2, value));
      CHECK(value == (revision >= 4 ? 0x00FF : 0));
   }
}

TEST_CASE("AvrJtagOcd: FORCE_BREAK stops the core, RUN starts it") {
   Bench b;
   b.target.pc = 0x123;
   uint16_t value = 0;
   REQUIRE(b.ocd.writeRegister(reg::bcr, bcr::bit14));
   REQUIRE(b.ocd.readRegister(reg::control, value));
   CHECK(value == control::running);

   REQUIRE(b.ocd.forceBreak());
   CHECK(b.target.stopped);
   REQUIRE(b.ocd.readRegister(reg::control, value));
   CHECK(value == (control::stopped | control::stoppedAlso));
   REQUIRE(b.ocd.readRegister(reg::bsr, value));
   CHECK(value == bsr::forceBreak);
   // A write to the BSR does not change it.
   REQUIRE(b.ocd.writeRegister(reg::bsr, 0x4000));
   REQUIRE(b.ocd.readRegister(reg::bsr, value));
   CHECK(value == bsr::forceBreak);

   REQUIRE(b.ocd.run());
   CHECK_FALSE(b.target.stopped);
   CHECK(b.target.pc == 0x123);
   // Each call scans the IR, also with the same instruction.
   REQUIRE(b.ocd.forceBreak());
   REQUIRE(b.ocd.forceBreak());
   const std::vector<uint8_t> tail(b.target.irScans.end() - 2, b.target.irScans.end());
   CHECK(tail == std::vector<uint8_t>{ ir::forceBreak, ir::forceBreak });
   CHECK(b.target.stopped);
}

TEST_CASE("AvrJtagOcd: RUN with BCR bit 13 is one step") {
   Bench b;
   b.target.stop(0x40, bsr::forceBreak);
   REQUIRE(b.ocd.writeRegister(reg::bcr, bcr::bit15 | bcr::step));
   REQUIRE(b.ocd.run());
   CHECK(b.target.stopped);
   CHECK(b.target.pc == 0x41);
   uint16_t value = 0;
   REQUIRE(b.ocd.readRegister(reg::bsr, value));
   CHECK(value == bsr::step);
   REQUIRE(b.ocd.readRegister(reg::bcr, value));
   CHECK(value == bcr::bit15);
}

TEST_CASE("AvrJtagOcd: no OCD without OCDEN") {
   Bench b;
   b.target.ocden = false;
   REQUIRE(b.ocd.forceBreak());
   CHECK_FALSE(b.target.stopped);
   REQUIRE(b.ocd.writeRegister(reg::break1, 0x55));
   CHECK(b.target.ocdRegs[1] == 0);
}

TEST_CASE("AvrJtagOcd: AVR_RESET holds and releases the core") {
   Bench b;
   b.target.resetVector = 0x3800;
   b.target.stop(0x40, bsr::forceBreak);
   REQUIRE(b.ocd.reset(true));
   CHECK(b.target.inReset);
   CHECK_FALSE(b.target.stopped);
   REQUIRE(b.ocd.reset(false));
   CHECK_FALSE(b.target.inReset);
   CHECK(b.target.pc == 0x3800);
   CHECK(b.target.drScans.back() == MockAvrJtag::DrScan{ ir::avrReset, 1, 0 });
}

TEST_CASE("AvrJtagOcd: instruction execute scan layout of each revision") {
   SUBCASE("revision 4: 38 bits, the capture example") {
      Bench b(4);
      b.target.stop(0xCC, bsr::break1);
      REQUIRE(b.ocd.execute(ldi(31, 0x02)));
      CHECK(b.target.drScans.back() == MockAvrJtag::DrScan{ ir::execute, 38, 0x383C800000 });
   }
   SUBCASE("revision 3: 32 bits") {
      Bench b(3);
      b.target.stop(0xCC, bsr::break1);
      REQUIRE(b.ocd.execute(ldi(31, 0x02)));
      CHECK(b.target.drScans.back() == MockAvrJtag::DrScan{ ir::execute, 32, 0xE0F20000 });
   }
}

TEST_CASE("AvrJtagOcd: the PC field") {
   for(const uint8_t revision : { uint8_t{ 3 }, uint8_t{ 4 } }) {
      CAPTURE(revision);
      Bench b(revision);
      const uint32_t step = 2;
      b.target.stop(0xCC, bsr::break1);
      uint32_t field = 0;
      REQUIRE(b.ocd.execute(ldi(16, 0x00), &field));
      CHECK(field == 0xCD);
      REQUIRE(b.ocd.execute(ldi(17, 0x00), &field));
      CHECK(field == 0xCD + step);
      // EIJMP: the field is Z + 1, and the PC is EIND:Z.
      REQUIRE(b.ocd.execute(ldi(31, 0x00)));
      REQUIRE(b.ocd.execute(ldi(30, 0xBE)));
      REQUIRE(b.ocd.execute(eijmp));
      REQUIRE(b.ocd.execute(ldi(16, 0x00), &field));
      CHECK(field == 0xBF);
      CHECK(b.target.pc == 0xBE);
      CHECK(b.target.unknownInstructions.empty());
   }
}

TEST_CASE("AvrJtagOcd: a field that is not 0 reads a flash word") {
   Bench b(4);
   b.target.flash[0x198] = 0xE8;
   b.target.flash[0x199] = 0x2F;
   b.target.stop(0x105, bsr::forceBreak);
   // The two scans of docs/avr-jtag-ocd.md, "Read a flash word"
   uint16_t instruction = 0x4094;
   uint32_t field = 0xCC;
   REQUIRE(b.ocd.instructionScan(instruction, field));
   CHECK(field == 0x106);
   instruction = 0xFFFF;
   field = 0x106;
   REQUIRE(b.ocd.instructionScan(instruction, field));
   CHECK(instruction == 0x2FE8);
   CHECK(field == 0x106);
   CHECK(b.target.drScans.back() == MockAvrJtag::DrScan{ ir::execute, 38, 0x3FFFC00106 });
   CHECK(b.target.executed.empty());
}

TEST_CASE("AvrJtagOcd: the mock core reads and writes registers and memory") {
   Bench b;
   b.target.stop(0x40, bsr::forceBreak);
   b.target.data[0x3F + 0x20] = 0x35; // SREG
   b.target.data[0x0123] = 0xA5;

   REQUIRE(b.ocd.execute(ldi(16, 0x42)));
   REQUIRE(b.ocd.execute(mov(3, 16)));
   CHECK(b.readCpuRegister(3) == 0x42);
   uint16_t value = 0;
   REQUIRE(b.ocd.readRegister(reg::control, value));
   CHECK((value & control::ocdrFull) == 0);

   REQUIRE(b.ocd.execute(in(20, 0x3F)));
   CHECK(b.readCpuRegister(20) == 0x35);

   REQUIRE(b.ocd.execute(ldi(31, 0x01)));
   REQUIRE(b.ocd.execute(ldi(30, 0x23)));
   REQUIRE(b.ocd.execute(ldZ(7)));
   CHECK(b.readCpuRegister(7) == 0xA5);

   REQUIRE(b.ocd.execute(ldi(29, 0x02)));
   REQUIRE(b.ocd.execute(ldi(28, 0x00)));
   REQUIRE(b.ocd.execute(stY(16)));
   b.flush();
   CHECK(b.target.data[0x0200] == 0x42);

   REQUIRE(b.ocd.execute(0xFFFF));
   b.flush();
   CHECK(b.target.unknownInstructions == std::vector<uint16_t>{ 0xFFFF });
}

TEST_CASE("AvrJtagOcd: the mock core writes a flash page with SPM") {
   Bench b;
   b.target.stop(0x3F00, bsr::forceBreak);
   const auto spmCommand = [&](uint8_t command) {
      REQUIRE(b.ocd.execute(ldi(16, command)));
      REQUIRE(b.ocd.execute(stY(16)));
      REQUIRE(b.ocd.execute(spm));
      b.flush();
      CHECK((b.target.data[b.target.spmcsr] & 0x01u) == 0);
   };
   std::fill(b.target.flash.begin() + 0x100, b.target.flash.begin() + 0x180, 0x00);
   REQUIRE(b.ocd.execute(ldi(29, 0x00)));
   REQUIRE(b.ocd.execute(ldi(28, 0x57)));
   // Z = 0x0100, the page byte address
   REQUIRE(b.ocd.execute(ldi(31, 0x01)));
   REQUIRE(b.ocd.execute(ldi(30, 0x00)));
   spmCommand(0x03);
   CHECK(b.target.flash[0x100] == 0xFF);
   CHECK(b.target.flash[0x17F] == 0xFF);
   spmCommand(0x11);
   // Word at byte address 0x0102 = 0x9598
   REQUIRE(b.ocd.execute(ldi(30, 0x02)));
   REQUIRE(b.ocd.execute(ldi(16, 0x98)));
   REQUIRE(b.ocd.execute(mov(0, 16)));
   REQUIRE(b.ocd.execute(ldi(16, 0x95)));
   REQUIRE(b.ocd.execute(mov(1, 16)));
   spmCommand(0x01);
   REQUIRE(b.ocd.execute(ldi(30, 0x00)));
   spmCommand(0x05);
   CHECK(b.target.flash[0x102] == 0x98);
   CHECK(b.target.flash[0x103] == 0x95);
   CHECK(b.target.flash[0x100] == 0xFF);
   CHECK(b.target.spmPageErases == 1);
   CHECK(b.target.spmPageWrites == 1);
   CHECK(b.target.unknownInstructions.empty());
}

TEST_CASE("AvrJtagOcd: the core executes nothing while it runs") {
   Bench b;
   REQUIRE(b.ocd.execute(ldi(16, 0x42)));
   CHECK(b.target.data[16] == 0);
   CHECK(b.target.executed.empty());
}
