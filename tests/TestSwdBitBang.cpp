// The SWD bit-bang engine on mock pins with a mock target.

#include "swd/SwdBitBang.h"

#include "MockBackground.h"
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
constexpr uint8_t writeSelect = 0xB1;
constexpr uint8_t readApDrw = 0x9F;
constexpr uint8_t writeTargetsel = 0x99;

// The rising edges of a transfer: request, turnaround, ACK, data phase
// with its turnaround, idle cycles. The same for each ACK.
constexpr std::size_t transferEdges(unsigned turnaround) {
   return 8 + turnaround + 3 + turnaround + 33 + swd::defaultIdleCycles;
}

struct Bench {
   explicit Bench(uint8_t turnaround = 1, uint16_t retries = 0, uint16_t delay = 1)
      : pins(turnaround)
      , port(pins) {
      port.init();
      port.configure(turnaround, retries, delay);
   }

   // Checks the line state after a transfer: SWCLK high, SWDIO driven low,
   // no contention, and the target in step with the host.
   void checkIdle() const {
      CHECK(pins.swclkLevel());
      CHECK(pins.hostDrives());
      CHECK_FALSE(pins.swdio());
      CHECK(pins.contentions == 0);
      CHECK(pins.undrivenSamples == 0);
      CHECK(pins.desyncs == 0);
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

TEST_CASE("SWD bit-bang: delay 0 runs untimed, with the same bits") {
   Bench timed(1, 0, 1);
   Bench untimed(1, 0, 0);
   CHECK(timed.port.isTimed());
   CHECK_FALSE(untimed.port.isTimed());
   for(Bench *bench : { &timed, &untimed }) {
      bench->pins.readData = 0x2BA01477;
      uint32_t data = 0;
      const uint32_t start = bench->pins.time;
      CHECK(bench->port.transfer(readDpidr, data) == pswd::ackOk);
      CHECK(data == 0x2BA01477);
      CHECK(bench->pins.risingEdges.size() == transferEdges(1));
      bench->checkIdle();
      if(bench == &untimed) {
         // No cycle counter read for each half period: only the
         // background timer reads it.
         CHECK(bench->pins.time - start < 4);
      } else {
         CHECK(bench->pins.time - start > 2 * transferEdges(1));
      }
   }
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
   CHECK(bench.pins.risingEdges.size() == transferEdges(1));
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
      CHECK(bench.pins.risingEdges.size() == transferEdges(1));
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
   CHECK(bench.pins.risingEdges.size() == 2 * transferEdges(2));
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

TEST_CASE("SWD bit-bang: WAIT repeats the transfer after an ABORT with ORUNERRCLR") {
   for(const bool overrunDetect : { false, true }) {
      CAPTURE(overrunDetect);
      SUBCASE("OK after two WAIT") {
         Bench bench(1, 2);
         bench.pins.overrunDetect = overrunDetect;
         bench.pins.acks = { pswd::ackWait, pswd::ackWait };
         bench.pins.readData = 7;
         uint32_t data = 0;
         CHECK(bench.port.transfer(readApDrw, data) == pswd::ackOk);
         CHECK(data == 7);
         REQUIRE(bench.pins.transfers.size() == 5);
         for(const std::size_t i : { 1u, 3u }) {
            CHECK(bench.pins.transfers[i].request == writeAbort);
            CHECK(bench.pins.transfers[i].written == 0x10);
         }
         CHECK(bench.pins.transfers[4].request == readApDrw);
         CHECK_FALSE(bench.pins.stickyOrun);
         CHECK(bench.pins.risingEdges.size() == 5 * transferEdges(1));
         bench.checkIdle();
      }
      SUBCASE("WAIT after all retries") {
         Bench bench(1, 2);
         bench.pins.overrunDetect = overrunDetect;
         bench.pins.acks = { pswd::ackWait, pswd::ackWait, pswd::ackWait, pswd::ackOk };
         uint32_t data = 0x55;
         CHECK(bench.port.transfer(readApDrw, data) == (pswd::ackWait | pswd::retryOutFlag));
         CHECK(data == 0x55);
         CHECK(bench.pins.transfers.size() == 5);
         // OpenOCD clears STICKYORUN after the failed transfer.
         CHECK(bench.pins.stickyOrun == overrunDetect);
         bench.checkIdle();
      }
      SUBCASE("no retry") {
         Bench bench(1, 0);
         bench.pins.overrunDetect = overrunDetect;
         bench.pins.acks = { pswd::ackWait };
         uint32_t data = 0x1234;
         CHECK(bench.port.transfer(writeSelect, data) == (pswd::ackWait | pswd::retryOutFlag));
         REQUIRE(bench.pins.transfers.size() == 1);
         // The write drives its data phase also after WAIT.
         CHECK(bench.pins.transfers[0].written == 0x1234);
         bench.checkIdle();
      }
   }
}

TEST_CASE("SWD bit-bang: with ORUNDETECT, a WAIT without ABORT gives FAULT") {
   // The mock target itself: the reason for the ABORT before a retry.
   Bench bench(1, 0);
   bench.pins.overrunDetect = true;
   bench.pins.acks = { pswd::ackWait };
   uint32_t data = 0;
   CHECK(bench.port.transfer(readApDrw, data) == (pswd::ackWait | pswd::retryOutFlag));
   CHECK(bench.port.transfer(readApDrw, data) == (pswd::ackFault | pswd::faultFlag));
   // DPIDR still answers.
   bench.pins.readData = 0x1BA01477;
   CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
   CHECK(data == 0x1BA01477);
   bench.checkIdle();
}

TEST_CASE("SWD bit-bang: FAULT runs the data phase, and the next transfer works") {
   for(const bool read : { true, false }) {
      CAPTURE(read);
      Bench bench(1, 5);
      bench.pins.overrunDetect = true;
      bench.pins.acks = { pswd::ackFault };
      bench.pins.readData = 0x1BA01477;
      uint32_t data = 0x1234;
      CHECK(bench.port.transfer(read ? readApDrw : writeSelect, data) == (pswd::ackFault | pswd::faultFlag));
      CHECK(data == 0x1234);
      CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
      CHECK(data == 0x1BA01477);
      REQUIRE(bench.pins.transfers.size() == 2);
      CHECK(bench.pins.transfers[1].request == readDpidr);
      CHECK(bench.pins.risingEdges.size() == 2 * transferEdges(1));
      bench.checkIdle();
   }
}

TEST_CASE("SWD bit-bang: no reply is an ACK error, and the host clocks past the data phase") {
   Bench bench(1, 5);
   bench.pins.acks = { 0 };
   uint32_t data = 0x99;
   CHECK(bench.port.transfer(readDpidr, data) == (0x07 | pswd::ackErrorFlag));
   CHECK(data == 0x99);
   CHECK(bench.pins.transfers.size() == 1);
   CHECK(bench.pins.risingEdges.size() == transferEdges(1));
   bench.checkIdle();
}

TEST_CASE("SWD bit-bang: a write without ACK (TARGETSEL) still drives its data") {
   Bench bench(1, 5);
   bench.pins.acks = { 0 };
   uint32_t data = 0x01002927;
   CHECK(bench.port.transfer(writeTargetsel, data) == (0x07 | pswd::ackErrorFlag));
   REQUIRE(bench.pins.transfers.size() == 1);
   CHECK(bench.pins.transfers[0].request == writeTargetsel);
   CHECK(bench.pins.transfers[0].written == 0x01002927);
   CHECK(bench.pins.transfers[0].writtenParity == pswd::parity(0x01002927));
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

TEST_CASE("SWD bit-bang: each half period is the half period") {
   Bench bench(1, 0, 1);
   const uint32_t waitCycles = 154;
   const uint32_t start = bench.pins.time;
   uint32_t data = 0;
   CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
   REQUIRE(bench.pins.edges.size() == 2 * transferEdges(1));
   // The counter wraps in this test (pins.time starts near 2^32).
   CHECK(bench.pins.edges.back() < start);
   for(std::size_t i = 1; i < bench.pins.edges.size(); ++i) {
      CHECK(bench.pins.edges[i] - bench.pins.edges[i - 1] >= waitCycles);
      CHECK(bench.pins.edges[i] - bench.pins.edges[i - 1] <= waitCycles + 2);
   }
}

TEST_CASE("SWD bit-bang: the code before each edge does not change the half period") {
   // The edge comes 20 cycles after the end of the wait. Waits that start
   // after the edge (the engine before #78) make each half period longer
   // or shorter by this code.
   Bench bench(1, 0, 1);
   bench.pins.edgeDelayCycles = 20;
   const uint32_t start = bench.pins.time;
   uint32_t data = 0;
   CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
   const auto &edges = bench.pins.edges;
   REQUIRE(edges.size() == 2 * transferEdges(1));
   // The first half period starts with the operation, so the first edge
   // also comes after a full half period.
   CHECK(edges[0] - start >= 154);
   CHECK(edges[1] - edges[0] <= 154 + 20 + 2);
   for(std::size_t i = 2; i < edges.size(); ++i) {
      CAPTURE(i);
      CHECK(edges[i] - edges[i - 1] >= 154);
      CHECK(edges[i] - edges[i - 1] <= 156);
   }
}

TEST_CASE("SWD bit-bang: at the fastest setting the engine does not wait") {
   Bench bench(1, 0, 0);
   uint32_t data = 0;
   CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
   REQUIRE(bench.pins.edges.size() == 2 * transferEdges(1));
   for(std::size_t i = 1; i < bench.pins.edges.size(); ++i) {
      CHECK(bench.pins.edges[i] - bench.pins.edges[i - 1] <= 2);
   }
}

TEST_CASE("SWD bit-bang: after a late edge, the half periods do not get shorter to catch up") {
   Bench bench(1, 0, 1);
   bench.pins.stallEdge = 10;
   bench.pins.stallCycles = 500;
   uint32_t data = 0;
   CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
   const auto &edges = bench.pins.edges;
   REQUIRE(edges.size() > 12);
   CHECK(edges[10] - edges[9] >= 500);
   for(std::size_t i = 1; i < edges.size(); ++i) {
      CAPTURE(i);
      CHECK(edges[i] - edges[i - 1] >= 154);
   }
}

TEST_CASE("SWD bit-bang: a line reset and the JTAG-to-SWD sequence before a transfer") {
   Bench bench;
   // swd_seq_jtag_to_swd of OpenOCD (136 bits), then swd_seq_line_reset (64 bits)
   const std::vector<uint8_t> jtagToSwd{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x9E, 0xE7,
                                         0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
   const std::vector<uint8_t> lineReset{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
   bench.port.sequenceOut(136, art::span<const uint8_t>(jtagToSwd.data(), jtagToSwd.size()));
   bench.port.sequenceOut(64, art::span<const uint8_t>(lineReset.data(), lineReset.size()));
   CHECK(bench.pins.lineResets > 0);
   bench.pins.transfers.clear();
   bench.pins.readData = 0x1BA01477;
   uint32_t data = 0;
   CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
   CHECK(data == 0x1BA01477);
   REQUIRE(bench.pins.transfers.size() == 1);
   CHECK(bench.pins.transfers[0].request == readDpidr);
   bench.checkIdle();
}

namespace {

// Samples the time, the SWCLK level and the rising edges of the pins.
vsltest::MockBackground backgroundOf(const MockSwdPins &pins) {
   return vsltest::MockBackground(
   [&pins] { return vsltest::MockBackground::Call{ pins.time, pins.swclkLevel(), pins.risingEdges.size() }; });
}

// 1 ms at 72 MHz
constexpr uint32_t msCycles = 72'000;

} // namespace

TEST_CASE("SWD bit-bang: WAIT retries service the background between the attempts, with SWCLK high") {
   MockSwdPins pins;
   auto background = backgroundOf(pins);
   BitBangPort<MockSwdPins> port(pins, &background);
   port.init();
   // delay 20: a half period of 21 us, so each attempt and its ABORT take
   // about 4.5 ms.
   port.configure(1, 20, 20);
   pins.overrunDetect = true;
   pins.acks = std::deque<uint8_t>(20, pswd::ackWait);
   pins.readData = 9;
   uint32_t data = 0;
   REQUIRE(port.transfer(readApDrw, data) == pswd::ackOk);
   CHECK(data == 9);
   REQUIRE(pins.transfers.size() == 41);
   // A call before each attempt
   CHECK(background.calls.size() == 21);
   for(std::size_t i = 0; i < background.calls.size(); ++i) {
      CAPTURE(i);
      CHECK(background.calls[i].clock);
      // Before the attempt and after the ABORT of the last one
      CHECK(background.calls[i].count == 2 * i * transferEdges(1));
   }
   CHECK(pins.contentions == 0);
   CHECK(pins.desyncs == 0);
}

TEST_CASE("SWD bit-bang: a long sequence services the background between bytes") {
   for(const bool out : { true, false }) {
      CAPTURE(out);
      MockSwdPins pins;
      pins.decodes = false;
      auto background = backgroundOf(pins);
      BitBangPort<MockSwdPins> port(pins, &background);
      port.init();
      port.configure(1, 0, 20);
      const uint32_t start = pins.time;
      // 2000 bits of 42 us: 84 ms
      std::vector<uint8_t> data(250, 0xFF);
      if(out) {
         port.sequenceOut(2000, art::span<const uint8_t>(data.data(), data.size()));
      } else {
         port.sequenceIn(2000, art::span<uint8_t>(data.data(), data.size()));
      }
      REQUIRE(pins.risingEdges.size() == 2000);
      const uint32_t elapsed = pins.time - start;
      // The engine polls after each byte, so a call can come up to 8 bits late.
      CHECK(background.calls.size() >= elapsed / (msCycles + 16 * halfPeriodCycles(72'000'000, 20)));
      CHECK(background.calls.size() <= elapsed / msCycles + 1);
      for(const auto &call : background.calls) {
         CHECK(call.clock);
         CHECK(call.count % 8 == 0);
      }
   }
}

TEST_CASE("SWD bit-bang: the background interval runs on over fast transfers") {
   MockSwdPins pins;
   auto background = backgroundOf(pins);
   BitBangPort<MockSwdPins> port(pins, &background);
   port.init();
   port.configure(1, 0, 1);
   const uint32_t start = pins.time;
   // 300 transfers at 235 kHz: about 14000 cycles each, 60 ms together
   for(int i = 0; i < 300; ++i) {
      uint32_t data = 0;
      REQUIRE(port.transfer(readDpidr, data) == pswd::ackOk);
   }
   // The engine polls before each transfer, so a call can come up to one
   // transfer late.
   const uint32_t elapsed = pins.time - start;
   CHECK(background.calls.size() >= elapsed / (msCycles + elapsed / 300));
   CHECK(background.calls.size() <= elapsed / msCycles + 1);
}

// CMSIS-DAP transfer rules (swd::TransferRules)

namespace {

constexpr swd::TransferRules cmsisDapRules{ 0, false, false, true, true };

// The rising edges of a transfer without the data phase: request,
// turnaround, ACK, turnaround, idle cycles
constexpr std::size_t shortTransferEdges(unsigned turnaround, unsigned idle) {
   return 8 + turnaround + 3 + turnaround + idle;
}

} // namespace

TEST_CASE("SWD bit-bang: the default rules are the Versaloon rules, and CONFIG sets them again") {
   Bench bench;
   const auto checkVersaloonRules = [&bench] {
      CHECK(bench.port.rules().idleCycles == swd::defaultIdleCycles);
      CHECK(bench.port.rules().abortOnWait);
      CHECK(bench.port.rules().dataPhase);
      CHECK_FALSE(bench.port.rules().idleOnlyAfterOk);
      CHECK_FALSE(bench.port.rules().releaseOnAckError);
   };
   checkVersaloonRules();
   // A multi image: the CMSIS-DAP processor sets its rules at start, then
   // the Versaloon SWD block runs INIT or CONFIG.
   bench.port.setRules(cmsisDapRules);
   bench.port.configure(1, 0, 1);
   checkVersaloonRules();
}

TEST_CASE("SWD bit-bang: the idle cycles after a transfer are configurable") {
   for(const uint8_t idle : { 0, 1, 7, 255 }) {
      CAPTURE(idle);
      Bench bench;
      bench.port.setRules({ idle, true, true });
      bench.pins.readData = 0x2BA01477;
      uint32_t data = 0;
      CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
      CHECK(data == 0x2BA01477);
      data = 0x12345678;
      CHECK(bench.port.transfer(writeSelect, data) == pswd::ackOk);
      REQUIRE(bench.pins.transfers.size() == 2);
      CHECK(bench.pins.transfers[1].written == 0x12345678);
      const std::size_t each = transferEdges(1) - swd::defaultIdleCycles + idle;
      REQUIRE(bench.pins.risingEdges.size() == 2 * each);
      // The idle cycles drive SWDIO low.
      for(std::size_t i = each - idle; i < each; ++i) {
         CHECK(bench.pins.risingEdges[i].hostDrives);
         CHECK_FALSE(bench.pins.risingEdges[i].level);
      }
      bench.checkIdle();
   }
}

TEST_CASE("SWD bit-bang: without abortOnWait, WAIT repeats the transfer without ABORT") {
   SUBCASE("OK after two WAIT") {
      Bench bench(1, 2);
      bench.port.setRules({ swd::defaultIdleCycles, false, true });
      bench.pins.acks = { pswd::ackWait, pswd::ackWait };
      bench.pins.readData = 7;
      uint32_t data = 0;
      CHECK(bench.port.transfer(readApDrw, data) == pswd::ackOk);
      CHECK(data == 7);
      REQUIRE(bench.pins.transfers.size() == 3);
      for(const auto &transfer : bench.pins.transfers) {
         CHECK(transfer.request == readApDrw);
      }
      CHECK(bench.pins.risingEdges.size() == 3 * transferEdges(1));
      bench.checkIdle();
   }
   SUBCASE("WAIT after all retries") {
      Bench bench(1, 3);
      bench.port.setRules({ swd::defaultIdleCycles, false, true });
      bench.pins.acks = std::deque<uint8_t>(5, pswd::ackWait);
      uint32_t data = 0x55;
      CHECK(bench.port.transfer(writeSelect, data) == (pswd::ackWait | pswd::retryOutFlag));
      REQUIRE(bench.pins.transfers.size() == 4);
      for(const auto &transfer : bench.pins.transfers) {
         CHECK(transfer.request == writeSelect);
         CHECK(transfer.written == 0x55);
      }
      bench.checkIdle();
   }
   SUBCASE("with ORUNDETECT, the target replies FAULT to the retry") {
      // The debugger must clear STICKYORUN itself: the engine does not.
      Bench bench(1, 2);
      bench.port.setRules({ swd::defaultIdleCycles, false, true });
      bench.pins.overrunDetect = true;
      bench.pins.acks = { pswd::ackWait };
      uint32_t data = 0;
      CHECK(bench.port.transfer(readApDrw, data) == (pswd::ackFault | pswd::faultFlag));
      CHECK(bench.pins.transfers.size() == 2);
      CHECK(bench.pins.stickyOrun);
      bench.checkIdle();
   }
}

// A RetryStop that stops at its call number stopAt (1 is the first call)
class CountingStop final : public swd::RetryStop {
   public:
   explicit CountingStop(int stopAt)
      : stopAt_(stopAt) {
   }

   bool stopRetries() override {
      return ++calls >= stopAt_;
   }

   int calls{ 0 };

   private:
   int stopAt_;
};

TEST_CASE("SWD bit-bang: a RetryStop ends the WAIT retries") {
   SUBCASE("stop after the third WAIT") {
      Bench bench(1, 1000);
      bench.port.setRules({ swd::defaultIdleCycles, false, true });
      CountingStop stop(3);
      bench.port.setRetryStop(&stop);
      bench.pins.acks = std::deque<uint8_t>(10, pswd::ackWait);
      uint32_t data = 0;
      CHECK(bench.port.transfer(readApDrw, data) == (pswd::ackWait | pswd::retryOutFlag));
      CHECK(stop.calls == 3);
      CHECK(bench.pins.transfers.size() == 3);
      bench.checkIdle();
   }
   SUBCASE("no call without WAIT") {
      Bench bench(1, 1000);
      CountingStop stop(1);
      bench.port.setRetryStop(&stop);
      bench.pins.acks = { pswd::ackOk, pswd::ackFault };
      uint32_t data = 0;
      CHECK(bench.port.transfer(readApDrw, data) == pswd::ackOk);
      CHECK(bench.port.transfer(readApDrw, data) == (pswd::ackFault | pswd::faultFlag));
      CHECK(stop.calls == 0);
   }
   SUBCASE("a stop that returns false lets the retries run") {
      Bench bench(1, 1000);
      bench.port.setRules({ swd::defaultIdleCycles, false, true });
      CountingStop stop(100);
      bench.port.setRetryStop(&stop);
      bench.pins.acks = { pswd::ackWait, pswd::ackWait };
      uint32_t data = 0;
      CHECK(bench.port.transfer(readApDrw, data) == pswd::ackOk);
      CHECK(stop.calls == 2);
      CHECK(bench.pins.transfers.size() == 3);
      bench.checkIdle();
   }
}

TEST_CASE("SWD bit-bang: without dataPhase, WAIT and FAULT end after the turnaround") {
   for(const uint8_t ack : { pswd::ackWait, pswd::ackFault }) {
      for(const bool read : { true, false }) {
         for(uint8_t turnaround = 1; turnaround <= 4; ++turnaround) {
            CAPTURE(ack);
            CAPTURE(read);
            CAPTURE(turnaround);
            Bench bench(turnaround, 0);
            bench.port.setRules({ swd::defaultIdleCycles, false, false });
            bench.pins.dataPhase = false;
            bench.pins.acks = { ack };
            bench.pins.readData = 0x1BA01477;
            uint32_t data = 0x1234;
            const uint8_t flags = ack == pswd::ackWait ? pswd::retryOutFlag : pswd::faultFlag;
            CHECK(bench.port.transfer(read ? readApDrw : writeSelect, data) == (ack | flags));
            CHECK(data == 0x1234);
            CHECK(bench.pins.risingEdges.size() == shortTransferEdges(turnaround, swd::defaultIdleCycles));
            // The host drives SWDIO from the cycle after the turnaround.
            const std::size_t driven = 8 + turnaround + 3 + turnaround;
            CHECK_FALSE(bench.pins.risingEdges[driven - 1].hostDrives);
            CHECK(bench.pins.risingEdges[driven].hostDrives);
            // The next transfer is in step.
            CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
            CHECK(data == 0x1BA01477);
            REQUIRE(bench.pins.transfers.size() == 2);
            CHECK(bench.pins.transfers[1].request == readDpidr);
            CHECK(bench.pins.risingEdges.size() == shortTransferEdges(turnaround, swd::defaultIdleCycles) + transferEdges(turnaround));
            bench.checkIdle();
         }
      }
   }
}

TEST_CASE("SWD bit-bang: without dataPhase, OK and a missing ACK still run the data phase") {
   SUBCASE("OK read and write") {
      Bench bench;
      bench.port.setRules(cmsisDapRules);
      bench.pins.dataPhase = false;
      bench.pins.readData = 0xCAFEF00D;
      uint32_t data = 0;
      CHECK(bench.port.transfer(readApDrw, data) == pswd::ackOk);
      CHECK(data == 0xCAFEF00D);
      data = 0x0BADC0DE;
      CHECK(bench.port.transfer(writeSelect, data) == pswd::ackOk);
      REQUIRE(bench.pins.transfers.size() == 2);
      CHECK(bench.pins.transfers[1].written == 0x0BADC0DE);
      CHECK(bench.pins.risingEdges.size() == 2 * (transferEdges(1) - swd::defaultIdleCycles));
      bench.checkIdle();
   }
   SUBCASE("no ACK") {
      Bench bench;
      bench.port.setRules(cmsisDapRules);
      bench.pins.dataPhase = false;
      bench.pins.acks = { 0 };
      uint32_t data = 0x99;
      CHECK(bench.port.transfer(readDpidr, data) == (0x07 | pswd::ackErrorFlag));
      CHECK(bench.pins.risingEdges.size() == transferEdges(1) - swd::defaultIdleCycles);
      bench.checkIdle();
   }
}

TEST_CASE("SWD bit-bang: with idleOnlyAfterOk, only an OK ACK gets the idle cycles") {
   const swd::TransferRules rules{ swd::defaultIdleCycles, false, false, true, false };
   for(const uint8_t ack : { pswd::ackOk, pswd::ackWait, pswd::ackFault, uint8_t{ 0 } }) {
      for(const bool read : { true, false }) {
         CAPTURE(ack);
         CAPTURE(read);
         Bench bench;
         bench.port.setRules(rules);
         bench.pins.dataPhase = false;
         bench.pins.acks = { ack };
         uint32_t data = 0x1234;
         static_cast<void>(bench.port.transfer(read ? readApDrw : writeSelect, data));
         std::size_t edges = transferEdges(1);
         if(ack == pswd::ackWait || ack == pswd::ackFault) {
            edges = shortTransferEdges(1, 0);
         } else if(ack == 0) {
            edges -= swd::defaultIdleCycles;
         }
         CHECK(bench.pins.risingEdges.size() == edges);
         // The next transfer is in step.
         bench.pins.readData = 0x1BA01477;
         CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
         CHECK(data == 0x1BA01477);
         CHECK(bench.pins.risingEdges.size() == edges + transferEdges(1));
         bench.checkIdle();
      }
   }
}

TEST_CASE("SWD bit-bang: a protocol error on a write, while the target drives read data") {
   // The target decodes a read and replies OK. The host sees ACK 0b011.
   for(const bool release : { true, false }) {
      CAPTURE(release);
      Bench bench;
      bench.port.setRules({ swd::defaultIdleCycles, true, true, false, release });
      bench.pins.rnwFlipped = true;
      bench.pins.ackNoise = 0x02;
      bench.pins.readData = 0xFFFF0000;
      uint32_t data = 0x0000FFFF;
      CHECK(bench.port.transfer(writeSelect, data) == (0x03 | pswd::ackErrorFlag));
      CHECK(data == 0x0000FFFF);
      CHECK(bench.pins.risingEdges.size() == transferEdges(1));
      if(release) {
         CHECK(bench.pins.contentions == 0);
         // The next transfer is in step.
         bench.pins.readData = 0x1BA01477;
         CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
         CHECK(data == 0x1BA01477);
         REQUIRE(bench.pins.transfers.size() == 2);
         CHECK(bench.pins.transfers[1].request == readDpidr);
         bench.checkIdle();
      } else {
         // The Versaloon rules drive the write data: contention.
         CHECK(bench.pins.contentions > 0);
      }
   }
}

TEST_CASE("SWD bit-bang: with releaseOnAckError, a write without ACK releases SWDIO") {
   Bench bench;
   bench.port.setRules(cmsisDapRules);
   bench.pins.acks = { 0 };
   uint32_t data = 0x01002927;
   CHECK(bench.port.transfer(writeTargetsel, data) == (0x07 | pswd::ackErrorFlag));
   const auto &edges = bench.pins.risingEdges;
   REQUIRE(edges.size() == transferEdges(1) - swd::defaultIdleCycles);
   for(std::size_t i = 8; i < edges.size(); ++i) {
      CAPTURE(i);
      CHECK_FALSE(edges[i].hostDrives);
   }
   CHECK(bench.pins.contentions == 0);
}

TEST_CASE("SWD bit-bang: CMSIS-DAP rules, WAIT then OK, with turnarounds of 1-4 cycles") {
   for(uint8_t turnaround = 1; turnaround <= 4; ++turnaround) {
      CAPTURE(turnaround);
      Bench bench(turnaround, 5);
      bench.port.setRules({ 2, false, false, true, true });
      bench.pins.dataPhase = false;
      bench.pins.acks = { pswd::ackWait, pswd::ackWait, pswd::ackOk, pswd::ackWait, pswd::ackOk };
      bench.pins.readData = 0x600DF00D;
      uint32_t data = 0;
      CHECK(bench.port.transfer(readApDrw, data) == pswd::ackOk);
      CHECK(data == 0x600DF00D);
      data = 0xFEEDBEEF;
      CHECK(bench.port.transfer(writeSelect, data) == pswd::ackOk);
      REQUIRE(bench.pins.transfers.size() == 5);
      CHECK(bench.pins.transfers[4].written == 0xFEEDBEEF);
      CHECK(bench.pins.risingEdges.size() ==
            3 * shortTransferEdges(turnaround, 0) + 2 * (transferEdges(turnaround) - swd::defaultIdleCycles + 2));
      bench.checkIdle();
   }
}

TEST_CASE("SWD bit-bang: half period of a clock in Hz") {
   CHECK(swd::halfPeriodCyclesOfHz(72'000'000, 1'000'000) == 36);
   CHECK(swd::halfPeriodCyclesOfHz(72'000'000, 999'999) == 37);
   CHECK(swd::halfPeriodCyclesOfHz(72'000'000, 235'000) == halfPeriodCycles(72'000'000, 1));
   CHECK(swd::halfPeriodCyclesOfHz(72'000'000, 1) == 36'000'000);
   // 0 Hz: the slowest clock
   CHECK(swd::halfPeriodCyclesOfHz(72'000'000, 0) == 36'000'000);
}

TEST_CASE("SWD bit-bang: the clock in Hz rounds down to an available rate") {
   Bench bench;
   // 1.85 MHz to maxClockHz: timed. The untimed engine is faster there (#118).
   for(const uint32_t hz :
       { 0u, 1u, 1000u, 72'000u, 100'001u, 235'000u, 1'000'000u, 1'500'000u, 1'850'000u, 2'000'000u, swd::maxClockHz - 1 }) {
      CAPTURE(hz);
      bench.port.setClockHz(hz);
      CHECK(bench.port.isTimed());
      const double actual = 72e6 / (2.0 * bench.port.halfCycles());
      CHECK(actual <= (hz == 0 ? 1.0 : hz));
      // The next shorter half period would be faster than hz.
      CHECK(72e6 / (2.0 * (bench.port.halfCycles() - 1)) > hz);
   }
   for(const uint32_t hz : { swd::maxClockHz, 4'000'000u, 10'000'000u, UINT32_MAX }) {
      CAPTURE(hz);
      bench.port.setClockHz(hz);
      // Untimed, as at delay 0
      CHECK_FALSE(bench.port.isTimed());
      CHECK(bench.port.halfCycles() == halfPeriodCycles(72'000'000, 0));
   }
}

TEST_CASE("SWD bit-bang: a fast core runs all clocks timed") {
   CHECK(swd::runsUntimed(72'000'000));
   CHECK_FALSE(swd::runsUntimed(250'000'000));
   Bench bench;
   bench.pins.coreClockHz = 250'000'000;
   bench.port.configure(1, 0, 0);
   CHECK(bench.port.isTimed());
   CHECK(bench.port.halfCycles() == halfPeriodCycles(250'000'000, 0));
   for(const uint32_t hz : { swd::maxClockHz, 4'000'000u, 10'000'000u }) {
      CAPTURE(hz);
      bench.port.setClockHz(hz);
      CHECK(bench.port.isTimed());
      CHECK(250e6 / (2.0 * bench.port.halfCycles()) <= hz);
   }
}

TEST_CASE("SWD bit-bang: setTurnaround() and setRetries() keep the clock in Hz") {
   // A target with a turnaround of 3 cycles, OK after two WAIT
   MockSwdPins pins(3);
   BitBangPort<MockSwdPins> port(pins);
   port.init();
   port.configure(1, 0, 1);
   port.setRules(cmsisDapRules);
   port.setClockHz(1'000'000);
   port.setTurnaround(3);
   port.setRetries(2);
   CHECK(port.halfCycles() == 36);
   CHECK(port.isTimed());
   pins.dataPhase = false;
   pins.acks = { pswd::ackWait, pswd::ackWait };
   pins.readData = 0x1234;
   uint32_t data = 0;
   CHECK(port.transfer(readApDrw, data) == pswd::ackOk);
   CHECK(data == 0x1234);
   CHECK(pins.transfers.size() == 3);
   CHECK(port.halfCycles() == 36);
   CHECK(pins.contentions == 0);
   CHECK(pins.desyncs == 0);
}

TEST_CASE("SWD bit-bang: transfers at a clock in Hz") {
   Bench bench;
   bench.port.setRules(cmsisDapRules);
   bench.port.setClockHz(1'000'000);
   bench.pins.readData = 0x2BA01477;
   uint32_t data = 0;
   CHECK(bench.port.transfer(readDpidr, data) == pswd::ackOk);
   CHECK(data == 0x2BA01477);
   const auto &edges = bench.pins.edges;
   REQUIRE(edges.size() == 2 * (transferEdges(1) - swd::defaultIdleCycles));
   for(std::size_t i = 1; i < edges.size(); ++i) {
      CAPTURE(i);
      CHECK(edges[i] - edges[i - 1] >= 36);
      CHECK(edges[i] - edges[i - 1] <= 36 + 2);
   }
   bench.checkIdle();
}

TEST_CASE("SWD bit-bang: sequences of DAP_SWJ_Sequence and DAP_SWD_Sequence") {
   Bench bench;
   bench.pins.decodes = false;
   // DAP_SWJ_Sequence: up to 256 bits
   std::vector<uint8_t> out(32);
   for(std::size_t i = 0; i < out.size(); ++i) {
      out[i] = static_cast<uint8_t>(i * 37 + 1);
   }
   bench.port.sequenceOut(256, art::span<const uint8_t>(out.data(), out.size()));
   REQUIRE(bench.pins.risingEdges.size() == 256);
   for(std::size_t i = 0; i < 256; ++i) {
      CAPTURE(i);
      CHECK(bench.pins.risingEdges[i].hostDrives);
      CHECK(bench.pins.risingEdges[i].level == (((out[i / 8] >> (i % 8)) & 1u) != 0));
   }
   // DAP_SWD_Sequence: up to 64 bits in each direction
   std::vector<uint8_t> in(8, 0);
   bench.port.sequenceIn(64, art::span<uint8_t>(in.data(), in.size()));
   CHECK(in == std::vector<uint8_t>(8, 0xFF));
   CHECK_FALSE(bench.pins.hostDrives());
   bench.port.sequenceOut(64, art::span<const uint8_t>(out.data(), 8));
   CHECK(bench.pins.hostDrives());
   CHECK(bench.pins.risingEdges.size() == 256 + 64 + 64);
   CHECK(bench.pins.swclkLevel());
   CHECK(bench.pins.contentions == 0);
}
