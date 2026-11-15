// SPDX-License-Identifier: GPL-3.0-or-later
// The AVR ISP engine on a mock AVR: the speed table, the instruction over
// the SPI and over bit-bang, and the programming enable with the sync.

#include "avr/IspEngine.h"

#include "MockBackground.h"
#include "MockIspTarget.h"

#include <doctest/doctest.h>

#include <cstdint>

namespace {

using avr::Instruction;
using avr::spiShiftFor;
using Engine = avr::IspEngine<mupptest::MockIspPins, mupptest::MockIspSpi>;

constexpr uint32_t spiClockHz = 72'000'000;
constexpr uint32_t cyclesPerMs = 72'000;

constexpr avr::EnableParams enableParams{
   .stabDelayMs = 20,
   .cmdexeDelayMs = 1,
   .synchLoops = 32,
   .byteDelayMs = 0,
   .pollValue = 0x53,
   .pollIndex = 3,
   .command{ { 0xAC, 0x53, 0x00, 0x00 } },
};

struct Fixture {
   explicit Fixture(uint32_t kHz) {
      REQUIRE(engine.setSpeed(kHz));
      engine.connect();
   }

   mupptest::MockIspTarget target;
   mupptest::MockIspPins pins{ target };
   Engine engine{ pins };
};

Instruction readSignature(Engine &engine, uint8_t index) {
   Instruction response{};
   REQUIRE(engine.instruction({ { 0x30, 0x00, index, 0x00 } }, response));
   return response;
}

} // namespace

TEST_CASE("AVR ISP: the SPI rates are 18 MHz to 281 kHz, bit-bang below, SCK never faster than the request") {
   CHECK(spiShiftFor(spiClockHz, 100'000) == 2);
   CHECK(spiShiftFor(spiClockHz, 18'000) == 2);
   CHECK(spiShiftFor(spiClockHz, 17'999) == 3);
   CHECK(spiShiftFor(spiClockHz, 9000) == 3);
   CHECK(spiShiftFor(spiClockHz, 4500) == 4);
   CHECK(spiShiftFor(spiClockHz, 2250) == 5);
   CHECK(spiShiftFor(spiClockHz, 1125) == 6);
   CHECK(spiShiftFor(spiClockHz, 563) == 7);
   CHECK(spiShiftFor(spiClockHz, 562) == 8);
   CHECK(spiShiftFor(spiClockHz, 282) == 8);
   // 281.25 kHz is above 281 kHz: bit-bang
   CHECK(spiShiftFor(spiClockHz, 281) == 0);
   CHECK(spiShiftFor(spiClockHz, 250) == 0);
   CHECK(spiShiftFor(spiClockHz, 1) == 0);
   CHECK(spiShiftFor(spiClockHz, 0) == 0);
}

TEST_CASE("AVR ISP: setSpeed selects the SPI or bit-bang, 0 is refused") {
   mupptest::MockIspTarget target;
   mupptest::MockIspPins pins{ target };
   Engine engine{ pins };
   // Before setSpeed: bit-bang at 100 kHz
   CHECK(engine.spiShift() == 0);
   CHECK(engine.halfCycles() == 360);

   CHECK(engine.setSpeed(2000));
   CHECK(engine.spiShift() == 6);
   CHECK(engine.spi().shift() == 6);

   CHECK(engine.setSpeed(32));
   CHECK(engine.spiShift() == 0);
   // 72 MHz / (2 * 32 kHz) = 1125
   CHECK(engine.halfCycles() == 1125);

   CHECK_FALSE(engine.setSpeed(0));
   CHECK(engine.spiShift() == 0);
   CHECK(engine.halfCycles() == 1125);
}

TEST_CASE("AVR ISP: connect drives the pins with RESET asserted, disconnect releases them") {
   Fixture f(1000);
   CHECK(f.pins.driven);
   CHECK(f.target.inReset());
   CHECK(f.engine.spi().inits == 1);
   CHECK_FALSE(f.target.sckHighAtReset);

   f.engine.disconnect();
   CHECK_FALSE(f.pins.driven);
   CHECK_FALSE(f.target.inReset());
   CHECK(f.engine.spi().finis == 1);
}

TEST_CASE("AVR ISP: programming enable and signature read, over the SPI and over bit-bang") {
   for(const uint32_t kHz : { 18'000u, 281u + 1u, 250u, 32u }) {
      CAPTURE(kHz);
      Fixture f(kHz);
      REQUIRE(f.engine.enableProgramming(enableParams));
      CHECK(f.target.enabled);
      CHECK(f.target.frames.size() == 1);

      const Instruction sig0 = readSignature(f.engine, 0);
      // The echo: each response byte is the byte before it.
      CHECK(sig0[1] == 0x30);
      CHECK(sig0[2] == 0x00);
      CHECK(sig0[3] == 0x1E);
      CHECK(readSignature(f.engine, 1)[3] == 0x95);
      CHECK(readSignature(f.engine, 2)[3] == 0x0F);
      CHECK_FALSE(f.pins.mosiChangedWhileSckHigh);
      CHECK_FALSE(f.engine.spi().sckHighAtTransfer);
      CHECK_FALSE(f.pins.sckLevel());
      CHECK(f.engine.spi().transfers.empty() == (f.engine.spiShift() == 0));
   }
}

TEST_CASE("AVR ISP: bit-bang gives the requested SCK period") {
   Fixture f(32);
   Instruction response{};
   REQUIRE(f.engine.instruction({ { 0xAC, 0x53, 0x00, 0x00 } }, response));
   REQUIRE(f.pins.risingTimes.size() == 32);
   const uint32_t period = 2 * 1125;
   for(std::size_t i = 1; i < f.pins.risingTimes.size(); ++i) {
      CAPTURE(i);
      const uint32_t delta = f.pins.risingTimes[i] - f.pins.risingTimes[i - 1];
      // One pass of the wait loop is one time step of the mock; a new byte
      // starts its pacer again.
      CHECK(delta >= period);
      CHECK(delta <= period + 8);
   }
}

TEST_CASE("AVR ISP: the RESET pulse and the stabilisation delay come before the first instruction") {
   Fixture f(1000);
   const uint32_t connected = f.pins.time;
   REQUIRE(f.engine.enableProgramming(enableParams));
   // connect() asserts RESET, the pulse asserts it again.
   REQUIRE(f.pins.assertTimes.size() == 2);
   CHECK(f.target.resets == 2);
   const uint32_t pulseEnd = f.pins.assertTimes[1];
   CHECK(pulseEnd - connected >= 2 * avr::resetPulseUs * 72u);
   REQUIRE_FALSE(f.pins.risingTimes.empty());
   CHECK(f.pins.risingTimes[0] - pulseEnd >= 20u * cyclesPerMs);
   CHECK_FALSE(f.target.sckHighAtReset);
}

TEST_CASE("AVR ISP: a target out of sync gets one SCK pulse per failed attempt") {
   for(const uint32_t kHz : { 1000u, 100u }) {
      CAPTURE(kHz);
      Fixture f(kHz);
      // Two bits short of a frame: two pulses align the frames.
      f.target.syncOffset = 30;
      avr::EnableParams params = enableParams;
      params.cmdexeDelayMs = 2;
      REQUIRE(f.engine.enableProgramming(params));
      CHECK(f.target.enabled);
      // Three attempts of 32 bits and two pulses
      REQUIRE(f.pins.risingTimes.size() == 3 * 32 + 2);
      // cmdexeDelayMs after each attempt, before the pulse
      for(const std::size_t pulse : { 32u, 65u }) {
         CAPTURE(pulse);
         CHECK(f.pins.risingTimes[pulse] - f.pins.risingTimes[pulse - 1] >= 2 * cyclesPerMs);
      }
      CHECK(readSignature(f.engine, 0)[3] == 0x1E);
   }
}

TEST_CASE("AVR ISP: no sync after synchLoops attempts") {
   Fixture f(1000);
   // The engine never gets 0x53 back: the frames never align in 4 attempts.
   f.target.syncOffset = 20;
   avr::EnableParams params = enableParams;
   params.synchLoops = 4;
   CHECK_FALSE(f.engine.enableProgramming(params));
   CHECK(f.pins.risingTimes.size() == 4 * 32 + 3);
}

TEST_CASE("AVR ISP: synchLoops 0 is one attempt, pollIndex 0 does not check, pollIndex 5 fails") {
   Fixture f(1000);
   f.target.syncOffset = 20;
   avr::EnableParams params = enableParams;
   params.synchLoops = 0;
   CHECK_FALSE(f.engine.enableProgramming(params));
   CHECK(f.pins.risingTimes.size() == 32);

   params.pollIndex = 0;
   CHECK(f.engine.enableProgramming(params));
   CHECK(f.pins.risingTimes.size() == 64);

   // In sync, but a poll index above 4 (from the host) never matches.
   f.target.syncOffset = 0;
   params.pollIndex = 5;
   CHECK_FALSE(f.engine.enableProgramming(params));
   CHECK(f.pins.risingTimes.size() == 96);
}

TEST_CASE("AVR ISP: byteDelay waits between the bytes of the programming enable") {
   Fixture f(1000);
   avr::EnableParams params = enableParams;
   params.byteDelayMs = 2;
   REQUIRE(f.engine.enableProgramming(params));
   REQUIRE(f.pins.risingTimes.size() == 32);
   for(std::size_t byte = 1; byte < 4; ++byte) {
      CHECK(f.pins.risingTimes[byte * 8] - f.pins.risingTimes[byte * 8 - 1] >= 2 * cyclesPerMs);
   }
}

TEST_CASE("AVR ISP: an SPI fault ends the instruction and the programming enable") {
   Fixture f(4000);
   REQUIRE(f.engine.spiShift() != 0);
   f.engine.spi().fail = true;
   Instruction response{};
   CHECK_FALSE(f.engine.instruction({ { 0x30, 0x00, 0x00, 0x00 } }, response));
   CHECK_FALSE(f.engine.enableProgramming(enableParams));
   // The programming enable stops at the first fault.
   CHECK(f.engine.spi().transfers.size() == 2);
}

TEST_CASE("AVR ISP: the Background runs in the waits and between bytes, with SCK low") {
   mupptest::MockIspTarget target;
   mupptest::MockIspPins pins{ target };
   mupptest::MockBackground background(
   [&pins] { return mupptest::MockBackground::Call{ pins.time, pins.sckLevel(), pins.risingTimes.size() }; });
   Engine engine{ pins, &background };
   REQUIRE(engine.setSpeed(8));
   engine.connect();
   REQUIRE(engine.enableProgramming(enableParams));
   // 20 ms of stabilisation delay: about one call per ms
   CHECK(background.calls.size() >= 20);
   for(const auto &call : background.calls) {
      CHECK_FALSE(call.clock);
   }

   // 4 bytes at 8 kHz take 4 ms: calls between the bytes
   const std::size_t before = background.calls.size();
   static_cast<void>(readSignature(engine, 0));
   CHECK(background.calls.size() > before);
   for(std::size_t i = before; i < background.calls.size(); ++i) {
      CHECK(background.calls[i].count % 8 == 0);
   }
}
