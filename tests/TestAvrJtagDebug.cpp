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
      // The PC field at RUN: Z + 1 + 10 (revision 4), Z + 1 + 5 (revision 3)
      CHECK(b.target.runFields.back() == (revision >= 4 ? 0xC9u : 0xCAu));

      // r0 to r15 with LDI r16 and MOV, r16 to r31 with LDI
      namespace d = avr::jtagDebug;
      REQUIRE(b.target.executed.size() >= 48 + 6 + 5);
      CHECK(b.target.executed[0] == d::ldi(16, 0x40));
      CHECK(b.target.executed[1] == d::mov(0, 16));
      CHECK(b.target.executed[11] == d::mov(5, 16));
      CHECK(b.target.executed[10] == d::ldi(16, 0xA5));
      CHECK(b.target.executed[32] == d::ldi(16, 0x50));
      CHECK(b.target.executed[47] == d::ldi(31, 0x5F));
      const uint16_t z = revision >= 4 ? 0xC9 - 11 : 0xC9 - 5;
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
      SUBCASE("word 0 with LPM, odd address and length") {
         std::vector<uint8_t> read(5);
         REQUIRE(b.debug.readFlash(0, read.data(), 5) == JtagDebugStatus::Ok);
         CHECK(read == std::vector<uint8_t>(b.target.flash.begin(), b.target.flash.begin() + 5));
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
