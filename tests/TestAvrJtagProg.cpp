// SPDX-License-Identifier: GPL-3.0-or-later
// JTAG programming of the classic AVR chips (avr/AvrJtagProg.h) on the TAP
// layer, the JTAG bit-bang engine with mock pins and the model of the
// programming interface (MockAvrJtag.h).

#include "avr/AvrJtagProg.h"
#include "avr/AvrJtagTap.h"
#include "jtag/JtagBitBang.h"

#include "MockAvrJtag.h"
#include "MockJtagPins.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

namespace {

using avr::AvrJtagProg;
using avr::Fuse;
using avr::JtagProgDevice;
using mupptest::MockAvrJtag;
using Bytes = std::vector<uint8_t>;
namespace cmd = avr::jtagProg::cmd;

using Pins = mupptest::MockJtagPins<false>;

// The ATmega32U4: 128-byte flash pages, 4-byte EEPROM pages
constexpr JtagProgDevice m32u4{ 128, 32768, 4, false };

struct Bench {
   explicit Bench(uint32_t kHz = 1000) {
      REQUIRE(tap.open(kHz));
   }

   MockAvrJtag target;
   Pins pins{ target };
   jtag::BitBangPort<Pins> port{ pins };
   avr::AvrJtagTap tap{ port };
   AvrJtagProg prog{ tap };
};

Bytes pattern(std::size_t count, uint8_t seed) {
   Bytes bytes(count);
   for(std::size_t i = 0; i < count; ++i) {
      bytes[i] = static_cast<uint8_t>(seed + i * 7u);
   }
   return bytes;
}

std::size_t countOf(const std::vector<uint16_t> &commands, uint16_t value) {
   return static_cast<std::size_t>(std::count(commands.begin(), commands.end(), value));
}

} // namespace

TEST_CASE("AvrJtagProg: enter and leave the programming mode") {
   Bench b;
   REQUIRE(b.prog.enter());
   CHECK(b.prog.programming());
   CHECK(b.target.inReset);
   CHECK(b.target.programming);
   CHECK(b.prog.leave());
   CHECK_FALSE(b.prog.programming());
   CHECK_FALSE(b.target.programming);
   CHECK_FALSE(b.target.inReset);
   CHECK(b.target.commands == std::vector<uint16_t>{ cmd::noOperation, cmd::idle3 });
   CHECK(b.target.unknownCommands.empty());
}

TEST_CASE("AvrJtagProg: no operation outside the programming mode") {
   Bench b;
   const std::size_t edges = b.pins.rising.size();
   uint8_t value = 0;
   Bytes data(4);
   CHECK_FALSE(b.prog.chipErase());
   CHECK_FALSE(b.prog.readSignature(0, value));
   CHECK_FALSE(b.prog.readCalibration(0, value));
   CHECK_FALSE(b.prog.readFuse(Fuse::Low, value));
   CHECK_FALSE(b.prog.writeFuse(Fuse::Low, 0xFF));
   CHECK_FALSE(b.prog.readLockBits(value));
   CHECK_FALSE(b.prog.writeLockBits(0xFF));
   CHECK_FALSE(b.prog.readFlash(m32u4, 0, data));
   CHECK_FALSE(b.prog.writeFlashPage(m32u4, 0, Bytes(128, 0)));
   CHECK_FALSE(b.prog.readEeprom(0, data));
   CHECK_FALSE(b.prog.writeEeprom(m32u4, 0, data));
   CHECK(b.pins.rising.size() == edges);
}

TEST_CASE("AvrJtagProg: signature and calibration byte") {
   Bench b;
   REQUIRE(b.prog.enter());
   Bytes signature(3);
   for(uint8_t i = 0; i < 3; ++i) {
      REQUIRE(b.prog.readSignature(i, signature[i]));
   }
   CHECK(signature == Bytes{ 0x1E, 0x95, 0x87 });
   uint8_t calibration = 0;
   REQUIRE(b.prog.readCalibration(0, calibration));
   CHECK(calibration == 0x5A);
   REQUIRE(b.prog.readCalibration(3, calibration));
   CHECK(calibration == 0xA3);
   CHECK(b.target.commands.back() == cmd::idle7);
   CHECK(countOf(b.target.commands, cmd::loadAddressLow | 0x03u) == 1);
   CHECK(b.target.unknownCommands.empty());
}

TEST_CASE("AvrJtagProg: chip erase polls until ready") {
   Bench b;
   std::fill(b.target.flash.begin(), b.target.flash.end(), 0x00);
   std::fill(b.target.eeprom.begin(), b.target.eeprom.end(), 0x00);
   b.target.lockBits = 0xFC;
   b.target.busyPolls = 5;
   REQUIRE(b.prog.enter());
   REQUIRE(b.prog.chipErase());
   CHECK(b.target.chipErases == 1);
   CHECK(std::all_of(b.target.flash.begin(), b.target.flash.end(), [](uint8_t v) { return v == 0xFF; }));
   CHECK(std::all_of(b.target.eeprom.begin(), b.target.eeprom.end(), [](uint8_t v) { return v == 0xFF; }));
   CHECK(b.target.lockBits == 0xFF);
   const std::vector<uint16_t> start(b.target.commands.begin(), b.target.commands.begin() + 4);
   CHECK(start == std::vector<uint16_t>{ cmd::chipErase1, cmd::chipErase2, cmd::chipErase3, cmd::chipErase3 });
   // Two idle commands and the busy results: more than one poll
   CHECK(b.target.commands.size() > 6);
   CHECK(b.target.unknownCommands.empty());
}

TEST_CASE("AvrJtagProg: chip erase keeps the EEPROM with EESAVE") {
   Bench b;
   b.target.fuses[1] = 0x91;
   std::fill(b.target.eeprom.begin(), b.target.eeprom.end(), 0x12);
   REQUIRE(b.prog.enter());
   REQUIRE(b.prog.chipErase());
   CHECK(b.target.eeprom[0] == 0x12);
}

TEST_CASE("AvrJtagProg: a write without ready times out") {
   Bench b(100);
   b.target.stuckBusy = true;
   REQUIRE(b.prog.enter());
   CHECK_FALSE(b.prog.chipErase());
   // writeTimeoutMs at 100 kHz, 20 TCK cycles for each poll
   const std::size_t polls = avr::jtagProg::writeTimeoutMs * 100 / avr::jtagProg::commandScanCycles + 1;
   CHECK(countOf(b.target.commands, cmd::chipErase3) == 2 + polls);
}

TEST_CASE("AvrJtagProg: flash page write and read, word by word") {
   Bench b;
   const Bytes page = pattern(128, 0x11);
   REQUIRE(b.prog.enter());
   REQUIRE(b.prog.chipErase());
   REQUIRE(b.prog.writeFlashPage(m32u4, 0x0180, page));
   CHECK(b.target.flashPageWrites == 1);
   CHECK(Bytes(b.target.flash.begin() + 0x180, b.target.flash.begin() + 0x200) == page);
   CHECK(b.target.flash[0x17F] == 0xFF);
   CHECK(b.target.flash[0x200] == 0xFF);
   // No extended address byte for 32 KiB
   CHECK(std::none_of(b.target.commands.begin(), b.target.commands.end(),
                      [](uint16_t c) { return (c & 0xFF00u) == cmd::loadAddressExtended; }));
   CHECK(countOf(b.target.commands, cmd::latch) == 64);

   Bytes read(256);
   REQUIRE(b.prog.readFlash(m32u4, 0x0100, read));
   CHECK(std::all_of(read.begin(), read.begin() + 0x80, [](uint8_t v) { return v == 0xFF; }));
   CHECK(Bytes(read.begin() + 0x80, read.end()) == page);
   CHECK(b.target.unknownCommands.empty());
}

TEST_CASE("AvrJtagProg: flash read at an odd address and length") {
   Bench b;
   std::iota(b.target.flash.begin(), b.target.flash.end(), uint8_t{ 0 });
   REQUIRE(b.prog.enter());
   Bytes read(5);
   REQUIRE(b.prog.readFlash(m32u4, 0x01FF, read));
   CHECK(read == Bytes{ 0xFF, 0x00, 0x01, 0x02, 0x03 });
   CHECK(b.target.unknownCommands.empty());
}

TEST_CASE("AvrJtagProg: flash page with the full-page bit stream") {
   Bench b;
   b.target.fullPageBitstream = true;
   constexpr JtagProgDevice m16{ 128, 16384, 4, true };
   const Bytes page = pattern(128, 0x5C);
   REQUIRE(b.prog.enter());
   REQUIRE(b.prog.chipErase());
   REQUIRE(b.prog.writeFlashPage(m16, 0x0080, page));
   CHECK(Bytes(b.target.flash.begin() + 0x80, b.target.flash.begin() + 0x100) == page);
   CHECK(b.target.flashPageWrites == 1);
   CHECK(countOf(b.target.commands, cmd::latch) == 0);
   CHECK(b.target.irScans.back() == avr::tap::ir::progCommands);
   CHECK(b.target.unknownCommands.empty());
}

TEST_CASE("AvrJtagProg: extended address above 128 KiB") {
   Bench b;
   constexpr JtagProgDevice m2560{ 256, 262144, 8, false };
   b.target.flash.assign(262144, 0xFF);
   b.target.flashPageBytes = 256;
   const Bytes page = pattern(256, 0x21);
   REQUIRE(b.prog.enter());
   REQUIRE(b.prog.writeFlashPage(m2560, 0x3FF00, page));
   CHECK(Bytes(b.target.flash.begin() + 0x3FF00, b.target.flash.end()) == page);
   CHECK(countOf(b.target.commands, cmd::loadAddressExtended | 0x01u) == 1);
   Bytes read(4);
   REQUIRE(b.prog.readFlash(m2560, 0x1FFFE, read));
   CHECK(read == Bytes{ 0xFF, 0xFF, 0xFF, 0xFF });
   REQUIRE(b.prog.readFlash(m2560, 0x3FFFC, read));
   CHECK(read == Bytes(page.end() - 4, page.end()));
   CHECK(b.target.unknownCommands.empty());
}

TEST_CASE("AvrJtagProg: flash page arguments") {
   Bench b;
   REQUIRE(b.prog.enter());
   const std::size_t edges = b.pins.rising.size();
   CHECK_FALSE(b.prog.writeFlashPage(m32u4, 0x0040, Bytes(128, 0)));
   CHECK_FALSE(b.prog.writeFlashPage(m32u4, 0x0000, Bytes(64, 0)));
   CHECK_FALSE(b.prog.writeFlashPage(m32u4, 0x8000, Bytes(128, 0)));
   Bytes read(2);
   CHECK_FALSE(b.prog.readFlash(m32u4, 0x7FFF, read));
   CHECK_FALSE(b.prog.readFlash(m32u4, 0xFFFFFFFFu, read));
   constexpr JtagProgDevice odd{ 127, 32768, 4, false };
   CHECK_FALSE(b.prog.writeFlashPage(odd, 0, Bytes(127, 0)));
   CHECK(b.pins.rising.size() == edges);
}

TEST_CASE("AvrJtagProg: EEPROM write over two pages and read") {
   Bench b;
   const Bytes data = pattern(6, 0x40);
   REQUIRE(b.prog.enter());
   REQUIRE(b.prog.writeEeprom(m32u4, 2, data));
   CHECK(b.target.eepromPageWrites == 2);
   CHECK(Bytes(b.target.eeprom.begin() + 2, b.target.eeprom.begin() + 8) == data);
   CHECK(b.target.eeprom[1] == 0xFF);
   CHECK(b.target.eeprom[8] == 0xFF);
   b.target.eeprom[0x3FF] = 0xA5;
   Bytes read(8);
   REQUIRE(b.prog.readEeprom(1, read));
   CHECK(read == Bytes{ 0xFF, 0x40, 0x47, 0x4E, 0x55, 0x5C, 0x63, 0xFF });
   uint8_t last = 0;
   REQUIRE(b.prog.readEeprom(0x3FF, { &last, 1 }));
   CHECK(last == 0xA5);
   CHECK_FALSE(b.prog.readEeprom(0xFFFF, read));
   CHECK_FALSE(b.prog.writeEeprom(m32u4, 0xFFFE, data));
   CHECK(b.target.unknownCommands.empty());
}

TEST_CASE("AvrJtagProg: the commands of each fuse write") {
   struct Case {
      Fuse fuse;
      uint16_t idle;
      uint16_t strobe;
   };
   for(const Case c : { Case{ Fuse::Low, cmd::idle3, cmd::strobe3 }, Case{ Fuse::High, cmd::idle7, cmd::strobe7 },
                        Case{ Fuse::Extended, cmd::idleB, cmd::strobeB } }) {
      CAPTURE(static_cast<int>(c.fuse));
      Bench b;
      REQUIRE(b.prog.enter());
      REQUIRE(b.prog.writeFuse(c.fuse, 0xCB));
      std::vector<uint16_t> expected{ cmd::enterFuseWrite, cmd::loadDataLow | 0xCBu, c.idle, c.strobe, c.idle, c.idle };
      // The busyPolls results after the strobe are not ready: those of the
      // two idle commands and of the first polls. Each scan returns the
      // result of the scan before it, so the probe polls busyPolls times.
      expected.insert(expected.end(), b.target.busyPolls, c.idle);
      CHECK(b.target.commands == expected);
   }
}

TEST_CASE("AvrJtagProg: fuses") {
   Bench b;
   REQUIRE(b.prog.enter());
   uint8_t value = 0;
   REQUIRE(b.prog.readFuse(Fuse::Low, value));
   CHECK(value == 0x5E);
   REQUIRE(b.prog.readFuse(Fuse::High, value));
   CHECK(value == 0x99);
   REQUIRE(b.prog.readFuse(Fuse::Extended, value));
   CHECK(value == 0xF3);

   REQUIRE(b.prog.writeFuse(Fuse::Low, 0xFF));
   REQUIRE(b.prog.writeFuse(Fuse::High, 0x19));
   REQUIRE(b.prog.writeFuse(Fuse::Extended, 0xCB));
   CHECK(b.target.fuses == std::array<uint8_t, 3>{ 0xFF, 0x19, 0xCB });
   REQUIRE(b.prog.readFuse(Fuse::High, value));
   CHECK(value == 0x19);
   CHECK(countOf(b.target.commands, cmd::strobeB) == 1);
   CHECK(b.target.unknownCommands.empty());
}

TEST_CASE("AvrJtagProg: lock bits") {
   Bench b;
   REQUIRE(b.prog.enter());
   REQUIRE(b.prog.writeLockBits(0x3C));
   CHECK(b.target.lockBits == 0xFC);
   uint8_t value = 0;
   REQUIRE(b.prog.readLockBits(value));
   CHECK(value == 0xFC);
   CHECK(countOf(b.target.commands, cmd::loadDataLow | 0xFCu) == 1);
   CHECK(b.target.unknownCommands.empty());
}
