// The clock pacer of the bit-bang engines (ClockPacer.h): edges on
// deadlines, also with code between the edges, and late edges.

#include "ClockPacer.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

// Each call of now() is one cycle. edge() records the time of an edge.
struct Clock {
   // Starts near the wrap of the counter.
   uint32_t time{ 0xFFFFFF00u };
   std::vector<uint32_t> edges;

   uint32_t now() {
      return time++;
   }
   void edge() {
      edges.push_back(time);
   }
};

void edges(Clock &clock, pacing::ClockPacer &pacer, uint32_t count, uint32_t cycles, uint32_t codeCycles) {
   for(uint32_t i = 0; i < count; ++i) {
      clock.time += codeCycles;
      pacer.wait(clock, cycles);
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

TEST_CASE("Clock pacer: after a late edge, the next half period is complete") {
   Clock clock;
   pacing::ClockPacer pacer;
   pacer.start(clock);
   edges(clock, pacer, 5, 40, 10);
   // An interrupt between the end of the wait and the edge
   pacer.wait(clock, 40);
   clock.time += 500;
   clock.edge();
   pacer.edge(clock);
   edges(clock, pacer, 5, 40, 10);
   const auto &e = clock.edges;
   CHECK(e[5] - e[4] >= 540);
   for(std::size_t i = 6; i < e.size(); ++i) {
      CAPTURE(i);
      CHECK(e[i] - e[i - 1] >= 40);
      CHECK(e[i] - e[i - 1] <= 41);
   }
}

TEST_CASE("Clock pacer: an edge in the slack keeps the deadlines") {
   Clock clock;
   pacing::ClockPacer pacer;
   pacer.start(clock);
   const uint32_t start = clock.time;
   pacer.wait(clock, 40);
   clock.time += pacing::lateSlackCycles - 2;
   clock.edge();
   pacer.edge(clock);
   edges(clock, pacer, 1, 40, 0);
   // The second edge is on its deadline: the first edge was late within the slack.
   CHECK(clock.edges[1] - start == 80);
}
