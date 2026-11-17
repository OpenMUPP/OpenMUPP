// SPDX-License-Identifier: GPL-3.0-or-later
// The AVR8 generic scope on debugWIRE (protocol/avr/Avr8Scope.h) through the
// HID function and the EDBG transport: the sessions of avrdude
// (jtagice3_dw) and Bloom on the mock ATmega328P of tests/MockAvrDw.h, the
// break events, the breakpoints, the target pins and a host that goes away.

#include "EdbgBench.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

using namespace edbgtest;

namespace {

namespace avr8 = avr::avr8;

uint8_t byte(uint32_t value, unsigned index) {
   return static_cast<uint8_t>(value >> (8u * index));
}

void putU32(Bytes &bytes, uint32_t value) {
   for(unsigned i = 0; i < 4; ++i) {
      bytes.push_back(byte(value, i));
   }
}

// avrdude struct mega_device_desc for the ATmega328P, as jtag3_initialize()
// fills it: the boot address is the start of the last 4 pages (words), the
// EEPROM registers are I/O addresses, SPMCSR is a data space address.
Bytes megaDeviceDesc(uint16_t pageBytes = 128, uint32_t flashBytes = 32768) {
   Bytes desc{ byte(pageBytes, 0), byte(pageBytes, 1) };
   putU32(desc, flashBytes);
   putU32(desc, 0);
   putU32(desc, (flashBytes - pageBytes * 4u) / 2u);
   desc.insert(desc.end(), { 0x00, 0x01, 0x00, 0x04, 4, 1, 1, 0, 0, 0, 0x31, 0x22, 0x21, 0x1F, 0x20, 0x57, 0x00 });
   return desc;
}

Bytes avr8Set(uint8_t context, uint8_t address, const Bytes &value) {
   Bytes command{ avr8::command::set, 0x00, context, address, static_cast<uint8_t>(value.size()) };
   command.insert(command.end(), value.begin(), value.end());
   return command;
}

Bytes memoryCommand(uint8_t id, uint8_t type, uint32_t address, uint32_t length) {
   Bytes command{ id, 0x00, type };
   putU32(command, address);
   putU32(command, length);
   return command;
}

Bytes readMemory(uint8_t type, uint32_t address, uint32_t length) {
   return memoryCommand(avr8::command::readMemory, type, address, length);
}

Bytes writeMemory(uint8_t type, uint32_t address, const Bytes &values) {
   Bytes command = memoryCommand(avr8::command::writeMemory, type, address, static_cast<uint32_t>(values.size()));
   command.push_back(0x00);
   command.insert(command.end(), values.begin(), values.end());
   return command;
}

Bytes withAddress(uint8_t id, uint32_t address) {
   Bytes command{ id, 0x00 };
   putU32(command, address);
   return command;
}

Bytes pcResponse(uint16_t word) {
   return { 0x83, 0x00, byte(word, 0), byte(word, 1), 0x00, 0x00 };
}

struct Bench : EdbgBench {
   Bytes avr8(const Bytes &command) {
      return transact(handler::avr8, command);
   }

   // avrdude jtagice3_dw: jtag3_open_dw() and jtag3_initialize(). An
   // empty desc: no device context (stk500v2 before DISABLE_DEBUGWIRE).
   void avrdudeOpen(const Bytes &desc = megaDeviceDesc()) {
      // jtag3_edbg_prepare(): DAP_Connect and DAP_HostStatus
      CHECK(hidCommand({ 0x02, 0x01 })[1] == 0x01);
      CHECK(hidCommand({ 0x01, 0x00, 0x01 })[1] == 0x00);
      CHECK(transact(handler::housekeeping, { 0x10, 0x00 }) == ok);
      CHECK(transact(handler::housekeeping, { 0x02, 0x00, 0x00, 0x01, 0x02 }) == data({ 2, 3 }));
      // Arch tiny (debugWIRE), session programming, connection debugWIRE
      CHECK(avr8(avr8Set(0, 0, { 0x01 })) == ok);
      CHECK(avr8(avr8Set(0, 1, { 0x01 })) == ok);
      CHECK(avr8(avr8Set(1, 0, { 0x05 })) == ok);
      if(!desc.empty()) {
         CHECK(avr8(avr8Set(2, 0, desc)) == ok);
      }
   }

   // AVR sign-on (ACTIVATE_PHYSICAL) without and with external reset
   Bytes signOn() {
      return avr8({ avr8::command::activatePhysical, 0x00, 0x00 });
   }

   // jtag3_close(): AVR sign-off, sign-off, DAP_LED and DAP_Disconnect
   void avrdudeClose() {
      CHECK(avr8({ avr8::command::deactivatePhysical, 0x00 }) == ok);
      CHECK(transact(handler::housekeeping, { 0x11, 0x00, 0x00 }) == ok);
      CHECK(hidCommand({ 0x03 })[1] == 0x00);
   }

   // Bloom: EdbgAvr8Interface::init() and setDebugWireAndJtagParameters()
   // for the ATmega328P, one parameter at a time
   void bloomInit() {
      const auto set = [this](uint8_t context, uint8_t address, const Bytes &value) {
         CHECK(avr8(avr8Set(context, address, value)) == ok);
      };
      set(3, 0x00, { 0x00 });
      set(0, 0x00, { 0x01 });
      set(0, 0x01, { 0x02 });
      set(1, 0x00, { 0x05 });
      set(2, 0x00, { 0x80, 0x00 });
      set(2, 0x02, { 0x00, 0x80, 0x00, 0x00 });
      set(2, 0x06, { 0x00, 0x00, 0x00, 0x00 });
      // boot_section_1: the smallest boot section
      set(2, 0x0A, { 0x00, 0x3F, 0x00, 0x00 });
      set(2, 0x0E, { 0x00, 0x01 });
      set(2, 0x10, { 0x00, 0x04 });
      set(2, 0x12, { 0x04 });
      set(2, 0x13, { 0x01 });
      set(2, 0x14, { 0x01 });
      set(2, 0x18, { 0x31 });
      set(2, 0x1A, { 0x21 });
      set(2, 0x19, { 0x22 });
      set(2, 0x1B, { 0x1F });
      set(2, 0x1C, { 0x20 });
      set(2, 0x1D, { 0x57 });
      set(2, 0x1E, { 0x46 });
   }

   // Bloom activate(): ACTIVATE_PHYSICAL, ATTACH with break, the event
   void bloomActivate() {
      REQUIRE(signOn() == ok);
      CHECK(avr8({ avr8::command::attach, 0x00, 0x01 }) == ok);
      CHECK(event() == breakEvent(avr.runStopPc, false));
   }

   // The AVR_EVT report: the event frame, or empty
   Bytes event() {
      const Bytes report = hidCommand({ avr::edbg::avrEvt });
      REQUIRE(report[0] == avr::edbg::avrEvt);
      const std::size_t size = (std::size_t{ report[1] } << 8) | report[2];
      return Bytes(report.begin() + 3, report.begin() + 3 + static_cast<std::ptrdiff_t>(size));
   }

   // The frame of a break event: the sequence is the count of events
   Bytes breakEvent(uint16_t pc, bool atBreak) {
      Bytes frame{ avr::edbg::sof, 0x00, byte(events, 0), byte(events, 1), handler::avr8, 0x40 };
      putU32(frame, pc);
      frame.insert(frame.end(), { static_cast<uint8_t>(atBreak ? 1 : 0), 0x01, 0x00 });
      ++events;
      return frame;
   }

   // The program runs on the mock target: some time passes.
   void runFor(uint32_t bits) {
      dwPins.setTime(dwPins.time() + uint64_t{ bits } * dwBitCycles);
   }

   void checkClean() const {
      CHECK(avr.badInstructions == 0);
      CHECK(avr.badContexts == 0);
      CHECK(avr.unknownCommands == 0);
      CHECK(avr.spmOutsideBoot == 0);
   }

   uint16_t events{ 0 };
};

} // namespace

TEST_CASE("EDBG debugWIRE: avrdude reads the signature, the flash and the EEPROM") {
   Bench b;
   std::iota(b.avr.flash.begin(), b.avr.flash.end(), uint8_t{ 0 });
   b.avr.eeprom[0x10] = 0x5A;
   b.avrdudeOpen();
   CHECK(b.dwPins.inits == 0);
   REQUIRE(b.signOn() == ok);
   CHECK_FALSE(b.avr.running);
   CHECK(b.owner.owner() == PinClient::CmsisDapHid);
   // The signature: the dW ID with 0x1E
   CHECK(b.avr8(readMemory(avr8::memory::signature, 0, 3)) == data({ 0x1E, 0x95, 0x0F }));
   // paged_load(): 256 bytes of flash per read
   const Bytes flash = b.avr8(readMemory(avr8::memory::flashPage, 0x100, 256));
   REQUIRE(flash.size() == 259);
   CHECK(std::equal(flash.begin() + 2, flash.end() - 1, b.avr.flash.begin() + 0x100));
   // read_byte() of EEPROM: one page of 4 bytes
   CHECK(b.avr8(readMemory(avr8::memory::eeprom, 0x10, 4)) == data({ 0x5A, 0xFF, 0xFF, 0xFF }));
   // Sign-off: the program starts again from the reset.
   b.avrdudeClose();
   CHECK(b.avr.resets == 1);
   CHECK(b.avr.running);
   CHECK(b.avr.goPc == 0);
   CHECK_FALSE(b.dwPins.driven);
   CHECK(b.owner.owner() == PinClient::None);
   b.checkClean();
}

TEST_CASE("EDBG debugWIRE: avrdude writes flash pages and EEPROM") {
   Bench b;
   SUBCASE("ATmega328P") {
   }
   SUBCASE("a smallest boot section of 2 pages: the host boot address is outside it") {
      // As an ATmega168: avrdude sends the start of the last 4 pages.
      b.avr.device.bootStartWords = 0x3F80;
   }
   b.avrdudeOpen();
   REQUIRE(b.signOn() == ok);
   const Bytes pages = pattern(256, 9);
   CHECK(b.avr8(writeMemory(avr8::memory::flashPage, 0x7F00, pages)) == ok);
   CHECK(std::equal(pages.begin(), pages.end(), b.avr.flash.begin() + 0x7F00));
   CHECK(b.avr.pageErases == 2);
   CHECK(b.avr.pageWrites == 2);
   CHECK(b.avr.rwwEnables == 2);
   // write_byte() of EEPROM
   CHECK(b.avr8(writeMemory(avr8::memory::eeprom, 0x3FF, { 0xA5 })) == ok);
   CHECK(b.avr.eeprom[0x3FF] == 0xA5);
   b.checkClean();
   // Not whole pages, outside the flash, other memory types
   CHECK(b.avr8(writeMemory(avr8::memory::flashPage, 0x40, pattern(128, 1))) == failed(0x37));
   CHECK(b.avr8(writeMemory(avr8::memory::flashPage, 0, pattern(64, 1))) == failed(0x37));
   CHECK(b.avr8(writeMemory(avr8::memory::flashPage, 0x7F80, pattern(256, 1))) == failed(0x38));
   CHECK(b.avr8(writeMemory(0xB2, 0, { 0xFF })) == failed(0x34));
   CHECK(b.avr8(readMemory(0xB3, 0, 1)) == failed(0x34));
   CHECK(b.avr8(readMemory(avr8::memory::eeprom, 0x3FF, 2)) == failed(0x38));
   CHECK(b.avr8(readMemory(avr8::memory::sram, 0, 0)) == failed(0x35));
   CHECK(b.avr8(readMemory(avr8::memory::sram, 0, 600)) == failed(0x35));
   CHECK(b.avr8({ avr8::command::readMemory, 0x00, avr8::memory::sram, 0, 0 }) == failed(0x35));
   Bytes shortWrite = writeMemory(avr8::memory::sram, 0x100, { 1, 2, 3 });
   shortWrite.pop_back();
   CHECK(b.avr8(shortWrite) == failed(0x35));
   CHECK(b.avr.pageWrites == 2);
   b.avrdudeClose();
}

TEST_CASE("EDBG debugWIRE: ACTIVATE_PHYSICAL checks the parameters and the pins") {
   Bench b;
   SUBCASE("ISP connection") {
      b.avrdudeOpen();
      CHECK(b.avr8(avr8Set(1, 0, { 0x01 })) == ok);
   }
   SUBCASE("no variant and no physical") {
   }
   SUBCASE("a flash page of 256 bytes") {
      b.avrdudeOpen(megaDeviceDesc(256, 65536));
   }
   SUBCASE("more than 64 KB flash") {
      b.avrdudeOpen(megaDeviceDesc(128, 131072));
   }
   SUBCASE("SPMCSR outside the I/O registers in the data space") {
      b.avrdudeOpen();
      CHECK(b.avr8(avr8Set(2, 0x1D, { 0x60 })) == ok);
   }
   SUBCASE("EECR outside the I/O space") {
      b.avrdudeOpen();
      CHECK(b.avr8(avr8Set(2, 0x1B, { 0x3F + 0x20 })) == ok);
   }
   SUBCASE("the v2 interface owns the pins") {
      b.avrdudeOpen();
      CHECK(b.hidCommand({ 0x03 })[1] == 0x00);
      CHECK(b.bulkCommand({ 0x02, 0x01 }) == Bytes{ 0x02, 0x01 });
   }
   SUBCASE("the ISP programming mode uses the pins") {
      b.avrdudeOpen();
      b.enterProgmode();
   }
   CHECK(b.signOn() == failed(0x33));
   CHECK(b.dwPins.inits == 0);
   CHECK_FALSE(b.avr8Scope.active());
}

TEST_CASE("EDBG debugWIRE: no target") {
   Bench b;
   b.avr.disabled = true;
   b.avrdudeOpen();
   CHECK(b.hidCommand({ 0x03 })[1] == 0x00);
   CHECK(b.signOn() == failed(0x10));
   CHECK_FALSE(b.dwPins.driven);
   CHECK(b.owner.owner() == PinClient::None);
   CHECK(b.avr8(readMemory(avr8::memory::sram, 0x100, 1)) == failed(0x31));
}

TEST_CASE("EDBG debugWIRE: ISP waits for the end of the session") {
   Bench b;
   b.avrdudeOpen();
   REQUIRE(b.signOn() == ok);
   CHECK(b.isp(enterRequest) == Bytes{ isp::command::enterProgmode, isp::status::failed });
   CHECK(b.pins.inits == 0);
   // DISABLE_DEBUGWIRE: ISP works until a power cycle.
   CHECK(b.avr8({ avr8::command::disableDebugWire, 0x00 }) == ok);
   CHECK(b.avr.disables == 1);
   CHECK_FALSE(b.dwPins.driven);
   CHECK_FALSE(b.avr8Scope.active());
   b.enterProgmode();
}

// stk500v2_program_enable(): ISP fails on a part with DWEN, so avrdude
// turns debugWIRE off without a device context (issue #69).
TEST_CASE("EDBG debugWIRE: avrdude leaves debugWIRE without a device context") {
   Bench b;
   // ATtiny85: DWDR is 0x22. The default DwDevice has 0x31, OSCCAL here.
   b.avr.device = mupptest::MockAvrDw::attiny85;
   constexpr uint8_t osccal = 0x31;
   b.avr.io(osccal) = 0xA5;
   b.avr.io(avr::avrIoSreg) = 0x83;
   b.avrdudeOpen({});
   REQUIRE(b.signOn() == ok);
   CHECK(b.avr8Scope.active());
   // Only the disable, DETACH and DEACTIVATE_PHYSICAL work.
   CHECK(b.avr8(readMemory(avr8::memory::sram, 0x60, 1)) == failed(0x33));
   CHECK(b.avr8({ avr8::command::getId, 0x00 }) == failed(0x33));
   CHECK(b.avr8({ avr8::command::run, 0x00 }) == failed(0x33));
   SUBCASE("DISABLE_DEBUGWIRE, then ISP") {
      CHECK(b.avr8({ avr8::command::disableDebugWire, 0x00 }) == ok);
      CHECK(b.avr.disables == 1);
      CHECK_FALSE(b.avr8Scope.active());
      // Bloom with disable_debug_wire_pre_disconnect sends DETACH next.
      CHECK(b.avr8({ avr8::command::detach, 0x00 }) == ok);
      b.avrdudeClose();
      b.enterProgmode();
      CHECK(b.avr.io(osccal) == 0xA5);
      CHECK(b.avr.io(avr::avrIoSreg) == 0x83);
   }
   SUBCASE("DEACTIVATE_PHYSICAL: reset, the target runs") {
      // The reset clears the I/O of the mock. An SREG read through the
      // wrong DWDR gets no answer, and the sign-off fails.
      b.avrdudeClose();
      CHECK(b.avr.resets == 1);
      CHECK(b.avr.running);
      CHECK(b.avr.disables == 0);
   }
   b.checkClean();
}

TEST_CASE("EDBG debugWIRE: a Bloom debug session") {
   Bench b;
   for(uint8_t i = 0; i < 32; ++i) {
      b.avr.regs[i] = static_cast<uint8_t>(0x40u + i);
   }
   b.avr.io(avr::avrIoSreg) = 0x82;
   b.avr.data[0x100] = 0x11;
   b.avr.data[0x101] = 0x22;
   // The program at 0x0000-0x00FF: words 0x1000 + address
   for(uint32_t word = 0; word < 0x80; ++word) {
      b.avr.flash[2 * word] = byte(0x1000 + word, 0);
      b.avr.flash[2 * word + 1] = byte(0x1000 + word, 1);
   }
   b.bloomInit();
   b.bloomActivate();
   // getDeviceId(): Bloom reverses the 4 bytes and takes the last 3.
   CHECK(b.avr8({ avr8::command::getId, 0x00 }) == data({ 0x0F, 0x95, 0x1E, 0x00 }));
   // Registers, SREG and SRAM. DWDR (0x51) reads as 0.
   Bytes regs;
   for(uint8_t i = 0; i < 32; ++i) {
      regs.push_back(static_cast<uint8_t>(0x40u + i));
   }
   CHECK(b.avr8(readMemory(avr8::memory::sram, 0x00, 32)) == data(regs));
   CHECK(b.avr8(readMemory(avr8::memory::sram, 0x5F, 1)) == data({ 0x82 }));
   b.avr.data[0x50] = 0x33;
   b.avr.data[0x52] = 0x44;
   CHECK(b.avr8(readMemory(avr8::memory::sram, 0x50, 3)) == data({ 0x33, 0x00, 0x44 }));
   // A masked read: bytes 0 and 2 are excluded.
   Bytes masked = memoryCommand(avr8::command::readMemoryMasked, avr8::memory::sram, 0x100, 3);
   masked.push_back(0b010);
   CHECK(b.avr8(masked) == data({ 0x00, 0x22, 0x00 }));
   CHECK(b.avr8(writeMemory(avr8::memory::sram, 0x100, { 0x99, 0x98 })) == ok);
   CHECK(b.avr.data[0x100] == 0x99);
   CHECK(b.avr.data[0x101] == 0x98);
   CHECK(b.avr8(writeMemory(avr8::memory::sram, 0x1E, { 0x12, 0x34 })) == ok);
   // The PC: words
   CHECK(b.avr8({ avr8::command::pcRead, 0x00 }) == pcResponse(b.avr.runStopPc));
   CHECK(b.avr8(withAddress(avr8::command::pcWrite, 0x0010)) == ok);
   CHECK(b.avr8(withAddress(avr8::command::pcWrite, 0x4000)) == failed(0x38));
   CHECK(b.avr8({ 0x60, 0x00 }) == failed(0x91));
   CHECK(b.avr8({ avr8::command::pageErase, 0x00, 0, 0, 0, 0 }) == failed(0x90));

   SUBCASE("hardware breakpoint, step, stop") {
      // One hardware breakpoint (number 1), byte address
      CHECK(b.avr8({ avr8::command::hwBreakSet, 0x00, 0x01, 0x01, 0x40, 0x00, 0x00, 0x00, 0x03 }) == ok);
      CHECK(b.avr8({ avr8::command::hwBreakSet, 0x00, 0x01, 0x02, 0x50, 0x00, 0x00, 0x00, 0x03 }) == failed(0x81));
      CHECK(b.avr8({ avr8::command::run, 0x00 }) == ok);
      CHECK(b.avr.goPc == 0x0010);
      CHECK(b.avr.goBreak == 0x0020);
      // The registers of the context go back to the target.
      CHECK(b.avr.goRegs[30] == 0x12);
      CHECK(b.avr.goRegs[31] == 0x34);
      CHECK(b.avr.goSreg == 0x82);
      // Running: no event yet, memory commands fail.
      CHECK(b.event().empty());
      CHECK(b.avr8(readMemory(avr8::memory::sram, 0x100, 1)) == failed(0x32));
      CHECK(b.avr8({ avr8::command::pcRead, 0x00 }) == failed(0x32));
      b.runFor(400);
      CHECK(b.event() == b.breakEvent(0x0020, true));
      CHECK(b.event().empty());
      CHECK(b.avr8({ avr8::command::pcRead, 0x00 }) == pcResponse(0x0020));
      // Step: one event
      CHECK(b.avr8({ avr8::command::step, 0x00, 0x01, 0x01 }) == ok);
      CHECK(b.event() == b.breakEvent(0x0021, false));
      // Run without the hardware breakpoint, then stop
      CHECK(b.avr8({ avr8::command::hwBreakClear, 0x00, 0x01 }) == ok);
      CHECK(b.avr8({ avr8::command::run, 0x00 }) == ok);
      CHECK_FALSE(b.avr.goWithBreak);
      b.runFor(400);
      CHECK(b.event().empty());
      CHECK(b.avr8({ avr8::command::stop, 0x00, 0x01 }) == ok);
      CHECK(b.event() == b.breakEvent(b.avr.runStopPc, false));
      // Run to an address: the hardware breakpoint for one run
      CHECK(b.avr8(withAddress(avr8::command::runTo, 0x0300)) == ok);
      CHECK(b.avr.goBreak == 0x0300);
      b.runFor(400);
      CHECK(b.event() == b.breakEvent(0x0300, true));
      // Reset: the target stops at 0.
      CHECK(b.avr8({ avr8::command::reset, 0x00, 0x01 }) == ok);
      CHECK(b.event() == b.breakEvent(0x0000, false));
      CHECK(b.avr.resets == 1);
   }

   SUBCASE("software breakpoints") {
      const uint16_t at = 0x0040;
      const auto flashWord = [&b](uint16_t word) {
         return static_cast<uint16_t>(b.avr.flash[2u * word] | (b.avr.flash[2u * word + 1u] << 8));
      };
      // Byte addresses. Not in flash before the run.
      CHECK(b.avr8(withAddress(avr8::command::swBreakSet, 2u * at)) == ok);
      CHECK(b.avr8(withAddress(avr8::command::swBreakSet, 0x7FFF)) == failed(0x38));
      CHECK(flashWord(at) == 0x1040);
      CHECK(b.avr8({ avr8::command::run, 0x00 }) == ok);
      CHECK(flashWord(at) == avr8::breakOpcode);
      CHECK(b.avr.pageWrites == 1);
      b.runFor(400);
      CHECK(b.event() == b.breakEvent(at, true));
      // A read of flash gives BREAK, as the EDBG tools do.
      CHECK(b.avr8(readMemory(avr8::memory::spm, 2u * at, 2)) == data({ 0x98, 0x95 }));
      // Run from the breakpoint: the original word for one step
      CHECK(b.avr8({ avr8::command::run, 0x00 }) == ok);
      CHECK(b.avr.breakSteps == 0);
      CHECK(b.avr.steps == 1);
      CHECK(b.avr.goPc == at + 1u);
      CHECK(flashWord(at) == avr8::breakOpcode);
      CHECK(b.avr.pageWrites == 3);
      CHECK(b.avr8({ avr8::command::stop, 0x00, 0x01 }) == ok);
      CHECK(b.event() == b.breakEvent(b.avr.runStopPc, false));
      // A flash write over the breakpoint keeps it, with the new word.
      Bytes page = pattern(128, 0x30);
      CHECK(b.avr8(writeMemory(avr8::memory::flashPage, 0x80, page)) == ok);
      CHECK(flashWord(at) == avr8::breakOpcode);
      // Clear: the next run writes the new original word back.
      CHECK(b.avr8(withAddress(avr8::command::swBreakClear, 2u * at)) == ok);
      CHECK(b.avr8(withAddress(avr8::command::pcWrite, 0x0010)) == ok);
      CHECK(b.avr8({ avr8::command::run, 0x00 }) == ok);
      CHECK(flashWord(at) == static_cast<uint16_t>(page[0] | (page[1] << 8)));
      CHECK(b.avr8({ avr8::command::stop, 0x00, 0x01 }) == ok);
      CHECK(b.event() == b.breakEvent(b.avr.runStopPc, false));
      // Clear all, and a breakpoint that never got into flash
      const uint32_t writes = b.avr.pageWrites;
      CHECK(b.avr8(withAddress(avr8::command::swBreakSet, 0x0200)) == ok);
      CHECK(b.avr8({ avr8::command::swBreakClearAll, 0x00 }) == ok);
      CHECK(b.avr8({ avr8::command::step, 0x00, 0x01, 0x01 }) == ok);
      CHECK(b.event() == b.breakEvent(static_cast<uint16_t>(b.avr.runStopPc + 1u), false));
      CHECK(b.avr.pageWrites == writes);
   }

   // Bloom deactivate(): DETACH, DEACTIVATE_PHYSICAL
   CHECK(b.avr8({ avr8::command::detach, 0x00 }) == ok);
   CHECK(b.avr.running);
   CHECK(b.avr8({ avr8::command::deactivatePhysical, 0x00 }) == ok);
   CHECK_FALSE(b.dwPins.driven);
   CHECK(b.avr.running);
   b.checkClean();
}

TEST_CASE("EDBG debugWIRE: the host goes away") {
   Bench b;
   b.bloomInit();
   b.bloomActivate();
   const uint16_t at = 0x0400;
   b.avr.flash[2u * at] = 0x0C;
   b.avr.flash[2u * at + 1u] = 0x94;
   CHECK(b.avr8(withAddress(avr8::command::swBreakSet, 2u * at)) == ok);
   SUBCASE("bus reset while the target runs") {
      // The program starts after the breakpoint: it runs until the reset.
      CHECK(b.avr8(withAddress(avr8::command::pcWrite, at + 0x100u)) == ok);
      CHECK(b.avr8({ avr8::command::run, 0x00 }) == ok);
      REQUIRE(b.avr.flash[2u * at] == 0x98);
      b.enumerate();
   }
   SUBCASE("USB suspend while the target is stopped") {
      CHECK(b.avr8({ avr8::command::run, 0x00 }) == ok);
      REQUIRE(b.avr.flash[2u * at] == 0x98);
      b.runFor(400);
      CHECK(b.event() == b.breakEvent(at, true));
      b.controller().busSuspend();
      b.controller().busResume();
   }
   // The breakpoint is gone from flash, the program runs, the line is free.
   CHECK(b.avr.flash[2u * at] == 0x0C);
   CHECK(b.avr.flash[2u * at + 1u] == 0x94);
   CHECK(b.avr.running);
   CHECK_FALSE(b.dwPins.driven);
   CHECK_FALSE(b.avr8Scope.active());
   CHECK(b.owner.owner() == PinClient::None);
   CHECK(b.bulkCommand({ 0x02, 0x01 }) == Bytes{ 0x02, 0x01 });
}

TEST_CASE("EDBG debugWIRE: avrdude gives a boot address also to the parts without a boot section") {
   Bench b;
   SUBCASE("ATmega48: 4 KB, no boot section, no RWW") {
      b.avrdudeOpen(megaDeviceDesc(64, 4096));
      REQUIRE(b.signOn() == ok);
      CHECK(b.dwTarget.device().bootStartWords == 0);
   }
   SUBCASE("8 KB: the last page is the SPM address") {
      b.avrdudeOpen(megaDeviceDesc(64, 8192));
      REQUIRE(b.signOn() == ok);
      CHECK(b.dwTarget.device().bootStartWords == 0x0FE0);
   }
   SUBCASE("Bloom without a boot section") {
      b.bloomInit();
      CHECK(b.avr8(avr8Set(2, 0x0A, { 0x00, 0x00, 0x00, 0x00 })) == ok);
      REQUIRE(b.signOn() == ok);
      CHECK(b.dwTarget.device().bootStartWords == 0);
   }
}

TEST_CASE("EDBG debugWIRE: DETACH removes the software breakpoints") {
   Bench b;
   b.bloomInit();
   b.bloomActivate();
   const uint16_t at = 0x0400;
   b.avr.flash[2u * at] = 0x0C;
   b.avr.flash[2u * at + 1u] = 0x94;
   CHECK(b.avr8(withAddress(avr8::command::swBreakSet, 2u * at)) == ok);
   CHECK(b.avr8({ avr8::command::run, 0x00 }) == ok);
   b.runFor(400);
   CHECK(b.event() == b.breakEvent(at, true));
   CHECK(b.avr8({ avr8::command::detach, 0x00 }) == ok);
   CHECK(b.avr.flash[2u * at] == 0x0C);
   CHECK(b.avr.flash[2u * at + 1u] == 0x94);
   CHECK(b.avr.running);
   CHECK(b.avr.goPc == at);
   CHECK(b.avr8({ avr8::command::deactivatePhysical, 0x00 }) == ok);
   CHECK_FALSE(b.dwPins.driven);
   b.checkClean();
}
