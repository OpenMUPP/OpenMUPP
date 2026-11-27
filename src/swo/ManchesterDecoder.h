// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The software decoder of the SWO Manchester mode (swo/SwoReceiver.h). No
// CMSIS dependency, so the host unit tests can check it.
//
// The TPIU Manchester format (Arm CoreSight TPIU, "Serial wire output"):
//
// - The line is low when idle.
// - Each bit has two halves. A 1 is high, then low. A 0 is low, then high.
//   So each bit has an edge at its middle: a falling edge for a 1, a
//   rising edge for a 0. Two equal bits also have an edge at the bit
//   boundary.
// - A frame starts with a start bit (a 1), then one or more bytes, least
//   significant bit first. After the frame, the line stays low for at
//   least one bit time.
//
// The input is the time of each edge, from a 16-bit timer that counts up
// and wraps (TIM5 on the CH32V307), and the direction of the edge. The
// decoder uses only the 16-bit difference of two times, so the wrap of
// the timer has no effect inside a frame. A frame ends when the line stays
// at a rising edge that is too late for a bit middle, or at idle() when the
// line stays low for more than 1.5 bit times.
//
// Limit: a gap between two edges of more than one timer period (65536
// ticks) looks like the gap modulo 65536. idle() must thus see the end of
// a frame within one timer period. The timer setup makes the period at
// least 2048 bit times (manchesterTiming()), and the main loop calls idle()
// at least each millisecond.
//
// Tolerance: an edge in the middle of a bit is valid from 0.75 to 1.25 bit
// times after the previous middle. So the bit time of the target can be
// from about 15 % shorter to 25 % longer than the bit time of
// DAP_SWO_Baudrate, less the jitter. Shorter is the smaller limit: a start
// bit after the shortest gap (1.5 bit times after the last middle) must
// come after the window of a middle (1.25 nominal bit times).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace swo {

// The timer setup and the limits of the decoder, in timer ticks
struct ManchesterTiming {
   // The timer prescaler: the counter counts at clock / (prescaler + 1).
   uint16_t prescaler;
   // An edge that comes earlier than this after the previous one is a
   // glitch (0.25 bit times).
   uint16_t minHalf;
   // The range of a bit middle after the previous middle (0.75 and 1.25
   // bit times)
   uint16_t minBit;
   uint16_t maxBit;
   // A low line for longer than this ends the frame (1.5 bit times).
   uint16_t frameEnd;
};

// The most ticks per bit. Less ticks per bit give a longer timer period in
// bit times: 32 ticks give at least 2048 bit times.
inline constexpr uint32_t maxTicksPerBit = 32;

// The timer setup for a bit rate, at the timer clock `clock`. The smallest
// prescaler that gives at most maxTicksPerBit ticks per bit. Empty when
// the bit rate is 0, or too low for the 16-bit prescaler, or higher than
// clock / 16 (less than 16 ticks per bit).
constexpr std::optional<ManchesterTiming> manchesterTiming(uint32_t clock, uint32_t baud) {
   if(baud == 0 || baud > clock / 16) {
      return std::nullopt;
   }
   const uint64_t perBit = uint64_t{ maxTicksPerBit } * baud;
   const uint64_t divider = (clock + perBit - 1) / perBit;
   if(divider > 0x10000) {
      return std::nullopt;
   }
   // Ticks per bit, in quarters, rounded: 4 * clock / (divider * baud)
   const uint64_t quarterDivisor = divider * baud;
   const auto quarters = [&](uint64_t count) {
      return static_cast<uint16_t>((count * clock + quarterDivisor * 2) / (quarterDivisor * 4));
   };
   return ManchesterTiming{ static_cast<uint16_t>(divider - 1), quarters(1), quarters(3), quarters(5), quarters(6) };
}

// The baud rate of DAP_SWO_Baudrate in Manchester mode is the TPIU clock,
// as OpenOCD sends it: one clock is one half of a bit. Gives the real baud
// rate for a request: even, at most `max`. The bit rate is half of it.
constexpr uint32_t manchesterBaud(uint32_t baud, uint32_t max) {
   return std::min(baud, max) / 2 * 2;
}

class ManchesterDecoder {
   public:
   void configure(const ManchesterTiming &timing) {
      timing_ = timing;
   }

   // Starts again with no frame. high: the line level now. now: the timer
   // count now.
   void reset(bool high, uint16_t now) {
      state_ = high ? State::sync : State::idle;
      high_ = high;
      lastEdge_ = now;
      error_ = false;
   }

   // One edge at the timer count `time`. rising: the line went high. The
   // edges must come in their order. Calls sink(byte) for each decoded
   // byte.
   template <typename Sink>
   void edge(uint16_t time, bool rising, Sink &&sink) {
      if(rising == high_) {
         // A lost edge: start again at the next quiet line
         fail();
      } else {
         switch(state_) {
         case State::idle:
            startBit(time);
            break;
         case State::sync:
            if(rising && since(time, lastEdge_) > timing_.frameEnd) {
               startBit(time);
            }
            break;
         case State::start:
            // The falling edge in the middle of the start bit
            if(inRange(since(time, mark_), timing_.minHalf, timing_.minBit)) {
               mark_ = time;
               bits_ = 0;
               boundary_ = false;
               state_ = State::data;
            } else {
               fail();
            }
            break;
         case State::data:
            dataEdge(time, rising, sink);
            break;
         }
      }
      high_ = rising;
      lastEdge_ = time;
   }

   // No edge up to the timer count `now`. Ends a frame on a quiet low line,
   // and drops a frame on a line that stays high.
   void idle(uint16_t now) {
      const uint16_t quiet = since(now, lastEdge_);
      if(!high_) {
         if(quiet > timing_.frameEnd) {
            if(state_ == State::data) {
               endFrame();
            }
            state_ = State::idle;
         }
      } else if(state_ == State::start || state_ == State::data) {
         // A start bit or a 0 holds the line high for at most 1 bit time.
         if(quiet > timing_.maxBit) {
            fail();
         }
      }
   }

   // True after a framing error since the last call: a bit time out of the
   // range, or a frame that does not end at a byte boundary.
   [[nodiscard]] bool takeError() {
      const bool error = error_;
      error_ = false;
      return error;
   }

   private:
   enum class State : uint8_t {
      // Waits for a start bit
      idle,
      // After an error: waits for a start bit after a quiet low line
      sync,
      // After the rising edge of the start bit
      start,
      // In the bytes of a frame. mark_ is the time of the last bit middle.
      data,
   };

   static constexpr uint16_t since(uint16_t time, uint16_t from) {
      return static_cast<uint16_t>(time - from);
   }
   static constexpr bool inRange(uint16_t value, uint16_t low, uint16_t high) {
      return value >= low && value < high;
   }

   void startBit(uint16_t time) {
      mark_ = time;
      state_ = State::start;
   }

   void endFrame() {
      if(bits_ != 0) {
         error_ = true;
      }
      state_ = State::idle;
   }

   void fail() {
      error_ = true;
      state_ = State::sync;
   }

   template <typename Sink>
   void dataEdge(uint16_t time, bool rising, Sink &&sink) {
      const uint16_t sinceMiddle = since(time, mark_);
      if(rising && sinceMiddle > timing_.maxBit) {
         // Too late for a bit middle: the frame ended, and this edge
         // starts the next one. After a last 0, the line is low for only
         // one bit time between two frames.
         endFrame();
         startBit(time);
      } else if(sinceMiddle < timing_.minHalf) {
         fail();
      } else if(sinceMiddle < timing_.minBit) {
         // A bit boundary: one at most between two bit middles
         if(boundary_) {
            fail();
         }
         boundary_ = true;
      } else if(sinceMiddle <= timing_.maxBit) {
         // A bit middle: a falling edge is a 1, LSB first
         byte_ = static_cast<uint8_t>((byte_ >> 1) | (rising ? 0 : 0x80));
         if(++bits_ == 8) {
            sink(byte_);
            bits_ = 0;
         }
         mark_ = time;
         boundary_ = false;
      } else {
         fail();
      }
   }

   ManchesterTiming timing_{};
   State state_ = State::idle;
   bool high_ = false;
   bool boundary_ = false;
   bool error_ = false;
   uint8_t byte_ = 0;
   uint8_t bits_ = 0;
   uint16_t mark_ = 0;
   uint16_t lastEdge_ = 0;
};

// The two edge streams of the capture: the timer captures the rising edges
// on one channel and the falling edges on another, each with its own DMA
// into a ring of Size entries. Both channels capture the same filtered
// input, so the edges alternate between the two streams. merge() gives the
// edges to the decoder in their order. The caller gives the write counts
// of the two DMA channels since reset() (SwoRing::writeCount()).
template <std::size_t Size>
class ManchesterEdges {
   static_assert(Size >= 2 && (Size & (Size - 1)) == 0, "the counts wrap at 2^32: the size must be a power of 2");

   public:
   static constexpr std::size_t size = Size;

   // The DMA targets
   [[nodiscard]] uint16_t *rising() {
      return rising_.data();
   }
   [[nodiscard]] uint16_t *falling() {
      return falling_.data();
   }

   // Starts with empty rings. high: the line level at the start of the
   // capture. Then the first edge is a falling edge.
   void reset(bool high) {
      risingRead_ = 0;
      fallingRead_ = 0;
      fallingFirst_ = high ? 1 : 0;
   }

   // Gives the new edges to `decoder`, in their order, and returns true.
   // False: a DMA wrote over edges that are not read. merge() then drops
   // all edges, and the caller must reset the decoder.
   template <typename Decoder, typename Sink>
   bool merge(uint32_t risingWritten, uint32_t fallingWritten, Decoder &decoder, Sink &&sink) {
      risingStart_ = risingRead_;
      fallingStart_ = fallingRead_;
      if(risingWritten - risingRead_ > Size || fallingWritten - fallingRead_ > Size) {
         drop(risingWritten, fallingWritten);
         return false;
      }
      for(;;) {
         if(nextIsRising()) {
            if(risingRead_ == risingWritten) {
               return true;
            }
            decoder.edge(rising_[risingRead_++ % Size], true, sink);
         } else {
            if(fallingRead_ == fallingWritten) {
               return true;
            }
            decoder.edge(falling_[fallingRead_++ % Size], false, sink);
         }
      }
   }

   // True when the DMA wrote over edges during the last merge(): written
   // are the counts after the merge.
   [[nodiscard]] bool lapped(uint32_t risingWritten, uint32_t fallingWritten) const {
      return risingWritten - risingStart_ > Size || fallingWritten - fallingStart_ > Size;
   }

   // Drops all edges up to the write counts. The two counts can come from
   // two moments: then one stream has an edge too many, and that edge is
   // dropped too.
   void drop(uint32_t risingWritten, uint32_t fallingWritten) {
      risingRead_ = risingWritten;
      fallingRead_ = fallingWritten;
      const uint32_t lead = fallingRead_ - risingRead_ - fallingFirst_;
      if(lead == 1) {
         ++risingRead_;
      } else if(lead != 0 && lead != 0xFFFF'FFFFU) {
         // Counts that do not fit: start the two streams at the same edge
         // pair.
         fallingRead_ = risingRead_ + fallingFirst_;
      }
   }

   // Drops all edges up to the write counts, and sets the line level after
   // them. Use it when the level that the counts give is wrong: an edge
   // came while the capture started, and the streams have the wrong order.
   void realign(uint32_t risingWritten, uint32_t fallingWritten, bool high) {
      risingRead_ = risingWritten;
      fallingRead_ = fallingWritten;
      fallingFirst_ = fallingRead_ - risingRead_ + (high ? 1 : 0);
   }

   // True when merge() left edges that are not read: it waits for an edge
   // of the other stream. That is normal for an edge that just came, and
   // an error when no new edge comes.
   [[nodiscard]] bool waiting(uint32_t risingWritten, uint32_t fallingWritten) const {
      return risingWritten != risingRead_ || fallingWritten != fallingRead_;
   }

   // The line level after the edges that merge() gave to the decoder
   [[nodiscard]] bool lineHigh() const {
      return !nextIsRising();
   }

   private:
   // The falling edges since reset(), less the rising edges: fallingFirst_
   // before a rising edge, fallingFirst_ - 1 before a falling edge. Only
   // the difference counts: realign() can give any value.
   [[nodiscard]] bool nextIsRising() const {
      return fallingRead_ - risingRead_ == fallingFirst_;
   }

   std::array<uint16_t, Size> rising_{};
   std::array<uint16_t, Size> falling_{};
   uint32_t risingRead_ = 0;
   uint32_t fallingRead_ = 0;
   uint32_t risingStart_ = 0;
   uint32_t fallingStart_ = 0;
   uint32_t fallingFirst_ = 0;
};

} // namespace swo
