// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// SWD bit-bang engine: runs the sequences and transfers of an SWD port
// (protocol/SwdPort.h) on GPIO pins. Hardware-independent: a Pins type
// gives the pin access, so the host unit tests run the protocol on mock
// pins with a mock target. On target, board/BoardSwd.h gives FastPin
// access.
//
// The Pins type has these members:
//
//   void init();               SWCLK output high, SWDIO driven high
//   void fini();               SWCLK and SWDIO inputs
//   void begin();              before each operation: make now() count,
//                              and get the state of the pins (direction)
//   void swclk(bool);
//   void swdioDrive(bool);     SWDIO output at the level
//   void swdioRelease();       SWDIO input (with pull-up)
//   void swdioWrite(bool);     SWDIO level while it is an output
//   bool swdio();              SWDIO level
//   uint32_t now();            free-running core cycle counter
//   uint32_t coreHz();         core clock frequency
//
// Optional: static constexpr bool timedAtDelayZero = true runs delay 0
// timed also on a slow core (see timedAtDelayZero()).
// static constexpr uint32_t untimedHighPadNops and untimedLowPadNops set
// the NOP pads of the untimed engine (default untimedPadNops, see
// untimedPadNopsOf()).
// static constexpr bool unrolledBits = true unrolls the fixed-length bit
// loops of the untimed engine (see unrolledBits()).
//
// SWCLK is high between bits. One bit (ADIv5, B4.2):
//
//   1. Wait for the end of the high half period. SWCLK low. The host
//      writes SWDIO.
//   2. Wait for the end of the low half period. The host reads SWDIO. The
//      target changed it after the last rising edge.
//   3. SWCLK high: the target samples SWDIO, or drives its next bit.
//
// Each SWCLK edge comes at a deadline of a ClockPacer (ClockPacer.h): the
// loop code runs inside the half period, so SWCLK has the rate of the
// OpenOCD table on each board and build. An interrupt makes a half period
// longer, never shorter.
//
// At delay 0 (1.85 MHz) and from maxClockHz, the engine runs untimed on a
// slow core (see runsUntimed()), and on the CH32V307 also above 2 MHz with
// extra NOPs (see below): it does not read the cycle counter, and
// each half period is the bit code and the NOP pads (untimedPadNopsOf()).
// Thus its rate depends on the code: 2.42 to 2.7 MHz on the Mini
// (docs/clock-pacing.md, #118). The original firmware bursts at 2.25 to
// 3 MHz. On a fast core, delay 0 is timed at 1.85 MHz. The CH32V307 also
// runs delay 0 timed (Pins::timedAtDelayZero): its timed code makes
// 1.85 MHz, and the untimed engine would give 9.6 MHz.
//
// With Pins::untimedMinPeriodCycles (the CH32V307), a CMSIS-DAP request
// above the timed range runs untimed with extra NOPs in each half period
// (untimedPadFor()). The pad makes the shortest period not shorter than
// the request, and an interrupt only makes a period longer. Each small pad
// has its own instance of the engine. The larger pads use one instance
// with the NOP sled of the pins (Pins::padSled()).
//
// A transfer:
//
//   request (8 bits) | turnaround | ACK (3 bits) | data phase | idle
//
// The data phase runs for each ACK, as in the OpenOCD bitbang driver and
// the original firmware:
//
// - Read: 32 data bits and the parity bit with SWDIO released, then a
//   turnaround. Only an OK ACK gives data.
// - Write: a turnaround, then the host drives 32 data bits and the parity
//   bit.
//
// OpenOCD sets ORUNDETECT in the DP. Then the target expects the data
// phase also after WAIT and FAULT (ADIv5, B4.2.4), and a host without it
// loses sync with the target. Without ORUNDETECT, the target ignores the
// data phase. A write without ACK (multidrop TARGETSEL) also needs it: the
// target samples the data.
//
// After each transfer, the host drives SWDIO low for defaultIdleCycles
// cycles, as the original firmware does. OpenOCD does not send idle cycles
// to this adapter, and the target needs idle cycles to complete the last
// write.
//
// A WAIT repeats the transfer up to `retries` times. With ORUNDETECT, a
// WAIT sets STICKYORUN, and the DP replies FAULT until ABORT.ORUNERRCLR
// clears it. Thus, before each retry, the host writes ABORT with
// ORUNERRCLR, as the OpenOCD bitbang driver does.
//
// With a RetryStop (setRetryStop()), the engine asks it after each WAIT.
// When it returns true, the transfer ends as after the last retry. The
// engine asks it only after a WAIT: a transfer without WAIT has no extra
// cost. CMSIS-DAP connects it to DAP_TransferAbort.
//
// CMSIS-DAP has other transfer rules. setRules() changes them, and
// setClockHz() sets the clock in Hz (see TransferRules). setTurnaround()
// and setRetries() change one value each. configure() sets the clock
// from a delay value, so the CMSIS-DAP path does not call it.
//
// - The idle cycles come from DAP_TransferConfigure (0-255).
// - A WAIT repeats the transfer without the ABORT write. The debugger
//   clears STICKYORUN itself.
// - The data phase after WAIT and FAULT is optional (DAP_SWD_Configure).
//   Without it, the host clocks one turnaround after the ACK and then
//   drives SWDIO.
// - The idle cycles come only after an OK ACK.
// - After an ACK that is not OK, WAIT or FAULT (a protocol error), the host
//   clocks the data phase and the turnaround with SWDIO released, also for
//   a write. The target can then drive read data without contention: it
//   possibly decoded another request. CMSIS-DAP debuggers send a multidrop
//   TARGETSEL with DAP_SWD_Sequence, not as a transfer.
// - The turnaround is 1-4 cycles at each turnaround, as for configure().
//
// DAP_SWJ_Sequence and DAP_SWD_Sequence use sequenceOut() and
// sequenceIn(): each sequence is at most 256 bits, LSB first.
//
// The SWD block of the Versaloon protocol does not call setRules() or
// setClockHz(). In a multi image, the CMSIS-DAP processor sets its rules
// at start, also when the Versaloon personality is active. Thus
// configure() (INIT and CONFIG of the SWD block) sets the Versaloon rules
// again.
//
// With a protocol::Background, the engine calls Background::service()
// before each transfer and each retry, and between the bytes of a
// sequence, at most once per backgroundIntervalMs (protocol/Background.h).
// SWCLK is then high: the call makes that high half period longer.

#include <array>
#include <cstdint>
#include <utility>

#include "ClockPacer.h"
#include "IntDiv.h"

#include "protocol/Background.h"
#include "protocol/SwdPort.h"

namespace swd {

using protocol::swd::ackErrorFlag;
using protocol::swd::ackFault;
using protocol::swd::ackOk;
using protocol::swd::ackWait;
using protocol::swd::defaultRetries;
using protocol::swd::defaultTurnaround;
using protocol::swd::faultFlag;
using protocol::swd::parity;
using protocol::swd::parityErrorFlag;
using protocol::swd::readRequest;
using protocol::swd::retryOutFlag;

// The SWCLK frequency of the OpenOCD delay values 0-5 (vsllink.c,
// vsllink_swd_frequency()). Above 5, the frequency is 500000 / (delay + 1)
// Hz: a period of 2 * (delay + 1) us.
inline constexpr std::array<uint32_t, 6> fixedClockHz{ 1'850'000, 235'000, 130'000, 102'000, 85'000, 72'000 };

// Untimed (delay 0): the default count of NOP instructions in each half
// period. They keep a minimum low half period, so SWDIO has a setup time
// before the rising edge, and the target has time to drive SWDIO before
// the read. With the loop code, one bit takes about 27 to 30 cycles: 2.42
// to 2.7 MHz measured at delay 0 on the Mini, it changes with the code
// layout.
inline constexpr uint32_t untimedPadNops = 6;

// The DP ABORT write (encoded request) and its ORUNERRCLR bit
inline constexpr uint8_t abortRequest = 0x81;
inline constexpr uint32_t orunErrClr = 0x10;

// SWDIO low cycles after each transfer (Versaloon)
inline constexpr uint8_t defaultIdleCycles = 8;

// The transfer rules. The defaults are the Versaloon rules.
struct TransferRules {
   // SWDIO low cycles after each transfer
   uint8_t idleCycles{ defaultIdleCycles };
   // Write ABORT with ORUNERRCLR before each retry after a WAIT
   bool abortOnWait{ true };
   // Run the data phase also after WAIT and FAULT
   bool dataPhase{ true };
   // Send the idle cycles only after an OK ACK
   bool idleOnlyAfterOk{ false };
   // After a protocol error, release SWDIO also in the data phase of a write
   bool releaseOnAckError{ false };
};

// Stops the WAIT retries of a transfer. In an image with CMSIS-DAP, the
// processor sets it at start, also when the Versaloon personality is
// active. It then gets no packets, so the stop always returns false. A
// Versaloon-only image has none.
class RetryStop {
   public:
   // True: no more retries
   virtual bool stopRetries() = 0;

   protected:
   ~RetryStop() = default;
};

// Core cycles of a half SWCLK period for a frequency, rounded up: SWCLK is
// never faster than the frequency. 0 Hz gives the slowest clock (1 Hz).
// coreHz / (2 * hz) rounded up, in two 32-bit steps: ceil(ceil(a / b) / c)
// is ceil(a / (b * c)).
constexpr uint32_t halfPeriodCyclesOfHz(uint32_t coreHz, uint32_t hz) {
   return intdiv::ceilDiv(intdiv::ceilDiv(coreHz, 2u), hz == 0 ? 1u : hz);
}

// Core cycles of a half SWCLK period for an OpenOCD delay value, rounded
// up: SWCLK is never faster than OpenOCD expects.
constexpr uint32_t halfPeriodCycles(uint32_t coreHz, uint16_t delay) {
   if(delay < fixedClockHz.size()) {
      return halfPeriodCyclesOfHz(coreHz, fixedClockHz[delay]);
   }
   // Half a period is (delay + 1) us.
   return intdiv::mulDivCeil(coreHz, delay + 1u, 1'000'000u);
}

// From this SWCLK, the engine runs untimed on a slow core, as at delay 0.
// The value is above the untimed rate, with a margin, so a request from
// this value does not give a faster clock: 2.42 to 2.7 MHz on the Mini
// (#118). The untimed rate changes with the code layout. Measure again
// after a change of the engine. The CH32V307 runs the requests above 2 MHz
// untimed with NOP pads (Pins::untimedMinPeriodCycles), so it does not
// use this value.
inline constexpr uint32_t maxClockHz = 3'000'000;

// The cycles of the code of a timed half period, rounded up: 23 to 28 on
// the STM32F103 (1.3 to 1.56 MHz at 72 MHz, it changes with the code
// layout, #117) and 21 to 24 on the STM32H5 (5.2 to 6.1 MHz at 250 MHz,
// docs/clock-pacing.md). Measure again after a change of the engine or
// the pacer.
inline constexpr uint32_t timedCodeCycles = 28;

// True when the timed engine cannot make maxClockHz: the untimed engine is
// then the fastest. On the STM32H5 at 250 MHz, the untimed engine gives
// 13 MHz at a request of 4 MHz, so the timed engine makes all clocks.
constexpr bool runsUntimed(uint32_t coreHz) {
   return halfPeriodCyclesOfHz(coreHz, maxClockHz) < timedCodeCycles;
}

// True when Pins::timedAtDelayZero is true: Versaloon delay 0 then runs
// timed at 1.85 MHz, the rate of the OpenOCD table. The untimed engine is
// then only for CMSIS-DAP requests from maxClockHz, or above the timed
// range with Pins::untimedMinPeriodCycles. A compile-time value, so the
// code of the other boards does not change.
template <typename Pins>
constexpr bool timedAtDelayZero() {
   if constexpr(requires { Pins::timedAtDelayZero; }) {
      return Pins::timedAtDelayZero;
   } else {
      return false;
   }
}

// The NOP count at the end of the high half period (High) or of the low
// half period: Pins::untimedHighPadNops or Pins::untimedLowPadNops, else
// untimedPadNops. The low-half pad gives the SWDIO setup time before the
// rising edge. The target drives SWDIO after the rising edge, and the host
// reads it after the low-half pad. The high-half pad, the bit code and
// the low-half pad give the target time to drive SWDIO before the read.
// With a low-half pad of 0, the read comes directly after the falling
// edge.
template <typename Pins, bool High>
constexpr uint32_t untimedPadNopsOf() {
   if constexpr(High && requires { Pins::untimedHighPadNops; }) {
      return Pins::untimedHighPadNops;
   } else if constexpr(!High && requires { Pins::untimedLowPadNops; }) {
      return Pins::untimedLowPadNops;
   } else {
      return untimedPadNops;
   }
}

// True when Pins::unrolledBits is true: the untimed engine then unrolls the
// request (8 bits), the ACK (3 bits) and the data (32 bits). This removes
// the loop code from each bit, and the bit position is a constant. The
// code is about 1.5 KB larger, so only a board with free flash sets it.
template <typename Pins>
constexpr bool unrolledBits() {
   if constexpr(requires { Pins::unrolledBits; }) {
      return Pins::unrolledBits;
   } else {
      return false;
   }
}

// The pace of an operation: timedPace for the timed engine, sledPace for
// the untimed engine with the NOP sled of the pins (Pins::padSled()), else
// the extra NOPs of the untimed engine in each half period.
inline constexpr uint32_t timedPace = UINT32_MAX;
inline constexpr uint32_t sledPace = UINT32_MAX - 1;

// Pins::untimedMinPeriodCycles: the shortest SWCLK period of the untimed
// engine without extra NOPs, in core cycles, measured on the wire. With it,
// a request with a half period below Pins::untimedBelowHalfCycles runs
// untimed with extra NOPs, not timed. An interrupt only makes the untimed
// engine slower, so the clock is never faster than the request. 0: no
// padded engine.
template <typename Pins>
constexpr uint32_t untimedMinPeriodCycles() {
   if constexpr(requires { Pins::untimedMinPeriodCycles; }) {
      static_assert(Pins::untimedBelowHalfCycles >= timedCodeCycles);
      return Pins::untimedMinPeriodCycles;
   } else {
      return 0;
   }
}

// The count of pads: 0 to the pad of the longest period that runs untimed.
// A half period below untimedBelowHalfCycles is a period of
// 2 * untimedBelowHalfCycles - 2 cycles or less. 1: only pad 0, no padded
// engine.
template <typename Pins>
constexpr uint32_t untimedPadCount() {
   constexpr uint32_t minPeriod = untimedMinPeriodCycles<Pins>();
   if constexpr(minPeriod == 0) {
      return 1;
   } else {
      return intdiv::ceilDiv(2 * Pins::untimedBelowHalfCycles - 2 - minPeriod, 2u) + 1;
   }
}

// The extra NOPs in each half period, so that the shortest period
// (untimedMinPeriodCycles + 2 * pad) is not shorter than the period of hz.
template <typename Pins>
constexpr uint32_t untimedPadFor(uint32_t coreHz, uint32_t hz) {
   constexpr uint32_t minPeriod = untimedMinPeriodCycles<Pins>();
   const uint32_t period = intdiv::ceilDiv(coreHz, hz == 0 ? 1u : hz);
   const uint32_t pad = period > minPeriod ? intdiv::ceilDiv(period - minPeriod, 2u) : 0;
   return pad < untimedPadCount<Pins>() ? pad : untimedPadCount<Pins>() - 1;
}

// Pins::padSledCycles: the cycles of a jump into the NOP sled of the pins
// with 0 NOPs (Pins::setPadSled(), Pins::padSled()). A pad from this value
// runs in one instance with the sled; the smaller pads have one instance
// each. 0: no sled, one instance for each pad.
template <typename Pins>
constexpr uint32_t padSledCycles() {
   if constexpr(requires { Pins::padSledCycles; }) {
      return Pins::padSledCycles;
   } else {
      return 0;
   }
}

// The pads with their own instance of the untimed engine
template <typename Pins>
constexpr uint32_t untimedPadInstances() {
   constexpr uint32_t sled = padSledCycles<Pins>();
   return sled != 0 && sled < untimedPadCount<Pins>() ? sled : untimedPadCount<Pins>();
}

// Count NOP instructions in asm statements of up to 6 lines. Literal lines,
// not .rept: the compiler counts the lines of an asm statement as its size,
// and that size changes the code layout around it.
template <uint32_t Count>
[[gnu::always_inline]] inline void nops() {
   if constexpr(Count > 6) {
      nops<6>();
      nops<Count - 6>();
   } else if constexpr(Count == 1) {
      asm volatile("nop");
   } else if constexpr(Count == 2) {
      asm volatile("nop\n\tnop");
   } else if constexpr(Count == 3) {
      asm volatile("nop\n\tnop\n\tnop");
   } else if constexpr(Count == 4) {
      asm volatile("nop\n\tnop\n\tnop\n\tnop");
   } else if constexpr(Count == 5) {
      asm volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop");
   } else if constexpr(Count == 6) {
      asm volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop");
   }
}

// The SWD port of the SWD block on bit-banged pins.
template <typename Pins>
class BitBangPort final : public protocol::SwdPort {
   public:
   // background: serviced during the operations, nullptr for none.
   explicit BitBangPort(Pins &pins, protocol::Background *background = nullptr)
      : pins_(pins)
      , background_(background) {
   }

   void init() override {
      pins_.init();
   }

   void fini() override {
      pins_.fini();
   }

   // Versaloon: also sets the Versaloon rules (see the file comment).
   void configure(uint8_t turnaround, uint16_t retries, uint16_t delay) override {
      rules_ = TransferRules{};
      turnaround_ = turnaround;
      retries_ = retries;
      setHalfCycles(halfPeriodCycles(pins_.coreHz(), delay),
                    delay != 0 || timedAtDelayZero<Pins>() || !runsUntimed(pins_.coreHz()));
   }

   // CMSIS-DAP: the transfer rules. They stay until the next call.
   void setRules(const TransferRules &rules) {
      rules_ = rules;
   }

   // CMSIS-DAP: cycles at each turnaround, 1-4 (DAP_SWD_Configure). The
   // clock does not change.
   void setTurnaround(uint8_t turnaround) {
      turnaround_ = turnaround;
   }

   // CMSIS-DAP: retries after WAIT (DAP_TransferConfigure). The clock does
   // not change.
   void setRetries(uint16_t retries) {
      retries_ = retries;
   }

   // CMSIS-DAP: asked after each WAIT, nullptr for none. It stays until
   // the next call.
   void setRetryStop(RetryStop *stop) {
      stop_ = stop;
   }

   [[nodiscard]] const TransferRules &rules() const {
      return rules_;
   }

   // CMSIS-DAP: SWCLK in Hz, rounded down to the nearest available rate.
   // From maxClockHz, the engine runs untimed on a slow core (as at delay
   // 0). The code limits the timed rate: 1.3 to 1.56 MHz on the STM32F103
   // (#117), 5.2 to 6.1 MHz on the STM32H5.
   //
   // With Pins::untimedMinPeriodCycles, a request with a half period
   // shorter than Pins::untimedBelowHalfCycles runs untimed, with
   // untimedPadFor() extra NOPs: the shortest period is then not shorter
   // than the request.
   void setClockHz(uint32_t hz) {
      if constexpr(untimedPadCount<Pins>() > 1) {
         const uint32_t coreHz = pins_.coreHz();
         if(halfPeriodCyclesOfHz(coreHz, hz) < Pins::untimedBelowHalfCycles) {
            setHalfCycles(halfPeriodCycles(coreHz, 0), false, untimedPadFor<Pins>(coreHz, hz));
            return;
         }
      }
      if(hz >= maxClockHz && runsUntimed(pins_.coreHz())) {
         setHalfCycles(halfPeriodCycles(pins_.coreHz(), 0), false);
      } else {
         setHalfCycles(halfPeriodCyclesOfHz(pins_.coreHz(), hz), true);
      }
   }

   // Without pads (untimedPadCount() 1), the plain branch: the same code
   // as before the pads on the STM32 boards.
   void sequenceOut(uint16_t bitCount, art::span<const uint8_t> data) override {
      if constexpr(untimedPadCount<Pins>() == 1) {
         if(isTimed()) {
            sequenceOut<timedPace>(bitCount, data);
         } else {
            sequenceOut<0>(bitCount, data);
         }
      } else {
         withPace([&]<uint32_t Pace>() { sequenceOut<Pace>(bitCount, data); });
      }
   }

   void sequenceIn(uint16_t bitCount, art::span<uint8_t> data) override {
      if constexpr(untimedPadCount<Pins>() == 1) {
         if(isTimed()) {
            sequenceIn<timedPace>(bitCount, data);
         } else {
            sequenceIn<0>(bitCount, data);
         }
      } else {
         withPace([&]<uint32_t Pace>() { sequenceIn<Pace>(bitCount, data); });
      }
   }

   uint8_t transfer(uint8_t request, uint32_t &data) override {
      if constexpr(untimedPadCount<Pins>() == 1) {
         return isTimed() ? transfer<timedPace>(request, data) : transfer<0>(request, data);
      } else {
         return withPace([&]<uint32_t Pace>() { return transfer<Pace>(request, data); });
      }
   }

   [[nodiscard]] uint32_t halfCycles() const {
      return halfCycles_;
   }

   // False at delay 0 on a slow core: the engine then runs untimed (see
   // the file comment).
   [[nodiscard]] bool isTimed() const {
      return timed_;
   }

   // The extra NOPs of the untimed engine in each half period (see
   // setClockHz())
   [[nodiscard]] uint32_t untimedPad() const {
      return pad_;
   }

   private:
   // Pace timedPace: wait for each half period. Else untimed: no wait, no
   // read of the cycle counter, and Pace more NOPs in each half period.
   template <uint32_t Pace>
   void sequenceOut(uint16_t bitCount, art::span<const uint8_t> data) {
      if(bitCount == 0) {
         return;
      }
      begin();
      auto pacer = startPacer<Pace>();
      pins_.swdioDrive((data[0] & 1u) != 0);
      for(uint32_t i = 0; i < bitCount; ++i) {
         if(i % 8 == 0) {
            background_.poll(pins_);
         }
         writeBit<Pace>(pacer, ((data[i / 8] >> (i % 8)) & 1u) != 0);
      }
   }

   template <uint32_t Pace>
   void sequenceIn(uint16_t bitCount, art::span<uint8_t> data) {
      begin();
      auto pacer = startPacer<Pace>();
      pins_.swdioRelease();
      for(uint32_t i = 0; i < bitCount; ++i) {
         if(i % 8 == 0) {
            background_.poll(pins_);
         }
         if(readBit<Pace>(pacer)) {
            data[i / 8] = static_cast<uint8_t>(data[i / 8] | 1u << (i % 8));
         }
      }
   }

   template <uint32_t Pace>
   uint8_t transfer(uint8_t request, uint32_t &data) {
      begin();
      for(uint32_t attempt = 0;; ++attempt) {
         background_.poll(pins_);
         const uint8_t ack = transferOnce<Pace>(request, data);
         if(ack != ackWait) {
            return ack;
         }
         if(attempt >= retries_ || (stop_ != nullptr && stop_->stopRetries())) {
            return static_cast<uint8_t>(ack | retryOutFlag);
         }
         if(rules_.abortOnWait) {
            // An ABORT write gets OK also with STICKYORUN set.
            uint32_t clear = orunErrClr;
            static_cast<void>(transferOnce<Pace>(abortRequest, clear));
         }
      }
   }

   void setHalfCycles(uint32_t halfCycles, bool timed, uint32_t pad = 0) {
      halfCycles_ = halfCycles;
      timed_ = timed;
      if constexpr(untimedPadCount<Pins>() > 1) {
         pad_ = pad;
      } else {
         static_cast<void>(pad);
      }
   }

   // Calls fn with the pace of the operation: timedPace, or the pad of the
   // untimed engine. One instance of the engine code for each pad.
   template <typename Fn>
   [[gnu::always_inline]] auto withPace(Fn fn) {
      if(isTimed()) {
         return fn.template operator()<timedPace>();
      }
      if constexpr(untimedPadInstances<Pins>() < untimedPadCount<Pins>()) {
         if(pad_ >= untimedPadInstances<Pins>()) {
            pins_.setPadSled(pad_ - padSledCycles<Pins>());
            return fn.template operator()<sledPace>();
         }
      }
      return withPad(fn, std::make_index_sequence<untimedPadInstances<Pins>()>{});
   }

   template <typename Fn, std::size_t... P>
   [[gnu::always_inline]] auto withPad(Fn fn, std::index_sequence<P...>) {
      using Result = decltype(fn.template operator()<0>());
      using Entry = Result (*)(Fn &);
      static constexpr std::array<Entry, sizeof...(P)> table{ [](Fn &f) -> Result { return f.template operator()<P>(); }... };
      return table[pad_](fn);
   }

   // The pacer of one operation: a local, so its deadline stays in a
   // register. A member pacer was loaded and stored at each edge, because
   // the compiler assumes that a GPIO store can change it (#117). The first
   // half period starts now.
   template <uint32_t Pace>
   [[gnu::always_inline]] pacing::ClockPacer startPacer() {
      pacing::ClockPacer pacer;
      if constexpr(Pace == timedPace) {
         pacer.setHalfCycles(halfCycles_);
         pacer.start(pins_);
      }
      return pacer;
   }

   void begin() {
      pins_.begin();
      background_.begin(pins_);
   }

   // One transfer. Returns the ACK and the FAULT, ACK error and parity
   // flags.
   template <uint32_t Pace>
   uint8_t transferOnce(uint8_t request, uint32_t &data) {
      auto pacer = startPacer<Pace>();
      pins_.swdioDrive(true);
      writeBits<Pace, 8>(pacer, request);
      pins_.swdioRelease();
      clocks<Pace>(pacer, turnaround_);
      const auto ack = static_cast<uint8_t>(readBits<Pace, 3>(pacer));
      uint8_t result = ack;
      const bool ackError = ack != ackOk && ack != ackWait && ack != ackFault;
      if(!rules_.dataPhase && (ack == ackWait || ack == ackFault)) {
         // No data phase: the turnaround back to the host
         clocks<Pace>(pacer, turnaround_);
      } else if((request & readRequest) != 0 || (ackError && rules_.releaseOnAckError)) {
         // A read, or the back-off after a protocol error: SWDIO released
         const uint32_t value = readBits<Pace, 32>(pacer);
         const bool parityBit = readBit<Pace>(pacer);
         clocks<Pace>(pacer, turnaround_);
         if(ack == ackOk) {
            if(parityBit != parity(value)) {
               result |= parityErrorFlag;
            } else {
               data = value;
            }
         }
      } else {
         clocks<Pace>(pacer, turnaround_);
         pins_.swdioDrive((data & 1u) != 0);
         writeBits<Pace, 32>(pacer, data);
         writeBit<Pace>(pacer, parity(data));
      }
      if(ack == ackFault) {
         result |= faultFlag;
      } else if(ackError) {
         result |= ackErrorFlag;
      }
      pins_.swdioDrive(false);
      if(ack == ackOk || !rules_.idleOnlyAfterOk) {
         writeBits<Pace>(pacer, 0, rules_.idleCycles);
      }
      return result;
   }

   // Waits for the end of the high (High) or low half period. The SWCLK
   // edge must follow.
   template <uint32_t Pace, bool High>
   [[gnu::always_inline]] void waitHalfPeriod(pacing::ClockPacer &pacer) {
      if constexpr(Pace == timedPace) {
         pacer.wait(pins_);
      } else {
         if constexpr(Pace == sledPace) {
            nops<untimedPadNopsOf<Pins, High>()>();
            pins_.padSled();
         } else {
            nops<untimedPadNopsOf<Pins, High>() + Pace>();
         }
      }
   }

   // The SWCLK edge after waitHalfPeriod()
   template <uint32_t Pace>
   [[gnu::always_inline]] void swclk(pacing::ClockPacer &pacer, bool level) {
      pins_.swclk(level);
      if constexpr(Pace == timedPace) {
         pacer.edge(pins_);
      }
   }

   template <uint32_t Pace>
   [[gnu::always_inline]] void writeBit(pacing::ClockPacer &pacer, bool level) {
      waitHalfPeriod<Pace, true>(pacer);
      swclk<Pace>(pacer, false);
      pins_.swdioWrite(level);
      waitHalfPeriod<Pace, false>(pacer);
      swclk<Pace>(pacer, true);
   }

   template <uint32_t Pace>
   [[gnu::always_inline]] bool readBit(pacing::ClockPacer &pacer) {
      waitHalfPeriod<Pace, true>(pacer);
      swclk<Pace>(pacer, false);
      waitHalfPeriod<Pace, false>(pacer);
      const bool level = pins_.swdio();
      swclk<Pace>(pacer, true);
      return level;
   }

   template <uint32_t Pace>
   [[gnu::always_inline]] void writeBits(pacing::ClockPacer &pacer, uint32_t value, uint32_t count) {
      for(uint32_t i = 0; i < count; ++i) {
         writeBit<Pace>(pacer, (value & 1u) != 0);
         value >>= 1;
      }
   }

   template <uint32_t Pace>
   [[gnu::always_inline]] uint32_t readBits(pacing::ClockPacer &pacer, uint32_t count) {
      uint32_t value = 0;
      for(uint32_t i = 0; i < count; ++i) {
         value |= static_cast<uint32_t>(readBit<Pace>(pacer)) << i;
      }
      return value;
   }

   // A fixed count of bits: unrolled in the untimed engine when
   // unrolledBits<Pins>(), else the loop.
   template <uint32_t Pace, uint32_t Count>
   [[gnu::always_inline]] void writeBits(pacing::ClockPacer &pacer, uint32_t value) {
      if constexpr(Pace != timedPace && unrolledBits<Pins>()) {
         writeUnrolled<Pace>(pacer, value, std::make_index_sequence<Count>{});
      } else {
         writeBits<Pace>(pacer, value, Count);
      }
   }

   template <uint32_t Pace, uint32_t Count>
   [[gnu::always_inline]] uint32_t readBits(pacing::ClockPacer &pacer) {
      if constexpr(Pace != timedPace && unrolledBits<Pins>()) {
         return readUnrolled<Pace>(pacer, std::make_index_sequence<Count>{});
      } else {
         return readBits<Pace>(pacer, Count);
      }
   }

   template <uint32_t Pace, std::size_t... I>
   [[gnu::always_inline]] void writeUnrolled(pacing::ClockPacer &pacer, uint32_t value, std::index_sequence<I...>) {
      (writeBit<Pace>(pacer, ((value >> I) & 1u) != 0), ...);
   }

   template <uint32_t Pace, std::size_t... I>
   [[gnu::always_inline]] uint32_t readUnrolled(pacing::ClockPacer &pacer, std::index_sequence<I...>) {
      uint32_t value = 0;
      (readUnrolledBit<Pace, I>(pacer, value), ...);
      return value;
   }

   // The empty asm makes the value ready before the next bit: the code of
   // each bit stays in its high half period. There it gives the target
   // time to drive SWDIO, and no code is left for after the last bit.
   template <uint32_t Pace, std::size_t I>
   [[gnu::always_inline]] void readUnrolledBit(pacing::ClockPacer &pacer, uint32_t &value) {
      value |= static_cast<uint32_t>(readBit<Pace>(pacer)) << I;
      asm volatile("" : "+r"(value));
   }

   // Clock cycles with SWDIO released, for a turnaround
   template <uint32_t Pace>
   [[gnu::always_inline]] void clocks(pacing::ClockPacer &pacer, uint32_t count) {
      for(uint32_t i = 0; i < count; ++i) {
         static_cast<void>(readBit<Pace>(pacer));
      }
   }

   Pins &pins_;
   protocol::BackgroundTimer background_;
   // INIT of the SWD block sets the configuration.
   uint8_t turnaround_{ defaultTurnaround };
   uint16_t retries_{ defaultRetries };
   TransferRules rules_;
   RetryStop *stop_{ nullptr };
   uint32_t halfCycles_{ 0 };
   bool timed_{ true };
   // The extra NOPs of the untimed engine in each half period
   uint32_t pad_{ 0 };
};

} // namespace swd
