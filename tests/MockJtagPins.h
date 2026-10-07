#pragma once

// Mock pins for the JTAG bit-bang engine (jtag/JtagBitBang.h), with a
// simple target: a chain of flip-flops from TDI to TDO. The chain samples
// TDI on the rising edge of TCK and changes TDO on the falling edge, as a
// JTAG target does. Thus TDO is TDI delayed by the chain length.

#include <cstdint>
#include <deque>
#include <vector>

namespace vsltest {

template <bool Rtck = true>
class MockJtagPins {
   public:
   static constexpr bool hasRtck = Rtck;

   // The levels of TMS and TDI at a rising edge of TCK, and its time.
   struct RisingEdge {
      uint32_t time;
      bool tms;
      bool tdi;
   };

   explicit MockJtagPins(std::deque<bool> chain = { false })
      : chain_(std::move(chain))
      , tdo_(chain_.back()) {
   }

   void init() {
      ++inits;
   }
   void fini() {
      ++finis;
   }
   void begin() {
      ++begins;
   }

   void tck(bool level) {
      if(level == tck_) {
         return;
      }
      tck_ = level;
      tckChanged_ = time;
      edges.push_back(time);
      if(level) {
         rising.push_back({ time, tms_, tdi_ });
         chain_.push_front(tdi_);
         chain_.pop_back();
      } else {
         tdo_ = chain_.back();
      }
   }
   void tms(bool level) {
      tms_ = level;
   }
   void tdi(bool level) {
      tdi_ = level;
   }
   [[nodiscard]] bool tdo() const {
      return tdo_;
   }
   // RTCK follows TCK rtckLag time steps after each edge, or stays low.
   [[nodiscard]] bool rtck() const {
      if(rtckStuck) {
         return false;
      }
      return time - tckChanged_ >= rtckLag ? tck_ : !tck_;
   }

   // Each call is one time step.
   uint32_t now() {
      ++nowCalls;
      return time++;
   }
   [[nodiscard]] uint32_t coreHz() const {
      return 72'000'000;
   }

   [[nodiscard]] bool tckLevel() const {
      return tck_;
   }

   // Starts near the wrap of the counter.
   uint32_t time{ 0xFFFFFF00u };
   uint32_t rtckLag{ 3 };
   bool rtckStuck{ false };
   std::vector<uint32_t> edges;
   std::vector<RisingEdge> rising;
   uint32_t nowCalls{ 0 };
   int inits{ 0 };
   int finis{ 0 };
   int begins{ 0 };

   private:
   std::deque<bool> chain_;
   bool tck_{ false };
   bool tms_{ false };
   bool tdi_{ false };
   bool tdo_;
   uint32_t tckChanged_{ 0 };
};

} // namespace vsltest
