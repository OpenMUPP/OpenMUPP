#pragma once

// JTAG bit-bang engine: shifts the TDI and TMS streams of the JTAG_RAW
// block (protocol/versaloon/JtagRawBlock.h) out on GPIO pins and reads TDO.
// Hardware-independent: a Pins type gives the pin access, so the host unit
// tests run the bit logic on mock pins. On target, board/BoardJtag.h gives
// FastPin access (one store or load for each pin access).
//
// The Pins type has these members:
//
//   static constexpr bool hasRtck;  the board has RTCK
//   void init(bool tck);            TCK, TMS, TDI outputs (TCK at the
//                                   given level), TDO and RTCK inputs
//   void fini();                    TCK, TMS, TDI inputs
//   void begin();                   before each shift: make now() count
//   void tck(bool), tms(bool), tdi(bool);
//   bool tdo(), rtck();
//   uint32_t now();                 free-running core cycle counter
//   uint32_t coreHz();              core clock frequency
//
// With a protocol::Background, a shift calls Background::service() between
// bytes, at most once per backgroundIntervalMs (protocol/Background.h).
// TCK is then at its idle level: the call makes that half period longer.
// In timed mode, the half period starts again after the call, so the TDI
// and TMS writes of the next bit come a full half period before the rising
// edge.
//
// TCK idles low between shifts, or high (TckIdle::High). Only the rising
// edge clocks the TAP, so the idle level has no effect on the target. The
// SPI fast path (jtag/JtagFastPath.h) idles TCK high: its SPI clock idles
// high, and a change from low to high would be an extra rising edge.
//
// One bit, with TCK low at the start:
//
//   1. Write TDI and TMS. Wait for the low half period.
//   2. Read TDO. The target changed TDO at the last falling edge.
//   3. TCK high: the target samples TDI and TMS. Wait for the high half
//      period.
//   4. TCK low.
//
// With TCK high at the start, step 4 comes first: TCK low, then steps 1-3.
// The high half period of a bit then ends with the falling edge of the
// next bit.
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
//   and returns false. With TckIdle::High, TCK then stays low: TCK high
//   would be an extra rising edge.

#include <algorithm>
#include <cstdint>

#include "IntDiv.h"

#include "protocol/Background.h"
#include "protocol/JtagPort.h"

namespace jtag {

// The longest time that RTCK can take to follow a TCK edge
inline constexpr uint32_t rtckTimeoutMs = 10;

// Core cycles of a half TCK period, rounded up: TCK is never faster than
// kHz. kHz is not 0. coreHz / (2000 * kHz) rounded up, in two 32-bit
// steps: ceil(ceil(a / b) / c) is ceil(a / (b * c)).
constexpr uint32_t halfPeriodCycles(uint32_t coreHz, uint32_t kHz) {
   return intdiv::ceilDiv(intdiv::ceilDiv(coreHz, 2000u), kHz);
}

enum class Clocking : uint8_t { Timed, Adaptive };

// The TCK level between shifts
enum class TckIdle : bool { Low = false, High = true };

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

template <Clocking C, TckIdle Idle, typename Pins>
bool shiftBits(Pins &pins,
               protocol::BackgroundTimer &background,
               const Timing &timing,
               uint32_t bitCount,
               const uint8_t *tdi,
               const uint8_t *tms,
               uint8_t *tdo) {
   // TCK is at its idle level since the end of the last shift.
   uint32_t edge = 0;
   if constexpr(C == Clocking::Timed) {
      edge = pins.now();
   }
   for(uint32_t done = 0; done < bitCount; done += 8) {
      // TCK is at its idle level. After a call, the half period starts
      // again: the next TDI and TMS writes get their full setup time.
      if(background.poll(pins)) {
         if constexpr(C == Clocking::Timed) {
            edge = pins.now();
         }
      }
      const uint32_t bits = std::min<uint32_t>(8, bitCount - done);
      uint32_t tdiByte = *tdi++;
      uint32_t tmsByte = *tms++;
      uint32_t tdoByte = 0;
      for(uint32_t bit = 0; bit < bits; ++bit) {
         if constexpr(Idle == TckIdle::High) {
            if constexpr(C == Clocking::Timed) {
               waitFrom(pins, edge, timing.halfCycles);
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
         } else if constexpr(C == Clocking::Adaptive) {
            if(!waitRtck(pins, true, timing.timeoutCycles)) {
               if constexpr(Idle == TckIdle::Low) {
                  pins.tck(false);
               }
               return false;
            }
         }
         if constexpr(Idle == TckIdle::Low) {
            if constexpr(C == Clocking::Timed) {
               waitFrom(pins, edge, timing.halfCycles);
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
      }
      *tdo++ = static_cast<uint8_t>(tdoByte);
   }
   return true;
}

} // namespace detail

// The JTAG port of the JTAG_RAW block on bit-banged pins.
template <typename Pins, TckIdle Idle = TckIdle::Low>
class BitBangPort final : public protocol::JtagPort {
   public:
   // background: serviced during a shift, nullptr for none.
   explicit BitBangPort(Pins &pins, protocol::Background *background = nullptr)
      : pins_(pins)
      , background_(background) {
   }

   void init() override {
      pins_.init(Idle == TckIdle::High);
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
      background_.begin(pins_);
      switch(timing_.clocking) {
      case Clocking::Timed:
         return detail::shiftBits<Clocking::Timed, Idle>(pins_, background_, timing_, bitCount, tdi.data(), tms.data(),
                                                         tdo.data());
      case Clocking::Adaptive:
         return detail::shiftBits<Clocking::Adaptive, Idle>(pins_, background_, timing_, bitCount, tdi.data(),
                                                            tms.data(), tdo.data());
      }
      return false;
   }

   [[nodiscard]] const Timing &timing() const {
      return timing_;
   }

   private:
   Pins &pins_;
   protocol::BackgroundTimer background_;
   // INIT of the JTAG_RAW block sets the speed. Until then: the fastest
   // rate.
   Timing timing_{ Clocking::Timed, 0, 0 };
};

} // namespace jtag
