// SPDX-License-Identifier: GPL-3.0-or-later
// The AVR ISP scope (STK500v2 commands) on the ISP engine and a mock AVR:
// the programming mode, the memories, the poll modes, the extended address
// and the checks of the host fields.

#include "protocol/avr/IspScope.h"

#include "MockIspTarget.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace {

using Engine = avr::IspEngine<mupptest::MockIspPins, mupptest::MockIspSpi>;
using Scope = avr::IspScope<Engine>;
using Bytes = std::vector<uint8_t>;
namespace cmd = avr::isp::command;
namespace status = avr::isp::status;

constexpr uint32_t cyclesPerMs = 72'000;

// The values of avrdude.conf for the ATmega2560
const Bytes enterRequest{ cmd::enterProgmode, 200, 1, 1, 32, 0, 0x53, 3, 0xAC, 0x53, 0x00, 0x00 };
constexpr uint8_t flashPageMode = 0x41;
constexpr uint8_t eepromPageMode = 0x41;

struct Fixture {
   Bytes run(const Bytes &request) {
      const std::size_t length = scope.process(request, response);
      return Bytes(response.begin(), response.begin() + static_cast<std::ptrdiff_t>(length));
   }

   void enter() {
      REQUIRE(run({ cmd::setBaud, 0xA0, 0x0F }) == Bytes{ cmd::setBaud, status::ok });
      REQUIRE(run(enterRequest) == Bytes{ cmd::enterProgmode, status::ok });
      target.frames.clear();
   }

   void loadAddress(uint32_t address) {
      REQUIRE(run({ cmd::loadAddress, static_cast<uint8_t>(address >> 24), static_cast<uint8_t>(address >> 16),
                    static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address) }) == Bytes{ cmd::loadAddress, status::ok });
   }

   static Bytes program(uint8_t id, uint8_t mode, uint8_t delayMs, std::array<uint8_t, 5> cmds, const Bytes &data, std::size_t numBytes) {
      Bytes request{
         id, static_cast<uint8_t>(numBytes >> 8), static_cast<uint8_t>(numBytes), mode, delayMs, cmds[0], cmds[1], cmds[2], cmds[3], cmds[4]
      };
      request.insert(request.end(), data.begin(), data.end());
      return request;
   }

   Bytes programFlash(uint8_t mode, const Bytes &data, uint8_t delayMs = 10, uint8_t poll1 = 0x00) {
      return run(program(cmd::programFlash, mode, delayMs, { 0x40, 0x4C, 0x20, poll1, 0x00 }, data, data.size()));
   }

   Bytes readFlash(std::size_t bytes) {
      return run({ cmd::readFlash, static_cast<uint8_t>(bytes >> 8), static_cast<uint8_t>(bytes), 0x20 });
   }

   [[nodiscard]] std::size_t countFrames(uint8_t first) const {
      return static_cast<std::size_t>(std::count_if(target.frames.begin(), target.frames.end(),
                                                    [first](const auto &frame) { return frame[0] == first; }));
   }

   mupptest::MockIspTarget target;
   mupptest::MockIspPins pins{ target };
   Engine engine{ pins };
   Scope scope{ engine };
   std::array<uint8_t, avr::isp::maxResponseBytes> response{};
};

Bytes pattern(std::size_t bytes, uint8_t seed) {
   Bytes data(bytes);
   for(std::size_t i = 0; i < bytes; ++i) {
      data[i] = static_cast<uint8_t>(seed + i * 7u);
   }
   return data;
}

Bytes readReply(uint8_t id, const Bytes &data) {
   Bytes reply{ id, status::ok };
   reply.insert(reply.end(), data.begin(), data.end());
   reply.push_back(status::ok);
   return reply;
}

} // namespace

TEST_CASE("AVR ISP scope: commands outside the scope are unknown, an empty request has no response") {
   Fixture f;
   for(const uint8_t id : { 0x01, 0x02, 0x03, 0x05, 0x07, 0x20, 0xFF }) {
      CAPTURE(id);
      CHECK(f.run({ id, 0x00, 0x00, 0x00 }) == Bytes{ id, status::unknown });
   }
   CHECK(f.run({}).empty());
   const Bytes request{ cmd::getBaud };
   std::array<uint8_t, 3> small{};
   CHECK(f.scope.process(request, small) == 0);
   CHECK(f.pins.inits == 0);
}

TEST_CASE("AVR ISP scope: target commands before ENTER_PROGMODE fail without a pin change") {
   Fixture f;
   const std::vector<Bytes> requests{
      { cmd::chipErase, 10, 0, 0xAC, 0x80, 0x00, 0x00 },
      { cmd::readFlash, 0x00, 0x02, 0x20 },
      { cmd::readEeprom, 0x00, 0x02, 0xA0 },
      { cmd::readSignature, 4, 0x30, 0x00, 0x00, 0x00 },
      { cmd::readFuse, 4, 0x50, 0x00, 0x00, 0x00 },
      { cmd::readLock, 4, 0x58, 0x00, 0x00, 0x00 },
      { cmd::readOsccal, 4, 0x38, 0x00, 0x00, 0x00 },
      { cmd::programFuse, 0xAC, 0xA0, 0x00, 0xE2 },
      { cmd::programLock, 0xAC, 0xE0, 0x00, 0xFC },
      Fixture::program(cmd::programFlash, flashPageMode | 0x80, 10, { 0x40, 0x4C, 0x20, 0, 0 }, { 1, 2 }, 2),
      Fixture::program(cmd::programEeprom, eepromPageMode | 0x80, 10, { 0xC1, 0xC2, 0xA0, 0, 0 }, { 1 }, 1),
   };
   for(const Bytes &request : requests) {
      CAPTURE(request[0]);
      CHECK(f.run(request) == Bytes{ request[0], status::failed });
   }
   CHECK(f.pins.inits == 0);
   CHECK(f.pins.risingTimes.empty());
   CHECK(f.target.lowFuse == 0x62);
}

TEST_CASE("AVR ISP scope: ENTER_PROGMODE and LEAVE_PROGMODE drive and release the pins") {
   Fixture f;
   f.enter();
   CHECK(f.scope.programming());
   CHECK(f.pins.driven);
   CHECK(f.target.enabled);
   CHECK(f.target.inReset());

   const uint32_t before = f.pins.time;
   CHECK(f.run({ cmd::leaveProgmode, 2, 3 }) == Bytes{ cmd::leaveProgmode, status::ok });
   CHECK_FALSE(f.scope.programming());
   CHECK_FALSE(f.pins.driven);
   CHECK_FALSE(f.target.inReset());
   CHECK(f.pins.time - before >= 5 * cyclesPerMs);
   CHECK(f.pins.finis == 1);

   // A second LEAVE_PROGMODE does not touch the pins: another interface
   // can use them now.
   CHECK(f.run({ cmd::leaveProgmode, 0, 0 }) == Bytes{ cmd::leaveProgmode, status::ok });
   CHECK(f.pins.finis == 1);
}

TEST_CASE("AVR ISP scope: ENTER_PROGMODE without the sync fails and releases the pins") {
   Fixture f;
   f.target.syncOffset = 20;
   Bytes request = enterRequest;
   request[4] = 2;
   CHECK(f.run(request) == Bytes{ cmd::enterProgmode, status::failed });
   CHECK_FALSE(f.scope.programming());
   CHECK(f.pins.inits == 1);
   CHECK_FALSE(f.pins.driven);
   CHECK_FALSE(f.target.inReset());
}

TEST_CASE("AVR ISP scope: ENTER_PROGMODE checks the pollIndex and the length before it drives the pins") {
   Fixture f;
   Bytes request = enterRequest;
   request[7] = 5;
   CHECK(f.run(request) == Bytes{ cmd::enterProgmode, status::failed });
   request[7] = 0xFF;
   CHECK(f.run(request) == Bytes{ cmd::enterProgmode, status::failed });
   request = enterRequest;
   request.pop_back();
   CHECK(f.run(request) == Bytes{ cmd::enterProgmode, status::failed });
   CHECK(f.pins.inits == 0);

   // pollIndex 0: no check
   request = enterRequest;
   request[7] = 0;
   CHECK(f.run(request) == Bytes{ cmd::enterProgmode, status::ok });
}

TEST_CASE("AVR ISP scope: SET_BAUD sets the SCK frequency, GET_BAUD reads it, 0 is invalid") {
   Fixture f;
   CHECK(f.run({ cmd::getBaud }) == Bytes{ cmd::getBaud, 100, 0, status::ok });
   CHECK(f.run({ cmd::setBaud, 0xA0, 0x0F }) == Bytes{ cmd::setBaud, status::ok });
   CHECK(f.engine.spiShift() == 5);
   CHECK(f.run({ cmd::getBaud }) == Bytes{ cmd::getBaud, 0xA0, 0x0F, status::ok });
   CHECK(f.run({ cmd::setBaud, 0x00, 0x00 }) == Bytes{ cmd::setBaud, status::baudInvalid });
   CHECK(f.run({ cmd::setBaud, 0x20 }) == Bytes{ cmd::setBaud, status::failed });
   CHECK(f.run({ cmd::getBaud }) == Bytes{ cmd::getBaud, 0xA0, 0x0F, status::ok });
   // The largest value: the fastest SPI rate, never faster than the request
   CHECK(f.run({ cmd::setBaud, 0xFF, 0xFF }) == Bytes{ cmd::setBaud, status::ok });
   CHECK(f.engine.spiShift() == avr::spiMinShift);
}

TEST_CASE("AVR ISP scope: signature, fuses, lock and calibration read at retAddr") {
   Fixture f;
   f.enter();
   CHECK(f.run({ cmd::readSignature, 4, 0x30, 0x00, 0x00, 0x00 }) == Bytes{ cmd::readSignature, status::ok, 0x1E, status::ok });
   CHECK(f.run({ cmd::readSignature, 4, 0x30, 0x00, 0x01, 0x00 }) == Bytes{ cmd::readSignature, status::ok, 0x95, status::ok });
   CHECK(f.run({ cmd::readSignature, 4, 0x30, 0x00, 0x02, 0x00 }) == Bytes{ cmd::readSignature, status::ok, 0x0F, status::ok });
   CHECK(f.run({ cmd::readFuse, 4, 0x50, 0x00, 0x00, 0x00 }) == Bytes{ cmd::readFuse, status::ok, 0x62, status::ok });
   CHECK(f.run({ cmd::readFuse, 4, 0x58, 0x08, 0x00, 0x00 }) == Bytes{ cmd::readFuse, status::ok, 0xD9, status::ok });
   CHECK(f.run({ cmd::readFuse, 4, 0x50, 0x08, 0x00, 0x00 }) == Bytes{ cmd::readFuse, status::ok, 0xFF, status::ok });
   CHECK(f.run({ cmd::readLock, 4, 0x58, 0x00, 0x00, 0x00 }) == Bytes{ cmd::readLock, status::ok, 0xFF, status::ok });
   CHECK(f.run({ cmd::readOsccal, 4, 0x38, 0x00, 0x00, 0x00 }) == Bytes{ cmd::readOsccal, status::ok, 0xA5, status::ok });
   // retAddr 2: the echo of the first byte
   CHECK(f.run({ cmd::readSignature, 2, 0x30, 0x00, 0x00, 0x00 }) == Bytes{ cmd::readSignature, status::ok, 0x30, status::ok });
}

TEST_CASE("AVR ISP scope: a retAddr out of 1-4 or a short read command fails without an instruction") {
   Fixture f;
   f.enter();
   for(const uint8_t retAddr : { 0, 5, 0xFF }) {
      CAPTURE(retAddr);
      CHECK(f.run({ cmd::readFuse, retAddr, 0x50, 0x00, 0x00, 0x00 }) == Bytes{ cmd::readFuse, status::failed });
   }
   CHECK(f.run({ cmd::readSignature, 4, 0x30, 0x00, 0x00 }) == Bytes{ cmd::readSignature, status::failed });
   CHECK(f.run({ cmd::programFuse, 0xAC, 0xA0, 0x00 }) == Bytes{ cmd::programFuse, status::failed });
   CHECK(f.run({ cmd::loadAddress, 0, 0, 0 }) == Bytes{ cmd::loadAddress, status::failed });
   CHECK(f.run({ cmd::leaveProgmode, 0 }) == Bytes{ cmd::leaveProgmode, status::failed });
   CHECK(f.run({ cmd::chipErase, 10, 0, 0xAC, 0x80, 0x00 }) == Bytes{ cmd::chipErase, status::failed });
   CHECK(f.target.frames.empty());
   CHECK(f.scope.programming());
}

TEST_CASE("AVR ISP scope: fuses and lock are programmed") {
   Fixture f;
   f.enter();
   CHECK(f.run({ cmd::programFuse, 0xAC, 0xA0, 0x00, 0xE2 }) == Bytes{ cmd::programFuse, status::ok });
   CHECK(f.run({ cmd::programFuse, 0xAC, 0xA8, 0x00, 0xD8 }) == Bytes{ cmd::programFuse, status::ok });
   CHECK(f.run({ cmd::programFuse, 0xAC, 0xA4, 0x00, 0xFD }) == Bytes{ cmd::programFuse, status::ok });
   CHECK(f.run({ cmd::programLock, 0xAC, 0xE0, 0x00, 0xFC }) == Bytes{ cmd::programLock, status::ok });
   CHECK(f.target.lowFuse == 0xE2);
   CHECK(f.target.highFuse == 0xD8);
   CHECK(f.target.extendedFuse == 0xFD);
   CHECK(f.target.lock == 0xFC);
   CHECK(f.run({ cmd::readFuse, 4, 0x50, 0x00, 0x00, 0x00 }) == Bytes{ cmd::readFuse, status::ok, 0xE2, status::ok });
}

TEST_CASE("AVR ISP scope: chip erase with the delay or with RDY/BSY polling") {
   Fixture f;
   f.enter();
   f.target.flash[100] = 0x12;
   f.target.eeprom[5] = 0x34;
   uint32_t before = f.pins.time;
   CHECK(f.run({ cmd::chipErase, 10, 0, 0xAC, 0x80, 0x00, 0x00 }) == Bytes{ cmd::chipErase, status::ok });
   CHECK(f.pins.time - before >= 10 * cyclesPerMs);
   CHECK(f.target.chipErases == 1);
   CHECK(f.target.flash[100] == 0xFF);
   CHECK(f.target.eeprom[5] == 0xFF);
   CHECK(f.countFrames(0xF0) == 0);

   f.target.busyPolls = 3;
   before = f.pins.time;
   CHECK(f.run({ cmd::chipErase, 50, 1, 0xAC, 0x80, 0x00, 0x00 }) == Bytes{ cmd::chipErase, status::ok });
   CHECK(f.target.chipErases == 2);
   CHECK(f.countFrames(0xF0) == 4);
   // RDY/BSY polling does not wait the eraseDelay.
   CHECK(f.pins.time - before < 50 * cyclesPerMs);

   // Busy for longer than the poll timeout
   f.target.busyPolls = 1'000'000;
   CHECK(f.run({ cmd::chipErase, 10, 1, 0xAC, 0x80, 0x00, 0x00 }) == Bytes{ cmd::chipErase, status::cmdTimeout });

   // pollMethod 2 is not defined.
   f.target.busyPolls = 0;
   CHECK(f.run({ cmd::chipErase, 10, 2, 0xAC, 0x80, 0x00, 0x00 }) == Bytes{ cmd::chipErase, status::failed });
   CHECK(f.target.chipErases == 3);
}

TEST_CASE("AVR ISP scope: flash page write and read, the address moves on") {
   Fixture f;
   f.enter();
   f.target.busyPolls = 2;
   const Bytes first = pattern(256, 1);
   const Bytes second = pattern(256, 77);
   f.loadAddress(0);
   CHECK(f.programFlash(flashPageMode | 0x80, first) == Bytes{ cmd::programFlash, status::ok });
   // No LOAD_ADDRESS: the next page
   CHECK(f.programFlash(flashPageMode | 0x80, second) == Bytes{ cmd::programFlash, status::ok });
   CHECK(f.target.pageWrites == std::vector<uint32_t>{ 0, 128 });
   CHECK(std::equal(first.begin(), first.end(), f.target.flash.begin()));
   CHECK(std::equal(second.begin(), second.end(), f.target.flash.begin() + 256));
   CHECK(f.countFrames(0x4D) == 0);
   // RDY/BSY polling after each page: busyPolls polls, then ready
   CHECK(f.countFrames(0xF0) == 2 * 3);

   f.loadAddress(0);
   CHECK(f.readFlash(256) == readReply(cmd::readFlash, first));
   CHECK(f.readFlash(256) == readReply(cmd::readFlash, second));
   // Low and high bytes: 0x20 and 0x28
   CHECK(f.countFrames(0x20) == 256);
   CHECK(f.countFrames(0x28) == 256);
}

TEST_CASE("AVR ISP scope: a page in two commands, only the last one writes the page") {
   Fixture f;
   f.enter();
   const Bytes data = pattern(256, 3);
   f.loadAddress(128);
   const Bytes firstHalf(data.begin(), data.begin() + 128);
   const Bytes secondHalf(data.begin() + 128, data.end());
   CHECK(f.programFlash(flashPageMode, firstHalf) == Bytes{ cmd::programFlash, status::ok });
   CHECK(f.target.pageWrites.empty());
   CHECK(f.programFlash(flashPageMode | 0x80, secondHalf) == Bytes{ cmd::programFlash, status::ok });
   CHECK(f.target.pageWrites == std::vector<uint32_t>{ 128 });
   CHECK(std::equal(data.begin(), data.end(), f.target.flash.begin() + 256));
}

TEST_CASE("AVR ISP scope: the extended address is loaded for bit 31 and at each 64K-word boundary") {
   Fixture f;
   f.enter();
   // The last page of 256 KB: word 0x1FF80, extended byte 1
   const Bytes data = pattern(256, 9);
   f.loadAddress(0x8000'0000u | 0x1FF80u);
   CHECK(f.programFlash(flashPageMode | 0x80, data) == Bytes{ cmd::programFlash, status::ok });
   CHECK(std::equal(data.begin(), data.end(), f.target.flash.begin() + 0x3FF00));
   CHECK(f.target.pageWrites == std::vector<uint32_t>{ 0x1FF80 });
   REQUIRE(f.countFrames(0x4D) == 1);
   CHECK(f.target.frames[0] == std::array<uint8_t, 4>{ { 0x4D, 0x00, 0x01, 0x00 } });

   // A read over the boundary of word 0xFFFF and word 0x10000
   f.target.flash[0x1FFFE] = 0x11;
   f.target.flash[0x1FFFF] = 0x22;
   f.target.flash[0x20000] = 0x33;
   f.target.flash[0x20001] = 0x44;
   f.target.frames.clear();
   f.loadAddress(0x8000'0000u | 0xFFFFu);
   CHECK(f.readFlash(4) == readReply(cmd::readFlash, { 0x11, 0x22, 0x33, 0x44 }));
   REQUIRE(f.countFrames(0x4D) == 2);
   CHECK(f.target.frames[0] == std::array<uint8_t, 4>{ { 0x4D, 0x00, 0x00, 0x00 } });
   CHECK(f.target.frames[3] == std::array<uint8_t, 4>{ { 0x4D, 0x00, 0x01, 0x00 } });

   // Without bit 31: no extended address, the address bits 16-23 do not go
   // to the target.
   f.target.frames.clear();
   f.target.extended = 0;
   f.loadAddress(0x10000u);
   CHECK(f.readFlash(2) == readReply(cmd::readFlash, { f.target.flash[0], f.target.flash[1] }));
   CHECK(f.countFrames(0x4D) == 0);
}

TEST_CASE("AVR ISP scope: flash word mode with value polling, the delay for a poll value") {
   Fixture f;
   f.enter();
   f.target.busyPolls = 2;
   f.loadAddress(0x40);
   const Bytes data{ 0x12, 0xFF, 0x34, 0x56 };
   const uint32_t before = f.pins.time;
   // avrdude: mode 0x04 for value polling in word mode, poll1 0xFF
   CHECK(f.run(Fixture::program(cmd::programFlash, 0x84, 5, { 0x60, 0x00, 0x20, 0xFF, 0x00 }, data, data.size())) ==
         Bytes{ cmd::programFlash, status::ok });
   CHECK(std::equal(data.begin(), data.end(), f.target.flash.begin() + 0x80));
   CHECK(f.countFrames(0x60) == 2);
   CHECK(f.countFrames(0x68) == 2);
   // Three bytes with polls (two busy, one ready), the 0xFF byte waits the
   // delay.
   CHECK(f.countFrames(0x20) + f.countFrames(0x28) == 3 * 3);
   CHECK(f.pins.time - before >= 5 * cyclesPerMs);

   f.target.busyPolls = 1'000'000;
   CHECK(f.run(Fixture::program(cmd::programFlash, 0x04, 5, { 0x60, 0x00, 0x20, 0xFF, 0x00 }, { 0x01 }, 1)) ==
         Bytes{ cmd::programFlash, status::cmdTimeout });
   CHECK(f.run(Fixture::program(cmd::programFlash, 0x08, 5, { 0x60, 0x00, 0x20, 0xFF, 0x00 }, { 0x01 }, 1)) ==
         Bytes{ cmd::programFlash, status::rdyBsyTimeout });
}

TEST_CASE("AVR ISP scope: page mode value polling reads the first byte that is not a poll value") {
   Fixture f;
   f.enter();
   f.target.busyPolls = 3;
   f.loadAddress(0);
   Bytes data(256, 0xFF);
   data[1] = 0x11;
   CHECK(f.programFlash(0xA1, data, 10, 0xFF) == Bytes{ cmd::programFlash, status::ok });
   CHECK(f.target.flash[1] == 0x11);
   // Value polling reads the high byte of word 0: three busy reads, then
   // the value.
   CHECK(f.countFrames(0x28) == 4);
   CHECK(f.countFrames(0x20) == 0);
   CHECK(f.target.frames.back() == std::array<uint8_t, 4>{ { 0x28, 0x00, 0x00, 0x00 } });

   // Only poll values in the page: the delay
   f.target.frames.clear();
   const uint32_t before = f.pins.time;
   CHECK(f.programFlash(0xA1, Bytes(256, 0xFF), 7, 0xFF) == Bytes{ cmd::programFlash, status::ok });
   CHECK(f.countFrames(0x28) + f.countFrames(0x20) == 0);
   CHECK(f.pins.time - before >= 7 * cyclesPerMs);
}

TEST_CASE("AVR ISP scope: EEPROM page and byte write, read back") {
   Fixture f;
   f.enter();
   const Bytes data = pattern(8, 5);
   f.loadAddress(0x10);
   CHECK(f.run(Fixture::program(cmd::programEeprom, eepromPageMode | 0x80, 10, { 0xC1, 0xC2, 0xA0, 0x00, 0x00 }, data,
                                data.size())) == Bytes{ cmd::programEeprom, status::ok });
   CHECK(std::equal(data.begin(), data.end(), f.target.eeprom.begin() + 0x10));
   CHECK(f.countFrames(0xC1) == 8);
   CHECK(f.countFrames(0xC2) == 1);

   // Byte mode with value polling: poll1 0xFF and poll2 0x00 cannot be
   // polled.
   f.target.busyPolls = 1;
   f.target.frames.clear();
   f.loadAddress(0x20);
   CHECK(f.run(Fixture::program(cmd::programEeprom, 0x04, 2, { 0xC0, 0x00, 0xA0, 0xFF, 0x00 }, { 0x00, 0x42 }, 2)) ==
         Bytes{ cmd::programEeprom, status::ok });
   CHECK(f.countFrames(0xA0) == 2);
   CHECK(f.target.eeprom[0x20] == 0x00);
   CHECK(f.target.eeprom[0x21] == 0x42);

   f.loadAddress(0x10);
   Bytes expected = data;
   expected.insert(expected.end(), 8, 0xFF);
   expected.insert(expected.end(), { 0x00, 0x42 });
   CHECK(f.run({ cmd::readEeprom, 0x00, 18, 0xA0 }) == readReply(cmd::readEeprom, expected));
}

TEST_CASE("AVR ISP scope: NumBytes from the host is checked against the limit and the data") {
   Fixture f;
   f.enter();
   const Bytes data = pattern(avr::isp::maxDataBytes + 1, 0);
   // Above the limit, with the data
   CHECK(f.programFlash(flashPageMode | 0x80, data) == Bytes{ cmd::programFlash, status::failed });
   // NumBytes 10 with 5 data bytes
   CHECK(f.run(Fixture::program(cmd::programFlash, flashPageMode | 0x80, 10, { 0x40, 0x4C, 0x20, 0, 0 }, pattern(5, 0), 10)) ==
         Bytes{ cmd::programFlash, status::failed });
   CHECK(f.run(Fixture::program(cmd::programEeprom, eepromPageMode | 0x80, 10, { 0xC1, 0xC2, 0xA0, 0, 0 },
                                pattern(5, 0), 0xFFFF)) == Bytes{ cmd::programEeprom, status::failed });
   // The header without poll2
   CHECK(f.run({ cmd::programFlash, 0, 0, flashPageMode, 10, 0x40, 0x4C, 0x20, 0x00 }) == Bytes{ cmd::programFlash, status::failed });
   CHECK(f.readFlash(avr::isp::maxDataBytes + 1) == Bytes{ cmd::readFlash, status::failed });
   CHECK(f.readFlash(0xFFFF) == Bytes{ cmd::readFlash, status::failed });
   CHECK(f.run({ cmd::readEeprom, 0x00, 0x01 }) == Bytes{ cmd::readEeprom, status::failed });
   CHECK(f.target.frames.empty());

   // NumBytes 0 does nothing.
   CHECK(f.readFlash(0) == Bytes{ cmd::readFlash, status::ok, status::ok });
   CHECK(f.programFlash(flashPageMode, {}) == Bytes{ cmd::programFlash, status::ok });
   CHECK(f.target.frames.empty());
}

TEST_CASE("AVR ISP scope: a read that does not fit the response buffer fails") {
   Fixture f;
   f.enter();
   std::array<uint8_t, 10> small{};
   const Bytes fits{ cmd::readFlash, 0x00, 7, 0x20 };
   CHECK(f.scope.process(fits, small) == 10);
   CHECK(small[1] == status::ok);
   CHECK(small[9] == status::ok);
   const Bytes tooLong{ cmd::readFlash, 0x00, 8, 0x20 };
   const std::size_t frames = f.target.frames.size();
   REQUIRE(f.scope.process(tooLong, small) == 2);
   CHECK(small[1] == status::failed);
   CHECK(f.target.frames.size() == frames);
}

TEST_CASE("AVR ISP scope: an SPI fault gives STATUS_PHY_ERROR") {
   Fixture f;
   f.enter();
   f.engine.spi().fail = true;
   CHECK(f.run({ cmd::readSignature, 4, 0x30, 0x00, 0x00, 0x00 }) == Bytes{ cmd::readSignature, status::phyError });
   CHECK(f.run({ cmd::programFuse, 0xAC, 0xA0, 0x00, 0xE2 }) == Bytes{ cmd::programFuse, status::phyError });
   CHECK(f.readFlash(4) == Bytes{ cmd::readFlash, status::phyError });
   CHECK(f.programFlash(flashPageMode | 0x80, pattern(4, 0)) == Bytes{ cmd::programFlash, status::phyError });
   CHECK(f.run({ cmd::chipErase, 10, 0, 0xAC, 0x80, 0x00, 0x00 }) == Bytes{ cmd::chipErase, status::phyError });
}
