// SPDX-License-Identifier: GPL-3.0-or-later
// The software decoder of the SWO Manchester mode (swo/ManchesterDecoder.h):
// the timer setup, the decoder on encoded frames, and the merge of the two
// edge streams of the capture.

#include "swo/ManchesterDecoder.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

struct Edge {
   uint32_t time;
   bool rising;
};

// Encodes frames as the TPIU does: the start bit, the bytes LSB first, and
// the line low between the frames. Times are in timer ticks; the 16-bit
// decoder gets them modulo 65536.
class Encoder {
   public:
   Encoder(double ticksPerBit, uint32_t start)
      : half_(ticksPerBit / 2)
      , time_(start) {
   }

   // shift(k): the jitter of the edge, in ticks
   template <typename Jitter>
   void frame(const Bytes &bytes, Jitter shift, int extraBits = 0, bool extraValue = false) {
      half(true);
      half(false);
      for(const uint8_t byte : bytes) {
         for(int bit = 0; bit < 8; ++bit) {
            this->bit(((byte >> bit) & 1) != 0);
         }
      }
      for(int i = 0; i < extraBits; ++i) {
         bit(extraValue);
      }
      flush(shift);
   }
   void frame(const Bytes &bytes) {
      frame(bytes, [](std::size_t) { return 0.0; });
   }

   // The line low for `bits` bit times
   void gap(double bits) {
      position_ += bits * 2;
   }

   [[nodiscard]] double now() const {
      return time_ + position_ * half_;
   }

   std::vector<Edge> edges;

   private:
   void bit(bool value) {
      half(value);
      half(!value);
   }
   void half(bool level) {
      levels_.push_back(level);
   }
   template <typename Jitter>
   void flush(Jitter shift) {
      // The halves of this frame, then the low line
      levels_.push_back(false);
      bool line = false;
      for(std::size_t i = 0; i < levels_.size(); ++i) {
         if(levels_[i] != line) {
            line = levels_[i];
            const double at = time_ + (position_ + static_cast<double>(i)) * half_ + shift(edges.size());
            edges.push_back({ static_cast<uint32_t>(at + 0.5), line });
         }
      }
      position_ += static_cast<double>(levels_.size() - 1);
      levels_.clear();
   }

   double half_;
   double time_;
   double position_ = 0;
   std::vector<bool> levels_;
};

constexpr uint32_t clock = 144'000'000;

struct Bench {
   explicit Bench(uint32_t baud, uint32_t start = 0) {
      const auto t = swo::manchesterTiming(clock, baud);
      REQUIRE(t.has_value());
      timing = *t;
      ticksPerBit = static_cast<double>(clock) / (timing.prescaler + 1) / baud;
      decoder.configure(timing);
      decoder.reset(false, static_cast<uint16_t>(start));
   }

   void feed(const std::vector<Edge> &edges) {
      for(const Edge &e : edges) {
         decoder.edge(static_cast<uint16_t>(e.time), e.rising, [this](uint8_t b) { out.push_back(b); });
      }
   }

   void idleAt(double time) {
      decoder.idle(static_cast<uint16_t>(static_cast<uint32_t>(time)));
   }

   swo::ManchesterTiming timing{};
   double ticksPerBit = 0;
   swo::ManchesterDecoder decoder;
   Bytes out;
};

const Bytes pattern{ 0x00, 0xFF, 0xA5, 0x5A, 0x01, 0x80, 0x33 };

} // namespace

TEST_CASE("Manchester timing: the prescaler gives 16 to 32 ticks per bit") {
   // 144 MHz / 5 = 28.8 MHz: 28.8 ticks per bit at 1 Mbit/s
   const auto t = swo::manchesterTiming(clock, 1'000'000);
   REQUIRE(t.has_value());
   CHECK(t->prescaler == 4);
   CHECK(t->minHalf == 7);
   CHECK(t->minBit == 22);
   CHECK(t->maxBit == 36);
   CHECK(t->frameEnd == 43);
   // 9 Mbit/s: 16 ticks per bit, no prescaler
   const auto fast = swo::manchesterTiming(clock, 9'000'000);
   REQUIRE(fast.has_value());
   CHECK(fast->prescaler == 0);
   CHECK(fast->minBit == 12);
   CHECK(fast->maxBit == 20);
   // The limits: less than 16 ticks per bit, a prescaler above 65535
   CHECK_FALSE(swo::manchesterTiming(clock, 9'000'001).has_value());
   CHECK(swo::manchesterTiming(clock, 69).has_value());
   CHECK_FALSE(swo::manchesterTiming(clock, 68).has_value());
   CHECK_FALSE(swo::manchesterTiming(clock, 0).has_value());
   // The timer period is at least 2048 bit times
   for(const uint32_t baud : { 69U, 1000U, 115'200U, 500'000U, 1'000'000U, 2'000'000U }) {
      const auto timing = swo::manchesterTiming(clock, baud);
      REQUIRE(timing.has_value());
      const double ticksPerBit = static_cast<double>(clock) / (timing->prescaler + 1) / baud;
      CHECK(ticksPerBit <= 32.0);
      CHECK(ticksPerBit > 16.0);
   }
}

TEST_CASE("Manchester decoder: one frame gives its bytes") {
   Bench b(1'000'000, 100);
   Encoder enc(b.ticksPerBit, 200);
   enc.frame(pattern);
   b.feed(enc.edges);
   CHECK(b.out == pattern);
   b.idleAt(enc.now() + 10 * b.ticksPerBit);
   CHECK_FALSE(b.decoder.takeError());
}

TEST_CASE("Manchester decoder: a frame across the wrap of the 16-bit timer") {
   Bench b(115'200, 65'000);
   Encoder enc(b.ticksPerBit, 65'400);
   enc.frame(pattern);
   REQUIRE(enc.edges.back().time > 65'536);
   b.feed(enc.edges);
   CHECK(b.out == pattern);
   CHECK_FALSE(b.decoder.takeError());
}

TEST_CASE("Manchester decoder: frames with the shortest gap, after a last 1 and after a last 0") {
   Bench b(1'000'000);
   Encoder enc(b.ticksPerBit, 50);
   enc.frame({ 0x80 }); // last bit 1
   enc.gap(1);
   enc.frame({ 0x7F }); // last bit 0
   enc.gap(1);
   enc.frame({ 0x12, 0x34 });
   b.feed(enc.edges);
   CHECK(b.out == Bytes{ 0x80, 0x7F, 0x12, 0x34 });
   b.idleAt(enc.now() + 2 * b.ticksPerBit);
   CHECK_FALSE(b.decoder.takeError());
}

TEST_CASE("Manchester decoder: a long gap between frames, also longer than the timer period") {
   Bench b(2'000'000);
   Encoder enc(b.ticksPerBit, 10);
   enc.frame({ 0x11 });
   const double end = enc.now();
   enc.gap(5000); // more than 65536 ticks
   enc.frame({ 0x22 });
   // The main loop calls idle() in the gap
   const std::vector<Edge> all = enc.edges;
   std::vector<Edge> first;
   std::vector<Edge> second;
   for(const Edge &e : all) {
      (e.time < end + 100 ? first : second).push_back(e);
   }
   b.feed(first);
   b.idleAt(end + 3 * b.ticksPerBit);
   b.feed(second);
   CHECK(b.out == Bytes{ 0x11, 0x22 });
   CHECK_FALSE(b.decoder.takeError());
}

TEST_CASE("Manchester decoder: a target bit rate 12 % away, with jitter, decodes") {
   for(const double factor : { 0.88, 1.12 }) {
      Bench b(1'000'000);
      Encoder enc(b.ticksPerBit * factor, 0);
      // +-1 tick of jitter on each edge
      enc.frame(pattern, [](std::size_t k) { return (k % 3 == 0) ? 1.0 : (k % 3 == 1 ? -1.0 : 0.0); });
      b.feed(enc.edges);
      CHECK(b.out == pattern);
      b.idleAt(enc.now() + 3 * b.ticksPerBit * factor);
      CHECK_FALSE(b.decoder.takeError());
   }
}

TEST_CASE("Manchester decoder: many random frames, gaps and jitter across many timer wraps") {
   for(const uint32_t baud : { 9'600U, 1'000'000U, 2'000'000U }) {
      Bench b(baud);
      Encoder enc(b.ticksPerBit * 1.05, 0);
      uint32_t seed = 12345;
      auto next = [&seed] {
         seed = seed * 1'103'515'245U + 12'345U;
         return seed >> 16;
      };
      Bytes sent;
      for(int frame = 0; frame < 300; ++frame) {
         Bytes bytes(1 + next() % 8);
         for(uint8_t &byte : bytes) {
            byte = static_cast<uint8_t>(next());
         }
         sent.insert(sent.end(), bytes.begin(), bytes.end());
         enc.frame(bytes, [&next](std::size_t) { return static_cast<double>(next() % 3) - 1.0; });
         // 1 to 64 bit times: the gaps stay shorter than the timer period
         enc.gap(1 + next() % 64);
      }
      REQUIRE(enc.edges.back().time > 5 * 65'536U);
      b.feed(enc.edges);
      b.idleAt(enc.now());
      CHECK(b.out == sent);
      CHECK_FALSE(b.decoder.takeError());
   }
}

TEST_CASE("Manchester decoder: a bit rate 40 % too low gives a framing error") {
   Bench b(1'000'000);
   Encoder enc(b.ticksPerBit * 1.4, 0);
   enc.frame(pattern);
   b.feed(enc.edges);
   CHECK(b.out != pattern);
   CHECK(b.decoder.takeError());
   CHECK_FALSE(b.decoder.takeError());
}

TEST_CASE("Manchester decoder: a frame that does not end at a byte boundary gives a framing error") {
   Bench b(1'000'000);
   Encoder enc(b.ticksPerBit, 0);
   enc.frame({ 0x5A }, [](std::size_t) { return 0.0; }, 3, true);
   b.feed(enc.edges);
   CHECK(b.out == Bytes{ 0x5A });
   CHECK_FALSE(b.decoder.takeError());
   // The frame ends at idle()
   b.idleAt(enc.now() + 2 * b.ticksPerBit);
   CHECK(b.decoder.takeError());
}

TEST_CASE("Manchester decoder: after a glitch, the decoder waits for a quiet line") {
   Bench b(1'000'000);
   Encoder enc(b.ticksPerBit, 0);
   enc.frame({ 0xA5, 0xA5 });
   // A glitch: an extra pair of edges in the middle of the frame
   std::vector<Edge> edges = enc.edges;
   const uint32_t at = edges[6].time + 3;
   edges.insert(edges.begin() + 7, { { at, !edges[6].rising }, { at + 1, edges[6].rising } });
   b.feed(edges);
   CHECK(b.decoder.takeError());
   b.idleAt(enc.now() + 2 * b.ticksPerBit);

   Encoder next(b.ticksPerBit, static_cast<uint32_t>(enc.now() + 3 * b.ticksPerBit));
   next.frame({ 0x42 });
   b.out.clear();
   b.feed(next.edges);
   CHECK(b.out == Bytes{ 0x42 });
   CHECK_FALSE(b.decoder.takeError());
}

TEST_CASE("Manchester decoder: a line that stays high gives a framing error at idle()") {
   Bench b(1'000'000);
   b.feed({ { 100, true } });
   b.idleAt(100 + 0.5 * b.ticksPerBit);
   CHECK_FALSE(b.decoder.takeError());
   b.idleAt(100 + 2 * b.ticksPerBit);
   CHECK(b.decoder.takeError());
   // Two rising edges in a row: an edge is lost
   b.feed({ { 200, false }, { 300, true }, { 310, true } });
   CHECK(b.decoder.takeError());
}

TEST_CASE("Manchester decoder: a capture that starts with a high line waits for a quiet line") {
   swo::ManchesterDecoder decoder;
   const auto timing = swo::manchesterTiming(clock, 1'000'000);
   REQUIRE(timing.has_value());
   decoder.configure(*timing);
   decoder.reset(true, 0);
   Bytes out;
   auto sink = [&out](uint8_t byte) { out.push_back(byte); };
   const double ticksPerBit = 144.0 / (timing->prescaler + 1);
   // The end of a frame: the line falls, a short low time, then a frame
   decoder.edge(10, false, sink);
   Encoder enc(ticksPerBit, static_cast<uint32_t>(10 + 0.5 * ticksPerBit));
   enc.frame({ 0x99 });
   for(const Edge &e : enc.edges) {
      decoder.edge(static_cast<uint16_t>(e.time), e.rising, sink);
   }
   CHECK(out.empty());
   // After a quiet low line, the next frame decodes
   Encoder next(ticksPerBit, static_cast<uint32_t>(enc.now() + 2 * ticksPerBit));
   next.frame({ 0x66 });
   for(const Edge &e : next.edges) {
      decoder.edge(static_cast<uint16_t>(e.time), e.rising, sink);
   }
   CHECK(out == Bytes{ 0x66 });
}

namespace {

using Edges = swo::ManchesterEdges<8>;

// The two DMA channels of the test: they write the edges into the rings
struct FakeCapture {
   Edges &edges;
   uint32_t rising = 0;
   uint32_t falling = 0;

   void write(const std::vector<Edge> &list) {
      for(const Edge &e : list) {
         if(e.rising) {
            edges.rising()[rising++ % Edges::size] = static_cast<uint16_t>(e.time);
         } else {
            edges.falling()[falling++ % Edges::size] = static_cast<uint16_t>(e.time);
         }
      }
   }
};

struct Recorder {
   std::vector<Edge> got;
   void edge(uint16_t time, bool rising, void (*)(uint8_t)) {
      got.push_back({ time, rising });
   }
};

void noSink(uint8_t) {
}

} // namespace

TEST_CASE("Manchester edges: merge gives the two streams in their order") {
   Edges edges;
   edges.reset(false);
   FakeCapture dma{ edges };
   Recorder rec;
   dma.write({ { 1, true }, { 2, false }, { 3, true } });
   CHECK(edges.merge(dma.rising, dma.falling, rec, noSink));
   REQUIRE(rec.got.size() == 3);
   CHECK(rec.got[0].time == 1);
   CHECK(rec.got[1].time == 2);
   CHECK(rec.got[2].time == 3);
   CHECK(rec.got[2].rising);
   CHECK(edges.lineHigh());
   // The next falling edge is not there yet: a rising edge in its stream
   // waits (it cannot come before it).
   dma.write({ { 4, false }, { 5, true } });
   CHECK(edges.merge(dma.rising, dma.falling - 1, rec, noSink));
   CHECK(rec.got.size() == 3);
   CHECK(edges.merge(dma.rising, dma.falling, rec, noSink));
   REQUIRE(rec.got.size() == 5);
   CHECK(rec.got[3].time == 4);
   CHECK(rec.got[4].time == 5);
   CHECK_FALSE(edges.lapped(dma.rising, dma.falling));
}

TEST_CASE("Manchester edges: a capture that starts high takes a falling edge first") {
   Edges edges;
   edges.reset(true);
   FakeCapture dma{ edges };
   Recorder rec;
   dma.write({ { 7, false }, { 9, true } });
   CHECK(edges.merge(dma.rising, dma.falling, rec, noSink));
   REQUIRE(rec.got.size() == 2);
   CHECK_FALSE(rec.got[0].rising);
   CHECK(rec.got[1].rising);
}

TEST_CASE("Manchester edges: an overflow drops the edges, and the order stays") {
   Edges edges;
   edges.reset(false);
   FakeCapture dma{ edges };
   Recorder rec;
   std::vector<Edge> many;
   for(uint32_t i = 0; i < 19; ++i) {
      many.push_back({ i, i % 2 == 0 });
   }
   dma.write(many); // 10 rising, 9 falling: more than 8
   CHECK_FALSE(edges.merge(dma.rising, dma.falling, rec, noSink));
   CHECK(rec.got.empty());
   // After 10 rising and 9 falling edges, the line is high
   CHECK(edges.lineHigh());
   dma.write({ { 30, false }, { 31, true } });
   CHECK(edges.merge(dma.rising, dma.falling, rec, noSink));
   REQUIRE(rec.got.size() == 2);
   CHECK(rec.got[0].time == 30);
   CHECK(rec.got[1].time == 31);
}

TEST_CASE("Manchester edges: drop with a falling count one ahead skips the rising edge before it") {
   Edges edges;
   edges.reset(false);
   FakeCapture dma{ edges };
   Recorder rec;
   dma.write({ { 1, true }, { 2, false } });
   // The rising count was read before the first rising edge came
   edges.drop(0, dma.falling);
   CHECK_FALSE(edges.lineHigh());
   dma.write({ { 3, true }, { 4, false } });
   CHECK(edges.merge(dma.rising, dma.falling, rec, noSink));
   REQUIRE(rec.got.size() == 2);
   CHECK(rec.got[0].time == 3);
   CHECK(rec.got[1].time == 4);
}

TEST_CASE("Manchester edges: realign corrects the order after an edge at the start of the capture") {
   Edges edges;
   // The line was low at the read, but went high before the capture started.
   edges.reset(false);
   FakeCapture dma{ edges };
   Recorder rec;
   dma.write({ { 5, false }, { 9, true }, { 12, false } });
   // The merge waits for a rising edge first: wrong order
   CHECK(edges.merge(dma.rising, dma.falling, rec, noSink));
   REQUIRE(rec.got.size() == 2);
   CHECK(rec.got[0].time == 9);
   CHECK(rec.got[1].time == 5);
   // The edge at 12 waits for a rising edge before it, which never comes.
   CHECK(edges.waiting(dma.rising, dma.falling));
   // The line is low now: realign drops the edge at 12, and the order is right.
   edges.realign(dma.rising, dma.falling, false);
   CHECK_FALSE(edges.lineHigh());
   rec.got.clear();
   dma.write({ { 20, true }, { 25, false } });
   CHECK(edges.merge(dma.rising, dma.falling, rec, noSink));
   REQUIRE(rec.got.size() == 2);
   CHECK(rec.got[0].time == 20);
   CHECK(rec.got[1].time == 25);
   CHECK_FALSE(edges.waiting(dma.rising, dma.falling));
}

TEST_CASE("Manchester edges: lapped finds a DMA write over the edges of the last merge") {
   Edges edges;
   edges.reset(false);
   FakeCapture dma{ edges };
   Recorder rec;
   dma.write({ { 1, true }, { 2, false }, { 3, true }, { 4, false } });
   CHECK(edges.merge(dma.rising, dma.falling, rec, noSink));
   CHECK_FALSE(edges.lapped(dma.rising, dma.falling));
   // 8 more pairs during the merge: the first edges were written again
   std::vector<Edge> more;
   for(uint32_t i = 0; i < 16; ++i) {
      more.push_back({ 10 + i, i % 2 == 0 });
   }
   dma.write(more);
   CHECK(edges.lapped(dma.rising, dma.falling));
}
