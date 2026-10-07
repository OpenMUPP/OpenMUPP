#pragma once

// JTAG bit-bang engine: shifts the TDI and TMS streams of the JTAG_RAW
// block (protocol/JtagRawBlock.h) out on GPIO pins and reads TDO.
// Hardware-independent: a Pins type gives the pin access, so the host unit
// tests run the bit logic on mock pins. On target, board/BoardJtag.h gives
// FastPin access (one store or load for each pin access).
//
// The Pins type has these members:
//
//   static constexpr bool hasRtck;  the board has RTCK
//   void init();                    TCK, TMS, TDI outputs (TCK low), TDO
//                                   and RTCK inputs
//   void fini();                    TCK, TMS, TDI inputs
//   void begin();                   before each shift: make now() count
//   void tck(bool), tms(bool), tdi(bool);
//   bool tdo(), rtck();
//   uint32_t now();                 free-running core cycle counter
//   uint32_t coreHz();              core clock frequency
//
// One bit, with TCK low at the start:
//
//   1. Write TDI and TMS. Wait for the low half period.
//   2. Read TDO. The target changed TDO at the last falling edge.
//   3. TCK high: the target samples TDI and TMS. Wait for the high half
//      period.
//   4. TCK low.
//
// Clocking modes:
//
// - Timed: each half period waits for halfCycles core cycles from its TCK
//   edge. An interrupt makes a half period longer, never shorter. When
//   halfCycles is shorter than the loop, TCK runs at the fastest rate of
//   the loop. There is no loop without waits: its TCK high time would be
//   only a few cycles, because the compiler puts the bit bookkeeping into
//   the low half period.
// - Adaptive: after each TCK edge, wait until RTCK has the same level. When
//   RTCK does not follow within rtckTimeoutMs, the shift stops with TCK low
//   and returns false.

#include <algorithm>
#include <cstdint>

#include "protocol/JtagRawBlock.h"

namespace jtag {

// The longest time that RTCK can take to follow a TCK edge
inline constexpr uint32_t rtckTimeoutMs = 10;

// Core cycles of a half TCK period, rounded up: TCK is never faster than
// kHz. kHz is not 0.
constexpr uint32_t halfPeriodCycles(uint32_t coreHz, uint32_t kHz) {
   const uint64_t cyclesPerPeriod = 2ull * kHz * 1000u;
   return static_cast<uint32_t>((coreHz + cyclesPerPeriod - 1) / cyclesPerPeriod);
}

enum class Clocking : uint8_t { Timed, Adaptive };

struct Timing {
   Clocking clocking;
   // Timed: core cycles of a half TCK period
   uint32_t halfCycles;
   // Adaptive: core cycles that RTCK can take to follow TCK
   uint32_t timeoutCycles;

   constexpr bool operator==(const Timing &) const = default;
};

namespace detail {

template <typename Pins>
[[gnu::always_inline]] inline void waitFrom(Pins &pins, uint32_t start, uint32_t cycles) {
   // Unsigned difference: correct across the wrap of the counter.
   while(pins.now() - start < cycles) {
   }
}

template <typename Pins>
[[gnu::always_inline]] inline bool waitRtck(Pins &pins, bool level, uint32_t timeoutCycles) {
   const uint32_t start = pins.now();
   while(pins.rtck() != level) {
      if(pins.now() - start >= timeoutCycles) {
         return false;
      }
   }
   return true;
}

template <Clocking C, typename Pins>
bool shiftBits(Pins &pins, const Timing &timing, uint32_t bitCount, const uint8_t *tdi, const uint8_t *tms, uint8_t *tdo) {
   // TCK is low since the end of the last shift.
   uint32_t edge = 0;
   if constexpr(C == Clocking::Timed) {
      edge = pins.now();
   }
   for(uint32_t done = 0; done < bitCount; done += 8) {
      const uint32_t bits = std::min<uint32_t>(8, bitCount - done);
      uint32_t tdiByte = *tdi++;
      uint32_t tmsByte = *tms++;
      uint32_t tdoByte = 0;
      for(uint32_t bit = 0; bit < bits; ++bit) {
         pins.tdi((tdiByte & 1u) != 0);
         pins.tms((tmsByte & 1u) != 0);
         tdiByte >>= 1;
         tmsByte >>= 1;
         if constexpr(C == Clocking::Timed) {
            waitFrom(pins, edge, timing.halfCycles);
         }
         tdoByte |= static_cast<uint32_t>(pins.tdo()) << bit;
         pins.tck(true);
         if constexpr(C == Clocking::Timed) {
            edge = pins.now();
            waitFrom(pins, edge, timing.halfCycles);
         } else if constexpr(C == Clocking::Adaptive) {
            if(!waitRtck(pins, true, timing.timeoutCycles)) {
               pins.tck(false);
               return false;
            }
         }
         pins.tck(false);
         if constexpr(C == Clocking::Timed) {
            edge = pins.now();
         } else if constexpr(C == Clocking::Adaptive) {
            if(!waitRtck(pins, false, timing.timeoutCycles)) {
               return false;
            }
         }
      }
      *tdo++ = static_cast<uint8_t>(tdoByte);
   }
   return true;
}

} // namespace detail

// The JTAG port of the JTAG_RAW block on bit-banged pins.
template <typename Pins>
class BitBangPort final : public protocol::JtagPort {
   public:
   explicit BitBangPort(Pins &pins)
      : pins_(pins) {
   }

   void init() override {
      pins_.init();
   }

   void fini() override {
      pins_.fini();
   }

   [[nodiscard]] bool setSpeed(uint32_t kHz) override {
      if(kHz == 0) {
         if(!Pins::hasRtck) {
            return false;
         }
         timing_ = { Clocking::Adaptive, 0, pins_.coreHz() / 1000u * rtckTimeoutMs };
         return true;
      }
      timing_ = { Clocking::Timed, halfPeriodCycles(pins_.coreHz(), kHz), 0 };
      return true;
   }

   [[nodiscard]] bool
   shift(uint32_t bitCount, art::span<const uint8_t> tdi, art::span<const uint8_t> tms, art::span<uint8_t> tdo) override {
      pins_.begin();
      switch(timing_.clocking) {
      case Clocking::Timed:
         return detail::shiftBits<Clocking::Timed>(pins_, timing_, bitCount, tdi.data(), tms.data(), tdo.data());
      case Clocking::Adaptive:
         return detail::shiftBits<Clocking::Adaptive>(pins_, timing_, bitCount, tdi.data(), tms.data(), tdo.data());
      }
      return false;
   }

   [[nodiscard]] const Timing &timing() const {
      return timing_;
   }

   private:
   Pins &pins_;
   // INIT of the JTAG_RAW block sets the speed. Until then: the fastest
   // rate.
   Timing timing_{ Clocking::Timed, 0, 0 };
};

} // namespace jtag
