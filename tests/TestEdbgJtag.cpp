// SPDX-License-Identifier: GPL-3.0-or-later
// The AVR8 generic scope on JTAG (protocol/avr/Avr8Scope.h,
// avr/AvrJtagTarget.h) through the HID function and the EDBG transport: the
// programming sessions of avrdude (jtagice3, JTAG) and the debug sessions
// of Bloom on the mock ATmega32U4 of tests/MockAvrJtag.h, the parameters of
// Bloom, the checks of the host fields, the target pins and a host that
// goes away.

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

   // Bloom EdbgAvr8Interface::init() and Avr8::activate(): the parameters,
   // ACTIVATE_PHYSICAL, ATTACH with break 0, and the break event of the
   // stop
   void bloomOpen(const Bytes &desc = megaDeviceDesc()) {
      CHECK(avr8(avr8Set(1, 0x21, { 200, 0 })) == ok);
      CHECK(avr8(avr8Set(1, 0x01, { 0, 0, 0, 0 })) == ok);
      CHECK(avr8(avr8Set(0, 0, { 0x02 })) == ok);
      CHECK(avr8(avr8Set(0, 1, { 0x02 })) == ok);
      CHECK(avr8(avr8Set(1, 0, { 0x04 })) == ok);
      CHECK(avr8(avr8Set(2, 0, desc)) == ok);
      REQUIRE(avr8({ avr8::command::activatePhysical, 0x00, 0x00 }) == data(idcodeBytes));
   }

   void bloomAttach() {
      REQUIRE(avr8({ avr8::command::attach, 0x00, 0x00 }) == ok);
      CHECK(event() == breakEvent(jtagAvr.pc, false));
   }

   // Avr8::deactivate(): DETACH, DEACTIVATE_PHYSICAL
   void bloomClose() {
      CHECK(avr8({ avr8::command::detach, 0x00 }) == ok);
      CHECK(avr8({ avr8::command::deactivatePhysical, 0x00 }) == ok);
   }

   // The AVR_EVT report: the event frame, or empty
   Bytes event() {
      const Bytes report = hidCommand({ avr::edbg::avrEvt });
      REQUIRE(report[0] == avr::edbg::avrEvt);
      const std::size_t size = (std::size_t{ report[1] } << 8) | report[2];
      return Bytes(report.begin() + 3, report.begin() + 3 + static_cast<std::ptrdiff_t>(size));
   }

   // The frame of a break event: the sequence is the count of events
   Bytes breakEvent(uint32_t pc, bool atBreak) {
      Bytes frame{ avr::edbg::sof, 0x00, byte(events, 0), byte(events, 1), handler::avr8, 0x40 };
      putU32(frame, pc);
      frame.insert(frame.end(), { static_cast<uint8_t>(atBreak ? 1 : 0), 0x01, 0x00 });
      ++events;
      return frame;
   }

   Bytes pcRead() {
      return avr8({ avr8::command::pcRead, 0x00 });
   }

   Bytes run() {
      return avr8({ avr8::command::run, 0x00 });
   }

   Bytes stop() {
      return avr8({ avr8::command::stop, 0x00, 0x01 });
   }

   Bytes step() {
      return avr8({ avr8::command::step, 0x00, 0x01, 0x01 });
   }

   // Bloom: type 1 (program), a byte address, mode 3
   Bytes breakSet(uint8_t number, uint32_t address, uint8_t type = 1) {
      Bytes command{ avr8::command::hwBreakSet, 0x00, type, number };
      putU32(command, address);
      command.push_back(0x03);
      return avr8(command);
   }

   // AVaRICE: type 2 (data), a data space address, mode 0 read, 1 write,
   // 2 access
   Bytes dataBreakSet(uint8_t number, uint32_t address, uint8_t mode) {
      Bytes command{ avr8::command::hwBreakSet, 0x00, 0x02, number };
      putU32(command, address);
      command.push_back(mode);
      return avr8(command);
   }

   // AVaRICE range BP: type 3 (data mask), the mask in the address field,
   // mode 0
   Bytes maskSet(uint8_t number, uint32_t mask) {
      Bytes command{ avr8::command::hwBreakSet, 0x00, 0x03, number };
      putU32(command, mask);
      command.push_back(0x00);
      return avr8(command);
   }

   Bytes breakClear(uint8_t number) {
      return avr8({ avr8::command::hwBreakClear, 0x00, number });
   }

   // SW_BREAK_SET or SW_BREAK_CLEAR with byte addresses
   Bytes swBreak(bool set, const std::vector<uint32_t> &addresses) {
      Bytes command{ set ? avr8::command::swBreakSet : avr8::command::swBreakClear, 0x00 };
      for(const uint32_t address : addresses) {
         putU32(command, address);
      }
      return avr8(command);
   }

   uint16_t events{ 0 };

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

// jtag3_read_byte(), mem_is_sigrow() and mem_is_in_sigrow(): one byte at a
// time, SIGN_JTAG for the even and OSCCAL for the odd bytes, at the index
// address / 2
TEST_CASE("EDBG JTAG: avrdude reads the signature row and the serial number") {
   Bench b;
   b.avrdudeOpen();
   REQUIRE(b.signOn() == data(idcodeBytes));
   b.jtagProgmode();
   SUBCASE("sigrow, prodsig: the calibration bytes 0 to 3") {
      for(uint8_t index = 0; index < 4; ++index) {
         CHECK(b.avr8(readMemory(memory::osccal, index, 1)) == data({ b.jtagAvr.sigrow[2u * index + 1u] }));
      }
      CHECK(b.avr8(readMemory(memory::osccal, 0, 4)) == data({ 0x5A, 0xA1, 0xA2, 0xA3 }));
   }
   SUBCASE("sernum: index 7 to 11") {
      for(uint8_t at = 0x0E; at < 0x18; ++at) {
         const uint8_t type = at % 2 == 0 ? memory::signature : memory::osccal;
         CHECK(b.avr8(readMemory(type, at / 2u, 1)) == data({ b.jtagAvr.sigrow[at] }));
      }
      CHECK(b.avr8(readMemory(memory::signature, 7, 1)) == data({ 0x4E }));
   }
   CHECK(b.jtagAvr.unknownCommands.empty());
   b.avrdudeClose();
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
   CHECK(b.jtagTarget.device().spmcsr == 0x57);
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
   // The debug commands other than the run control need ATTACH.
   CHECK(b.pcRead() == failed(0x32));
   CHECK(b.breakSet(1, 0x200) == failed(0x32));
   CHECK(b.avr8({ avr8::command::disableDebugWire, 0x00 }) == failed(0x90));
   CHECK(b.avr8(memoryCommand(avr8::command::readMemoryMasked, memory::sram, 0x100, 1)) == failed(0x32));
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

// AVaRICE jtag3: ACTIVATE_PHYSICAL before the device context (issue #102)
TEST_CASE("EDBG JTAG: AVaRICE sends the device context after the sign-on") {
   Bench b;
   CHECK(b.avr8(avr8Set(0, 0, { 0x02 })) == ok);
   CHECK(b.avr8(avr8Set(0, 1, { 0x02 })) == ok);
   CHECK(b.avr8(avr8Set(1, 0, { 0x04 })) == ok);
   REQUIRE(b.signOn() == data(idcodeBytes));
   CHECK(b.jtagScope.active());
   CHECK(b.avr8({ avr8::command::getId, 0x00 }) == data(idcodeBytes));
   SUBCASE("a valid device context") {
      CHECK(b.avr8(avr8Set(2, 0, megaDeviceDesc())) == ok);
      b.bloomAttach();
      CHECK(b.jtagAvr.stopped);
      b.bloomClose();
   }
   SUBCASE("no device context") {
      CHECK(b.avr8({ avr8::command::attach, 0x00, 0x01 }) == failed(0x33));
      CHECK(b.avr8({ avr8::command::enterProgmode, 0x00 }) == failed(0x33));
      CHECK(b.avr8({ avr8::command::detach, 0x00 }) == ok);
      CHECK(b.avr8({ avr8::command::deactivatePhysical, 0x00 }) == ok);
   }
   SUBCASE("a device context out of range") {
      CHECK(b.avr8(avr8Set(2, 0, megaDeviceDesc(512, 32768))) == ok);
      CHECK(b.avr8({ avr8::command::enterProgmode, 0x00 }) == failed(0x33));
      CHECK(b.avr8({ avr8::command::deactivatePhysical, 0x00 }) == ok);
   }
   b.checkReleased();
}

// AVaRICE jtag3: no ATTACH, the first RESET starts the debug mode (issue
// #108)
TEST_CASE("EDBG JTAG: AVaRICE resets the target without ATTACH") {
   Bench b;
   b.jtagAvr.pc = 0x123;
   CHECK(b.avr8(avr8Set(0, 0, { 0x02 })) == ok);
   CHECK(b.avr8(avr8Set(0, 1, { 0x02 })) == ok);
   CHECK(b.avr8(avr8Set(1, 0, { 0x04 })) == ok);
   REQUIRE(b.signOn() == data(idcodeBytes));
   CHECK(b.avr8(avr8Set(2, 0, megaDeviceDesc())) == ok);
   CHECK(b.avr8(avr8Set(1, 0x21, { 200, 0 })) == ok);
   // The OCDEN check: the fuses in the programming mode
   b.jtagProgmode();
   SUBCASE("RESET in the programming mode") {
      CHECK(b.avr8({ avr8::command::reset, 0x00, 0x01 }) == failed(0x32));
      CHECK(b.stop() == failed(0x32));
   }
   CHECK(b.avr8(readMemory(memory::fuses, 0, 3)).size() == 6);
   CHECK(b.avr8({ avr8::command::leaveProgmode, 0x00 }) == ok);
   CHECK(b.avr8(avr8Set(3, 0, { 0x00 })) == ok);
   SUBCASE("RESET") {
      CHECK(b.avr8({ avr8::command::reset, 0x00, 0x01 }) == ok);
      // One event only: the one of the reset
      CHECK(b.event() == b.breakEvent(0x0000, false));
      CHECK(b.event().empty());
      CHECK(b.jtagAvr.stopped);
      CHECK(b.pcRead() == Bytes{ 0x83, 0x00, 0x00, 0x00, 0x00, 0x00 });
      CHECK(b.avr8(readMemory(memory::sram, 0x100, 1)).size() == 4);
      CHECK(b.breakSet(1, 0x200) == ok);
      CHECK(b.run() == ok);
      CHECK(b.jtagAvr.reach(0x100));
      CHECK(b.event() == b.breakEvent(0x100, true));
   }
   SUBCASE("STOP (capture a running program)") {
      CHECK(b.stop() == ok);
      CHECK(b.event() == b.breakEvent(b.jtagAvr.pc, false));
      CHECK(b.event().empty());
      CHECK(b.jtagAvr.stopped);
   }
   SUBCASE("RUN") {
      CHECK(b.run() == ok);
      CHECK_FALSE(b.jtagAvr.stopped);
      CHECK(b.event().empty());
   }
   SUBCASE("no OCD answer (OCDEN not programmed)") {
      b.jtagAvr.ocden = false;
      CHECK(b.avr8({ avr8::command::reset, 0x00, 0x01 }) == failed(0x50));
   }
   // AVaRICE "stop debugging" and the sign-off
   b.bloomClose();
   CHECK_FALSE(b.jtagAvr.stopped);
   b.checkReleased();
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
   CHECK(b.avr8(readMemory(memory::signature, 0x100, 1)) == failed(0x38));
   CHECK(b.avr8(readMemory(memory::osccal, 0xFF, 2)) == failed(0x38));
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

TEST_CASE("EDBG JTAG: a Bloom debug session") {
   Bench b;
   b.jtagAvr.pc = 0x123;
   for(uint8_t r = 0; r < 32; ++r) {
      b.jtagAvr.reg(r) = static_cast<uint8_t>(0x40 + r);
   }
   b.jtagAvr.data[0x50] = 0x11;
   b.jtagAvr.data[0x52] = 0x33;
   b.jtagAvr.data[0x100] = 0x5A;
   b.jtagAvr.flash[0x200] = 0x0C;
   b.jtagAvr.flash[0x201] = 0x94;
   b.bloomOpen();
   b.bloomAttach();
   CHECK(b.jtagAvr.stopped);
   // Bloom stop(): STOP of the stopped core, one more event
   CHECK(b.stop() == ok);
   CHECK(b.event() == b.breakEvent(0x123, false));
   CHECK(b.pcRead() == Bytes{ 0x83, 0x00, 0x23, 0x01, 0x00, 0x00 });

   // Registers, I/O, SRAM: SRAM 0x20. The OCDR (0x51) reads as 0.
   Bytes registers(32);
   for(uint8_t r = 0; r < 32; ++r) {
      registers[r] = static_cast<uint8_t>(0x40 + r);
   }
   CHECK(b.avr8(readMemory(memory::sram, 0, 32)) == data(registers));
   CHECK(b.avr8(readMemory(memory::sram, 0x50, 3)) == data({ 0x11, 0x00, 0x33 }));
   Bytes masked = memoryCommand(avr8::command::readMemoryMasked, memory::sram, 0x50, 3);
   masked.push_back(0x05);
   CHECK(b.avr8(masked) == data({ 0x11, 0x00, 0x33 }));
   CHECK(b.avr8(readMemory(memory::sram, 0x100, 1)) == data({ 0x5A }));
   CHECK(b.avr8(writeMemory(memory::sram, 0x100, { 0x01, 0x02 })) == ok);
   CHECK(b.jtagAvr.data[0x100] == 0x01);
   CHECK(b.jtagAvr.data[0x101] == 0x02);
   // A register write goes to the target at the next run.
   CHECK(b.avr8(writeMemory(memory::sram, 5, { 0x99 })) == ok);
   // Flash: SPM, read only
   CHECK(b.avr8(readMemory(memory::spm, 0x200, 2)) == data({ 0x0C, 0x94 }));
   CHECK(b.avr8(writeMemory(memory::spm, 0, pattern(128, 0))) == failed(0x34));
   // EEPROM
   CHECK(b.avr8(writeMemory(memory::eeprom, 0x10, { 0xA5, 0x5A })) == ok);
   CHECK(b.jtagAvr.eeprom[0x10] == 0xA5);
   CHECK(b.avr8(readMemory(memory::eeprom, 0x10, 2)) == data({ 0xA5, 0x5A }));

   // A hardware breakpoint at byte address 0x200 (word 0x100)
   CHECK(b.breakSet(1, 0x200) == ok);
   CHECK(b.run() == ok);
   CHECK_FALSE(b.jtagAvr.stopped);
   CHECK(b.jtagAvr.reg(5) == 0x99);
   CHECK(b.event().empty());
   CHECK(b.pcRead() == failed(0x32));
   CHECK(b.avr8(readMemory(memory::sram, 0x100, 1)) == failed(0x32));
   CHECK(b.run() == failed(0x32));
   CHECK(b.jtagAvr.reach(0x100));
   CHECK(b.event() == b.breakEvent(0x100, true));
   CHECK(b.event().empty());
   CHECK(b.pcRead() == Bytes{ 0x83, 0x00, 0x00, 0x01, 0x00, 0x00 });

   // gdb steps over the breakpoint: clear, step, set
   CHECK(b.breakClear(1) == ok);
   CHECK(b.step() == ok);
   CHECK(b.event() == b.breakEvent(0x101, false));
   CHECK(b.breakSet(1, 0x200) == ok);

   // RUN_TO a word address
   CHECK(b.avr8({ avr8::command::runTo, 0x00, 0x80, 0x01, 0x00, 0x00 }) == ok);
   CHECK(b.jtagAvr.reach(0x180));
   CHECK(b.event() == b.breakEvent(0x180, true));

   // STOP of a running core: cause 0
   CHECK(b.breakClear(1) == ok);
   CHECK(b.run() == ok);
   CHECK_FALSE(b.jtagAvr.reach(0x100));
   CHECK(b.stop() == ok);
   CHECK(b.event() == b.breakEvent(0x100, false));

   // PC_WRITE, then a step from there
   CHECK(b.avr8({ avr8::command::pcWrite, 0x00, 0x40, 0x00, 0x00, 0x00 }) == ok);
   CHECK(b.pcRead() == Bytes{ 0x83, 0x00, 0x40, 0x00, 0x00, 0x00 });
   CHECK(b.step() == ok);
   CHECK(b.event() == b.breakEvent(0x41, false));

   // RESET: the core stops at the reset vector, cause 0.
   CHECK(b.avr8({ avr8::command::reset, 0x00, 0x01 }) == ok);
   CHECK(b.event() == b.breakEvent(0x0000, false));

   CHECK(b.breakSet(2, 0x300) == ok);
   b.bloomClose();
   CHECK_FALSE(b.jtagAvr.stopped);
   // No breakpoint enable stays in the BCR.
   CHECK((b.jtagAvr.ocdRegs[8] & 0x0FF8) == 0);
   CHECK(b.jtagAvr.unknownInstructions.empty());
   b.checkReleased();
}

TEST_CASE("EDBG JTAG: Bloom checks of the debug commands") {
   Bench b;
   b.bloomOpen();
   b.bloomAttach();
   SUBCASE("breakpoints") {
      CHECK(b.breakSet(0, 0x200) == failed(0x81));
      CHECK(b.breakSet(4, 0x200) == failed(0x81));
      CHECK(b.breakClear(4) == failed(0x81));
      // Data breakpoints and masks: not on number 1, no other types
      CHECK(b.breakSet(1, 0x200, 2) == failed(0x81));
      CHECK(b.maskSet(1, 0xFFFFFFFE) == failed(0x81));
      CHECK(b.breakSet(2, 0x200, 4) == failed(0x90));
      CHECK(b.dataBreakSet(2, 0x200, 3) == failed(0x39));
      CHECK(b.dataBreakSet(2, 0x10000, 0) == failed(0x38));
      CHECK(b.avr8({ avr8::command::hwBreakSet, 0x00, 0x02, 0x02, 0x00, 0x02, 0x00, 0x00 }) == failed(0x35));
      CHECK(b.breakSet(1, 0x201) == failed(0x38));
      CHECK(b.breakSet(1, 0x8000) == failed(0x38));
      CHECK(b.avr8({ avr8::command::hwBreakSet, 0x00, 0x01, 0x01, 0x00 }) == failed(0x35));
      // Software breakpoints: byte addresses in the flash
      CHECK(b.swBreak(true, { 0x201 }) == failed(0x38));
      CHECK(b.swBreak(true, { 0x8000 }) == failed(0x38));
      CHECK(b.avr8({ avr8::command::swBreakSet, 0x00, 0x00, 0x02 }) == failed(0x35));
      CHECK(b.avr8({ avr8::command::swBreakClearAll, 0x00 }) == ok);
   }
   SUBCASE("PC and run to") {
      CHECK(b.avr8({ avr8::command::pcWrite, 0x00, 0x00, 0x40, 0x00, 0x00 }) == failed(0x38));
      CHECK(b.avr8({ avr8::command::pcWrite, 0x00, 0x00 }) == failed(0x35));
      CHECK(b.avr8({ avr8::command::runTo, 0x00, 0x00, 0x40, 0x00, 0x00 }) == failed(0x38));
      CHECK(b.jtagAvr.runFields.empty());
   }
   SUBCASE("memory") {
      CHECK(b.avr8(readMemory(memory::sram, 0xFFFF, 2)) == failed(0x38));
      CHECK(b.avr8(readMemory(memory::spm, 0x7FFF, 2)) == failed(0x38));
      CHECK(b.avr8(readMemory(memory::eeprom, 0x3FF, 2)) == failed(0x38));
      CHECK(b.avr8(readMemory(memory::fuses, 2, 2)) == failed(0x38));
      CHECK(b.avr8(readMemory(memory::flashPage, 0, 2)) == failed(0x34));
      CHECK(b.avr8(writeMemory(memory::lockBits, 0, { 0xFC })) == failed(0x34));
      Bytes masked = memoryCommand(avr8::command::readMemoryMasked, memory::eeprom, 0, 1);
      masked.push_back(0x01);
      CHECK(b.avr8(masked) == failed(0x34));
      // No programming mode: no chip erase
      CHECK(b.avr8(eraseCommand()) == failed(0x32));
   }
   SUBCASE("a second ATTACH is a STOP") {
      CHECK(b.avr8({ avr8::command::attach, 0x00, 0x01 }) == ok);
      CHECK(b.event() == b.breakEvent(b.jtagAvr.pc, false));
   }
   SUBCASE("ATTACH in the programming mode") {
      CHECK(b.avr8({ avr8::command::enterProgmode, 0x00 }) == ok);
      CHECK(b.avr8({ avr8::command::attach, 0x00, 0x00 }) == failed(0x32));
      CHECK(b.stop() == failed(0x32));
      CHECK(b.avr8({ avr8::command::leaveProgmode, 0x00 }) == ok);
   }
   SUBCASE("the OCDR does not answer: no context") {
      b.jtagAvr.ocdrStuck = true;
      CHECK(b.avr8({ avr8::command::reset, 0x00, 0x01 }) == failed(0x13));
      CHECK(b.pcRead() == failed(0x13));
      CHECK(b.avr8(readMemory(memory::sram, 0x100, 1)) == failed(0x13));
      CHECK(b.run() == failed(0x13));
      b.jtagAvr.ocdrStuck = false;
      CHECK(b.avr8({ avr8::command::reset, 0x00, 0x01 }) == ok);
      CHECK(b.event() == b.breakEvent(0x0000, false));
   }
   b.bloomClose();
   b.checkReleased();
}

TEST_CASE("EDBG JTAG: data breakpoints") {
   struct Case {
      uint8_t mode;
      bool stopOnRead;
      bool stopOnWrite;
   };
   // EDBG mode 0 read, 1 write, 2 access
   for(const Case c : { Case{ 0, true, false }, Case{ 1, false, true }, Case{ 2, true, true } }) {
      for(const uint8_t number : { uint8_t{ 2 }, uint8_t{ 3 } }) {
         for(const bool write : { false, true }) {
            CAPTURE(c.mode);
            CAPTURE(number);
            CAPTURE(write);
            Bench b;
            b.bloomOpen();
            b.bloomAttach();
            CHECK(b.dataBreakSet(number, 0x0210, c.mode) == ok);
            CHECK(b.run() == ok);
            CHECK_FALSE(b.jtagAvr.access(0x41, 0x0211, write));
            CHECK(b.event().empty());
            // An LDS or STS at word 0x40: the core stops after it.
            const bool expected = write ? c.stopOnWrite : c.stopOnRead;
            CHECK(b.jtagAvr.access(0x42, 0x0210, write) == expected);
            if(expected) {
               CHECK(b.event() == b.breakEvent(0x42, true));
               CHECK(b.pcRead() == Bytes{ 0x83, 0x00, 0x42, 0x00, 0x00, 0x00 });
            } else {
               CHECK(b.event().empty());
               CHECK(b.stop() == ok);
               CHECK(b.event() == b.breakEvent(0x42, false));
            }
            b.bloomClose();
            CHECK((b.jtagAvr.ocdRegs[8] & 0x0FF8) == 0);
         }
      }
   }
}

// AVaRICE "rwatch" on a 2-byte variable: a data breakpoint on number 2 and
// its mask on number 3 (issue #111)
TEST_CASE("EDBG JTAG: a data breakpoint with a mask (AVaRICE range BP)") {
   Bench b;
   b.bloomOpen();
   b.bloomAttach();
   CHECK(b.dataBreakSet(2, 0x0110, 0) == ok);
   CHECK(b.maskSet(3, 0xFFFFFFFE) == ok);
   CHECK(b.run() == ok);
   CHECK(b.jtagAvr.ocdRegs[3] == 0x0110);
   CHECK(b.jtagAvr.ocdRegs[2] == 0xFFFE);
   CHECK_FALSE(b.jtagAvr.access(0x41, 0x0112, false));
   CHECK(b.event().empty());
   // The second byte of the variable also stops the core.
   CHECK(b.jtagAvr.access(0x42, 0x0111, false));
   CHECK(b.event() == b.breakEvent(0x42, true));
   // A program breakpoint cannot use the pair, the mask number is in use.
   CHECK(b.breakSet(3, 0x300) == failed(0x81));
   CHECK(b.maskSet(3, 0xFFFFFFFC) == failed(0x81));
   CHECK(b.breakClear(3) == ok);
   CHECK(b.breakClear(2) == ok);
   CHECK(b.breakSet(2, 0x200) == ok);
   CHECK(b.maskSet(3, 0xFFFFFFFE) == failed(0x81));
   b.bloomClose();
   CHECK((b.jtagAvr.ocdRegs[8] & 0x0FF8) == 0);
   b.checkReleased();
}

TEST_CASE("EDBG JTAG: data and program breakpoints share numbers 2 and 3") {
   Bench b;
   b.bloomOpen();
   b.bloomAttach();
   CHECK(b.breakSet(2, 0x200) == ok);
   CHECK(b.dataBreakSet(2, 0x0210, 1) == failed(0x81));
   CHECK(b.dataBreakSet(3, 0x0210, 1) == ok);
   CHECK(b.dataBreakSet(3, 0x0211, 1) == failed(0x81));
   CHECK(b.breakSet(3, 0x300) == failed(0x81));
   CHECK(b.breakClear(3) == ok);
   CHECK(b.breakSet(3, 0x300) == ok);
   // The program breakpoint at word 0x100 still stops the core.
   CHECK(b.run() == ok);
   CHECK(b.jtagAvr.reach(0x100));
   CHECK(b.event() == b.breakEvent(0x100, true));
}

// Bloom manage_ocden_fuse_bit: ATTACH without OCDEN, then the fuse write
// with FUSES outside the programming mode
TEST_CASE("EDBG JTAG: Bloom programs OCDEN in the debug mode") {
   Bench b;
   b.jtagAvr.ocden = false;
   b.jtagAvr.ocdenFromFuse = true;
   b.jtagAvr.pc = 0x234;
   b.bloomOpen();
   REQUIRE(b.avr8({ avr8::command::attach, 0x00, 0x00 }) == ok);
   CHECK(b.event().empty());
   // "Device not under control - check OCDEN fuse bit"
   CHECK(b.stop() == failed(0x50));
   CHECK(b.avr8({ avr8::command::reset, 0x00, 0x01 }) == failed(0x50));
   CHECK(b.avr8({ avr8::command::getId, 0x00 }) == data(idcodeBytes));
   CHECK(b.avr8(readMemory(memory::fuses, 0, 3)) == data({ 0x5E, 0x99, 0xF3 }));
   CHECK_FALSE(b.jtagAvr.programming);
   CHECK(b.avr8(writeMemory(memory::fuses, 1, { 0x19 })) == ok);
   CHECK(b.jtagAvr.fuses[1] == 0x19);
   CHECK(b.jtagAvr.ocden);
   CHECK_FALSE(b.jtagAvr.programming);
   // The new attach stopped the core: no event, Bloom sends STOP and RESET.
   CHECK(b.jtagAvr.stopped);
   CHECK(b.event().empty());
   CHECK(b.stop() == ok);
   CHECK(b.event() == b.breakEvent(b.jtagAvr.pc, false));
   CHECK(b.avr8({ avr8::command::reset, 0x00, 0x01 }) == ok);
   CHECK(b.event() == b.breakEvent(0x0000, false));
   // Avr8::deactivate(): OCDEN off again, then DETACH
   CHECK(b.avr8(writeMemory(memory::fuses, 1, { 0x99 })) == ok);
   CHECK_FALSE(b.jtagAvr.ocden);
   CHECK(b.stop() == failed(0x50));
   b.bloomClose();
   b.checkReleased();
}

// Avr8::writeMemory() for the flash: the breakpoints off, ENTER_PROGMODE,
// chip erase, FLASH_PAGE, LEAVE_PROGMODE, then DEACTIVATE_PHYSICAL,
// ACTIVATE_PHYSICAL, ATTACH and STOP
TEST_CASE("EDBG JTAG: Bloom loads a program") {
   Bench b;
   b.bloomOpen();
   b.bloomAttach();
   // A stop at a breakpoint keeps its BCR enable.
   CHECK(b.breakSet(1, 0x200) == ok);
   CHECK(b.run() == ok);
   CHECK(b.jtagAvr.reach(0x100));
   CHECK(b.event() == b.breakEvent(0x100, true));
   CHECK(b.avr8({ avr8::command::swBreakClearAll, 0x00 }) == ok);
   CHECK(b.breakClear(1) == ok);
   CHECK(b.avr8({ avr8::command::enterProgmode, 0x00 }) == ok);
   CHECK(b.jtagAvr.programming);
   // No breakpoint enable stays for the new program.
   CHECK((b.jtagAvr.ocdRegs[8] & 0x0FF8) == 0);
   CHECK(b.stop() == failed(0x32));
   CHECK(b.avr8(eraseCommand()) == ok);
   const Bytes page = pattern(128, 0x60);
   CHECK(b.avr8(writeMemory(memory::flashPage, 0, page)) == ok);
   CHECK(b.avr8({ avr8::command::leaveProgmode, 0x00 }) == ok);
   CHECK(b.avr8({ avr8::command::deactivatePhysical, 0x00 }) == ok);
   CHECK(b.jtagPins.inits == b.jtagPins.finis);
   // No new device context
   REQUIRE(b.avr8({ avr8::command::activatePhysical, 0x00, 0x00 }) == data(idcodeBytes));
   b.bloomAttach();
   CHECK(b.stop() == ok);
   CHECK(b.event() == b.breakEvent(b.jtagAvr.pc, false));
   CHECK(b.avr8(readMemory(memory::spm, 0, 128)) == data(page));
   b.bloomClose();
   b.checkReleased();
}

TEST_CASE("EDBG JTAG: a break event above 64K words (ATmega2560)") {
   Bench b;
   b.jtagAvr.ocdRevision = 4;
   b.jtagAvr.flashPageBytes = 256;
   b.jtagAvr.flash.assign(0x40000, 0xFF);
   b.bloomOpen(megaDeviceDesc(256, 0x40000, 4));
   b.bloomAttach();
   CHECK(b.breakSet(3, 0x3E000) == ok);
   CHECK(b.run() == ok);
   CHECK(b.jtagAvr.reach(0x1F000));
   CHECK(b.event() == b.breakEvent(0x1F000, true));
   CHECK(b.pcRead() == Bytes{ 0x83, 0x00, 0x00, 0xF0, 0x01, 0x00 });
   b.bloomClose();
}

TEST_CASE("EDBG JTAG: the host goes away in the debug mode") {
   Bench b;
   b.bloomOpen();
   b.bloomAttach();
   CHECK(b.breakSet(1, 0x200) == ok);
   CHECK(b.run() == ok);
   SUBCASE("USB bus reset") {
      b.enumerate();
   }
   SUBCASE("DAP_Disconnect") {
      CHECK(b.hidCommand({ 0x03 })[1] == 0x00);
   }
   // The core runs without the breakpoints.
   CHECK_FALSE(b.jtagAvr.stopped);
   CHECK((b.jtagAvr.ocdRegs[8] & 0x0FF8) == 0);
   CHECK_FALSE(b.jtagAvr.reach(0x100));
   b.checkReleased();
}

namespace {

uint16_t flashWord(const mupptest::MockAvrJtag &avr, uint32_t word) {
   return static_cast<uint16_t>(avr.flash[word * 2u] | avr.flash[word * 2u + 1u] << 8u);
}

} // namespace

TEST_CASE("EDBG JTAG: software breakpoints") {
   Bench b;
   for(std::size_t i = 0; i < b.jtagAvr.flash.size(); ++i) {
      b.jtagAvr.flash[i] = static_cast<uint8_t>(i * 5 + 3);
   }
   const std::vector<uint8_t> program = b.jtagAvr.flash;
   b.bloomOpen();
   b.bloomAttach();

   // Words 0x100 and 0x102 are in one page, word 0x180 in another.
   CHECK(b.swBreak(true, { 0x200, 0x204 }) == ok);
   CHECK(b.swBreak(true, { 0x300 }) == ok);
   // Nothing goes into flash before the run.
   CHECK(b.jtagAvr.spmPageWrites == 0);
   CHECK(b.run() == ok);
   CHECK(b.jtagAvr.spmPageWrites == 2);
   CHECK(flashWord(b.jtagAvr, 0x100) == 0x9598);
   CHECK(flashWord(b.jtagAvr, 0x102) == 0x9598);
   CHECK(flashWord(b.jtagAvr, 0x180) == 0x9598);
   CHECK(flashWord(b.jtagAvr, 0x101) == (program[0x202] | program[0x203] << 8));

   // The core stops at the BREAK: cause 1. A flash read gives the BREAK.
   CHECK(b.jtagAvr.reach(0x100));
   CHECK(b.event() == b.breakEvent(0x100, true));
   CHECK(b.avr8(readMemory(memory::spm, 0x200, 2)) == data({ 0x98, 0x95 }));

   // A step from the breakpoint: the original word for one step, then
   // BREAK again (two page writes)
   CHECK(b.step() == ok);
   CHECK(b.event() == b.breakEvent(0x101, false));
   CHECK(b.jtagAvr.spmPageWrites == 4);
   CHECK(flashWord(b.jtagAvr, 0x100) == 0x9598);

   // A run from a breakpoint steps over it first.
   CHECK(b.run() == ok);
   CHECK(b.jtagAvr.reach(0x102));
   CHECK(b.event() == b.breakEvent(0x102, true));
   CHECK(b.run() == ok);
   CHECK(b.jtagAvr.spmPageWrites == 6);
   CHECK(flashWord(b.jtagAvr, 0x102) == 0x9598);
   CHECK_FALSE(b.jtagAvr.stopped);

   // A clear writes the original word at the next run.
   CHECK(b.stop() == ok);
   CHECK(b.event() == b.breakEvent(b.jtagAvr.pc, false));
   CHECK(b.swBreak(false, { 0x300 }) == ok);
   CHECK(flashWord(b.jtagAvr, 0x180) == 0x9598);
   CHECK(b.run() == ok);
   CHECK(flashWord(b.jtagAvr, 0x180) == (program[0x300] | program[0x301] << 8));
   CHECK_FALSE(b.jtagAvr.reach(0x180));

   // Hardware breakpoints work next to them.
   CHECK(b.stop() == ok);
   CHECK(b.event() == b.breakEvent(b.jtagAvr.pc, false));
   CHECK(b.breakSet(1, 0x80) == ok);
   CHECK(b.run() == ok);
   CHECK(b.jtagAvr.reach(0x40));
   CHECK(b.event() == b.breakEvent(0x40, true));

   // DETACH removes all of them: the flash is the program again.
   b.bloomClose();
   CHECK(b.jtagAvr.flash == program);
   CHECK_FALSE(b.jtagAvr.stopped);
   CHECK(b.jtagAvr.unknownInstructions.empty());
   b.checkReleased();
}

TEST_CASE("EDBG JTAG: software breakpoint checks") {
   Bench b;
   b.bloomOpen();
   SUBCASE("not attached") {
      CHECK(b.swBreak(true, { 0x200 }) == failed(0x32));
      CHECK(b.swBreak(false, { 0x200 }) == failed(0x32));
      CHECK(b.avr8({ avr8::command::swBreakClearAll, 0x00 }) == ok);
   }
   SUBCASE("a set and a clear before the run: no flash write") {
      b.bloomAttach();
      CHECK(b.swBreak(true, { 0x200 }) == ok);
      CHECK(b.swBreak(false, { 0x200 }) == ok);
      CHECK(b.run() == ok);
      CHECK(b.jtagAvr.spmPageWrites == 0);
   }
   SUBCASE("SW_BREAK_CLEAR_ALL") {
      b.bloomAttach();
      CHECK(b.swBreak(true, { 0x200 }) == ok);
      CHECK(b.step() == ok);
      CHECK(b.event() == b.breakEvent(b.jtagAvr.pc, false));
      CHECK(flashWord(b.jtagAvr, 0x100) == 0x9598);
      CHECK(b.avr8({ avr8::command::swBreakClearAll, 0x00 }) == ok);
      CHECK(b.step() == ok);
      CHECK(flashWord(b.jtagAvr, 0x100) == 0xFFFF);
   }
   SUBCASE("the step fails while the core runs") {
      b.bloomAttach();
      CHECK(b.swBreak(true, { 0x200 }) == ok);
      CHECK(b.run() == ok);
      CHECK(b.step() == failed(0x32));
   }
   SUBCASE("a failed page write keeps the entry for the next commit") {
      b.bloomAttach();
      CHECK(b.swBreak(true, { 0x200 }) == ok);
      CHECK(b.run() == ok);
      CHECK(flashWord(b.jtagAvr, 0x100) == 0x9598);
      CHECK(b.stop() == ok);
      CHECK(b.event() == b.breakEvent(b.jtagAvr.pc, false));
      CHECK(b.swBreak(false, { 0x200 }) == ok);
      // The boot lock bits stop SPM: the read back fails.
      b.jtagAvr.spmLocked = true;
      CHECK(b.run() == failed(0x13));
      CHECK(flashWord(b.jtagAvr, 0x100) == 0x9598);
      b.jtagAvr.spmLocked = false;
      CHECK(b.run() == ok);
      CHECK(flashWord(b.jtagAvr, 0x100) == 0xFFFF);
      b.bloomClose();
      CHECK(flashWord(b.jtagAvr, 0x100) == 0xFFFF);
   }
   SUBCASE("a failed insert, then DETACH") {
      b.bloomAttach();
      CHECK(b.swBreak(true, { 0x200 }) == ok);
      b.jtagAvr.spmLocked = true;
      CHECK(b.run() == failed(0x13));
      b.jtagAvr.spmLocked = false;
      b.bloomClose();
      CHECK(flashWord(b.jtagAvr, 0x100) == 0xFFFF);
      CHECK(b.jtagAvr.spmPageWrites == 0);
   }
   SUBCASE("too many") {
      b.bloomAttach();
      for(uint32_t i = 0; i < avr8::maxSoftwareBreaks; ++i) {
         REQUIRE(b.swBreak(true, { 0x400 + 2 * i }) == ok);
      }
      CHECK(b.swBreak(true, { 0x200 }) == failed(0x81));
   }
}

TEST_CASE("EDBG JTAG: software breakpoints and a program load") {
   Bench b;
   b.bloomOpen();
   b.bloomAttach();
   CHECK(b.swBreak(true, { 0x200 }) == ok);
   CHECK(b.run() == ok);
   CHECK(flashWord(b.jtagAvr, 0x100) == 0x9598);
   CHECK(b.stop() == ok);
   CHECK(b.event() == b.breakEvent(b.jtagAvr.pc, false));
   // ENTER_PROGMODE ends the debug mode: the BREAK goes out of flash.
   CHECK(b.avr8({ avr8::command::enterProgmode, 0x00 }) == ok);
   CHECK(flashWord(b.jtagAvr, 0x100) == 0xFFFF);
   CHECK(b.avr8(eraseCommand()) == ok);
   const Bytes page = pattern(128, 0x60);
   CHECK(b.avr8(writeMemory(memory::flashPage, 0x200, page)) == ok);
   CHECK(b.avr8({ avr8::command::leaveProgmode, 0x00 }) == ok);
   CHECK(b.avr8({ avr8::command::deactivatePhysical, 0x00 }) == ok);
   REQUIRE(b.avr8({ avr8::command::activatePhysical, 0x00, 0x00 }) == data(idcodeBytes));
   b.bloomAttach();
   // No old breakpoint comes back.
   const uint32_t writes = b.jtagAvr.spmPageWrites;
   CHECK(b.run() == ok);
   CHECK(b.jtagAvr.spmPageWrites == writes);
   CHECK(b.stop() == ok);
   CHECK(b.event() == b.breakEvent(b.jtagAvr.pc, false));
   CHECK(b.avr8(readMemory(memory::spm, 0x200, 128)) == data(page));
   b.bloomClose();
   b.checkReleased();
}

TEST_CASE("EDBG JTAG: a software breakpoint above 64K words (ATmega2560)") {
   Bench b;
   b.jtagAvr.ocdRevision = 4;
   b.jtagAvr.flashPageBytes = 256;
   b.jtagAvr.flash.assign(0x40000, 0xFF);
   b.bloomOpen(megaDeviceDesc(256, 0x40000, 4));
   b.bloomAttach();
   CHECK(b.swBreak(true, { 0x3E000 }) == ok);
   CHECK(b.run() == ok);
   CHECK(flashWord(b.jtagAvr, 0x1F000) == 0x9598);
   CHECK(b.jtagAvr.reach(0x1F000));
   CHECK(b.event() == b.breakEvent(0x1F000, true));
   b.bloomClose();
   CHECK(flashWord(b.jtagAvr, 0x1F000) == 0xFFFF);
}
