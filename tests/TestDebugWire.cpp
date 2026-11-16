// SPDX-License-Identifier: GPL-3.0-or-later
// The debugWIRE physical layer on a mock dW target: the break and the baud
// rate measurement, send with the echo check, receive, the break of the
// target, and the interrupt lock.

#include "avr/DebugWireEngine.h"

#include "MockBackground.h"
#include "MockDebugWireTarget.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using avr::DwStatus;
using Engine = avr::DebugWireEngine<mupptest::MockDwPins>;

constexpr uint32_t clockHz = mupptest::MockDwPins::clockHz;

// The bit time of a target at the clock: dW runs at the clock / 128.
constexpr uint32_t bitCyclesAt(uint32_t targetHz) {
   return static_cast<uint32_t>((uint64_t{ clockHz } * 128u + targetHz / 2u) / targetHz);
}

struct Fixture {
   explicit Fixture(uint32_t targetHz, protocol::Background *background = nullptr)
      : target(bitCyclesAt(targetHz))
      , engine(pins, background) {
      engine.connect();
   }

   mupptest::MockDwTarget target;
   mupptest::MockDwPins pins{ target };
   Engine engine;
};

} // namespace

TEST_CASE("debugWIRE: sync measures the bit time of the target and switches the receiver on") {
   for(const uint32_t targetHz : { 128'000u, 1'000'000u, 8'000'000u, 16'000'000u, 20'000'000u }) {
      CAPTURE(targetHz);
      Fixture f{ targetHz };
      CHECK_FALSE(f.engine.synced());
      REQUIRE(f.engine.sync() == DwStatus::Ok);
      CHECK(f.engine.synced());
      // Polling gives some time steps of error over 8 bits.
      CHECK(f.engine.bitCycles() >= f.target.bitCycles - 1);
      CHECK(f.engine.bitCycles() <= f.target.bitCycles + 1);
      const uint32_t baud = targetHz / 128u;
      CHECK(f.engine.baud() >= baud - baud / 100u);
      CHECK(f.engine.baud() <= baud + baud / 100u);
      CHECK(f.pins.rxBaud == f.engine.baud());
      // The receiver starts after the 0x55: it has no byte.
      CHECK(f.pins.rxRead() < 0);
      CHECK_FALSE(f.pins.locked);
   }
}

TEST_CASE("debugWIRE: the first break is long enough for the slowest target, later breaks are 40 bits") {
   Fixture f{ 8'000'000 };
   REQUIRE(f.engine.sync() == DwStatus::Ok);
   REQUIRE(f.target.breaks.size() == 1);
   CHECK(f.target.breaks[0].second >= uint64_t{ clockHz } / 1'000'000u * avr::dwFirstBreakUs);
   CHECK(f.target.breaks[0].second > 10u * (clockHz / avr::dwMinBaud));

   REQUIRE(f.engine.sync() == DwStatus::Ok);
   REQUIRE(f.target.breaks.size() == 2);
   CHECK(f.target.breaks[1].second >= 40u * f.target.bitCycles - 40u);
   CHECK(f.target.breaks[1].second <= 41u * f.target.bitCycles);
   CHECK(f.engine.synced());
}

TEST_CASE("debugWIRE: no target gives NoSync after the time limit, and the line is released") {
   Fixture f{ 8'000'000 };
   f.target.present = false;
   const uint64_t start = f.pins.time();
   CHECK(f.engine.sync() == DwStatus::NoSync);
   CHECK_FALSE(f.engine.synced());
   const uint64_t limit = uint64_t{ clockHz } / 1'000'000u * (avr::dwFirstBreakUs + avr::dwSyncTimeoutUs);
   CHECK(f.pins.time() - start >= limit);
   CHECK(f.pins.time() - start < limit + clockHz / 100u);
   CHECK(f.pins.sense());
   CHECK_FALSE(f.pins.locked);
   const uint8_t byte = 0x06;
   CHECK(f.engine.send(&byte, 1) == DwStatus::NotSynced);
}

TEST_CASE("debugWIRE: sync rejects a line that is not 0x55") {
   Fixture f{ 8'000'000 };
   f.target.present = false;
   const uint32_t bit = f.target.bitCycles;
   // The probe break starts now and lasts dwFirstBreakUs.
   const uint64_t after = f.pins.time() + uint64_t{ clockHz } / 1'000'000u * avr::dwFirstBreakUs + 2000u;
   SUBCASE("0x00") {
      f.target.send(0x00, after);
   }
   SUBCASE("0xF0") {
      f.target.send(0xF0, after);
   }
   SUBCASE("0x55 with a short bit 7") {
      for(uint64_t low = 0; low < 8; low += 2) {
         f.target.pullLow(after + low * bit, after + (low + 1u) * bit);
      }
      f.target.pullLow(after + 8u * bit, after + 8u * bit + bit / 4u);
   }
   SUBCASE("line held low") {
      f.target.pullLow(0, UINT64_MAX);
   }
   CHECK(f.engine.sync() == DwStatus::NoSync);
   CHECK_FALSE(f.engine.synced());
   CHECK_FALSE(f.pins.locked);
}

TEST_CASE("debugWIRE: send gives the bytes to the target at its bit time and checks the echo") {
   for(const uint32_t targetHz : { 128'000u, 1'000'000u, 8'000'000u, 20'000'000u }) {
      CAPTURE(targetHz);
      Fixture f{ targetHz };
      REQUIRE(f.engine.sync() == DwStatus::Ok);
      const std::array<uint8_t, 6> bytes{ { 0x00, 0xFF, 0x55, 0xAA, 0x83, 0xF0 } };
      CHECK(f.engine.send(bytes.data(), bytes.size()) == DwStatus::Ok);
      CHECK(f.target.received == std::vector<uint8_t>(bytes.begin(), bytes.end()));
      CHECK(f.target.framingErrors == 0);
      // The receiver read each echo.
      CHECK(f.pins.rxRead() < 0);
      CHECK(f.pins.sense());
   }
}

TEST_CASE("debugWIRE: the interrupts are off for one byte at most") {
   Fixture f{ 1'000'000 };
   REQUIRE(f.engine.sync() == DwStatus::Ok);
   const std::array<uint8_t, 4> bytes{ { 0x01, 0x02, 0x03, 0x04 } };
   f.pins.maxLockedCycles = 0;
   const int locks = f.pins.locks;
   REQUIRE(f.engine.send(bytes.data(), bytes.size()) == DwStatus::Ok);
   CHECK(f.pins.locks == locks + 4);
   CHECK(f.pins.maxLockedCycles >= 10u * f.engine.bitCycles());
   CHECK(f.pins.maxLockedCycles <= 10u * f.engine.bitCycles() + 8u);
   CHECK_FALSE(f.pins.locked);
}

TEST_CASE("debugWIRE: a target that sends during a byte gives Collision") {
   Fixture f{ 8'000'000 };
   REQUIRE(f.engine.sync() == DwStatus::Ok);
   // Low in data bit 0 of the second byte
   const uint64_t secondStart = f.pins.time() + 10u * f.engine.bitCycles() + 4u * f.engine.bitCycles();
   f.target.pullLow(secondStart, secondStart + 25u * f.engine.bitCycles());
   const std::array<uint8_t, 3> bytes{ { 0xFF, 0xFF, 0xFF } };
   CHECK(f.engine.send(bytes.data(), bytes.size()) == DwStatus::Collision);
   CHECK_FALSE(f.pins.locked);
}

TEST_CASE("debugWIRE: receive reads the reply of the target") {
   Fixture f{ 16'000'000 };
   REQUIRE(f.engine.sync() == DwStatus::Ok);
   f.target.replies[0xF0] = { false, { 0x12, 0x34 } };
   const uint8_t command = 0xF0;
   REQUIRE(f.engine.send(&command, 1) == DwStatus::Ok);
   std::array<uint8_t, 2> reply{};
   CHECK(f.engine.receive(reply.data(), reply.size()) == DwStatus::Ok);
   CHECK(reply == std::array<uint8_t, 2>{ { 0x12, 0x34 } });

   SUBCASE("no more bytes: Timeout after 40 bit times") {
      const uint64_t start = f.pins.time();
      uint8_t extra = 0;
      CHECK(f.engine.receive(&extra, 1) == DwStatus::Timeout);
      CHECK(f.pins.time() - start >= 40u * f.engine.bitCycles());
      CHECK(f.pins.time() - start < 41u * f.engine.bitCycles());
   }
   SUBCASE("time limit in us") {
      const uint64_t start = f.pins.time();
      uint8_t extra = 0;
      CHECK(f.engine.receive(&extra, 1, 2000) == DwStatus::Timeout);
      CHECK(f.pins.time() - start >= 2000u * (clockHz / 1'000'000u));
   }
}

TEST_CASE("debugWIRE: the break and 0x55 of a target that stops") {
   Fixture f{ 8'000'000 };
   REQUIRE(f.engine.sync() == DwStatus::Ok);

   SUBCASE("no break yet: one check gives Timeout") {
      CHECK(f.engine.waitBreak(0) == DwStatus::Timeout);
   }
   SUBCASE("break after a command (reset)") {
      f.target.replies[0x07] = { true, {} };
      const uint8_t command = 0x07;
      REQUIRE(f.engine.send(&command, 1) == DwStatus::Ok);
      CHECK(f.engine.waitBreak(10'000) == DwStatus::Ok);
      CHECK(f.pins.rxRead() < 0);
   }
   SUBCASE("break while the target runs") {
      f.target.sendBreak(f.pins.time() + 1000u * f.engine.bitCycles());
      CHECK(f.engine.waitBreak(0) == DwStatus::Timeout);
      CHECK(f.engine.waitBreak(100'000) == DwStatus::Ok);
   }
   SUBCASE("receive sees the break") {
      f.target.sendBreak(f.pins.time() + 10u);
      uint8_t value = 0;
      CHECK(f.engine.receive(&value, 1) == DwStatus::Break);
      // The 0x55 follows.
      CHECK(f.engine.receive(&value, 1) == DwStatus::Ok);
      CHECK(value == 0x55);
   }
   SUBCASE("another byte is not a break") {
      f.target.send(0x42, f.pins.time() + 10u);
      CHECK(f.engine.waitBreak(10'000) == DwStatus::NoSync);
   }
}

TEST_CASE("debugWIRE: flush drops the received bytes") {
   Fixture f{ 8'000'000 };
   REQUIRE(f.engine.sync() == DwStatus::Ok);
   f.target.send(0x11, f.pins.time() + 10u);
   f.target.send(0x22, f.pins.time() + 10u);
   f.engine.waitUs(1000);
   f.engine.flush();
   uint8_t value = 0;
   CHECK(f.engine.receive(&value, 1, 100) == DwStatus::Timeout);
}

TEST_CASE("debugWIRE: the background runs in the break, never with the interrupts off") {
   mupptest::MockDwTarget *targetOf = nullptr;
   mupptest::MockDwPins *pinsOf = nullptr;
   bool lockedAtCall = false;
   mupptest::MockBackground background{ [&] {
      lockedAtCall = lockedAtCall || pinsOf->locked;
      return mupptest::MockBackground::Call{ static_cast<uint32_t>(pinsOf->time()), pinsOf->sense(), targetOf->received.size() };
   } };
   mupptest::MockDwTarget target{ bitCyclesAt(1'000'000) };
   mupptest::MockDwPins pins{ target };
   targetOf = &target;
   pinsOf = &pins;
   Engine engine{ pins, &background };
   engine.connect();
   REQUIRE(engine.sync() == DwStatus::Ok);
   // 30 ms of break: one call per ms. Then one call can come in the stop
   // bit of the 0x55.
   std::size_t inBreak = 0;
   for(const auto &call : background.calls) {
      inBreak += call.clock ? 0u : 1u;
   }
   CHECK(inBreak >= 29);
   CHECK(background.calls.size() <= 32);

   // 100 bytes at 7.8 kbaud: about 128 ms, a call before the bytes after
   // each ms
   background.calls.clear();
   std::vector<uint8_t> bytes(100, 0xA5);
   REQUIRE(engine.send(bytes.data(), static_cast<uint32_t>(bytes.size())) == DwStatus::Ok);
   CHECK(background.calls.size() >= 90);
   for(const auto &call : background.calls) {
      // The line is idle: between bytes
      CHECK(call.clock);
   }
   CHECK(target.received == bytes);
   CHECK_FALSE(lockedAtCall);
}

TEST_CASE("debugWIRE: connect and disconnect drive and release the pins") {
   mupptest::MockDwTarget target{ bitCyclesAt(8'000'000) };
   mupptest::MockDwPins pins{ target };
   Engine engine{ pins };
   engine.connect();
   CHECK(pins.inits == 1);
   CHECK(pins.driven);
   CHECK(pins.sense());
   REQUIRE(engine.sync() == DwStatus::Ok);
   engine.disconnect();
   CHECK(pins.finis == 1);
   CHECK_FALSE(pins.driven);
   CHECK_FALSE(engine.synced());
   CHECK(pins.sense());
   uint8_t value = 0;
   CHECK(engine.receive(&value, 1) == DwStatus::NotSynced);
   CHECK(engine.waitBreak(0) == DwStatus::NotSynced);
}
