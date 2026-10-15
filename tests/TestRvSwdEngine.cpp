// The RISC-V debug wire engine on mock pins with a mock target. The
// reference frames and times come from the WCH-LinkE captures in
// docs/wch-link-captures (docs/wch-link-protocol.md).

#include "rvswd/RvSwdEngine.h"

#include "MockBackground.h"
#include "MockRvSwdTarget.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using rvswd::RvSwdEngine;
using rvswd::Wire;
using vsltest::MockRvSwdTarget;

// Nanoseconds of core cycles at the 72 MHz of the mock
double nsOf(uint32_t cycles) {
   return cycles * 1000.0 / 72.0;
}

struct Bench {
   explicit Bench(Wire wire)
      : pins(wire)
      , engine(pins) {
      engine.init(wire);
   }

   // The line state after a frame: idle, no error on the wire
   void checkIdle() const {
      CHECK(pins.hostDrives());
      CHECK(pins.hostLevel());
      CHECK_FALSE(pins.swclkLevel());
      CHECK_FALSE(pins.irqLocked());
      CHECK_FALSE(pins.inFrame());
      CHECK(pins.protocolErrors == 0);
      CHECK(pins.contentions == 0);
      CHECK(pins.undrivenSamples == 0);
      CHECK(pins.unlockedChanges == 0);
   }

   MockRvSwdTarget pins;
   RvSwdEngine<MockRvSwdTarget> engine;
};

// Two wires: bits 1-53 of a capture line of tools/wch_link_frames_2wire.py
// (bit 0 is the clock before the start condition).
std::vector<bool> bitsOf(const std::string &frame) {
   std::vector<bool> bits;
   for(std::size_t i = 1; i < frame.size(); ++i) {
      bits.push_back(frame[i] == '1');
   }
   return bits;
}

// One wire: the bits of the low pulses on the line (500 ns threshold),
// from a line edge index on. Ends at a high level of more than 2 us.
std::string oneWireBits(const std::vector<MockRvSwdTarget::LineEdge> &edges, std::size_t first = 0) {
   std::string bits;
   for(std::size_t i = first; i + 1 < edges.size(); ++i) {
      const uint32_t width = edges[i + 1].time - edges[i].time;
      if(edges[i].level) {
         if(!bits.empty() && nsOf(width) > 2000) {
            break;
         }
         continue;
      }
      bits.push_back(nsOf(width) < 500 ? '1' : '0');
   }
   return bits;
}

// The index of the first falling line edge at or after a time
std::size_t firstFall(const std::vector<MockRvSwdTarget::LineEdge> &edges, uint32_t time) {
   for(std::size_t i = 0; i < edges.size(); ++i) {
      if(!edges[i].level && edges[i].time - time < 0x8000'0000u) {
         return i;
      }
   }
   return edges.size();
}

} // namespace

TEST_CASE("RVSWD: cycles of the times and clock rates at 72 MHz") {
   CHECK(rvswd::cyclesOfNs(72'000'000, 250) == 18);
   CHECK(rvswd::cyclesOfNs(72'000'000, 860) == 62);
   CHECK(rvswd::cyclesOfNs(72'000'000, 370) == 27);
   CHECK(rvswd::halfPeriodCycles(72'000'000, rvswd::lowClockHz) == 76);
   CHECK(rvswd::halfPeriodCycles(72'000'000, rvswd::highClockHz) == 40);
   CHECK(rvswd::halfPeriodCycles(72'000'000, 0) == 36'000'000);
   CHECK(rvswd::parity(0x5AA5'0400));
   CHECK_FALSE(rvswd::parity(0x8000'0001));
}

TEST_CASE("RVSWD: init and fini go to the pins, the idle levels") {
   for(const Wire wire : { Wire::Two, Wire::One }) {
      Bench bench(wire);
      CHECK(bench.pins.inits == 1);
      CHECK(bench.engine.wire() == wire);
      CHECK(bench.engine.halfCycles() == rvswd::halfPeriodCycles(72'000'000, rvswd::defaultClockHz));
      bench.checkIdle();
      bench.engine.fini();
      CHECK(bench.pins.finis == 1);
      CHECK_FALSE(bench.pins.hostDrives());
   }
}

TEST_CASE("RVSWD two wires: a write reaches the target") {
   Bench bench(Wire::Two);
   CHECK(bench.engine.write(0x10, 0x8000'0001) == 0);
   CHECK(bench.pins.registers[0x10] == 0x8000'0001);
   REQUIRE(bench.pins.accesses.size() == 1);
   CHECK(bench.pins.accesses[0].write);
   CHECK(bench.pins.accesses[0].address == 0x10);
   CHECK(bench.pins.starts == 1);
   CHECK(bench.pins.stops == 1);
   CHECK(bench.pins.frameBits.size() == 53);
   CHECK(bench.engine.head() == 0b0101);
   CHECK(bench.engine.tail() == 0b0111);
   bench.checkIdle();
}

TEST_CASE("RVSWD two wires: a read gives the register, the response and the tail bits") {
   Bench bench(Wire::Two);
   bench.pins.registers[0x04] = 0x0303'8083;
   uint32_t data = 0;
   CHECK(bench.engine.read(0x04, data) == 0);
   CHECK(data == 0x0303'8083);
   REQUIRE(bench.pins.accesses.size() == 1);
   CHECK_FALSE(bench.pins.accesses[0].write);
   CHECK(bench.engine.head() == 0b0101);
   CHECK(bench.engine.tail() == 0b0111);
   CHECK(bench.pins.stops == 1);
   bench.checkIdle();
}

TEST_CASE("RVSWD two wires: the frame bits are the bits of the capture") {
   // tools/wch_link_frames_2wire.py on v208-wlink-status-timing.csv
   struct Frame {
      uint8_t address;
      bool write;
      uint32_t data;
      const char *bits;
   };
   const Frame frames[] = {
      { 0x7E, true, 0x5AA5'0400, "111111101110000010110101010010100000100000000001100110" },
      { 0x10, true, 0x8000'0001, "100100001000000100000000000000000000000000000010000110" },
      { 0x05, true, 0x1FFF'F704, "100001011110000000111111111111111110111000001001100110" },
      { 0x11, false, 0x0000'0C82, "100100010000000000000000000000000001100100000100001110" },
      { 0x7F, false, 0x2080'053C, "111111110110100001000001000000000000101001111000001110" },
   };
   for(const Frame &frame : frames) {
      CAPTURE(frame.bits);
      Bench bench(Wire::Two);
      if(frame.write) {
         CHECK(bench.engine.write(frame.address, frame.data) == 0);
      } else {
         bench.pins.registers[frame.address] = frame.data;
         uint32_t data = 0;
         CHECK(bench.engine.read(frame.address, data) == 0);
         CHECK(data == frame.data);
      }
      // Nobody drives bits 12, 14, 50 and 52. In the capture, SWDIO rises
      // slowly there (analog), and the logic level is not defined. The
      // mock has the level of the pull-up.
      std::vector<bool> expected = bitsOf(frame.bits);
      for(const std::size_t bit : { 12u, 14u, 50u, 52u }) {
         expected[bit - 1] = true;
      }
      CHECK(bench.pins.frameBits == expected);
      bench.checkIdle();
   }
}

TEST_CASE("RVSWD two wires: the handshake bits of the probe") {
   // The analog capture of the WCH-LinkE (CH32V208): the probe drives bit
   // 13 low and bit 51 high, and releases SWDIO in bits 11, 12, 14, 49, 50
   // and 52.
   for(const bool write : { true, false }) {
      CAPTURE(write);
      Bench bench(Wire::Two);
      uint32_t data = 0;
      CHECK((write ? bench.engine.write(0x10, 1) : bench.engine.read(0x10, data)) == 0);
      const std::vector<bool> &host = bench.pins.frameHostBits;
      REQUIRE(host.size() == 53);
      for(const std::size_t bit : { 11u, 12u, 14u, 49u, 50u, 52u }) {
         CAPTURE(bit);
         CHECK_FALSE(host[bit - 1]);
      }
      CHECK(host[13 - 1]);
      CHECK_FALSE(bench.pins.frameBits[13 - 1]);
      CHECK(host[51 - 1]);
      CHECK(bench.pins.frameBits[51 - 1]);
      CHECK(bench.pins.droppedFrames == 0);
      bench.checkIdle();
   }
}

TEST_CASE("RVSWD two wires: SWCLK timing of the capture") {
   // v208-wlink-status-timing.csv and -low-timing.csv: a period of 2.09 us
   // (1.10 us for the fast writes). The low phase before bits 1, 15 and 53
   // is 2.1 to 2.2 us. Start to stop is 115.1 us at 2.09 us.
   struct Rate {
      uint32_t hz;
      double periodNs;
   };
   for(const Rate rate : { Rate{ rvswd::lowClockHz, 2090 }, Rate{ rvswd::highClockHz, 1100 } }) {
      CAPTURE(rate.hz);
      Bench bench(Wire::Two);
      bench.pins.swclkEdgeCycles = rvswd::loopOverheadCycles;
      bench.engine.setClockHz(rate.hz);
      bench.pins.risingEdges.clear();
      bench.pins.lineEdges.clear();
      CHECK(bench.engine.write(0x7E, 0x5AA5'0400) == 0);
      bench.checkIdle();
      // The clock before the start condition and bits 1-53
      const std::vector<uint32_t> &edges = bench.pins.risingEdges;
      REQUIRE(edges.size() == 54);
      // The mock adds the loop code at each SWCLK edge. The phases without
      // loop code (the conditions and the longer low phases) wait for the
      // full half period, so the mock makes them longer: up to 15 % at the
      // low rate, 30 % at the high rate (the loop code is 24 of 40 cycles).
      const double slack = rate.hz == rvswd::lowClockHz ? 1.15 : 1.30;
      const auto near = [slack](double ns, double expected) { return ns >= 0.95 * expected && ns <= slack * expected; };
      for(std::size_t bit = 2; bit <= 53; ++bit) {
         CAPTURE(bit);
         const double period = nsOf(edges[bit] - edges[bit - 1]);
         if(bit == 15 || bit == 53) {
            CHECK(near(period, 1.5 * rate.periodNs));
         } else {
            CHECK(period >= 0.95 * rate.periodNs);
            CHECK(period <= 1.05 * rate.periodNs);
         }
      }
      // The start condition after the first rising edge, and the stop
      // condition after the last
      std::vector<MockRvSwdTarget::LineEdge> conditions;
      for(const auto &edge : bench.pins.lineEdges) {
         if(edge.swclk && edge.host) {
            conditions.push_back(edge);
         }
      }
      REQUIRE(conditions.size() == 2);
      CHECK_FALSE(conditions[0].level);
      CHECK(conditions[1].level);
      CHECK(near(nsOf(conditions[0].time - edges[0]), rate.periodNs / 2));
      CHECK(near(nsOf(edges[1] - conditions[0].time), 1.5 * rate.periodNs));
      CHECK(near(nsOf(conditions[1].time - edges[53]), rate.periodNs / 2));
      if(rate.hz == rvswd::lowClockHz) {
         CHECK(nsOf(conditions[1].time - conditions[0].time) == doctest::Approx(115'100).epsilon(0.03));
      }
   }
}

TEST_CASE("RVSWD two wires: a wrong parity bit of the read data") {
   Bench bench(Wire::Two);
   bench.pins.registers[0x11] = 0x0000'0382;
   bench.pins.badParity = true;
   uint32_t data = 0x1234;
   CHECK(bench.engine.read(0x11, data) == rvswd::parityErrorFlag);
   CHECK(data == 0x1234);
   // The next read is right again.
   CHECK(bench.engine.read(0x11, data) == 0);
   CHECK(data == 0x0000'0382);
   bench.checkIdle();
}

TEST_CASE("RVSWD two wires: without a target, a read gives a parity error") {
   Bench bench(Wire::Two);
   bench.pins.present = false;
   uint32_t data = 0x1234;
   // SWDIO reads 1 from the pull-up: 32 ones have an even parity.
   CHECK(bench.engine.read(0x11, data) == rvswd::parityErrorFlag);
   CHECK(data == 0x1234);
   // Bit 13 is the low level of the probe.
   CHECK(bench.engine.head() == 0b1101);
   CHECK(bench.engine.tail() == 0b1111);
   bench.checkIdle();
}

TEST_CASE("RVSWD two wires: the address has 7 bits") {
   Bench bench(Wire::Two);
   CHECK(bench.engine.write(0x80 | 0x10, 1) == 0);
   REQUIRE(bench.pins.accesses.size() == 1);
   CHECK(bench.pins.accesses[0].address == 0x10);
   bench.checkIdle();
}

TEST_CASE("RVSWD two wires: line reset") {
   Bench bench(Wire::Two);
   bench.engine.lineReset();
   CHECK(bench.pins.lineResets == 1);
   CHECK(bench.pins.risingEdges.size() == rvswd::lineResetClocks);
   CHECK(bench.pins.starts == 0);
   // The WCH-LinkE before its first frame: 9 clocks with SWDIO high, 1 with
   // SWDIO low, 90 with SWDIO high. SWDIO changes only while SWCLK is low.
   std::vector<bool> expected(9, true);
   expected.push_back(false);
   expected.insert(expected.end(), 90, true);
   CHECK(bench.pins.idleBits == expected);
   for(const auto &edge : bench.pins.lineEdges) {
      CHECK_FALSE(edge.swclk);
   }
   bench.checkIdle();
   CHECK(bench.engine.write(0x10, 1) == 0);
   CHECK(bench.pins.registers[0x10] == 1);
   bench.checkIdle();
}

TEST_CASE("RVSWD: the background runs before each frame, with the interrupts on") {
   for(const Wire wire : { Wire::Two, Wire::One }) {
      CAPTURE(static_cast<int>(wire));
      MockRvSwdTarget pins(wire);
      vsltest::MockBackground background(
      [&pins] { return vsltest::MockBackground::Call{ pins.time, pins.irqLocked(), pins.risingEdges.size() }; });
      RvSwdEngine<MockRvSwdTarget> engine(pins, &background);
      engine.init(wire);
      // More than 1 ms of frames: about 9 frames at 115 us, 25 at 45 us
      const int frames = wire == Wire::Two ? 30 : 60;
      for(int i = 0; i < frames; ++i) {
         CHECK(engine.write(0x04, static_cast<uint32_t>(i)) == 0);
      }
      CHECK(pins.accesses.size() == static_cast<std::size_t>(frames));
      REQUIRE(background.calls.size() >= 2);
      for(std::size_t i = 0; i < background.calls.size(); ++i) {
         CHECK_FALSE(background.calls[i].clock);
         if(i > 0) {
            CHECK(background.calls[i].time - background.calls[i - 1].time >= 72'000u);
         }
      }
      CHECK(pins.protocolErrors == 0);
   }
}

TEST_CASE("RVSWD one wire: a write reaches the target, with the interrupts off") {
   Bench bench(Wire::One);
   CHECK(bench.engine.write(0x10, 0x8000'0001) == 0);
   CHECK(bench.pins.registers[0x10] == 0x8000'0001);
   REQUIRE(bench.pins.accesses.size() == 1);
   CHECK(bench.pins.accesses[0].write);
   CHECK(bench.pins.locks == 1);
   CHECK(bench.engine.head() == 0);
   CHECK(bench.engine.tail() == 0);
   bench.checkIdle();
}

TEST_CASE("RVSWD one wire: a read gives the register") {
   Bench bench(Wire::One);
   bench.pins.registers[0x7C] = 0x0001'0403;
   uint32_t data = 0;
   CHECK(bench.engine.read(0x7C, data) == 0);
   CHECK(data == 0x0001'0403);
   REQUIRE(bench.pins.accesses.size() == 1);
   CHECK_FALSE(bench.pins.accesses[0].write);
   CHECK(bench.pins.contentions == 0);
   bench.checkIdle();
}

TEST_CASE("RVSWD one wire: the pulses are the pulses of the capture") {
   // tools/wch_link_frames_1wire.py on v003-wlink-status-timing.csv, and
   // the duration from the first falling edge to the last rising edge
   struct Frame {
      uint8_t address;
      bool write;
      uint32_t data;
      const char *bits;
      double durationNs;
   };
   const Frame frames[] = {
      { 0x7E, true, 0x5AA5'0400, "11111110101011010101001010000010000000000", 39'670 },
      { 0x7D, true, 0x5AA5'0400, "11111101101011010101001010000010000000000", 39'670 },
      { 0x7C, false, 0x0001'0403, "11111100000000000000000010000010000000011", 44'260 },
      { 0x11, false, 0x004F'0C82, "10010001000000000010011110000110010000010", 42'770 },
   };
   for(const Frame &frame : frames) {
      CAPTURE(frame.bits);
      Bench bench(Wire::One);
      bench.pins.lineEdges.clear();
      const uint32_t start = bench.pins.time;
      if(frame.write) {
         CHECK(bench.engine.write(frame.address, frame.data) == 0);
      } else {
         bench.pins.registers[frame.address] = frame.data;
         uint32_t data = 0;
         CHECK(bench.engine.read(frame.address, data) == 0);
         CHECK(data == frame.data);
      }
      bench.checkIdle();
      const auto &edges = bench.pins.lineEdges;
      const std::size_t first = firstFall(edges, start);
      CHECK(oneWireBits(edges, first) == frame.bits);
      REQUIRE(first + 81 < edges.size());
      // A write: within 5 %. A read: the engine waits for the sample point
      // (600 ns) also for a 1, so the read is slower than the WCH-LinkE
      // (up to 15 %).
      const double duration = nsOf(edges[first + 81].time - edges[first].time);
      CHECK(duration >= 0.97 * frame.durationNs);
      CHECK(duration <= (frame.write ? 1.05 : 1.15) * frame.durationNs);
      // The gap to the next frame
      CHECK(nsOf(bench.pins.time - edges[first + 81].time) >= rvswd::oneWireFrameGapNs);
   }
}

TEST_CASE("RVSWD one wire: pulse widths of a write and the turnaround of a read") {
   Bench bench(Wire::One);
   bench.pins.lineEdges.clear();
   CHECK(bench.engine.write(0x7E, 0x5AA5'0400) == 0);
   const auto &edges = bench.pins.lineEdges;
   REQUIRE(edges.size() >= 82);
   for(std::size_t i = 0; i < 81; ++i) {
      CAPTURE(i);
      const double width = nsOf(edges[i + 1].time - edges[i].time);
      if(edges[i].level) {
         CHECK(width == doctest::Approx(rvswd::oneWireHighNs).epsilon(0.1));
      } else {
         const bool one = width < 500;
         CHECK(width == doctest::Approx(one ? rvswd::oneWireShortLowNs : rvswd::oneWireLongLowNs).epsilon(0.1));
      }
   }
   // A read: the high level after bit 8 (the ninth low pulse)
   bench.pins.lineEdges.clear();
   uint32_t data = 0;
   CHECK(bench.engine.read(0x7C, data) == 0);
   REQUIRE(edges.size() >= 19);
   CHECK(edges[17].level);
   CHECK(nsOf(edges[18].time - edges[17].time) == doctest::Approx(rvswd::oneWireTurnaroundHighNs).epsilon(0.1));
   bench.checkIdle();
}

TEST_CASE("RVSWD one wire: the loop code does not add to a frame") {
   // Each phase ends at a time from the start of the phase before. With
   // 8 cycles of code at each SWDIO write (82 writes), a write frame keeps
   // the duration of the capture (39.67 us).
   Bench bench(Wire::One);
   bench.pins.swdioWriteCycles = 8;
   bench.pins.lineEdges.clear();
   CHECK(bench.engine.write(0x7E, 0x5AA5'0400) == 0);
   const auto &edges = bench.pins.lineEdges;
   REQUIRE(edges.size() >= 82);
   CHECK(nsOf(edges[81].time - edges[0].time) == doctest::Approx(39'670).epsilon(0.02));
   bench.checkIdle();
}

TEST_CASE("RVSWD one wire: the clock rate has no effect") {
   Bench bench(Wire::One);
   bench.pins.lineEdges.clear();
   CHECK(bench.engine.write(0x04, 0x1234'5678) == 0);
   const auto first = bench.pins.lineEdges;
   bench.engine.setClockHz(rvswd::highClockHz);
   bench.pins.lineEdges.clear();
   CHECK(bench.engine.write(0x04, 0x1234'5678) == 0);
   const auto &second = bench.pins.lineEdges;
   REQUIRE(first.size() == second.size());
   for(std::size_t i = 0; i + 1 < first.size(); ++i) {
      CHECK(first[i + 1].time - first[i].time == second[i + 1].time - second[i].time);
   }
}

TEST_CASE("RVSWD one wire: a line held low ends the read") {
   Bench bench(Wire::One);
   bench.pins.stuckLow = true;
   uint32_t data = 0x1234;
   CHECK(bench.engine.read(0x11, data) == rvswd::lineLowFlag);
   CHECK(data == 0x1234);
   // SWDIO stays released, the interrupts are on again.
   CHECK_FALSE(bench.pins.hostDrives());
   CHECK_FALSE(bench.pins.irqLocked());
   CHECK(bench.pins.contentions == 0);
   // A line reset brings the target back in step.
   bench.pins.stuckLow = false;
   bench.pins.registers[0x11] = 0x0000'0382;
   bench.engine.lineReset();
   CHECK(bench.engine.read(0x11, data) == 0);
   CHECK(data == 0x0000'0382);
   bench.checkIdle();
}

TEST_CASE("RVSWD one wire: the host does not drive a line that the target holds low") {
   Bench bench(Wire::One);
   bench.pins.stuckLow = true;
   uint32_t data = 0x1234;
   CHECK(bench.engine.read(0x11, data) == rvswd::lineLowFlag);
   // The line is still low: the next frames end at once, with SWDIO
   // released.
   CHECK(bench.engine.read(0x11, data) == rvswd::lineLowFlag);
   CHECK(bench.engine.write(0x10, 1) == rvswd::lineLowFlag);
   CHECK(data == 0x1234);
   CHECK_FALSE(bench.pins.hostDrives());
   CHECK_FALSE(bench.pins.irqLocked());
   // A reset drives low (no fight) and releases the line again.
   bench.engine.lineReset();
   CHECK_FALSE(bench.pins.hostDrives());
   CHECK(bench.pins.contentions == 0);
   // When the target lets the line go, the frames run again.
   bench.pins.stuckLow = false;
   bench.pins.registers[0x11] = 0x0000'0382;
   bench.engine.lineReset();
   CHECK(bench.engine.read(0x11, data) == 0);
   CHECK(data == 0x0000'0382);
   bench.checkIdle();
}

TEST_CASE("RVSWD one wire: line reset") {
   MockRvSwdTarget pins(Wire::One);
   vsltest::MockBackground background(
   [&pins] { return vsltest::MockBackground::Call{ pins.time, pins.irqLocked(), pins.risingEdges.size() }; });
   RvSwdEngine<MockRvSwdTarget> engine(pins, &background);
   engine.init(Wire::One);
   pins.lineEdges.clear();
   engine.lineReset();
   CHECK(pins.lineResets == 1);
   REQUIRE(pins.lineEdges.size() == 2);
   CHECK_FALSE(pins.lineEdges[0].level);
   // The WCH-LinkE: 1.98 ms
   CHECK(nsOf(pins.lineEdges[1].time - pins.lineEdges[0].time) == doctest::Approx(2'000'000).epsilon(0.02));
   // The background runs in the 2 ms.
   CHECK(background.calls.size() >= 2);
   CHECK(pins.risingEdges.empty());
   CHECK(engine.write(0x10, 1) == 0);
   CHECK(pins.registers[0x10] == 1);
   CHECK(pins.protocolErrors == 0);
}
