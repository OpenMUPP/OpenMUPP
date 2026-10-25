// The clock pacer of the bit-bang engines (ClockPacer.h): edges on
// deadlines, also with code between the edges, and late edges.

#include "ClockPacer.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

// Each call of now() is `step` cycles. edge() records the time of an edge.
struct Clock {
   // Starts near the wrap of the counter.
   uint32_t time{ 0xFFFFFF00u };
   uint32_t step{ 1 };
   std::vector<uint32_t> edges;

   uint32_t now() {
      const uint32_t t = time;
      time += step;
      return t;
   }
   void edge() {
      edges.push_back(time);
   }
};

void edges(Clock &clock, pacing::ClockPacer &pacer, uint32_t count, uint32_t cycles, uint32_t codeCycles) {
   pacer.setHalfCycles(cycles);
   for(uint32_t i = 0; i < count; ++i) {
      clock.time += codeCycles;
      pacer.wait(clock);
      clock.edge();
      pacer.edge(clock);
   }
}

} // namespace

TEST_CASE("Clock pacer: the code between the edges does not change the period") {
   for(const uint32_t code : { 0u, 10u, 30u }) {
      CAPTURE(code);
      Clock clock;
      pacing::ClockPacer pacer;
      pacer.start(clock);
      const uint32_t start = clock.time;
      edges(clock, pacer, 100, 40, code);
      // The counter wraps in this test.
      CHECK(clock.edges.back() < start);
      for(std::size_t i = 1; i < clock.edges.size(); ++i) {
         CHECK(clock.edges[i] - clock.edges[i - 1] == 40);
      }
   }
}

TEST_CASE("Clock pacer: code longer than the half period makes the clock slower, not faster") {
   Clock clock;
   pacing::ClockPacer pacer;
   pacer.start(clock);
   edges(clock, pacer, 20, 40, 60);
   for(std::size_t i = 1; i < clock.edges.size(); ++i) {
      CHECK(clock.edges[i] - clock.edges[i - 1] >= 60);
   }
}

TEST_CASE("Clock pacer: after code longer than the half period, the next half period is complete") {
   Clock clock;
   pacing::ClockPacer pacer;
   pacer.start(clock);
   edges(clock, pacer, 5, 40, 10);
   // A background service call between two edges
   edges(clock, pacer, 1, 40, 500);
   edges(clock, pacer, 5, 40, 10);
   const auto &e = clock.edges;
   CHECK(e[5] - e[4] >= 500);
   for(std::size_t i = 6; i < e.size(); ++i) {
      CAPTURE(i);
      CHECK(e[i] - e[i - 1] >= 40);
      CHECK(e[i] - e[i - 1] <= 41);
   }
}

TEST_CASE("Clock pacer: after an interrupt between the wait and the edge, the next half period is complete") {
   Clock clock;
   pacing::ClockPacer pacer;
   pacer.start(clock);
   edges(clock, pacer, 2, 100, 0);
   pacer.wait(clock);
   clock.time += 300;
   clock.edge();
   pacer.edge(clock);
   edges(clock, pacer, 2, 100, 0);
   const auto &e = clock.edges;
   CHECK(e[2] - e[1] >= 400);
   CHECK(e[3] - e[2] >= 100);
   CHECK(e[3] - e[2] <= 102);
   CHECK(e[4] - e[3] == 100);
}

TEST_CASE("Clock pacer: an edge within the slack keeps the deadlines") {
   Clock clock;
   pacing::ClockPacer pacer;
   pacer.setHalfCycles(100);
   pacer.start(clock);
   const uint32_t start = clock.time;
   pacer.wait(clock);
   clock.time += pacing::lateSlackCycles - 2;
   clock.edge();
   pacer.edge(clock);
   edges(clock, pacer, 1, 100, 0);
   CHECK(clock.edges[1] - start == 200);
}

TEST_CASE("Clock pacer: the jitter of a slow wait loop does not make the clock slower") {
   // The wait loop on the STM32F103 at 72 MHz with flash wait states: some
   // cycles for each read of the counter. The edges spread by up to one
   // loop pass, but the deadlines stay.
   Clock clock;
   clock.step = 9;
   pacing::ClockPacer pacer;
   pacer.start(clock);
   const uint32_t start = clock.time;
   edges(clock, pacer, 1000, 154, 37);
   const auto &e = clock.edges;
   for(std::size_t i = 1; i < e.size(); ++i) {
      CAPTURE(i);
      CHECK(e[i] - e[i - 1] >= 154 - 9);
      CHECK(e[i] - e[i - 1] <= 154 + 9);
   }
   // The mean is exact: the last edge is less than one loop pass after its
   // deadline.
   CHECK(e.back() - start >= 1000 * 154);
   CHECK(e.back() - start < 1000 * 154 + 9);
}
