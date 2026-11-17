// SPDX-License-Identifier: GPL-3.0-or-later
// The AVR8 generic scope on JTAG (protocol/avr/Avr8Scope.h,
// avr/AvrJtagTarget.h) through the HID function and the EDBG transport: the
// programming sessions of avrdude (jtagice3, JTAG) on the mock ATmega32U4
// of tests/MockAvrJtag.h, the parameters of Bloom, the checks of the host
// fields, the target pins and a host that goes away.

#include "EdbgBench.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using namespace edbgtest;

namespace {

namespace avr8 = avr::avr8;
namespace memory = avr::avr8::memory;

uint8_t byte(uint32_t value, unsigned index) {
   return static_cast<uint8_t>(value >> (8u * index));
}

void putU32(Bytes &bytes, uint32_t value) {
   for(unsigned i = 0; i < 4; ++i) {
      bytes.push_back(byte(value, i));
   }
}

// The IDCODE of the mock ATmega32U4, LSB first
const Bytes idcodeBytes{ 0x3F, 0x70, 0x58, 0x49 };

// avrdude struct mega_device_desc for the ATmega32U4, as jtag3_initialize()
// fills it (ocdrev 3, EECR 0x3F)
Bytes megaDeviceDesc(uint16_t pageBytes = 128, uint32_t flashBytes = 32768, uint8_t ocdRevision = 3, bool fullPageBitstream = false) {
   Bytes desc{ byte(pageBytes, 0), byte(pageBytes, 1) };
   putU32(desc, flashBytes);
   putU32(desc, 0);
   putU32(desc, (flashBytes - pageBytes * 4u) / 2u);
   desc.insert(desc.end(), { 0x00, 0x01, 0x00, 0x04, 4, ocdRevision, 1, static_cast<uint8_t>(fullPageBitstream ? 1 : 0),
                             0, 0, 0x31, 0x22, 0x21, 0x1F, 0x20, 0x57, 0x00 });
   return desc;
}

Bytes avr8Set(uint8_t context, uint8_t address, const Bytes &value) {
   Bytes command{ avr8::command::set, 0x00, context, address, static_cast<uint8_t>(value.size()) };
   command.insert(command.end(), value.begin(), value.end());
   return command;
}

Bytes avr8Get(uint8_t context, uint8_t address, uint8_t bytes) {
   return { avr8::command::get, 0x00, context, address, bytes };
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

// jtag3_chip_erase(): mode 0, address 0
Bytes eraseCommand(uint8_t mode = 0) {
   return { avr8::command::erase, 0x00, mode, 0, 0, 0, 0 };
}

struct Bench : EdbgBench {
   Bytes avr8(const Bytes &command) {
      return transact(handler::avr8, command);
   }

   // avrdude jtagice3 with a JTAG part: jtag3_open() and
   // jtag3_initialize() up to the sign-on. clock: the -B value in kHz.
   void avrdudeOpen(const Bytes &desc = megaDeviceDesc(), uint16_t clockKHz = 0) {
      CHECK(hidCommand({ 0x02, 0x01 })[1] == 0x01);
      CHECK(transact(handler::housekeeping, { 0x10, 0x00 }) == ok);
      // Arch mega, session programming, connection JTAG
      CHECK(avr8(avr8Set(0, 0, { 0x02 })) == ok);
      CHECK(avr8(avr8Set(0, 1, { 0x01 })) == ok);
      CHECK(avr8(avr8Set(1, 0, { 0x04 })) == ok);
      if(clockKHz != 0) {
         CHECK(avr8(avr8Set(1, 0x20, { byte(clockKHz, 0), byte(clockKHz, 1) })) == ok);
      }
      CHECK(avr8(avr8Set(1, 0x01, { 0, 0, 0, 0 })) == ok);
      // jtag3_print_parms1() at -v
      CHECK(avr8(avr8Get(1, 0x20, 2)) == data({ clockKHz != 0 ? byte(clockKHz, 0) : uint8_t{ 200 }, byte(clockKHz, 1) }));
      CHECK(avr8(avr8Get(1, 0x21, 2)) == data({ 200, 0 }));
      CHECK(avr8(avr8Set(2, 0, desc)) == ok);
   }

   Bytes signOn() {
      return avr8({ avr8::command::activatePhysical, 0x00, 0x00 });
   }

   void jtagProgmode() {
      REQUIRE(avr8({ avr8::command::enterProgmode, 0x00 }) == ok);
   }

   // jtag3_close(): leave progmode, AVR sign-off, sign-off, DAP_Disconnect
   void avrdudeClose() {
      CHECK(avr8({ avr8::command::leaveProgmode, 0x00 }) == ok);
      CHECK(avr8({ avr8::command::deactivatePhysical, 0x00 }) == ok);
      CHECK(transact(handler::housekeeping, { 0x11, 0x00, 0x00 }) == ok);
      CHECK(hidCommand({ 0x03 })[1] == 0x00);
   }

   // The session is over: the pins are free, the core runs.
   void checkReleased() {
      CHECK_FALSE(jtagScope.active());
      CHECK(jtagPins.inits == jtagPins.finis);
      CHECK_FALSE(jtagAvr.programming);
      CHECK_FALSE(jtagAvr.inReset);
      CHECK(owner.owner() == PinClient::None);
   }
};

} // namespace

TEST_CASE("EDBG JTAG: avrdude reads and writes all memories") {
   Bench b;
   b.avrdudeOpen();
   REQUIRE(b.signOn() == data(idcodeBytes));
   CHECK(b.jtagScope.active());
   CHECK_FALSE(b.avr8Scope.active());
   CHECK(b.jtagPins.inits == 1);
   CHECK(b.dwPins.inits == 0);
   CHECK(b.avr8({ avr8::command::getId, 0x00 }) == data(idcodeBytes));
   b.jtagProgmode();
   CHECK(b.jtagAvr.programming);
   // A second ENTER_PROGMODE (jtag3_program_enable() keeps its own flag)
   b.jtagProgmode();
   CHECK(b.avr8(readMemory(memory::signature, 0, 3)) == data({ 0x1E, 0x95, 0x87 }));
   // The sigrow of avrdude: one byte at a time
   CHECK(b.avr8(readMemory(memory::signature, 2, 1)) == data({ 0x87 }));
   CHECK(b.avr8(readMemory(memory::osccal, 0, 1)) == data({ 0x5A }));

   b.jtagAvr.flash[0] = 0x12;
   CHECK(b.avr8(eraseCommand()) == ok);
   CHECK(b.jtagAvr.chipErases == 1);
   CHECK(b.jtagAvr.flash[0] == 0xFF);

   SUBCASE("flash: one page for each write, two pages for each read") {
      const Bytes first = pattern(128, 0x10);
      const Bytes second = pattern(128, 0x40);
      CHECK(b.avr8(writeMemory(memory::flashPage, 0x100, first)) == ok);
      CHECK(b.avr8(writeMemory(memory::flashPage, 0x180, second)) == ok);
      CHECK(b.jtagAvr.flashPageWrites == 2);
      Bytes both = first;
      both.insert(both.end(), second.begin(), second.end());
      CHECK(b.avr8(readMemory(memory::flashPage, 0x100, 256)) == data(both));
      // Any address and length
      CHECK(b.avr8(readMemory(memory::flashPage, 0x101, 3)) == data({ first[1], first[2], first[3] }));
   }
   SUBCASE("flash: two pages in one write") {
      const Bytes pages = pattern(256, 0x22);
      CHECK(b.avr8(writeMemory(memory::flashPage, 0x7F00, pages)) == ok);
      CHECK(b.jtagAvr.flashPageWrites == 2);
      CHECK(Bytes(b.jtagAvr.flash.begin() + 0x7F00, b.jtagAvr.flash.end()) == pages);
   }
   SUBCASE("EEPROM: one page for each write, 256 bytes for each read") {
      CHECK(b.avr8(writeMemory(memory::eepromPage, 8, { 1, 2, 3, 4 })) == ok);
      CHECK(b.jtagAvr.eepromPageWrites == 1);
      Bytes expected(256, 0xFF);
      std::copy_n(Bytes{ 1, 2, 3, 4 }.begin(), 4, expected.begin() + 8);
      CHECK(b.avr8(readMemory(memory::eepromPage, 0, 256)) == data(expected));
   }
   SUBCASE("fuses: OCDEN on and off, the extended fuse, the lock bits") {
      CHECK(b.avr8(readMemory(memory::fuses, 0, 3)) == data({ 0x5E, 0x99, 0xF3 }));
      CHECK(b.avr8(writeMemory(memory::fuses, 1, { 0x19 })) == ok);
      CHECK(b.jtagAvr.fuses[1] == 0x19);
      CHECK(b.avr8(writeMemory(memory::fuses, 1, { 0x99 })) == ok);
      CHECK(b.avr8(writeMemory(memory::fuses, 2, { 0xCB })) == ok);
      CHECK(b.avr8(readMemory(memory::fuses, 1, 2)) == data({ 0x99, 0xCB }));
      CHECK(b.avr8(writeMemory(memory::lockBits, 0, { 0xFC })) == ok);
      CHECK(b.avr8(readMemory(memory::lockBits, 0, 1)) == data({ 0xFC }));
   }
   CHECK(b.jtagAvr.unknownCommands.empty());
   b.avrdudeClose();
   b.checkReleased();
}

TEST_CASE("EDBG JTAG: the full-page bit stream of the ATmega16 and ATmega32") {
   Bench b;
   b.jtagAvr.fullPageBitstream = true;
   b.avrdudeOpen(megaDeviceDesc(128, 32768, 2, true));
   REQUIRE(b.signOn() == data(idcodeBytes));
   b.jtagProgmode();
   const Bytes page = pattern(128, 0x31);
   CHECK(b.avr8(writeMemory(memory::flashPage, 0x200, page)) == ok);
   CHECK(b.avr8(readMemory(memory::flashPage, 0x200, 128)) == data(page));
   CHECK(b.jtagTarget.device().prog.fullPageBitstream);
   CHECK(b.jtagTarget.device().ocdRevision == 2);
   b.avrdudeClose();
}

TEST_CASE("EDBG JTAG: Bloom sets the clock and the daisy chain before the variant") {
   Bench b;
   const auto set = [&b](uint8_t context, uint8_t address, const Bytes &value) {
      CHECK(b.avr8(avr8Set(context, address, value)) == ok);
   };
   // EdbgAvr8Interface::init(): the debug clock and the daisy chain first
   set(1, 0x21, { 200, 0 });
   set(1, 0x01, { 0, 0, 0, 0 });
   set(0, 0x00, { 0x02 });
   set(0, 0x01, { 0x02 });
   set(1, 0x00, { 0x04 });
   // setDebugWireAndJtagParameters(): one field at a time
   set(2, 0x00, { 0x80, 0x00 });
   set(2, 0x02, { 0x00, 0x80, 0x00, 0x00 });
   set(2, 0x10, { 0x00, 0x04 });
   set(2, 0x12, { 0x04 });
   set(2, 0x13, { 0x03 });
   set(2, 0x18, { 0x31 });
   CHECK(b.avr8({ avr8::command::activatePhysical, 0x00, 0x01 }) == data(idcodeBytes));
   CHECK(b.avr8({ avr8::command::getId, 0x00 }) == data(idcodeBytes));
   // The debug mode (OCD) is not available yet.
   for(const uint8_t command : { avr8::command::attach, avr8::command::stop, avr8::command::run, avr8::command::step,
                                 avr8::command::reset, avr8::command::pcRead, avr8::command::disableDebugWire }) {
      CHECK(b.avr8({ command, 0x00, 0x00 }) == failed(0x90));
   }
   CHECK(b.avr8(memoryCommand(avr8::command::readMemoryMasked, memory::sram, 0x100, 1)) == failed(0x90));
   CHECK(b.avr8({ avr8::command::detach, 0x00 }) == ok);
   CHECK(b.avr8({ avr8::command::deactivatePhysical, 0x00 }) == ok);
   b.checkReleased();
}

TEST_CASE("EDBG JTAG: the physical context") {
   Bench b;
   // One device in the chain only (avrdude -x jtagchain)
   CHECK(b.avr8(avr8Set(1, 0x01, { 1, 0, 0, 0 })) == failed(0x39));
   CHECK(b.avr8(avr8Set(1, 0x03, { 0, 8 })) == failed(0x39));
   CHECK(b.avr8(avr8Get(1, 0x01, 4)) == data({ 0, 0, 0, 0 }));
   CHECK(b.avr8(avr8Get(1, 0x02, 2)) == data({ 0, 0 }));
   // A clock of 0 kHz
   CHECK(b.avr8(avr8Set(1, 0x20, { 0, 0 })) == failed(0x3B));
   CHECK(b.avr8(avr8Set(1, 0x21, { 0, 0 })) == failed(0x3B));
   CHECK(b.avr8(avr8Get(1, 0x20, 2)) == data({ 200, 0 }));
   // One byte of a clock
   CHECK(b.avr8(avr8Set(1, 0x21, { 0x01 })) == ok);
   CHECK(b.avr8(avr8Get(1, 0x21, 2)) == data({ 0x01, 0x00 }));
   CHECK(b.avr8(avr8Set(1, 0x21, { 200 })) == ok);
   // Not one parameter: 0x20 and 0x21 are two parameters.
   CHECK(b.avr8(avr8Set(1, 0x20, { 1, 0, 1, 0 })) == failed(0x90));
   CHECK(b.avr8(avr8Get(1, 0x00, 2)) == failed(0x90));
   SUBCASE("avrdude -B 1000 before the sign-on") {
      b.avrdudeOpen(megaDeviceDesc(), 1000);
      REQUIRE(b.signOn() == data(idcodeBytes));
   }
   SUBCASE("a clock in the session") {
      b.avrdudeOpen();
      REQUIRE(b.signOn() == data(idcodeBytes));
      CHECK(b.avr8(avr8Set(1, 0x20, { 0xE8, 0x03 })) == ok);
      CHECK(b.avr8(avr8Get(1, 0x20, 2)) == data({ 0xE8, 0x03 }));
      b.jtagProgmode();
      CHECK(b.avr8(readMemory(memory::signature, 0, 3)) == data({ 0x1E, 0x95, 0x87 }));
   }
   // A new host starts with the defaults.
   b.enumerate();
   CHECK(b.avr8(avr8Get(1, 0x20, 2)) == data({ 200, 0 }));
   CHECK(b.avr8(avr8Get(1, 0x21, 2)) == data({ 200, 0 }));
}

TEST_CASE("EDBG JTAG: the device context") {
   Bench b;
   SUBCASE("none") {
      b.avrdudeOpen(Bytes(31, 0));
   }
   SUBCASE("a flash page of 512 bytes") {
      b.avrdudeOpen(megaDeviceDesc(512, 32768));
   }
   SUBCASE("more than 256 KiB flash") {
      b.avrdudeOpen(megaDeviceDesc(256, 0x80000));
   }
   SUBCASE("OCD revision 5") {
      b.avrdudeOpen(megaDeviceDesc(128, 32768, 5));
   }
   SUBCASE("no EEPROM page") {
      Bytes desc = megaDeviceDesc();
      desc[0x12] = 0;
      b.avrdudeOpen(desc);
   }
   CHECK(b.signOn() == failed(0x33));
   CHECK(b.jtagPins.inits == 0);
   CHECK_FALSE(b.jtagScope.active());
}

TEST_CASE("EDBG JTAG: the ATmega2560 (256-byte pages, 256 KiB)") {
   Bench b;
   b.jtagAvr.flashPageBytes = 256;
   b.jtagAvr.flash.assign(0x40000, 0xFF);
   b.avrdudeOpen(megaDeviceDesc(256, 0x40000, 4));
   REQUIRE(b.signOn() == data(idcodeBytes));
   b.jtagProgmode();
   const Bytes page = pattern(256, 0x05);
   CHECK(b.avr8(writeMemory(memory::flashPage, 0x3FF00, page)) == ok);
   CHECK(b.avr8(readMemory(memory::flashPage, 0x3FF00, 256)) == data(page));
   CHECK(b.avr8(readMemory(memory::flashPage, 0x3FF00, 257)) == failed(0x38));
   b.avrdudeClose();
}

TEST_CASE("EDBG JTAG: no JTAG answer") {
   Bench b;
   // JTAGEN not programmed: the IDCODE is not one of Atmel.
   b.jtagAvr.idcode = 0xFFFFFFFF;
   b.avrdudeOpen();
   CHECK(b.signOn() == failed(0x20));
   CHECK(b.avr8({ avr8::command::activatePhysical, 0x00, 0x01 }) == failed(0x20));
   CHECK_FALSE(b.jtagScope.active());
   CHECK(b.jtagPins.inits == 2);
   CHECK(b.jtagPins.finis == 2);
   CHECK(b.avr8({ avr8::command::enterProgmode, 0x00 }) == failed(0x31));
}

TEST_CASE("EDBG JTAG: the checks of the memory commands") {
   Bench b;
   b.avrdudeOpen();
   REQUIRE(b.signOn() == data(idcodeBytes));
   // Programming mode only
   CHECK(b.avr8(readMemory(memory::signature, 0, 3)) == failed(0x32));
   CHECK(b.avr8(writeMemory(memory::fuses, 0, { 0xFF })) == failed(0x32));
   CHECK(b.avr8(eraseCommand()) == failed(0x32));
   b.jtagProgmode();
   CHECK(b.avr8(eraseCommand(1)) == failed(0x39));
   CHECK(b.avr8({ avr8::command::erase, 0x00, 0x00 }) == failed(0x35));
   // Not page aligned, outside the device
   CHECK(b.avr8(writeMemory(memory::flashPage, 0x40, pattern(128, 0))) == failed(0x37));
   CHECK(b.avr8(writeMemory(memory::flashPage, 0x100, pattern(64, 0))) == failed(0x37));
   CHECK(b.avr8(writeMemory(memory::flashPage, 0x8000, pattern(128, 0))) == failed(0x38));
   CHECK(b.avr8(readMemory(memory::flashPage, 0x7FFF, 2)) == failed(0x38));
   CHECK(b.avr8(writeMemory(memory::eepromPage, 0x3FE, { 1, 2, 3 })) == failed(0x38));
   CHECK(b.avr8(readMemory(memory::fuses, 2, 2)) == failed(0x38));
   CHECK(b.avr8(writeMemory(memory::lockBits, 1, { 0xFC })) == failed(0x38));
   CHECK(b.avr8(readMemory(memory::signature, 3, 1)) == failed(0x38));
   CHECK(b.avr8(readMemory(memory::osccal, 0, 5)) == failed(0x38));
   // No such memory type in the programming mode, read only
   CHECK(b.avr8(readMemory(memory::sram, 0x100, 1)) == failed(0x34));
   CHECK(b.avr8(readMemory(memory::spm, 0, 2)) == failed(0x34));
   CHECK(b.avr8(writeMemory(memory::spm, 0, pattern(128, 0))) == failed(0x34));
   CHECK(b.avr8(writeMemory(memory::signature, 0, { 0x1E })) == failed(0x34));
   CHECK(b.avr8(readMemory(memory::flashPage, 0, 0)) == failed(0x35));
   CHECK(b.jtagAvr.flashPageWrites == 0);
   CHECK(b.jtagAvr.eepromPageWrites == 0);
   SUBCASE("a write that does not end") {
      b.jtagAvr.stuckBusy = true;
      CHECK(b.avr8(writeMemory(memory::fuses, 0, { 0xFF })) == failed(0x13));
   }
   b.avrdudeClose();
   b.checkReleased();
}

TEST_CASE("EDBG JTAG: ISP and debugWIRE wait for the end of the session") {
   Bench b;
   b.avrdudeOpen();
   REQUIRE(b.signOn() == data(idcodeBytes));
   CHECK(b.isp(enterRequest) == Bytes{ isp::command::enterProgmode, isp::status::failed });
   CHECK(b.pins.inits == 0);
   // A second sign-on: OK, still JTAG
   CHECK(b.signOn() == ok);
   CHECK(b.jtagPins.inits == 1);
   b.avrdudeClose();
   b.checkReleased();
   b.EdbgBench::enterProgmode();
}

TEST_CASE("EDBG JTAG: the host goes away in the programming mode") {
   Bench b;
   b.avrdudeOpen();
   REQUIRE(b.signOn() == data(idcodeBytes));
   b.jtagProgmode();
   REQUIRE(b.jtagAvr.inReset);
   SUBCASE("USB bus reset") {
      b.enumerate();
   }
   SUBCASE("DAP_Disconnect") {
      CHECK(b.hidCommand({ 0x03 })[1] == 0x00);
   }
   b.checkReleased();
   CHECK(b.avr8(avr8Get(0, 0, 1)) == data({ 0x00 }));
   CHECK(b.bulkCommand({ 0x02, 0x01 }) == Bytes{ 0x02, 0x01 });
}
