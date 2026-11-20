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

TEST_CASE("AvrJtagDebug: attach keeps the PC of a core that is already stopped") {
   for(const uint8_t revision : { uint8_t{ 3 }, uint8_t{ 4 } }) {
      CAPTURE(revision);
      Bench b(revision);
      b.target.stop(0x50, bsr::forceBreak);
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      CHECK(b.debug.pc() == 0x50);
      REQUIRE(b.debug.step() == JtagDebugStatus::Ok);
      CHECK(b.target.pc == 0x51);
   }
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

namespace {

// Fills r0 to r31 of the mock with 0x40 + n, as the known-pattern test of
// the research.
void fillRegisters(MockAvrJtag &target) {
   for(uint8_t r = 0; r < 32; ++r) {
      target.data[r] = static_cast<uint8_t>(0x40 + r);
   }
}

std::vector<uint8_t> registers(const MockAvrJtag &target) {
   return { target.data.begin(), target.data.begin() + 32 };
}

constexpr uint16_t dataEind = 0x5C;

} // namespace

TEST_CASE("AvrJtagDebug: the context after a stop") {
   for(const uint8_t revision : { uint8_t{ 3 }, uint8_t{ 4 } }) {
      CAPTURE(revision);
      Bench b{ revision };
      fillRegisters(b.target);
      const auto pattern = registers(b.target);
      b.target.data[dataEind] = 0x01;
      b.target.pc = 0x123;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      CHECK(b.debug.pc() == 0x123);
      CHECK(b.debug.eind() == 0x01);
      std::vector<uint8_t> read(32);
      REQUIRE(b.debug.readData(0, read.data(), 32) == JtagDebugStatus::Ok);
      CHECK(read == pattern);
      // OUT OCDR, rN for each register first, then IN r16, EIND
      REQUIRE(b.target.executed.size() == 34);
      CHECK(b.target.executed[0] == avr::jtagDebug::out(0x31, 0));
      CHECK(b.target.executed[31] == avr::jtagDebug::out(0x31, 31));
      CHECK(b.target.executed[32] == avr::jtagDebug::in(16, 0x3C));
      CHECK(b.target.unknownInstructions.empty());
   }
}

TEST_CASE("AvrJtagDebug: the write-back before a step, in the reference order") {
   for(const uint8_t revision : { uint8_t{ 3 }, uint8_t{ 4 } }) {
      CAPTURE(revision);
      Bench b{ revision };
      fillRegisters(b.target);
      b.target.pc = 0x200;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      // The memory access changes r16, r30 and r31 on the target.
      uint8_t sram = 0;
      REQUIRE(b.debug.readData(0x100, &sram, 1) == JtagDebugStatus::Ok);
      const uint8_t r5 = 0xA5;
      REQUIRE(b.debug.writeData(5, &r5, 1) == JtagDebugStatus::Ok);
      CHECK(b.target.data[5] == 0x45);
      b.debug.setPc(0xC9);
      b.target.executed.clear();
      REQUIRE(b.debug.step() == JtagDebugStatus::Ok);

      // The context read after the stop changes r16 (IN r16, EIND).
      auto expected = registers(b.target);
      fillRegisters(b.target);
      std::vector<uint8_t> pattern = registers(b.target);
      pattern[5] = 0xA5;
      pattern[16] = b.target.data[dataEind];
      CHECK(expected == pattern);
      CHECK(b.target.pc == 0xCA);
      CHECK(b.debug.pc() == 0xCA);
      CHECK(b.target.bsr == bsr::step);
      // The PC field at RUN: Z + 1 + 10, also on revision 3
      CHECK(b.target.runFields.back() == 0xC9u);

      // r0 to r15 with LDI r16 and MOV, r16 to r31 with LDI
      namespace d = avr::jtagDebug;
      REQUIRE(b.target.executed.size() >= 48 + 6 + 5);
      CHECK(b.target.executed[0] == d::ldi(16, 0x40));
      CHECK(b.target.executed[1] == d::mov(0, 16));
      CHECK(b.target.executed[11] == d::mov(5, 16));
      CHECK(b.target.executed[10] == d::ldi(16, 0xA5));
      CHECK(b.target.executed[32] == d::ldi(16, 0x50));
      CHECK(b.target.executed[47] == d::ldi(31, 0x5F));
      const uint16_t z = 0xC9 - 11;
      const std::vector<uint16_t> pcWrite{ d::ldi(31, 0),    d::ldi(30, static_cast<uint8_t>(z)),
                                           d::wdr,           d::ldi(16, 0),
                                           d::out(0x3C, 16), d::eijmp,
                                           d::ldi(31, 0x5F), d::ldi(30, 0x5E),
                                           d::ldi(16, 0),    d::out(0x3C, 16),
                                           d::ldi(16, 0x50) };
      CHECK(std::vector<uint16_t>(b.target.executed.begin() + 48, b.target.executed.begin() + 59) == pcWrite);
   }
}

TEST_CASE("AvrJtagDebug: PC writes at 0 and above 64 K words") {
   SUBCASE("PC 0 wraps") {
      for(const uint8_t revision : { uint8_t{ 3 }, uint8_t{ 4 } }) {
         CAPTURE(revision);
         Bench b{ revision };
         b.target.pc = 0x40;
         REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
         b.debug.setPc(0);
         REQUIRE(b.debug.step() == JtagDebugStatus::Ok);
         CHECK(b.target.pc == 1);
      }
   }
   SUBCASE("revision 4: EIND") {
      Bench b{ 4 };
      b.target.flash.assign(262144, 0xFF);
      b.target.data[dataEind] = 0x00;
      b.target.pc = 0x1F000;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      CHECK(b.debug.pc() == 0x1F000);
      const uint8_t eind = 0x01;
      REQUIRE(b.debug.writeData(dataEind, &eind, 1) == JtagDebugStatus::Ok);
      b.debug.setPc(0x1F010);
      REQUIRE(b.debug.step() == JtagDebugStatus::Ok);
      CHECK(b.target.pc == 0x1F011);
      CHECK(b.target.data[dataEind] == 0x01);
   }
}

TEST_CASE("AvrJtagDebug: run writes back the context") {
   Bench b;
   fillRegisters(b.target);
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   const uint8_t r30 = 0x12;
   REQUIRE(b.debug.writeData(30, &r30, 1) == JtagDebugStatus::Ok);
   b.debug.setPc(0x80);
   REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
   CHECK_FALSE(b.target.stopped);
   CHECK(b.target.pc == 0x80);
   CHECK(b.target.data[30] == 0x12);
   CHECK(b.target.data[16] == 0x50);
   // The context needs a stop.
   uint8_t value = 0;
   CHECK(b.debug.readData(0x100, &value, 1) == JtagDebugStatus::TargetRunning);
   CHECK(b.debug.run() == JtagDebugStatus::TargetRunning);
   CHECK(b.debug.step() == JtagDebugStatus::TargetRunning);
}

TEST_CASE("AvrJtagDebug: halt of a stopped core keeps the context") {
   Bench b;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   const uint8_t r16 = 0x77;
   REQUIRE(b.debug.writeData(16, &r16, 1) == JtagDebugStatus::Ok);
   const std::size_t scans = b.target.drScans.size();
   REQUIRE(b.debug.halt() == JtagDebugStatus::Ok);
   CHECK(b.target.drScans.size() == scans);
   uint8_t value = 0;
   REQUIRE(b.debug.readData(16, &value, 1) == JtagDebugStatus::Ok);
   CHECK(value == 0x77);
}

TEST_CASE("AvrJtagDebug: the data space") {
   Bench b;
   fillRegisters(b.target);
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   b.target.data[0x100] = 0x11;
   b.target.data[0x101] = 0x22;
   b.target.data[0x25] = 0x33; // PORTB, I/O 0x05
   b.target.data[0x80] = 0x44; // extended I/O

   std::vector<uint8_t> read(2);
   REQUIRE(b.debug.readData(0x100, read.data(), 2) == JtagDebugStatus::Ok);
   CHECK(read == std::vector<uint8_t>{ 0x11, 0x22 });
   uint8_t value = 0;
   b.target.executed.clear();
   REQUIRE(b.debug.readData(0x25, &value, 1) == JtagDebugStatus::Ok);
   CHECK(value == 0x33);
   CHECK(b.target.executed == std::vector<uint16_t>{ avr::jtagDebug::in(16, 0x05), avr::jtagDebug::out(0x31, 16) });
   REQUIRE(b.debug.readData(0x80, &value, 1) == JtagDebugStatus::Ok);
   CHECK(value == 0x44);

   const std::vector<uint8_t> write{ 0xA1, 0xA2 };
   REQUIRE(b.debug.writeData(0x200, write.data(), 2) == JtagDebugStatus::Ok);
   CHECK(b.target.data[0x200] == 0xA1);
   CHECK(b.target.data[0x201] == 0xA2);
   const uint8_t port = 0x5A;
   REQUIRE(b.debug.writeData(0x25, &port, 1) == JtagDebugStatus::Ok);
   CHECK(b.target.data[0x25] == 0x5A);

   // The scratch registers come from the context, not from the target.
   CHECK(b.target.data[16] != 0x50);
   std::vector<uint8_t> scratch(32);
   REQUIRE(b.debug.readData(0, scratch.data(), 32) == JtagDebugStatus::Ok);
   CHECK(scratch[16] == 0x50);
   CHECK(scratch[30] == 0x5E);
   CHECK(scratch[31] == 0x5F);

   // A range across the registers and the I/O
   std::vector<uint8_t> across(4);
   REQUIRE(b.debug.readData(0x1E, across.data(), 4) == JtagDebugStatus::Ok);
   CHECK(across == std::vector<uint8_t>{ 0x5E, 0x5F, b.target.data[0x20], b.target.data[0x21] });

   CHECK(b.debug.readData(0xFFFF, read.data(), 2) == JtagDebugStatus::OutOfRange);
   CHECK(b.target.unknownInstructions.empty());
}

TEST_CASE("AvrJtagDebug: flash read") {
   for(const uint8_t revision : { uint8_t{ 3 }, uint8_t{ 4 } }) {
      CAPTURE(revision);
      Bench b{ revision };
      for(std::size_t i = 0; i < b.target.flash.size(); ++i) {
         b.target.flash[i] = static_cast<uint8_t>(i * 7 + 1);
      }
      b.target.pc = 0x40;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);

      SUBCASE("two scans for each word, no instruction executes") {
         b.target.executed.clear();
         const std::size_t from = b.target.drScans.size();
         std::vector<uint8_t> read(4);
         REQUIRE(b.debug.readFlash(0x200, read.data(), 4) == JtagDebugStatus::Ok);
         CHECK(read == std::vector<uint8_t>(b.target.flash.begin() + 0x200, b.target.flash.begin() + 0x204));
         CHECK(b.target.executed.empty());
         const std::vector<MockAvrJtag::DrScan> scans(b.target.drScans.begin() + static_cast<std::ptrdiff_t>(from),
                                                      b.target.drScans.end());
         REQUIRE(scans.size() == 4);
         const uint8_t field = avr::jtagOcd::fieldBits(revision);
         CHECK(scans[0].data == ((uint64_t{ 0xFFFF } << field) | 0x100));
         CHECK(scans[2].data == ((uint64_t{ 0xFFFF } << field) | 0x101));
         // The second scan sends the PC field.
         CHECK((scans[1].data & ((uint64_t{ 1 } << field) - 1)) == b.target.pcField);
      }
      SUBCASE("word 0 as the flash size, odd address and length") {
         std::vector<uint8_t> read(5);
         b.target.executed.clear();
         REQUIRE(b.debug.readFlash(0, read.data(), 5) == JtagDebugStatus::Ok);
         CHECK(read == std::vector<uint8_t>(b.target.flash.begin(), b.target.flash.begin() + 5));
         CHECK(b.target.executed.empty());
         std::vector<uint8_t> odd(3);
         REQUIRE(b.debug.readFlash(0x101, odd.data(), 3) == JtagDebugStatus::Ok);
         CHECK(odd == std::vector<uint8_t>(b.target.flash.begin() + 0x101, b.target.flash.begin() + 0x104));
         CHECK(b.target.unknownInstructions.empty());
      }
      SUBCASE("range") {
         uint8_t value = 0;
         CHECK(b.debug.readFlash(32768, &value, 1) == JtagDebugStatus::OutOfRange);
         CHECK(b.debug.readFlash(32767, &value, 1) == JtagDebugStatus::Ok);
      }
   }
}

TEST_CASE("AvrJtagDebug: EEPROM read and write") {
   Bench b;
   for(std::size_t i = 0; i < b.target.eeprom.size(); ++i) {
      b.target.eeprom[i] = static_cast<uint8_t>(i ^ 0x5A);
   }
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);

   std::vector<uint8_t> read(3);
   REQUIRE(b.debug.readEeprom(0x1FE, read.data(), 3) == JtagDebugStatus::Ok);
   CHECK(read == std::vector<uint8_t>(b.target.eeprom.begin() + 0x1FE, b.target.eeprom.begin() + 0x201));

   b.target.eepeBusyReads = 3;
   const std::vector<uint8_t> write{ 0x01, 0x02 };
   REQUIRE(b.debug.writeEeprom(0x300, write.data(), 2) == JtagDebugStatus::Ok);
   CHECK(b.target.eeprom[0x300] == 0x01);
   CHECK(b.target.eeprom[0x301] == 0x02);
   CHECK(b.target.eepromWrites == 2);

   uint8_t value = 0;
   CHECK(b.debug.readEeprom(1024, &value, 1) == JtagDebugStatus::OutOfRange);
   CHECK(b.debug.writeEeprom(1023, write.data(), 2) == JtagDebugStatus::OutOfRange);

   b.target.eepeBusyReads = 1'000'000;
   CHECK(b.debug.writeEeprom(0, write.data(), 1) == JtagDebugStatus::Timeout);
   CHECK(b.target.unknownInstructions.empty());
}

TEST_CASE("AvrJtagDebug: EEPROM with a one-byte address register") {
   Bench b;
   b.target.eearhIo = 0x21;
   b.debug.setDevice({ .flashBytes = 32768, .eepromBytes = 256, .eearh = 0x21, .eearl = 0x21 });
   b.target.eeprom[0x42] = 0x99;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   uint8_t value = 0;
   b.target.executed.clear();
   REQUIRE(b.debug.readEeprom(0x42, &value, 1) == JtagDebugStatus::Ok);
   CHECK(value == 0x99);
   namespace d = avr::jtagDebug;
   CHECK(b.target.executed.front() == d::ldi(16, 0x42));
   CHECK(b.debug.readEeprom(256, &value, 1) == JtagDebugStatus::OutOfRange);
}

TEST_CASE("AvrJtagDebug: a failed context read blocks run, step and halt until a reset") {
   Bench b;
   fillRegisters(b.target);
   b.target.ocdrStuck = true;
   CHECK(b.debug.attach() == JtagDebugStatus::Timeout);
   CHECK(b.debug.isStopped());
   b.target.ocdrStuck = false;
   b.target.executed.clear();
   const std::size_t irScans = b.target.irScans.size();
   CHECK(b.debug.run() == JtagDebugStatus::NoContext);
   CHECK(b.debug.step() == JtagDebugStatus::NoContext);
   CHECK(b.debug.halt() == JtagDebugStatus::NoContext);
   uint8_t value = 0;
   CHECK(b.debug.readData(0, &value, 1) == JtagDebugStatus::NoContext);
   CHECK(b.debug.readFlash(0x100, &value, 1) == JtagDebugStatus::NoContext);
   // No write-back, no RUN
   CHECK(b.target.executed.empty());
   CHECK(std::find(b.target.irScans.begin() + static_cast<std::ptrdiff_t>(irScans), b.target.irScans.end(), ir::run) ==
         b.target.irScans.end());
   CHECK(b.target.stopped);

   REQUIRE(b.debug.reset() == JtagDebugStatus::Ok);
   REQUIRE(b.debug.readData(5, &value, 1) == JtagDebugStatus::Ok);
   CHECK(value == 0x45);
   CHECK(b.debug.run() == JtagDebugStatus::Ok);
}

TEST_CASE("AvrJtagDebug: EEPROM write with adjacent OUTs and a read back") {
   Bench b;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   b.target.executed.clear();
   const uint8_t value = 0x3C;
   REQUIRE(b.debug.writeEeprom(0x10, &value, 1) == JtagDebugStatus::Ok);
   namespace d = avr::jtagDebug;
   const auto &e = b.target.executed;
   const auto first = std::find(e.begin(), e.end(), d::out(0x1F, 16));
   REQUIRE(first != e.end());
   REQUIRE(first + 1 != e.end());
   CHECK(*(first + 1) == d::out(0x1F, 31));
   CHECK(*(first - 1) == d::ldi(31, 0x06));
   CHECK(*(first - 2) == d::ldi(16, 0x04));

   SUBCASE("the hardware ignores EEPE") {
      b.target.eepeIgnored = true;
      const uint8_t other = 0x99;
      CHECK(b.debug.writeEeprom(0x11, &other, 1) == JtagDebugStatus::VerifyFailed);
      CHECK(b.target.eeprom[0x11] != 0x99);
   }
   SUBCASE("r31 comes back at the write-back") {
      fillRegisters(b.target);
      REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
      CHECK(b.target.data[31] == 0x00);
   }
}

TEST_CASE("AvrJtagDebug: hardware breakpoints 1 to 3") {
   struct Case {
      uint8_t number;
      uint16_t bcrBits;
      uint16_t cause;
   };
   for(const Case c : { Case{ 1, 0x0400, bsr::break1 }, Case{ 2, 0x0160, bsr::break2 }, Case{ 3, 0x0098, bsr::break3 } }) {
      CAPTURE(c.number);
      Bench b;
      b.target.pc = 0x40;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      CHECK(b.debug.breakStatus() == bsr::forceBreak);
      REQUIRE(b.debug.setBreakpoint(c.number, 0xCC) == JtagDebugStatus::Ok);
      b.writes();
      REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
      CHECK(b.writes() == std::vector<uint32_t>{ 0x100000u | (uint32_t{ c.number } << 16) | 0xCC, 0x18C000u | c.bcrBits });

      bool stopped = true;
      CHECK_FALSE(b.target.reach(0xCB));
      REQUIRE(b.debug.status(stopped) == JtagDebugStatus::Ok);
      CHECK_FALSE(stopped);
      CHECK(b.target.reach(0xCC));
      REQUIRE(b.debug.status(stopped) == JtagDebugStatus::Ok);
      CHECK(stopped);
      CHECK(b.debug.pc() == 0xCC);
      CHECK(b.debug.breakStatus() == c.cause);

      // Cleared: the next run removes the enable.
      REQUIRE(b.debug.clearBreakpoint(c.number) == JtagDebugStatus::Ok);
      REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
      CHECK((b.target.ocdRegs[8] & avr::jtagDebug::breakBits) == 0);
      CHECK_FALSE(b.target.reach(0xCC));
   }
}

TEST_CASE("AvrJtagDebug: three breakpoints, halt, and the next run") {
   Bench b;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   REQUIRE(b.debug.setBreakpoint(1, 0x100) == JtagDebugStatus::Ok);
   REQUIRE(b.debug.setBreakpoint(2, 0x200) == JtagDebugStatus::Ok);
   REQUIRE(b.debug.setBreakpoint(3, 0x300) == JtagDebugStatus::Ok);
   REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
   CHECK(b.target.ocdRegs[8] == 0xC5F8);
   // Halt clears the enables (reference sequence), the next run sets them.
   REQUIRE(b.debug.halt() == JtagDebugStatus::Ok);
   CHECK(b.target.ocdRegs[8] == 0xC000);
   REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
   CHECK(b.target.ocdRegs[8] == 0xC5F8);
   CHECK(b.target.reach(0x200));
   bool stopped = false;
   REQUIRE(b.debug.status(stopped) == JtagDebugStatus::Ok);
   CHECK(b.debug.breakStatus() == bsr::break2);
   CHECK(b.debug.pc() == 0x200);
}

TEST_CASE("AvrJtagDebug: run to an address with reg 0") {
   Bench b;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   b.writes();
   REQUIRE(b.debug.run(0x50) == JtagDebugStatus::Ok);
   CHECK(b.writes() == std::vector<uint32_t>{ 0x100050, 0x18C800 });
   CHECK(b.target.reach(0x50));
   bool stopped = false;
   REQUIRE(b.debug.status(stopped) == JtagDebugStatus::Ok);
   CHECK(b.debug.breakStatus() == bsr::break0);
   // The hit clears the enable. A plain run does not set it again.
   REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
   CHECK((b.target.ocdRegs[8] & avr::jtagOcd::bcr::enable0) == 0);
   CHECK_FALSE(b.target.reach(0x50));
}

TEST_CASE("AvrJtagDebug: breakpoint address bits 23..16 on revision 4 only") {
   SUBCASE("revision 4") {
      Bench b{ 4 };
      b.target.flash.assign(262144, 0xFF);
      b.debug.setDevice({ .flashBytes = 262144 });
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      REQUIRE(b.debug.setBreakpoint(1, 0x1F000) == JtagDebugStatus::Ok);
      b.writes();
      REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
      CHECK(b.writes() == std::vector<uint32_t>{ 0x11F000, 0x150001, 0x18C400 });
      CHECK_FALSE(b.target.reach(0xF000));
      CHECK(b.target.reach(0x1F000));
   }
   SUBCASE("revision 3") {
      Bench b{ 3 };
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      REQUIRE(b.debug.setBreakpoint(1, 0x1000) == JtagDebugStatus::Ok);
      b.writes();
      REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
      CHECK(b.writes() == std::vector<uint32_t>{ 0x111000, 0x18C400 });
   }
}

TEST_CASE("AvrJtagDebug: breakpoint numbers and addresses out of range") {
   Bench b;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   CHECK(b.debug.setBreakpoint(0, 0x10) == JtagDebugStatus::OutOfRange);
   CHECK(b.debug.setBreakpoint(4, 0x10) == JtagDebugStatus::OutOfRange);
   CHECK(b.debug.setBreakpoint(1, 0x4000) == JtagDebugStatus::OutOfRange);
   CHECK(b.debug.clearBreakpoint(0) == JtagDebugStatus::OutOfRange);
   CHECK(b.debug.run(0x4000) == JtagDebugStatus::OutOfRange);
   CHECK(b.target.stopped);
   CHECK(b.debug.setBreakpoint(1, 0x3FFF) == JtagDebugStatus::Ok);
}

TEST_CASE("AvrJtagDebug: data breakpoints in each mode") {
   using Mode = avr::JtagDataMode;
   struct Case {
      uint8_t number;
      Mode mode;
      uint16_t bcrBits;
      bool stopOnRead;
      bool stopOnWrite;
      uint16_t cause;
   };
   for(const Case c :
       { Case{ 2, Mode::Read, 0x0140, true, false, bsr::break2 }, Case{ 2, Mode::Write, 0x0120, false, true, bsr::break2 },
         Case{ 2, Mode::Access, 0x0100, true, true, bsr::break2 }, Case{ 3, Mode::Read, 0x0090, true, false, bsr::break3 },
         Case{ 3, Mode::Write, 0x0088, false, true, bsr::break3 }, Case{ 3, Mode::Access, 0x0080, true, true, bsr::break3 } }) {
      CAPTURE(c.number);
      CAPTURE(static_cast<int>(c.mode));
      for(const bool write : { false, true }) {
         CAPTURE(write);
         Bench b;
         REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
         REQUIRE(b.debug.setDataBreakpoint(c.number, 0x0210, c.mode) == JtagDebugStatus::Ok);
         b.writes();
         REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
         CHECK(b.writes() == std::vector<uint32_t>{ 0x100000u | (uint32_t{ c.number } << 16) | 0x0210, 0x18C000u | c.bcrBits });

         // Another address, and the program address 0x210, do not stop.
         CHECK_FALSE(b.target.access(0x41, 0x0211, write));
         CHECK_FALSE(b.target.reach(0x210));
         // An LDS or STS (two words) at 0x40: the stop is after it.
         const bool expected = write ? c.stopOnWrite : c.stopOnRead;
         CHECK(b.target.access(0x42, 0x0210, write) == expected);
         bool stopped = false;
         REQUIRE(b.debug.status(stopped) == JtagDebugStatus::Ok);
         CHECK(stopped == expected);
         if(expected) {
            CHECK(b.debug.pc() == 0x42);
            CHECK(b.debug.breakStatus() == c.cause);
         }
      }
   }
}

TEST_CASE("AvrJtagDebug: data breakpoint numbers") {
   using Mode = avr::JtagDataMode;
   Bench b;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   // Reg 1 has no data mode.
   CHECK(b.debug.setDataBreakpoint(1, 0x100, Mode::Read) == JtagDebugStatus::OutOfRange);
   CHECK(b.debug.setDataBreakpoint(4, 0x100, Mode::Read) == JtagDebugStatus::OutOfRange);
   // First come, first served
   REQUIRE(b.debug.setBreakpoint(2, 0x80) == JtagDebugStatus::Ok);
   CHECK(b.debug.setDataBreakpoint(2, 0x100, Mode::Read) == JtagDebugStatus::InUse);
   REQUIRE(b.debug.setDataBreakpoint(3, 0x100, Mode::Write) == JtagDebugStatus::Ok);
   CHECK(b.debug.setDataBreakpoint(3, 0x101, Mode::Write) == JtagDebugStatus::InUse);
   CHECK(b.debug.setBreakpoint(3, 0x80) == JtagDebugStatus::InUse);
   // A program breakpoint replaces a program breakpoint, as before.
   CHECK(b.debug.setBreakpoint(2, 0x90) == JtagDebugStatus::Ok);
   REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
   CHECK(b.target.ocdRegs[8] == 0xC1E8);
   CHECK(b.target.ocdRegs[2] == 0x90);
   CHECK(b.target.ocdRegs[3] == 0x100);
   // Cleared: the number is free for the other kind.
   REQUIRE(b.debug.halt() == JtagDebugStatus::Ok);
   REQUIRE(b.debug.clearBreakpoint(3) == JtagDebugStatus::Ok);
   CHECK(b.debug.setBreakpoint(3, 0x80) == JtagDebugStatus::Ok);
   REQUIRE(b.debug.clearBreakpoint(2) == JtagDebugStatus::Ok);
   CHECK(b.debug.setDataBreakpoint(2, 0x100, Mode::Access) == JtagDebugStatus::Ok);
}

TEST_CASE("AvrJtagDebug: a data breakpoint on revision 4 writes 0 to the high register") {
   Bench b{ 4 };
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   REQUIRE(b.debug.setDataBreakpoint(3, 0x0210, avr::JtagDataMode::Write) == JtagDebugStatus::Ok);
   b.writes();
   REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
   CHECK(b.writes() == std::vector<uint32_t>{ 0x130210, 0x170000, 0x18C088 });
   CHECK(b.target.access(0x101, 0x0210, true));
}

TEST_CASE("AvrJtagDebug: a data breakpoint with a mask (range)") {
   using Mode = avr::JtagDataMode;
   // AVaRICE puts the data breakpoint on number 2 and the mask on number 3.
   // The OCD has the mask in reg 2 only: reg 3 gets the address.
   for(const bool maskFirst : { false, true }) {
      CAPTURE(maskFirst);
      Bench b;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      if(maskFirst) {
         REQUIRE(b.debug.setDataMask(3, 0xFFFE) == JtagDebugStatus::Ok);
      }
      REQUIRE(b.debug.setDataBreakpoint(2, 0x0110, Mode::Read) == JtagDebugStatus::Ok);
      if(!maskFirst) {
         REQUIRE(b.debug.setDataMask(3, 0xFFFE) == JtagDebugStatus::Ok);
      }
      b.writes();
      REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
      // Reg 3 = 0x110, reg 2 = 0xFFFE, BCR: mask mode, reg 3 enable, mode 2
      CHECK(b.writes() == std::vector<uint32_t>{ 0x130110, 0x12FFFE, 0x18C290 });
      CHECK_FALSE(b.target.access(0x41, 0x0112, false));
      CHECK_FALSE(b.target.access(0x41, 0x0111, true));
      CHECK(b.target.access(0x42, 0x0111, false));
      bool stopped = false;
      REQUIRE(b.debug.status(stopped) == JtagDebugStatus::Ok);
      CHECK(stopped);
      CHECK(b.debug.breakStatus() == bsr::break3);
   }
}

TEST_CASE("AvrJtagDebug: data mask numbers") {
   using Mode = avr::JtagDataMode;
   Bench b;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   CHECK(b.debug.setDataMask(1, 0xFFFE) == JtagDebugStatus::OutOfRange);
   CHECK(b.debug.setDataMask(4, 0xFFFE) == JtagDebugStatus::OutOfRange);
   // No mask next to a program breakpoint, and no program breakpoint next
   // to a mask
   REQUIRE(b.debug.setBreakpoint(2, 0x80) == JtagDebugStatus::Ok);
   CHECK(b.debug.setDataMask(3, 0xFFFE) == JtagDebugStatus::InUse);
   REQUIRE(b.debug.clearBreakpoint(2) == JtagDebugStatus::Ok);
   REQUIRE(b.debug.setDataMask(2, 0xFFF0) == JtagDebugStatus::Ok);
   CHECK(b.debug.setDataMask(2, 0xFFF0) == JtagDebugStatus::InUse);
   // No second mask on the other number
   CHECK(b.debug.setDataMask(3, 0xFFFE) == JtagDebugStatus::InUse);
   CHECK(b.debug.setBreakpoint(2, 0x80) == JtagDebugStatus::InUse);
   CHECK(b.debug.setBreakpoint(3, 0x80) == JtagDebugStatus::InUse);
   CHECK(b.debug.setBreakpoint(1, 0x80) == JtagDebugStatus::Ok);
   // A mask without a data breakpoint: no mask mode, reg 2 not written
   b.writes();
   REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
   CHECK(b.writes() == std::vector<uint32_t>{ 0x110080, 0x18C400 });
   REQUIRE(b.debug.halt() == JtagDebugStatus::Ok);
   // Mask on number 2, data breakpoint on number 3, write mode
   REQUIRE(b.debug.setDataBreakpoint(3, 0x0200, Mode::Write) == JtagDebugStatus::Ok);
   REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
   CHECK(b.target.ocdRegs[8] == 0xC688);
   CHECK(b.target.ocdRegs[2] == 0xFFF0);
   CHECK(b.target.ocdRegs[3] == 0x0200);
   CHECK(b.target.access(0x42, 0x020F, true));
   bool stopped = false;
   REQUIRE(b.debug.status(stopped) == JtagDebugStatus::Ok);
   REQUIRE(stopped);
   // Cleared: no mask mode at the next run
   REQUIRE(b.debug.clearBreakpoint(2) == JtagDebugStatus::Ok);
   REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
   CHECK(b.target.ocdRegs[8] == 0xC488);
}

TEST_CASE("AvrJtagDebug: a clear of the data breakpoint clears its mask") {
   using Mode = avr::JtagDataMode;
   for(const uint8_t data : { 2, 3 }) {
      CAPTURE(data);
      const auto mask = static_cast<uint8_t>(5 - data);
      Bench b;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      REQUIRE(b.debug.setDataBreakpoint(data, 0x0110, Mode::Read) == JtagDebugStatus::Ok);
      REQUIRE(b.debug.setDataMask(mask, 0xFFFE) == JtagDebugStatus::Ok);
      REQUIRE(b.debug.clearBreakpoint(data) == JtagDebugStatus::Ok);
      CHECK(b.debug.setBreakpoint(data, 0x8E) == JtagDebugStatus::Ok);
      CHECK(b.debug.setBreakpoint(mask, 0x90) == JtagDebugStatus::Ok);
   }
   // A clear of an empty number keeps the mask for a later data breakpoint.
   Bench b;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   REQUIRE(b.debug.setDataMask(3, 0xFFFE) == JtagDebugStatus::Ok);
   REQUIRE(b.debug.clearBreakpoint(2) == JtagDebugStatus::Ok);
   CHECK(b.debug.setBreakpoint(2, 0x8E) == JtagDebugStatus::InUse);
}

TEST_CASE("AvrJtagDebug: a data mask on revision 4 writes 0 to the high registers") {
   Bench b{ 4 };
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   REQUIRE(b.debug.setDataBreakpoint(2, 0x0210, avr::JtagDataMode::Access) == JtagDebugStatus::Ok);
   REQUIRE(b.debug.setDataMask(3, 0xFFFC) == JtagDebugStatus::Ok);
   b.writes();
   REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
   CHECK(b.writes() == std::vector<uint32_t>{ 0x130210, 0x170000, 0x12FFFC, 0x160000, 0x18C280 });
}

TEST_CASE("AvrJtagDebug: the BSR after a step and a reset") {
   Bench b;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   REQUIRE(b.debug.step() == JtagDebugStatus::Ok);
   CHECK(b.debug.breakStatus() == bsr::step);
   REQUIRE(b.debug.reset() == JtagDebugStatus::Ok);
   CHECK(b.debug.breakStatus() == bsr::break3);
}

TEST_CASE("AvrJtagDebug: a step after a cleared breakpoint has no stale enable") {
   SUBCASE("breakpoint 1 hit, cleared, step (gdb step-over)") {
      Bench b;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      REQUIRE(b.debug.setBreakpoint(1, 0xCC) == JtagDebugStatus::Ok);
      REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
      REQUIRE(b.target.reach(0xCC));
      bool stopped = false;
      REQUIRE(b.debug.status(stopped) == JtagDebugStatus::Ok);
      CHECK(b.target.ocdRegs[8] == 0xC400);
      REQUIRE(b.debug.clearBreakpoint(1) == JtagDebugStatus::Ok);
      b.writes();
      REQUIRE(b.debug.step() == JtagDebugStatus::Ok);
      const auto writes = b.writes();
      REQUIRE(writes.size() >= 2);
      CHECK(writes[0] == 0x18C000);
      CHECK(writes[1] == 0x18A000);
      CHECK(b.target.pc == 0xCD);
   }
   SUBCASE("a run to that did not hit") {
      Bench b;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      REQUIRE(b.debug.setBreakpoint(1, 0x40) == JtagDebugStatus::Ok);
      REQUIRE(b.debug.run(0x80) == JtagDebugStatus::Ok);
      CHECK(b.target.ocdRegs[8] == 0xCC00);
      REQUIRE(b.target.reach(0x40));
      bool stopped = false;
      REQUIRE(b.debug.status(stopped) == JtagDebugStatus::Ok);
      CHECK(b.debug.breakStatus() == bsr::break1);
      b.writes();
      REQUIRE(b.debug.step() == JtagDebugStatus::Ok);
      const auto writes = b.writes();
      REQUIRE(writes.size() >= 3);
      // Reg 1 again, the BCR without bit 11, then the step
      CHECK(writes[0] == 0x110040);
      CHECK(writes[1] == 0x18C400);
      CHECK(writes[2] == 0x18A400);
   }
}

namespace {

constexpr uint16_t dataRampz = 0x5B;
constexpr uint16_t dataSreg = 0x5F;
constexpr uint16_t dataSpmcsr = 0x57;

std::vector<uint8_t> flashPattern(std::size_t bytes, uint8_t seed) {
   std::vector<uint8_t> page(bytes);
   for(std::size_t i = 0; i < bytes; ++i) {
      page[i] = static_cast<uint8_t>(seed + i * 3);
   }
   return page;
}

std::size_t count(const std::vector<uint16_t> &ops, uint16_t op) {
   return static_cast<std::size_t>(std::count(ops.begin(), ops.end(), op));
}

} // namespace

TEST_CASE("AvrJtagDebug: flash page write with SPM") {
   namespace d = avr::jtagDebug;
   for(const uint8_t revision : { uint8_t{ 3 }, uint8_t{ 4 } }) {
      CAPTURE(revision);
      Bench b{ revision };
      std::fill(b.target.flash.begin(), b.target.flash.end(), 0x5A);
      // The smallest boot section of the ATmega32U4: 256 words
      b.target.bootStartWords = 0x3F00;
      b.target.spmBusyReads = 5;
      fillRegisters(b.target);
      const auto pattern = registers(b.target);
      b.target.data[dataSreg] = 0x35;
      b.target.data[dataSpmcsr] = 0x80;
      b.target.data[dataRampz] = 0x77;
      b.target.pc = 0x123;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      b.target.executed.clear();
      b.target.irScans.clear();
      b.writes();

      const auto page = flashPattern(128, 0x10);
      REQUIRE(b.debug.writeFlashPage(0x100, page.data()) == JtagDebugStatus::Ok);
      CHECK(std::equal(page.begin(), page.end(), b.target.flash.begin() + 0x100));
      CHECK(b.target.flash[0xFF] == 0x5A);
      CHECK(b.target.flash[0x180] == 0x5A);
      CHECK(b.target.spmPageErases == 1);
      CHECK(b.target.spmPageWrites == 1);
      CHECK(b.target.unknownInstructions.empty());

      // 64 page buffer loads, then erase, write, RWW enable: the flash
      // changes only at the end. Each SPM is one step in the boot section.
      // The EIJMP to the last page (word 0x3FC0) comes only when the
      // moving PC leaves it (issue #121).
      constexpr std::size_t spms = 64 + 3;
      CHECK(count(b.target.executed, d::spm) == spms);
      CHECK(b.target.spmOutsideBoot == 0);
      CHECK(count(b.target.executed, d::eijmp) >= 1);
      CHECK(count(b.target.executed, d::eijmp) < spms);
      CHECK(static_cast<std::size_t>(std::count(b.target.irScans.begin(), b.target.irScans.end(), ir::run)) == spms);
      const auto writes = b.writes();
      CHECK(static_cast<std::size_t>(std::count(writes.begin(), writes.end(), 0x18A000u)) == spms);
      CHECK(writes.back() == 0x18C000);
      const auto &e = b.target.executed;
      // The SPMCSR command of each SPM: the last LDI r18 before it (r18 is
      // loaded only when the command changes)
      std::vector<uint16_t> commands;
      uint16_t command = 0;
      for(const uint16_t op : e) {
         if((op & 0xFFF0u & ~0x0F00u) == (d::ldi(18, 0) & 0xFFF0u & ~0x0F00u)) {
            command = op;
         } else if(op == d::spm) {
            commands.push_back(command);
         }
      }
      std::vector<uint16_t> order(64, d::ldi(18, 0x01));
      order.insert(order.end(), { d::ldi(18, 0x03), d::ldi(18, 0x05), d::ldi(18, 0x11) });
      CHECK(commands == order);
      const auto firstSpm = std::find(e.begin(), e.end(), d::spm);
      REQUIRE(firstSpm - e.begin() >= 4);
      CHECK(std::vector<uint16_t>(firstSpm - 2, firstSpm + 1) == std::vector<uint16_t>{ d::ldi(18, 0x01), d::stY(18), d::spm });
      // EIND and Y once for the page, then the first EIJMP
      CHECK(count(e, d::ldi(28, 0x57)) == 1);
      const auto firstJump = std::find(e.begin(), e.end(), d::eijmp);
      REQUIRE(firstJump - e.begin() >= 7);
      CHECK(std::vector<uint16_t>(firstJump - 7, firstJump) ==
            std::vector<uint16_t>{ d::ldi(16, 0x00), d::out(0x3C, 16), d::ldi(29, 0x00), d::ldi(28, 0x57),
                                   d::ldi(30, 0x00), d::ldi(31, 0x3F), d::ldi(30, 0xC0) });
      // A page buffer load: r1:r0 with MOVW, no SPMCSR poll after it
      CHECK(std::find(e.begin(), firstSpm, d::movw(0, 16)) != firstSpm);
      CHECK(std::find(e.begin(), firstSpm, d::ldi(16, 0x10)) != firstSpm);
      const auto secondSpm = std::find(firstSpm + 1, e.end(), d::spm);
      REQUIRE(secondSpm != e.end());
      CHECK(std::find(firstSpm, secondSpm, d::ldZ(16)) == secondSpm);

      // The state comes back: SREG, SPMCSR without the command bits,
      // RAMPZ not touched below 64 KB, then the registers at the run.
      CHECK(b.target.data[dataSreg] == 0x35);
      CHECK(b.target.data[dataSpmcsr] == 0x80);
      CHECK(b.target.data[dataRampz] == 0x77);
      CHECK(count(e, d::out(0x3B, 16)) == 0);
      CHECK(b.debug.pc() == 0x123);
      std::vector<uint8_t> read(128);
      REQUIRE(b.debug.readFlash(0x100, read.data(), 128) == JtagDebugStatus::Ok);
      CHECK(read == page);
      REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
      CHECK(registers(b.target) == pattern);
      CHECK(b.target.pc == 0x123);
   }
}

TEST_CASE("AvrJtagDebug: revision 3 reads no flash word above the 16-bit field") {
   Bench b{ 3 };
   // 0x18000 words: word 0 goes as 0x18000, which the field cannot hold.
   b.target.flash.assign(196608, 0xFF);
   b.target.flash[0x1FFFE] = 0x34;
   b.target.flash[0x1FFFF] = 0x12;
   b.target.flashPageBytes = 256;
   b.debug.setDevice({ .flashBytes = 196608, .flashPageBytes = 256 });
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   std::vector<uint8_t> read(2);
   REQUIRE(b.debug.readFlash(0x1FFFE, read.data(), 2) == JtagDebugStatus::Ok);
   CHECK(read == std::vector<uint8_t>{ 0x34, 0x12 });
   CHECK(b.debug.readFlash(0x1FFFE, read.data(), 4) == JtagDebugStatus::OutOfRange);
   CHECK(b.debug.readFlash(0x20000, read.data(), 2) == JtagDebugStatus::OutOfRange);
   CHECK(b.debug.readFlash(0x20002, read.data(), 2) == JtagDebugStatus::OutOfRange);
   CHECK(b.debug.readFlash(0, read.data(), 2) == JtagDebugStatus::OutOfRange);
   const auto page = flashPattern(256, 0x21);
   b.target.executed.clear();
   CHECK(b.debug.writeFlashPage(0x20100, page.data()) == JtagDebugStatus::OutOfRange);
   CHECK(b.debug.writeFlashPage(0, page.data()) == JtagDebugStatus::OutOfRange);
   CHECK(b.target.executed.empty());
   REQUIRE(b.debug.writeFlashPage(0x1FF00, page.data()) == JtagDebugStatus::Ok);
   CHECK(std::equal(page.begin(), page.end(), b.target.flash.begin() + 0x1FF00));
}

TEST_CASE("AvrJtagDebug: flash page write above 64 KB with RAMPZ") {
   Bench b{ 4 };
   b.target.flash.assign(262144, 0xFF);
   b.target.flashPageBytes = 256;
   b.target.data[dataRampz] = 0x01;
   b.debug.setDevice({ .flashBytes = 262144, .flashPageBytes = 256 });
   b.target.pc = 0x1F000;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   const auto page = flashPattern(256, 0x21);
   REQUIRE(b.debug.writeFlashPage(0x20100, page.data()) == JtagDebugStatus::Ok);
   CHECK(std::equal(page.begin(), page.end(), b.target.flash.begin() + 0x20100));
   CHECK(b.target.flash[0x0100] == 0xFF);
   CHECK(b.target.data[dataRampz] == 0x01);
   namespace d = avr::jtagDebug;
   CHECK(count(b.target.executed, d::ldi(16, 0x02)) >= 1);
   // The last page: word 0x1FF80, EIND 1
   CHECK(count(b.target.executed, d::ldi(31, 0xFF)) >= 1);
   CHECK(count(b.target.executed, d::ldi(30, 0x80)) >= 1);
   CHECK(b.target.unknownInstructions.empty());
   REQUIRE(b.debug.step() == JtagDebugStatus::Ok);
   CHECK(b.target.pc == 0x1F001);
}

TEST_CASE("AvrJtagDebug: flash page write failures") {
   Bench b;
   const auto page = flashPattern(128, 0x40);
   SUBCASE("the core runs") {
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
      CHECK(b.debug.writeFlashPage(0x100, page.data()) == JtagDebugStatus::TargetRunning);
   }
   SUBCASE("range, alignment and SPMCSR") {
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      CHECK(b.debug.writeFlashPage(0x140, page.data()) == JtagDebugStatus::OutOfRange);
      CHECK(b.debug.writeFlashPage(32768, page.data()) == JtagDebugStatus::OutOfRange);
      b.debug.setDevice({ .spmcsr = 0 });
      CHECK(b.debug.writeFlashPage(0x100, page.data()) == JtagDebugStatus::OutOfRange);
      CHECK(b.target.spmPageErases == 0);
   }
   SUBCASE("the boot lock bits stop SPM") {
      b.target.spmLocked = true;
      b.target.data[dataSreg] = 0x35;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      CHECK(b.debug.writeFlashPage(0x100, page.data()) == JtagDebugStatus::VerifyFailed);
      CHECK(b.target.flash[0x100] == 0xFF);
      CHECK(b.target.data[dataSreg] == 0x35);
   }
   SUBCASE("SPMEN does not clear") {
      b.target.spmBusyReads = 1000000;
      b.target.data[dataSreg] = 0x35;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      CHECK(b.debug.writeFlashPage(0x100, page.data()) == JtagDebugStatus::Timeout);
      CHECK(b.target.spmPageErases == 1);
      CHECK(b.target.spmPageWrites == 0);
      // The state comes back also after the failure.
      CHECK(b.target.data[dataSreg] == 0x35);
   }
   SUBCASE("SPMCSR above the I/O registers: ATmega64 and ATmega128") {
      b.target.spmcsr = 0x68;
      b.debug.setDevice({ .spmcsr = 0x68 });
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      CHECK(b.debug.writeFlashPage(0x100, page.data()) == JtagDebugStatus::Ok);
      CHECK(std::equal(page.begin(), page.end(), b.target.flash.begin() + 0x100));
   }
   SUBCASE("an SPM step that does not stop: FORCE_BREAK and the restore") {
      fillRegisters(b.target);
      const auto pattern = registers(b.target);
      b.target.data[dataSreg] = 0x35;
      b.target.pc = 0x123;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      b.target.stepRuns = true;
      CHECK(b.debug.writeFlashPage(0x100, page.data()) == JtagDebugStatus::Timeout);
      CHECK(b.target.stopped);
      CHECK(b.debug.isStopped());
      CHECK(b.target.data[dataSreg] == 0x35);
      CHECK(b.target.spmPageWrites == 0);
      b.target.stepRuns = false;
      REQUIRE(b.debug.run() == JtagDebugStatus::Ok);
      CHECK(registers(b.target) == pattern);
      CHECK(b.target.pc == 0x123);
   }
   SUBCASE("an SPM step that does not stop, also not after FORCE_BREAK") {
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      b.target.stepRuns = true;
      b.target.stopDelay = 100'000;
      CHECK(b.debug.writeFlashPage(0x100, page.data()) == JtagDebugStatus::Timeout);
      CHECK_FALSE(b.debug.isStopped());
      b.target.executed.clear();
      CHECK(b.debug.writeFlashPage(0x100, page.data()) == JtagDebugStatus::TargetRunning);
      CHECK(b.target.executed.empty());
   }
   SUBCASE("no OCD") {
      b.target.ocden = false;
      REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
      CHECK(b.debug.writeFlashPage(0x100, page.data()) == JtagDebugStatus::NoOcd);
   }
}

TEST_CASE("AVR JTAG debug: the instructions that a step can execute in place of the flash word") {
   using avr::AvrJtagDebug;
   // LDS, STS, JMP, CALL, RJMP, RCALL, LDI, ADIW, LDD, STD, PUSH, CLI, WDR,
   // BRCS, BRNE, SBRC, SBRS, CPSE, SBIC, SBIS, RET, RETI
   for(const uint16_t op : { 0x9180, 0x9390, 0x940C, 0x940E, 0xCFED, 0xD000, 0xE38C, 0x9601, 0x8189, 0x839A, 0x930F,
                             0x94F8, 0x95A8, 0xF3B0, 0xF7F1, 0xFC00, 0xFE00, 0x1000, 0x9900, 0x9B00, 0x9508, 0x9518 }) {
      CAPTURE(op);
      CHECK(AvrJtagDebug::canStepInstead(op));
   }
   // LPM r0 Z+, ELPM r0 Z, IJMP, EIJMP, ICALL, EICALL, SLEEP, BREAK, LPM,
   // ELPM, SPM, SPM Z+
   for(const uint16_t op : { 0x9005, 0x9006, 0x9409, 0x9419, 0x9509, 0x9519, 0x9588, 0x9598, 0x95C8, 0x95D8, 0x95E8, 0x95F8 }) {
      CAPTURE(op);
      CHECK_FALSE(AvrJtagDebug::canStepInstead(op));
   }
}

namespace {

void putWord(MockAvrJtag &target, uint32_t word, uint16_t value) {
   target.flash[word * 2u] = static_cast<uint8_t>(value);
   target.flash[word * 2u + 1u] = static_cast<uint8_t>(value >> 8);
}

} // namespace

TEST_CASE("AvrJtagDebug: stepInstead of the branches, skips and returns") {
   Bench b;
   // SP = 0x0AFD
   b.target.data[0x5D] = 0xFD;
   b.target.data[0x5E] = 0x0A;
   b.target.pc = 0x100;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   const auto stepTo = [&](uint16_t op, uint32_t next) {
      REQUIRE(b.debug.stepInstead(op) == JtagDebugStatus::Ok);
      CHECK(b.debug.pc() == next);
      b.debug.setPc(0x100);
   };
   SUBCASE("BRNE: Z clear branches, Z set does not") {
      b.target.data[dataSreg] = 0x00;
      stepTo(0xF7F1, 0xFF); // BRNE .-2 words
      b.target.data[dataSreg] = 0x02;
      stepTo(0xF7F1, 0x101);
   }
   SUBCASE("BRCS: C set branches") {
      b.target.data[dataSreg] = 0x01;
      stepTo(0xF050, 0x10B); // BRCS .+10 words
      b.target.data[dataSreg] = 0x00;
      stepTo(0xF050, 0x101);
   }
   SUBCASE("SBRS and SBRC skip one word, or two over LDS") {
      uint8_t value = 0x04;
      REQUIRE(b.debug.writeData(24, &value, 1) == JtagDebugStatus::Ok);
      putWord(b.target, 0x101, 0xE000); // LDI
      stepTo(0xFF82, 0x102);            // SBRS r24, 2
      stepTo(0xFD82, 0x101);            // SBRC r24, 2
      putWord(b.target, 0x101, 0x9180); // LDS
      stepTo(0xFF82, 0x103);
      // A software breakpoint holds the next word: the original from the caller
      putWord(b.target, 0x101, 0x9598);
      REQUIRE(b.debug.stepInstead(0xFF82, uint16_t{ 0x9180 }) == JtagDebugStatus::Ok);
      CHECK(b.debug.pc() == 0x103);
   }
   SUBCASE("CPSE") {
      const uint8_t same[2] = { 7, 7 };
      REQUIRE(b.debug.writeData(16, same, 2) == JtagDebugStatus::Ok);
      putWord(b.target, 0x101, 0x0000);
      stepTo(0x1301, 0x102); // CPSE r16, r17
      const uint8_t other = 8;
      REQUIRE(b.debug.writeData(17, &other, 1) == JtagDebugStatus::Ok);
      stepTo(0x1301, 0x101);
   }
   SUBCASE("SBIS and SBIC") {
      b.target.data[0x20 + 0x05] = 0x08;
      putWord(b.target, 0x101, 0x0000);
      stepTo(0x9B2B, 0x102); // SBIS 0x05, 3
      stepTo(0x992B, 0x101); // SBIC 0x05, 3
   }
   SUBCASE("RET and RETI take the return address") {
      b.target.data[0xAFE] = 0x12;
      b.target.data[0xAFF] = 0x34;
      b.target.data[dataSreg] = 0x00;
      stepTo(0x9508, 0x1234);
      CHECK(b.target.data[0x5D] == 0xFF);
      CHECK(b.target.data[dataSreg] == 0x00);
      b.target.data[0x5D] = 0xFD;
      stepTo(0x9518, 0x1234);
      CHECK(b.target.data[dataSreg] == 0x80);
   }
   CHECK(b.target.unknownInstructions.empty());
}

TEST_CASE("AvrJtagDebug: stepInstead of CALL and RET with a 3-byte PC (ATmega2560)") {
   Bench b{ 4 };
   b.target.flash.assign(262144, 0xFF);
   b.debug.setDevice({ .flashBytes = 262144 });
   b.target.data[0x5D] = 0xFF;
   b.target.data[0x5E] = 0x0A;
   b.target.pc = 0x1F000;
   REQUIRE(b.debug.attach() == JtagDebugStatus::Ok);
   // CALL 0x1ABCD: k16 = 1, k15..0 in the second word
   putWord(b.target, 0x1F001, 0xABCD);
   REQUIRE(b.debug.stepInstead(0x940F) == JtagDebugStatus::Ok);
   CHECK(b.debug.pc() == 0x1ABCD);
   // SP - 3, the return address 0x01F002 with the high byte at the lowest address
   CHECK(b.target.data[0x5D] == 0xFC);
   CHECK(b.target.data[0x5E] == 0x0A);
   CHECK(b.target.data[0xAFD] == 0x01);
   CHECK(b.target.data[0xAFE] == 0xF0);
   CHECK(b.target.data[0xAFF] == 0x02);
   REQUIRE(b.debug.stepInstead(0x9508) == JtagDebugStatus::Ok);
   CHECK(b.debug.pc() == 0x1F002);
   CHECK(b.target.data[0x5D] == 0xFF);
   // JMP 0x1E000: k16 = 1
   putWord(b.target, 0x1F003, 0xE000);
   b.debug.setPc(0x1F002);
   REQUIRE(b.debug.stepInstead(0x940D) == JtagDebugStatus::Ok);
   CHECK(b.debug.pc() == 0x1E000);
   CHECK(b.target.unknownInstructions.empty());
}
