// The SWD bit-bang engine on mock pins with a mock target.

#include "swd/SwdBitBang.h"

#include "MockSwdPins.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

namespace pswd = protocol::swd;
using swd::BitBangPort;
using swd::halfPeriodCycles;
using vsltest::MockSwdPins;

constexpr uint8_t readDpidr = 0xA5;
constexpr uint8_t writeAbort = 0x81;

// The rising edges of a transfer: request, turnaround, ACK, idle cycles,
// and the bits of the data phase.
constexpr std::size_t transferEdges(unsigned turnaround, unsigned dataPhase) {
   return 8 + turnaround + 3 + dataPhase + swd::idleCycles;
}

struct Bench {
   explicit Bench(uint8_t turnaround = 1, uint16_t retries = 0, uint16_t delay = 1)
      : pins(turnaround)
      , port(pins) {
      port.init();
      port.configure(turnaround, retries, delay);
   }

   // Checks the line state after a transfer: SWCLK high, SWDIO driven low,
   // no contention.
   void checkIdle() const {
      CHECK(pins.swclkLevel());
      CHECK(pins.hostDrives());
      CHECK_FALSE(pins.swdio());
      CHECK(pins.contentions == 0);
      CHECK(pins.undrivenSamples == 0);
   }

   MockSwdPins pins;
   BitBangPort<MockSwdPins> port;
};

} // namespace

TEST_CASE("SWD bit-bang: half period of the OpenOCD delay values") {
   CHECK(halfPeriodCycles(72'000'000, 0) == 20);
   CHECK(halfPeriodCycles(72'000'000, 1) == 154);
   CHECK(halfPeriodCycles(72'000'000, 2) == 277);
   CHECK(halfPeriodCycles(72'000'000, 3) == 353);
   CHECK(halfPeriodCycles(72'000'000, 4) == 424);
   CHECK(halfPeriodCycles(72'000'000, 5) == 500);
   CHECK(halfPeriodCycles(72'000'000, 6) == 504);
   CHECK(halfPeriodCycles(72'000'000, 65535) == 65536u * 72u);
}

TEST_CASE("SWD bit-bang: the clock of each delay is within 10 % of the OpenOCD table, and not faster") {
   for(uint32_t delay = 0; delay <= 0xFFFF; ++delay) {
      CAPTURE(delay);
      // vsllink.c: the fixed points, then hz = 500000 / (delay + 1)
      const double expected =
      delay < swd::fixedClockHz.size() ? swd::fixedClockHz[delay] : 500000.0 / static_cast<double>(delay + 1);
      const double actual = 72e6 / (2.0 * halfPeriodCycles(72'000'000, static_cast<uint16_t>(delay)));
      REQUIRE(actual <= expected);
      REQUIRE(actual >= 0.9 * expected);
   }
}

TEST_CASE("SWD bit-bang: INIT and FINI go to the pins, CONFIG sets the half period") {
   Bench bench(1, 0, 0);
   CHECK(bench.pins.inits == 1);
   CHECK(bench.port.halfCycles() == 20);
   bench.port.fini();
   CHECK(bench.pins.finis == 1);
}

TEST_CASE("SWD bit-bang: a read transfer") {
   Bench bench;
   bench.pins.readData = 0x2BA01477;
   uint32_t data = 0;
   CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
   CHECK(data == 0x2BA01477);
   REQUIRE(bench.pins.transfers.size() == 1);
   CHECK(bench.pins.transfers[0].request == readDpidr);
   // Data phase: 33 bits and a turnaround
   CHECK(bench.pins.risingEdges.size() == transferEdges(1, 34));
   bench.checkIdle();
}

TEST_CASE("SWD bit-bang: a write transfer") {
   for(const uint32_t value : { 0x0000001Eu, 0x80000001u, 0xFFFFFFFFu, 0x12345678u }) {
      CAPTURE(value);
      Bench bench;
      uint32_t data = value;
      CHECK(bench.port.transfer(writeAbort, data) == pswd::ackOk);
      CHECK(data == value);
      REQUIRE(bench.pins.transfers.size() == 1);
      CHECK(bench.pins.transfers[0].request == writeAbort);
      CHECK(bench.pins.transfers[0].written == value);
      CHECK(bench.pins.transfers[0].writtenParity == pswd::parity(value));
      // Data phase: a turnaround and 33 bits
      CHECK(bench.pins.risingEdges.size() == transferEdges(1, 34));
      bench.checkIdle();
   }
}

TEST_CASE("SWD bit-bang: transfers with a turnaround of 2 cycles") {
   Bench bench(2);
   bench.pins.readData = 0xCAFEF00D;
   uint32_t data = 0;
   CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
   CHECK(data == 0xCAFEF00D);
   data = 0x50000000;
   CHECK(bench.port.transfer(pswd::encodeRequest(0x02), data) == pswd::ackOk);
   REQUIRE(bench.pins.transfers.size() == 2);
   CHECK(bench.pins.transfers[1].written == 0x50000000);
   CHECK(bench.pins.risingEdges.size() == 2 * transferEdges(2, 35));
   bench.checkIdle();
}

TEST_CASE("SWD bit-bang: transfers back to back") {
   Bench bench;
   bench.pins.readData = 0x11;
   for(int i = 0; i < 3; ++i) {
      uint32_t data = 0x1E;
      CHECK(bench.port.transfer(writeAbort, data) == pswd::ackOk);
      CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
      CHECK(data == 0x11);
   }
   CHECK(bench.pins.transfers.size() == 6);
   bench.checkIdle();
}

TEST_CASE("SWD bit-bang: WAIT repeats the transfer up to the retry count") {
   SUBCASE("OK after two WAIT") {
      Bench bench(1, 2);
      bench.pins.acks = { pswd::ackWait, pswd::ackWait };
      bench.pins.readData = 7;
      uint32_t data = 0;
      CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
      CHECK(data == 7);
      CHECK(bench.pins.transfers.size() == 3);
      CHECK(bench.pins.risingEdges.size() == 2 * transferEdges(1, 1) + transferEdges(1, 34));
      bench.checkIdle();
   }
   SUBCASE("WAIT after all retries") {
      Bench bench(1, 2);
      bench.pins.acks = { pswd::ackWait, pswd::ackWait, pswd::ackWait, pswd::ackOk };
      uint32_t data = 0x55;
      CHECK(bench.port.transfer(readDpidr, data) == (pswd::ackWait | pswd::retryOutFlag));
      CHECK(data == 0x55);
      CHECK(bench.pins.transfers.size() == 3);
      bench.checkIdle();
   }
   SUBCASE("no retry") {
      Bench bench(1, 0);
      bench.pins.acks = { pswd::ackWait };
      uint32_t data = 0;
      CHECK(bench.port.transfer(writeAbort, data) == (pswd::ackWait | pswd::retryOutFlag));
      CHECK(bench.pins.transfers.size() == 1);
      bench.checkIdle();
   }
}

TEST_CASE("SWD bit-bang: FAULT ends the transfer without a data phase") {
   Bench bench(1, 5);
   bench.pins.acks = { pswd::ackFault };
   uint32_t data = 0x1234;
   CHECK(bench.port.transfer(writeAbort, data) == (pswd::ackFault | pswd::faultFlag));
   CHECK(data == 0x1234);
   CHECK(bench.pins.transfers.size() == 1);
   CHECK(bench.pins.risingEdges.size() == transferEdges(1, 1));
   bench.checkIdle();
}

TEST_CASE("SWD bit-bang: no reply is an ACK error, and the host clocks past the data phase") {
   Bench bench(1, 5);
   bench.pins.acks = { 0 };
   uint32_t data = 0x99;
   CHECK(bench.port.transfer(readDpidr, data) == (0x07 | pswd::ackErrorFlag));
   CHECK(data == 0x99);
   CHECK(bench.pins.transfers.size() == 1);
   CHECK(bench.pins.risingEdges.size() == transferEdges(1, 34));
   bench.checkIdle();
}

TEST_CASE("SWD bit-bang: a read with a wrong parity bit is a parity error") {
   Bench bench;
   bench.pins.readData = 0x2BA01477;
   bench.pins.badParity = true;
   uint32_t data = 0;
   CHECK(bench.port.transfer(readDpidr, data) == (pswd::ackOk | pswd::parityErrorFlag));
   CHECK(data == 0);
   bench.checkIdle();
}

TEST_CASE("SWD bit-bang: OUT drives the bits LSB first") {
   Bench bench;
   bench.pins.decodes = false;
   const std::vector<uint8_t> data{ 0x9E, 0xE7, 0x05 };
   bench.port.sequenceOut(19, art::span<const uint8_t>(data.data(), data.size()));
   REQUIRE(bench.pins.risingEdges.size() == 19);
   for(std::size_t i = 0; i < 19; ++i) {
      CAPTURE(i);
      CHECK(bench.pins.risingEdges[i].hostDrives);
      CHECK(bench.pins.risingEdges[i].level == (((data[i / 8] >> (i % 8)) & 1u) != 0));
   }
   CHECK(bench.pins.swclkLevel());
   CHECK(bench.pins.begins == 1);
}

TEST_CASE("SWD bit-bang: OUT of 0 bits does not clock") {
   Bench bench;
   bench.port.sequenceOut(0, {});
   CHECK(bench.pins.edges.empty());
}

TEST_CASE("SWD bit-bang: IN releases SWDIO and reads the bits") {
   Bench bench;
   bench.pins.decodes = false;
   std::vector<uint8_t> data(2, 0);
   bench.port.sequenceIn(12, art::span<uint8_t>(data.data(), data.size()));
   // Nobody drives SWDIO: the pull-up gives 1.
   CHECK(data == std::vector<uint8_t>{ 0xFF, 0x0F });
   CHECK_FALSE(bench.pins.hostDrives());
   CHECK(bench.pins.risingEdges.size() == 12);
}

TEST_CASE("SWD bit-bang: each half period waits the half period without the loop code") {
   Bench bench(1, 0, 0);
   const uint32_t waitCycles = 20 - swd::loopOverheadCycles;
   const uint32_t start = bench.pins.time;
   uint32_t data = 0;
   CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
   REQUIRE(bench.pins.edges.size() == 2 * transferEdges(1, 34));
   // The counter wraps in this test (pins.time starts near 2^32).
   CHECK(bench.pins.edges.back() < start);
   for(std::size_t i = 1; i < bench.pins.edges.size(); ++i) {
      CHECK(bench.pins.edges[i] - bench.pins.edges[i - 1] >= waitCycles);
      CHECK(bench.pins.edges[i] - bench.pins.edges[i - 1] <= waitCycles + 2);
   }
}

TEST_CASE("SWD bit-bang: after a late edge, the half periods do not get shorter to catch up") {
   Bench bench(1, 0, 0);
   bench.pins.stallEdge = 10;
   bench.pins.stallCycles = 500;
   uint32_t data = 0;
   CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
   const auto &edges = bench.pins.edges;
   REQUIRE(edges.size() > 12);
   CHECK(edges[10] - edges[9] >= 500);
   for(std::size_t i = 1; i < edges.size(); ++i) {
      CAPTURE(i);
      CHECK(edges[i] - edges[i - 1] >= 20 - swd::loopOverheadCycles);
   }
}
