// The JTAG fast path on a mock SPI: the same bits and TDO as the bit-bang
// engine.

#include "jtag/JtagFastPath.h"

#include "MockBackground.h"
#include "MockJtagPins.h"
#include "MockJtagSpi.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <deque>
#include <random>
#include <vector>

namespace {

using jtag::FastPort;
using jtag::spiShiftFor;
using Pins = vsltest::MockJtagPins<>;
using Spi = vsltest::MockJtagSpi<Pins>;

constexpr uint32_t spiClockHz = 36'000'000;

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

// A fast port on its own pins and SPI, with a chain of flip-flops
struct Fixture {
   explicit Fixture(std::deque<bool> chain = { false, true, true })
      : pins(std::move(chain))
      , port(pins)
      , spi(port.spi()) {
      port.init();
   }

   Pins pins;
   FastPort<Pins, Spi> port;
   Spi &spi;
};

std::vector<uint8_t> randomBytes(std::mt19937 &random, std::size_t count) {
   std::vector<uint8_t> bytes(count);
   for(auto &byte : bytes) {
      byte = static_cast<uint8_t>(random());
   }
   return bytes;
}

} // namespace

TEST_CASE("JTAG fast path: the faster engine wins, TCK is never faster than the request") {
   // SPI rates: 9000, 4500, 2250, 1125, 562.5, 281.25, 140.625 kHz.
   // 18000 kHz is off (spiMinShift).
   CHECK(spiShiftFor(spiClockHz, 100'000) == 2);
   CHECK(spiShiftFor(spiClockHz, 18'000) == 2);
   CHECK(spiShiftFor(spiClockHz, 17'999) == 2);
   CHECK(spiShiftFor(spiClockHz, 9000) == 2);
   CHECK(spiShiftFor(spiClockHz, 4500) == 3);
   // Bit-bang is limited to 1000 kHz (bitBangMaxKHz): the SPI wins above.
   CHECK(spiShiftFor(spiClockHz, 4499) == 4);
   CHECK(spiShiftFor(spiClockHz, 3000) == 4);
   CHECK(spiShiftFor(spiClockHz, 2250) == 4);
   CHECK(spiShiftFor(spiClockHz, 2249) == 5);
   CHECK(spiShiftFor(spiClockHz, 1125) == 5);
   // Bit-bang gives the request exactly, the SPI less.
   CHECK(spiShiftFor(spiClockHz, 1124) == 0);
   CHECK(spiShiftFor(spiClockHz, 1000) == 0);
   CHECK(spiShiftFor(spiClockHz, 100) == 0);
   // Adaptive clocking
   CHECK(spiShiftFor(spiClockHz, 0) == 0);
   CHECK(spiShiftFor(spiClockHz, 0xFFFFFFFF) == 2);
}

TEST_CASE("JTAG fast path: SPI chunks of about 1 ms") {
   CHECK(jtag::spiChunkBytes(spiClockHz, 1) == 2250);
   CHECK(jtag::spiChunkBytes(spiClockHz, 4) == 281);
   CHECK(jtag::spiChunkBytes(spiClockHz, 8) == 17);
   CHECK(jtag::spiChunkBytes(4'000'000'000, 1) == 0xFFFF);
   CHECK(jtag::spiChunkBytes(8000, 8) == 1);
}

TEST_CASE("JTAG fast path: INIT and FINI go to the pins and the SPI, TCK idles high") {
   Fixture f;
   CHECK(f.pins.inits == 1);
   CHECK(f.spi.inits == 1);
   CHECK(f.pins.tckLevel());
   f.port.fini();
   CHECK(f.pins.finis == 1);
   CHECK(f.spi.finis == 1);
}

TEST_CASE("JTAG fast path: CONFIG selects the SPI divider or bit-bang") {
   Fixture f;
   REQUIRE(f.port.setSpeed(18'000));
   CHECK(f.port.spiShift() == 2);
   CHECK(f.spi.shift() == 2);
   REQUIRE(f.port.setSpeed(1000));
   CHECK(f.port.spiShift() == 0);
   CHECK(f.port.timing() == jtag::Timing{ jtag::Clocking::Timed, 36, 0 });
   REQUIRE(f.port.setSpeed(0));
   CHECK(f.port.spiShift() == 0);
   CHECK(f.port.timing().clocking == jtag::Clocking::Adaptive);

   vsltest::MockJtagPins<false> noRtck;
   FastPort<vsltest::MockJtagPins<false>, vsltest::MockJtagSpi<vsltest::MockJtagPins<false>>> port(noRtck);
   REQUIRE(port.setSpeed(9000));
   CHECK_FALSE(port.setSpeed(0));
   CHECK(port.spiShift() == 2);
}

TEST_CASE("JTAG fast path: whole bytes go to the SPI, the other bits to bit-bang") {
   Fixture f;
   REQUIRE(f.port.setSpeed(18'000));
   SUBCASE("19 bits") {
      REQUIRE(shift(f.port, 19, { 0xA5, 0x3C, 0x06 }, { 0x0F, 0xF0, 0x05 }).ok);
      CHECK(f.spi.transfers == std::vector<uint32_t>{ 2 });
      CHECK(f.pins.rising.size() == 19);
   }
   SUBCASE("7 bits") {
      REQUIRE(shift(f.port, 7, { 0x55 }, { 0x01 }).ok);
      CHECK(f.spi.transfers.empty());
      CHECK(f.pins.rising.size() == 7);
   }
   SUBCASE("16 bits") {
      REQUIRE(shift(f.port, 16, { 0x55, 0xAA }, { 0x01, 0x80 }).ok);
      CHECK(f.spi.transfers == std::vector<uint32_t>{ 2 });
   }
   SUBCASE("0 bits") {
      REQUIRE(shift(f.port, 0, {}, {}).ok);
      CHECK(f.spi.transfers.empty());
      CHECK(f.pins.edges.empty());
   }
   CHECK(f.pins.tckLevel());
   CHECK_FALSE(f.spi.tckLowAtTransfer);
}

TEST_CASE("JTAG fast path: bit-bang speeds do not use the SPI") {
   for(const uint32_t kHz : { 1000u, 0u }) {
      CAPTURE(kHz);
      Fixture f;
      REQUIRE(f.port.setSpeed(kHz));
      REQUIRE(shift(f.port, 24, { 1, 2, 3 }, { 4, 5, 6 }).ok);
      CHECK(f.spi.transfers.empty());
      CHECK(f.pins.rising.size() == 24);
   }
}

namespace {

// Shifts random bits through a fast port and a bit-bang port, and
// compares TDO and the TDI and TMS bits at each rising edge. Returns the
// SPI transfers of the fast port.
std::size_t compareWithBitBang(std::mt19937 &random, uint32_t kHz, uint32_t bitCount) {
   CAPTURE(kHz);
   CAPTURE(bitCount);
   const std::size_t bytes = protocol::jtagRaw::byteCount(bitCount);
   const auto tdi = randomBytes(random, bytes);
   const auto tms = randomBytes(random, bytes);
   const std::deque<bool> chain{ true, false, false, true, true };

   Fixture fast(chain);
   Pins pins(chain);
   jtag::BitBangPort<Pins> bitBang(pins);
   bitBang.init();
   REQUIRE(fast.port.setSpeed(kHz));
   REQUIRE(bitBang.setSpeed(kHz));

   const auto fastResult = shift(fast.port, bitCount, tdi, tms);
   const auto bitBangResult = shift(bitBang, bitCount, tdi, tms);
   REQUIRE(fastResult.ok);
   REQUIRE(bitBangResult.ok);
   CHECK(fastResult.tdo == bitBangResult.tdo);
   REQUIRE(fast.pins.rising.size() == pins.rising.size());
   for(std::size_t i = 0; i < pins.rising.size(); ++i) {
      CAPTURE(i);
      CHECK(fast.pins.rising[i].tdi == pins.rising[i].tdi);
      CHECK(fast.pins.rising[i].tms == pins.rising[i].tms);
   }
   CHECK_FALSE(fast.spi.tckLowAtTransfer);
   CHECK(fast.pins.tckLevel());
   return fast.spi.transfers.size();
}

} // namespace

TEST_CASE("JTAG fast path: same TDO and same bits at the rising edges as bit-bang") {
   std::mt19937 random(1234);
   for(const uint32_t kHz : { 18'000u, 9000u, 4500u, 2250u, 1000u, 0u }) {
      for(int round = 0; round < 40; ++round) {
         static_cast<void>(compareWithBitBang(random, kHz, 1 + random() % 300));
      }
   }
}

TEST_CASE("JTAG fast path: same TDO as bit-bang over several SPI chunks") {
   std::mt19937 random(5678);
   // 9 MHz: chunks of 1125 bytes; 2250 kHz: chunks of 281 bytes
   CHECK(compareWithBitBang(random, 9000, 2500 * 8 + 3) == 3);
   CHECK(compareWithBitBang(random, 2250, 700 * 8 + 3) == 3);
}

TEST_CASE("JTAG fast path: a long shift calls the background between SPI chunks, with TCK high") {
   Pins pins;
   vsltest::MockBackground background(
   [&pins] { return vsltest::MockBackground::Call{ pins.time, pins.tckLevel(), pins.rising.size() }; });
   FastPort<Pins, Spi> port(pins, &background);
   const Spi &spi = port.spi();
   port.init();
   REQUIRE(port.setSpeed(18'000));
   // 5000 bytes at 9 MHz: chunks of 1125 bytes, 1 ms each
   const std::vector<uint8_t> data(5000, 0x5A);
   REQUIRE(shift(port, 5000 * 8, data, data).ok);
   CHECK(spi.transfers == std::vector<uint32_t>{ 1125, 1125, 1125, 1125, 500 });
   REQUIRE(background.calls.size() >= 2);
   for(const auto &call : background.calls) {
      CHECK(call.clock);
      CHECK(call.count % (1125 * 8) == 0);
   }
}

TEST_CASE("JTAG fast path: a failed SPI transfer is a timeout") {
   Fixture f;
   REQUIRE(f.port.setSpeed(18'000));
   f.spi.fail = true;
   CHECK_FALSE(shift(f.port, 16, { 0, 0 }, { 0, 0 }).ok);
}
