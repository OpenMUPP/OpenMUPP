// SPDX-License-Identifier: GPL-3.0-or-later
// The debugWIRE target operations on a mock AVR behind the mock dW line:
// stop, go, step, reset, the saved context, data space, flash, EEPROM,
// signature and disable.

#include "avr/DebugWireTarget.h"

#include "MockAvrDw.h"
#include "MockDebugWireTarget.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

namespace {

using avr::DwStatus;
using Engine = avr::DebugWireEngine<mupptest::MockDwPins>;
using Target = avr::DebugWireTarget<Engine>;
using Avr = mupptest::MockAvrDw;

constexpr uint32_t clockHz = mupptest::MockDwPins::clockHz;

// A 16 MHz target: dW at 125 kbaud
constexpr uint32_t targetBitCycles = clockHz / (16'000'000 / 128);

avr::DwDevice deviceOf(const Avr::Device &avr) {
   return { .flashPageBytes = avr.pageBytes,
            .flashBytes = avr.flashBytes,
            .bootStartWords = avr.bootStartWords,
            .eepromBytes = avr.eepromBytes,
            .dwdr = avr.dwdr,
            .spmcsr = avr.spmcsr,
            .eearh = avr.eearh,
            .eearl = avr.eearl,
            .eecr = avr.eecr,
            .eedr = avr.eedr };
}

struct Fixture {
   explicit Fixture(const Avr::Device &device = Avr::atmega328p)
      : avr(line, device)
      , target(engine, deviceOf(device)) {
      for(uint8_t i = 0; i < 32; ++i) {
         avr.regs[i] = static_cast<uint8_t>(0xA0u + i);
      }
      avr.io(avr::avrIoSreg) = 0x83;
   }

   // No command that the mock AVR does not know or accept
   void checkClean() const {
      CHECK(avr.badInstructions == 0);
      CHECK(avr.badContexts == 0);
      CHECK(avr.unknownCommands == 0);
      CHECK(avr.spmOutsideBoot == 0);
   }

   mupptest::MockDwTarget line{ targetBitCycles };
   mupptest::MockDwPins pins{ line };
   Engine engine{ pins };
   Avr avr;
   Target target;
};

std::array<uint8_t, 32> originalRegisters() {
   std::array<uint8_t, 32> regs{};
   for(uint8_t i = 0; i < 32; ++i) {
      regs[i] = static_cast<uint8_t>(0xA0u + i);
   }
   return regs;
}

} // namespace

TEST_CASE("debugWIRE target: attach stops the program and saves the context") {
   Fixture f;
   CHECK_FALSE(f.target.attached());
   REQUIRE(f.target.attach() == DwStatus::Ok);
   CHECK(f.target.attached());
   CHECK(f.target.isStopped());
   CHECK_FALSE(f.avr.running);
   CHECK(f.target.pc() == f.avr.runStopPc);
   CHECK(f.target.sreg() == 0x83);
   std::array<uint8_t, 32> regs{};
   REQUIRE(f.target.readRegisters(0, regs.data(), 32) == DwStatus::Ok);
   CHECK(regs == originalRegisters());
   uint16_t signature = 0;
   REQUIRE(f.target.readSignature(signature) == DwStatus::Ok);
   CHECK(signature == 0x950F);
   f.checkClean();
}

TEST_CASE("debugWIRE target: go restores the context and sets the PC last") {
   Fixture f;
   REQUIRE(f.target.attach() == DwStatus::Ok);
   // Memory operations change r0, r28-r31 and SREG in the target.
   std::array<uint8_t, 16> sram{};
   REQUIRE(f.target.readData(0x100, sram.data(), sram.size()) == DwStatus::Ok);
   uint8_t eeprom = 0;
   REQUIRE(f.target.readEeprom(3, &eeprom, 1) == DwStatus::Ok);
   const std::array<uint8_t, 2> changed{ { 0x11, 0x22 } };
   REQUIRE(f.target.writeRegisters(4, changed.data(), 2) == DwStatus::Ok);
   f.target.setSreg(0x02);
   f.target.setPc(0x0400);

   REQUIRE(f.target.go(std::nullopt) == DwStatus::Ok);
   CHECK_FALSE(f.target.isStopped());
   CHECK(f.avr.running);
   CHECK(f.avr.gos == 1);
   CHECK_FALSE(f.avr.goWithBreak);
   CHECK(f.avr.goPc == 0x0400);
   auto expected = originalRegisters();
   expected[4] = 0x11;
   expected[5] = 0x22;
   CHECK(f.avr.goRegs == expected);
   CHECK(f.avr.goSreg == 0x02);
   f.checkClean();

   SUBCASE("the program runs until stop") {
      CHECK(f.target.poll() == DwStatus::Timeout);
      f.engine.waitUs(2000);
      CHECK(f.target.poll() == DwStatus::Timeout);
      std::array<uint8_t, 1> value{};
      CHECK(f.target.readData(0x100, value.data(), 1) == DwStatus::TargetRunning);
      CHECK(f.target.go(std::nullopt) == DwStatus::TargetRunning);
      REQUIRE(f.target.stop() == DwStatus::Ok);
      CHECK(f.target.isStopped());
      CHECK(f.target.pc() == f.avr.runStopPc);
      std::array<uint8_t, 32> regs{};
      REQUIRE(f.target.readRegisters(0, regs.data(), 32) == DwStatus::Ok);
      CHECK(regs == expected);
      CHECK(f.target.sreg() == 0x02);
   }
}

TEST_CASE("debugWIRE target: go with a hardware breakpoint stops there") {
   Fixture f;
   REQUIRE(f.target.attach() == DwStatus::Ok);
   f.target.setPc(0x0100);
   REQUIRE(f.target.go(uint16_t{ 0x0123 }) == DwStatus::Ok);
   CHECK(f.avr.goWithBreak);
   CHECK(f.avr.goBreak == 0x0123);
   CHECK(f.avr.goPc == 0x0100);
   CHECK(f.target.poll() == DwStatus::Timeout);
   // runBits bit times
   f.engine.waitUs(2000);
   REQUIRE(f.target.poll() == DwStatus::Ok);
   CHECK(f.target.isStopped());
   CHECK(f.target.pc() == 0x0123);
   CHECK(f.target.poll() == DwStatus::Ok);
   f.checkClean();
}

TEST_CASE("debugWIRE target: step executes one instruction") {
   Fixture f;
   REQUIRE(f.target.attach() == DwStatus::Ok);
   f.target.setPc(0x0200);
   const uint8_t value = 0x5A;
   REQUIRE(f.target.writeRegisters(16, &value, 1) == DwStatus::Ok);
   REQUIRE(f.target.step() == DwStatus::Ok);
   CHECK(f.avr.steps == 1);
   CHECK(f.avr.goRegs[16] == 0x5A);
   CHECK(f.target.isStopped());
   CHECK(f.target.pc() == 0x0201);
   REQUIRE(f.target.step() == DwStatus::Ok);
   CHECK(f.target.pc() == 0x0202);
   f.checkClean();
}

TEST_CASE("debugWIRE target: reset stops at address 0") {
   Fixture f;
   REQUIRE(f.target.attach() == DwStatus::Ok);
   REQUIRE(f.target.reset() == DwStatus::Ok);
   CHECK(f.avr.resets == 1);
   CHECK(f.target.isStopped());
   CHECK(f.target.pc() == 0);
   CHECK(f.target.sreg() == 0);
   f.checkClean();
}

TEST_CASE("debugWIRE target: data space through the saved context and the target memory") {
   Fixture f;
   std::iota(f.avr.data.begin() + 0x100, f.avr.data.end(), uint8_t{ 0 });
   f.avr.io(0x05) = 0x3C;
   REQUIRE(f.target.attach() == DwStatus::Ok);

   SUBCASE("SRAM in chunks") {
      std::vector<uint8_t> read(300);
      REQUIRE(f.target.readData(0x100, read.data(), 300) == DwStatus::Ok);
      for(uint16_t i = 0; i < 300; ++i) {
         CHECK(read[i] == static_cast<uint8_t>(i));
      }
      std::vector<uint8_t> written(300);
      std::iota(written.begin(), written.end(), uint8_t{ 0x40 });
      REQUIRE(f.target.writeData(0x300, written.data(), 300) == DwStatus::Ok);
      CHECK(std::equal(written.begin(), written.end(), f.avr.data.begin() + 0x300));
   }
   SUBCASE("registers, I/O and SREG in one read") {
      // 0x00-0x1F registers, 0x20-0x50 I/O, DWDR (0x51) excluded
      std::array<uint8_t, 0x51> read{};
      REQUIRE(f.target.readData(0, read.data(), read.size()) == DwStatus::Ok);
      const auto regs = originalRegisters();
      CHECK(std::equal(regs.begin(), regs.end(), read.begin()));
      CHECK(read[0x25] == 0x3C);
      std::array<uint8_t, 0x0E> high{};
      REQUIRE(f.target.readData(0x52, high.data(), high.size()) == DwStatus::Ok);
      // SREG at 0x5F from the saved context
      CHECK(high[0x5F - 0x52] == 0x83);
   }
   SUBCASE("a write to registers and SREG changes the saved context") {
      const std::array<uint8_t, 2> regs{ { 0x77, 0x88 } };
      REQUIRE(f.target.writeData(30, regs.data(), 2) == DwStatus::Ok);
      const uint8_t sreg = 0x01;
      REQUIRE(f.target.writeData(0x5F, &sreg, 1) == DwStatus::Ok);
      const uint8_t port = 0x99;
      REQUIRE(f.target.writeData(0x25, &port, 1) == DwStatus::Ok);
      CHECK(f.avr.io(0x05) == 0x99);
      REQUIRE(f.target.go(std::nullopt) == DwStatus::Ok);
      CHECK(f.avr.goRegs[30] == 0x77);
      CHECK(f.avr.goRegs[31] == 0x88);
      CHECK(f.avr.goSreg == 0x01);
   }
   SUBCASE("DWDR and ranges outside the data space are refused") {
      std::array<uint8_t, 4> buffer{};
      CHECK(f.target.readData(0x50, buffer.data(), 4) == DwStatus::InvalidArgument);
      CHECK(f.target.writeData(0x51, buffer.data(), 1) == DwStatus::InvalidArgument);
      CHECK(f.target.readData(0xFFFE, buffer.data(), 4) == DwStatus::InvalidArgument);
      CHECK(f.target.readRegisters(30, buffer.data(), 3) == DwStatus::InvalidArgument);
   }
   f.checkClean();
}

TEST_CASE("debugWIRE target: flash read") {
   Fixture f;
   std::iota(f.avr.flash.begin(), f.avr.flash.end(), uint8_t{ 7 });
   REQUIRE(f.target.attach() == DwStatus::Ok);
   std::vector<uint8_t> read(300);
   REQUIRE(f.target.readFlash(10, read.data(), 300) == DwStatus::Ok);
   CHECK(std::equal(read.begin(), read.end(), f.avr.flash.begin() + 10));
   CHECK(f.target.readFlash(32768 - 2, read.data(), 4) == DwStatus::InvalidArgument);
   f.checkClean();
}

TEST_CASE("debugWIRE target: flash page write through SPM") {
   for(const Avr::Device &device : { Avr::atmega328p, Avr::attiny85 }) {
      CAPTURE(device.signature);
      Fixture f{ device };
      // Old data in the page: the write must erase it first.
      const uint32_t page = 2u * device.pageBytes;
      std::fill(f.avr.flash.begin() + page, f.avr.flash.begin() + page + device.pageBytes, 0x0F);
      REQUIRE(f.target.attach() == DwStatus::Ok);
      std::vector<uint8_t> image(device.pageBytes);
      std::iota(image.begin(), image.end(), uint8_t{ 0x30 });

      SUBCASE("write") {
         REQUIRE(f.target.writeFlashPage(page, image.data()) == DwStatus::Ok);
         CHECK(std::equal(image.begin(), image.end(), f.avr.flash.begin() + page));
         CHECK(f.avr.pageErases == 1);
         CHECK(f.avr.pageWrites == 1);
         CHECK(f.avr.rwwEnables == (device.bootStartWords != 0 ? 1 : 0));
         // The next page does not change.
         CHECK(f.avr.flash[page + device.pageBytes] == 0xFF);
         std::vector<uint8_t> read(device.pageBytes);
         REQUIRE(f.target.readFlash(page, read.data(), device.pageBytes) == DwStatus::Ok);
         CHECK(read == image);
      }
      SUBCASE("erase") {
         REQUIRE(f.target.eraseFlashPage(page) == DwStatus::Ok);
         CHECK(f.avr.flash[page] == 0xFF);
         CHECK(f.avr.pageErases == 1);
      }
      SUBCASE("a lost answer gets a new attempt") {
         f.avr.spmLostAnswers = 1;
         REQUIRE(f.target.writeFlashPage(page, image.data()) == DwStatus::Ok);
         CHECK(std::equal(image.begin(), image.end(), f.avr.flash.begin() + page));
      }
      SUBCASE("no answer") {
         f.avr.spmLostAnswers = static_cast<int>(avr::dwSpmAttempts);
         CHECK(f.target.eraseFlashPage(page) == DwStatus::Timeout);
         CHECK(f.avr.pageErases == static_cast<int>(avr::dwSpmAttempts));
      }
      SUBCASE("bad addresses") {
         CHECK(f.target.writeFlashPage(page + 2, image.data()) == DwStatus::InvalidArgument);
         CHECK(f.target.eraseFlashPage(device.flashBytes) == DwStatus::InvalidArgument);
         CHECK(f.avr.pageErases == 0);
      }
      f.checkClean();
   }
}

TEST_CASE("debugWIRE target: SPM outside the boot section has no effect on a megaAVR") {
   Fixture f;
   avr::DwDevice wrong = deviceOf(Avr::atmega328p);
   wrong.bootStartWords = 0;
   f.target.setDevice(wrong);
   REQUIRE(f.target.attach() == DwStatus::Ok);
   CHECK(f.target.eraseFlashPage(0x100) == DwStatus::Timeout);
   CHECK(f.avr.spmOutsideBoot > 0);
   CHECK(f.avr.pageErases == 0);
}

TEST_CASE("debugWIRE target: EEPROM read and write") {
   Avr::Device oneByteAddress = Avr::attiny85;
   oneByteAddress.eepromBytes = 256;
   oneByteAddress.eearh = oneByteAddress.eearl;
   for(const Avr::Device &device : { Avr::atmega328p, Avr::attiny85, oneByteAddress }) {
      CAPTURE(device.eearh);
      Fixture f{ device };
      std::iota(f.avr.eeprom.begin(), f.avr.eeprom.end(), uint8_t{ 0x10 });
      REQUIRE(f.target.attach() == DwStatus::Ok);
      const uint16_t address = device.eepromBytes - 4u;
      std::array<uint8_t, 4> read{};
      REQUIRE(f.target.readEeprom(address, read.data(), 4) == DwStatus::Ok);
      CHECK(std::equal(read.begin(), read.end(), f.avr.eeprom.begin() + address));

      const std::array<uint8_t, 3> written{ { 0xDE, 0xAD, 0x42 } };
      REQUIRE(f.target.writeEeprom(address, written.data(), 3) == DwStatus::Ok);
      CHECK(f.avr.eepromWrites == 3);
      CHECK(std::equal(written.begin(), written.end(), f.avr.eeprom.begin() + address));
      CHECK(f.target.readEeprom(address, read.data(), 5) == DwStatus::InvalidArgument);
      f.checkClean();
   }
}

TEST_CASE("debugWIRE target: an EEPROM write that does not end gives Timeout") {
   Fixture f;
   f.avr.eepromBusyReads = 1'000'000;
   REQUIRE(f.target.attach() == DwStatus::Ok);
   const uint8_t value = 1;
   CHECK(f.target.writeEeprom(0, &value, 1) == DwStatus::Timeout);
}

TEST_CASE("debugWIRE target: runTimers clears bit 5 of the context bytes") {
   Fixture f;
   f.target.setRunTimers(true);
   REQUIRE(f.target.attach() == DwStatus::Ok);
   std::array<uint8_t, 4> read{};
   REQUIRE(f.target.readData(0x100, read.data(), 4) == DwStatus::Ok);
   CHECK(f.avr.timersRun);
   REQUIRE(f.target.go(std::nullopt) == DwStatus::Ok);
   CHECK(f.avr.timersRun);
   CHECK(f.avr.context == 0x40);
   f.checkClean();
}

TEST_CASE("debugWIRE target: disable gives the RESET pin back") {
   Fixture f;
   REQUIRE(f.target.attach() == DwStatus::Ok);
   REQUIRE(f.target.disable() == DwStatus::Ok);
   CHECK(f.avr.disables == 1);
   CHECK_FALSE(f.target.attached());
   CHECK_FALSE(f.engine.synced());
   CHECK_FALSE(f.pins.driven);
   CHECK(f.target.stop() == DwStatus::TargetRunning);
}

TEST_CASE("debugWIRE target: detach lets the program run") {
   Fixture f;
   REQUIRE(f.target.attach() == DwStatus::Ok);
   REQUIRE(f.target.detach() == DwStatus::Ok);
   CHECK(f.avr.running);
   CHECK(f.avr.gos == 1);
   CHECK_FALSE(f.target.attached());
   CHECK_FALSE(f.pins.driven);
}

TEST_CASE("debugWIRE target: no target") {
   Fixture f;
   f.line.present = false;
   CHECK(f.target.attach() == DwStatus::NoSync);
   CHECK_FALSE(f.target.isStopped());
   uint16_t signature = 0;
   CHECK(f.target.readSignature(signature) == DwStatus::TargetRunning);
}
