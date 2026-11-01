#pragma once

// Mock pins for the JTAG bit-bang engine (jtag/JtagBitBang.h), with a
// simple target: a chain of flip-flops from TDI to TDO. The chain samples
// TDI on the rising edge of TCK and changes TDO on the falling edge, as a
// JTAG target does. Thus TDO is TDI delayed by the chain length.
//
// A JtagTarget replaces the chain: it sees each TCK edge and gives TDO
// (MockJtagDp.h has a JTAG-DP).

#include <cstdint>
#include <deque>
#include <vector>

namespace mupptest {

class JtagTarget {
   public:
   // A rising edge of TCK, with the TMS and TDI levels
   virtual void rising(bool tms, bool tdi) = 0;
   // A falling edge of TCK. Returns the new TDO level.
   virtual bool falling() = 0;

   protected:
   ~JtagTarget() = default;
};

template <bool Rtck = true>
class MockJtagPins {
   public:
   static constexpr bool hasRtck = Rtck;

   // The levels of TMS and TDI at a rising edge of TCK, its time, and the
   // time of the last TMS or TDI write before it.
   struct RisingEdge {
      uint32_t time;
      bool tms;
      bool tdi;
      uint32_t dataWritten;
   };

   explicit MockJtagPins(std::deque<bool> chain = { false })
      : chain_(std::move(chain))
      , tdo_(chain_.back()) {
   }

   explicit MockJtagPins(JtagTarget &target)
      : target_(&target)
      , tdo_(false) {
   }

   // Sets the TCK level without an edge: the tests count only the edges
   // of the shifts.
   void init(bool tckLevel) {
      ++inits;
      tck_ = tckLevel;
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
      time += edgeDelayCycles;
      tckChanged_ = time;
      edges.push_back(time);
      if(level) {
         rising.push_back({ time, tms_, tdi_, dataWritten_ });
         if(target_ != nullptr) {
            target_->rising(tms_, tdi_);
         } else {
            chain_.push_front(tdi_);
            chain_.pop_back();
         }
      } else {
         tdo_ = target_ != nullptr ? target_->falling() : chain_.back();
      }
   }
   void tms(bool level) {
      tms_ = level;
      dataWritten_ = time;
   }
   void tdi(bool level) {
      tdi_ = level;
      dataWritten_ = time;
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
   // The engine code between the end of a wait and the edge (the TDO read
   // and the pin write on the probe): time added before each TCK edge
   uint32_t edgeDelayCycles{ 0 };
   bool rtckStuck{ false };
   std::vector<uint32_t> edges;
   std::vector<RisingEdge> rising;
   uint32_t nowCalls{ 0 };
   int inits{ 0 };
   int finis{ 0 };
   int begins{ 0 };

   private:
   JtagTarget *target_{ nullptr };
   std::deque<bool> chain_;
   bool tck_{ false };
   bool tms_{ false };
   bool tdi_{ false };
   bool tdo_;
   uint32_t tckChanged_{ 0 };
   uint32_t dataWritten_{ 0 };
};

} // namespace mupptest
