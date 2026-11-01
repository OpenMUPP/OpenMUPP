// SPDX-License-Identifier: GPL-3.0-or-later
// The JTAG bit-bang engine on mock pins.

#include "jtag/JtagBitBang.h"

#include "MockBackground.h"
#include "MockJtagPins.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <deque>
#include <vector>

namespace {

using jtag::BitBangPort;
using jtag::Clocking;
using jtag::halfPeriodCycles;
using jtag::TckIdle;
using jtag::Timing;
using Pins = mupptest::MockJtagPins<>;

bool bitOf(const std::vector<uint8_t> &bytes, std::size_t index) {
   return ((bytes[index / 8] >> (index % 8)) & 1u) != 0;
}

struct Result {
   bool ok;
   std::vector<uint8_t> tdo;
};

template <typename Port>
Result shift(Port &port, uint32_t bitCount, const std::vector<uint8_t> &tdi, const std::vector<uint8_t> &tms) {
   std::vector<uint8_t> tdo(tdi.size(), 0);
   const bool ok = port.shift(bitCount, art::span<const uint8_t>(tdi.data(), tdi.size()),
                              art::span<const uint8_t>(tms.data(), tms.size()), art::span<uint8_t>(tdo.data(), tdo.size()));
   return { ok, tdo };
}

} // namespace

TEST_CASE("JTAG bit-bang: half period from kHz, rounded up") {
   CHECK(halfPeriodCycles(72'000'000, 1000) == 36);
   CHECK(halfPeriodCycles(72'000'000, 100) == 360);
   CHECK(halfPeriodCycles(72'000'000, 7) == 5143);
   CHECK(halfPeriodCycles(72'000'000, 4500) == 8);
   CHECK(halfPeriodCycles(72'000'000, 4000) == 9);
   CHECK(halfPeriodCycles(72'000'000, 36'000) == 1);
   CHECK(halfPeriodCycles(72'000'000, 100'000) == 1);
   CHECK(halfPeriodCycles(72'000'000, 0xFFFFFFFF) == 1);
}

TEST_CASE("JTAG bit-bang: speed selects the clocking mode") {
   Pins pins;
   BitBangPort<Pins> port(pins);
   REQUIRE(port.setSpeed(1000));
   CHECK(port.timing() == Timing{ Clocking::Timed, 36, 0 });
   REQUIRE(port.setSpeed(4000));
   CHECK(port.timing() == Timing{ Clocking::Timed, 9, 0 });
   REQUIRE(port.setSpeed(100'000));
   CHECK(port.timing() == Timing{ Clocking::Timed, 1, 0 });
   REQUIRE(port.setSpeed(0));
   CHECK(port.timing() == Timing{ Clocking::Adaptive, 0, 720'000 });
}

TEST_CASE("JTAG bit-bang: adaptive clocking needs RTCK") {
   mupptest::MockJtagPins<false> pins;
   BitBangPort<mupptest::MockJtagPins<false>> port(pins);
   REQUIRE(port.setSpeed(500));
   CHECK_FALSE(port.setSpeed(0));
   CHECK(port.timing().clocking == Clocking::Timed);
}

TEST_CASE("JTAG bit-bang: INIT and FINI go to the pins") {
   Pins pins;
   BitBangPort<Pins> port(pins);
   port.init();
   port.fini();
   CHECK(pins.inits == 1);
   CHECK(pins.finis == 1);
}

TEST_CASE("JTAG bit-bang: bits go out LSB first, TDO is read before the rising edge") {
   for(const uint32_t kHz : { 1000u, 10'000u, 0u }) {
      CAPTURE(kHz);
      // Two flip-flops from TDI to TDO, with 1 and 0 at the start.
      Pins pins({ false, true });
      BitBangPort<Pins> port(pins);
      REQUIRE(port.setSpeed(kHz));
      const std::vector<uint8_t> tdi{ 0xA5, 0x3C, 0x06 };
      const std::vector<uint8_t> tms{ 0x0F, 0xF0, 0x05 };
      const uint32_t bitCount = 19;
      const auto result = shift(port, bitCount, tdi, tms);
      REQUIRE(result.ok);
      CHECK(pins.begins == 1);
      CHECK_FALSE(pins.tckLevel());
      REQUIRE(pins.rising.size() == bitCount);
      for(uint32_t i = 0; i < bitCount; ++i) {
         CAPTURE(i);
         CHECK(pins.rising[i].tdi == bitOf(tdi, i));
         CHECK(pins.rising[i].tms == bitOf(tms, i));
      }
      // TDO: the two start bits of the chain, then TDI.
      CHECK(bitOf(result.tdo, 0) == true);
      CHECK(bitOf(result.tdo, 1) == false);
      for(uint32_t i = 2; i < bitCount; ++i) {
         CAPTURE(i);
         CHECK(bitOf(result.tdo, i) == bitOf(tdi, i - 2));
      }
      // The bits above bitCount stay 0.
      CHECK((result.tdo[2] & 0xF8) == 0);
   }
}

TEST_CASE("JTAG bit-bang: 0 bits do not clock") {
   Pins pins;
   BitBangPort<Pins> port(pins);
   REQUIRE(port.setSpeed(1000));
   CHECK(shift(port, 0, {}, {}).ok);
   CHECK(pins.edges.empty());
}

TEST_CASE("JTAG bit-bang: timed half periods are not shorter than the speed") {
   Pins pins;
   BitBangPort<Pins> port(pins);
   REQUIRE(port.setSpeed(1000));
   const uint32_t start = pins.time;
   REQUIRE(shift(port, 16, { 0x12, 0x34 }, { 0, 0 }).ok);
   REQUIRE(pins.edges.size() == 32);
   // The counter wraps in this test (pins.time starts near 2^32).
   CHECK(pins.edges.back() < start);
   uint32_t last = start;
   for(const uint32_t edge : pins.edges) {
      CHECK(edge - last >= 36);
      CHECK(edge - last <= 38);
      last = edge;
   }
}

TEST_CASE("JTAG bit-bang: the code before each edge does not change the half period") {
   // The edge comes 20 cycles after the end of the wait. A wait from a time
   // stamp after the edge (the engine before #78) adds this code to each
   // half period.
   for(const TckIdle idle : { TckIdle::Low, TckIdle::High }) {
      CAPTURE(static_cast<bool>(idle));
      Pins pins;
      pins.edgeDelayCycles = 20;
      std::vector<uint32_t> edges;
      if(idle == TckIdle::Low) {
         BitBangPort<Pins> port(pins);
         REQUIRE(port.setSpeed(1000));
         REQUIRE(shift(port, 16, { 0x12, 0x34 }, { 0, 0 }).ok);
      } else {
         BitBangPort<Pins, TckIdle::High> port(pins);
         REQUIRE(port.setSpeed(1000));
         REQUIRE(shift(port, 16, { 0x12, 0x34 }, { 0, 0 }).ok);
      }
      // The mock starts with TCK low: with TckIdle::High, the first TCK low
      // is no edge.
      REQUIRE(pins.edges.size() == (idle == TckIdle::Low ? 32 : 31));
      for(std::size_t i = 1; i < pins.edges.size(); ++i) {
         CAPTURE(i);
         CHECK(pins.edges[i] - pins.edges[i - 1] >= 36);
         CHECK(pins.edges[i] - pins.edges[i - 1] <= 38);
      }
   }
}

TEST_CASE("JTAG bit-bang: the fastest speed waits in both half periods") {
   Pins pins;
   BitBangPort<Pins> port(pins);
   REQUIRE(port.setSpeed(100'000));
   REQUIRE(shift(port, 64, std::vector<uint8_t>(8, 0x55), std::vector<uint8_t>(8, 0)).ok);
   REQUIRE(pins.edges.size() == 128);
   for(std::size_t i = 1; i < pins.edges.size(); ++i) {
      CHECK(pins.edges[i] - pins.edges[i - 1] >= 1);
   }
}

TEST_CASE("JTAG bit-bang: adaptive clocking waits for RTCK after each edge") {
   Pins pins;
   pins.rtckLag = 5;
   BitBangPort<Pins> port(pins);
   REQUIRE(port.setSpeed(0));
   REQUIRE(shift(port, 8, { 0xFF }, { 0 }).ok);
   REQUIRE(pins.edges.size() == 16);
   for(std::size_t i = 1; i < pins.edges.size(); ++i) {
      CHECK(pins.edges[i] - pins.edges[i - 1] >= 5);
   }
}

TEST_CASE("JTAG bit-bang: RTCK that does not follow TCK is a timeout, TCK ends low") {
   Pins pins;
   pins.rtckStuck = true;
   BitBangPort<Pins> port(pins);
   REQUIRE(port.setSpeed(0));
   const uint32_t start = pins.time;
   const auto result = shift(port, 16, { 0xFF, 0xFF }, { 0, 0 });
   CHECK_FALSE(result.ok);
   CHECK_FALSE(pins.tckLevel());
   CHECK(pins.rising.size() == 1);
   // It gave up after the timeout of 10 ms at 72 MHz.
   CHECK(pins.time - start >= 720'000);
   CHECK(pins.time - start <= 720'010);
}

namespace {

// Samples the time, the TCK level and the rising edges of the pins.
mupptest::MockBackground backgroundOf(const Pins &pins) {
   return mupptest::MockBackground(
   [&pins] { return mupptest::MockBackground::Call{ pins.time, pins.tckLevel(), pins.rising.size() }; });
}

// 1 ms at 72 MHz
constexpr uint32_t msCycles = 72'000;

} // namespace

TEST_CASE("JTAG bit-bang: a long shift services the background between bytes, with TCK low") {
   for(const uint32_t kHz : { 10u, 0u }) {
      CAPTURE(kHz);
      Pins pins;
      // Adaptive: RTCK follows after 1800 cycles, about 10 kHz.
      pins.rtckLag = 1800;
      auto background = backgroundOf(pins);
      BitBangPort<Pins> port(pins, &background);
      REQUIRE(port.setSpeed(kHz));
      const uint32_t start = pins.time;
      // 64 bytes at 10 kHz: 51 ms
      const uint32_t bitCount = 512;
      REQUIRE(shift(port, bitCount, std::vector<uint8_t>(64, 0x5A), std::vector<uint8_t>(64, 0)).ok);
      REQUIRE(pins.rising.size() == bitCount);
      // A call at the first byte boundary after each interval: the gap is
      // 1 ms plus at most one byte (8 bits of 7200 cycles, and some loop
      // cycles).
      const uint32_t byteCycles = 8 * 7200 + 1000;
      REQUIRE_FALSE(background.calls.empty());
      CHECK(background.calls.front().time - start <= byteCycles);
      CHECK(pins.time - background.calls.back().time <= msCycles + byteCycles);
      for(std::size_t i = 0; i < background.calls.size(); ++i) {
         CAPTURE(i);
         const auto &call = background.calls[i];
         CHECK_FALSE(call.clock);
         CHECK(call.count % 8 == 0);
         if(i > 0) {
            const uint32_t gap = call.time - background.calls[i - 1].time;
            CHECK(gap >= msCycles);
            CHECK(gap <= msCycles + byteCycles);
         }
      }
   }
}

TEST_CASE("JTAG bit-bang: with the background, half periods are not shorter than the speed") {
   Pins pins;
   auto background = backgroundOf(pins);
   // The background takes 50000 cycles: about 0.7 ms.
   background.onService = [&pins] { pins.time += 50'000; };
   BitBangPort<Pins> port(pins, &background);
   REQUIRE(port.setSpeed(100));
   uint32_t last = pins.time;
   REQUIRE(shift(port, 1024, std::vector<uint8_t>(128, 0x33), std::vector<uint8_t>(128, 0)).ok);
   REQUIRE_FALSE(background.calls.empty());
   REQUIRE(pins.edges.size() == 2048);
   for(const uint32_t edge : pins.edges) {
      CHECK(edge - last >= 360);
      last = edge;
   }
}

TEST_CASE("JTAG bit-bang: with a slow background, TDI and TMS keep the setup time before the rising edge") {
   Pins pins;
   auto background = backgroundOf(pins);
   // The background takes 50000 cycles, longer than the half period.
   background.onService = [&pins] { pins.time += 50'000; };
   BitBangPort<Pins> port(pins, &background);
   REQUIRE(port.setSpeed(100));
   REQUIRE(shift(port, 1024, std::vector<uint8_t>(128, 0x33), std::vector<uint8_t>(128, 0x0F)).ok);
   REQUIRE(background.calls.size() > 5);
   REQUIRE(pins.rising.size() == 1024);
   // The half period starts at the falling edge, and the writes come some
   // steps later: as without a background, the setup time is the half
   // period less these steps.
   for(std::size_t i = 0; i < pins.rising.size(); ++i) {
      CAPTURE(i);
      CHECK(pins.rising[i].time - pins.rising[i].dataWritten >= 360 - 4);
   }
}

TEST_CASE("JTAG bit-bang: the background interval runs on over short shifts") {
   Pins pins;
   auto background = backgroundOf(pins);
   BitBangPort<Pins> port(pins, &background);
   REQUIRE(port.setSpeed(1000));
   const uint32_t start = pins.time;
   // 400 shifts of 8 bits: about 600 cycles each, 3.3 ms together
   for(int i = 0; i < 400; ++i) {
      REQUIRE(shift(port, 8, { 0x01 }, { 0 }).ok);
   }
   CHECK(background.calls.size() >= (pins.time - start) / msCycles - 1);
   CHECK(background.calls.size() <= (pins.time - start) / msCycles + 1);
}

namespace {
using IdleHighPort = BitBangPort<Pins, jtag::TckIdle::High>;
} // namespace

TEST_CASE("JTAG bit-bang, TCK idle high: INIT sets TCK high") {
   Pins pins;
   IdleHighPort port(pins);
   port.init();
   CHECK(pins.tckLevel());
   CHECK(pins.edges.empty());
}

TEST_CASE("JTAG bit-bang, TCK idle high: same bits and TDO as with TCK idle low") {
   for(const uint32_t kHz : { 1000u, 10'000u, 0u }) {
      CAPTURE(kHz);
      Pins low({ false, true });
      Pins high({ false, true });
      BitBangPort<Pins> lowPort(low);
      IdleHighPort highPort(high);
      lowPort.init();
      highPort.init();
      REQUIRE(lowPort.setSpeed(kHz));
      REQUIRE(highPort.setSpeed(kHz));
      const std::vector<uint8_t> tdi{ 0xA5, 0x3C, 0x06 };
      const std::vector<uint8_t> tms{ 0x0F, 0xF0, 0x05 };
      const auto lowResult = shift(lowPort, 19, tdi, tms);
      const auto highResult = shift(highPort, 19, tdi, tms);
      REQUIRE(lowResult.ok);
      REQUIRE(highResult.ok);
      CHECK(highResult.tdo == lowResult.tdo);
      CHECK(high.tckLevel());
      REQUIRE(high.rising.size() == low.rising.size());
      for(std::size_t i = 0; i < low.rising.size(); ++i) {
         CAPTURE(i);
         CHECK(high.rising[i].tdi == low.rising[i].tdi);
         CHECK(high.rising[i].tms == low.rising[i].tms);
      }
   }
}

TEST_CASE("JTAG bit-bang, TCK idle high: timed half periods are not shorter than the speed") {
   Pins pins;
   IdleHighPort port(pins);
   port.init();
   REQUIRE(port.setSpeed(1000));
   uint32_t last = pins.time;
   REQUIRE(shift(port, 16, { 0x12, 0x34 }, { 0, 0 }).ok);
   REQUIRE(pins.edges.size() == 32);
   for(const uint32_t edge : pins.edges) {
      CHECK(edge - last >= 36);
      CHECK(edge - last <= 38);
      last = edge;
   }
   // TDI and TMS change after the falling edge, a half period before the
   // rising edge.
   for(const auto &rising : pins.rising) {
      CHECK(rising.time - rising.dataWritten >= 36 - 4);
   }
}

TEST_CASE("JTAG bit-bang, TCK idle high: RTCK that does not follow is a timeout, TCK stays low") {
   Pins pins;
   pins.rtckStuck = true;
   IdleHighPort port(pins);
   port.init();
   REQUIRE(port.setSpeed(0));
   CHECK_FALSE(shift(port, 16, { 0xFF, 0xFF }, { 0, 0 }).ok);
   // RTCK is stuck low: the falling edge passes, the rising edge times out.
   CHECK(pins.rising.size() == 1);
   CHECK(pins.tckLevel());

   Pins stuckHigh;
   IdleHighPort highPort(stuckHigh);
   highPort.init();
   REQUIRE(highPort.setSpeed(0));
   stuckHigh.rtckLag = 0xFFFFFFFFu;
   CHECK_FALSE(shift(highPort, 16, { 0xFF, 0xFF }, { 0, 0 }).ok);
   // RTCK does not follow the first falling edge: no rising edge, TCK low.
   CHECK(stuckHigh.rising.empty());
   CHECK_FALSE(stuckHigh.tckLevel());
}

TEST_CASE("JTAG bit-bang, TCK idle high: the background runs between bytes, with TCK high") {
   Pins pins;
   auto background = backgroundOf(pins);
   IdleHighPort port(pins, &background);
   port.init();
   REQUIRE(port.setSpeed(10));
   REQUIRE(shift(port, 512, std::vector<uint8_t>(64, 0x5A), std::vector<uint8_t>(64, 0)).ok);
   REQUIRE_FALSE(background.calls.empty());
   for(const auto &call : background.calls) {
      CHECK(call.clock);
      CHECK(call.count % 8 == 0);
   }
}
