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
//   void begin();              before each operation: make now() count
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
// slow core (see runsUntimed()): it does not read the cycle counter, and
// each half period is the loop code and untimedPadNops NOP instructions.
// Thus its rate depends on the code: 2.42 to 2.7 MHz on the Mini
// (docs/clock-pacing.md, #118). The original firmware bursts at 2.25 to
// 3 MHz. On a fast core, delay 0 is timed at 1.85 MHz. The CH32V307 also
// runs delay 0 timed (Pins::timedAtDelayZero): its timed code makes
// 1.85 MHz, and the untimed engine would give 5.1 MHz.
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

// Untimed (delay 0): NOP instructions in each half period. They keep a
// minimum low half period, so SWDIO has a setup time before the rising
// edge, and the target has time to drive SWDIO before the read. With the
// loop code, one bit takes about 27 to 30 cycles: 2.42 to 2.7 MHz measured
// at delay 0 on the Mini, it changes with the code layout.
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
// The value is above the untimed rate, so a request from this value does
// not give a faster clock. The untimed rate changes with the code layout:
// 2.42 to 2.7 MHz on the Mini (#118), 4.0 MHz on the CH32V307 at 144 MHz
// (docs/clock-pacing.md). Measure again after a change of the engine.
#if defined(__riscv)
inline constexpr uint32_t maxClockHz = 4'500'000;
#else
inline constexpr uint32_t maxClockHz = 3'000'000;
#endif

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

#if defined(__riscv)
// The CH32V307 (144 MHz) keeps the untimed engine from maxClockHz: it is
// faster than the timed code limit (3.27 MHz, docs/clock-pacing.md).
static_assert(runsUntimed(144'000'000));
#endif

// True when Pins::timedAtDelayZero is true: Versaloon delay 0 then runs
// timed at 1.85 MHz, the rate of the OpenOCD table. The untimed engine is
// then only for CMSIS-DAP requests from maxClockHz. A compile-time value, so
// the code of the other boards does not change.
template <typename Pins>
constexpr bool timedAtDelayZero() {
   if constexpr(requires { Pins::timedAtDelayZero; }) {
      return Pins::timedAtDelayZero;
   } else {
      return false;
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
   // (#117), 2.76 MHz on the CH32V307, 5.2 to 6.1 MHz on the STM32H5.
   void setClockHz(uint32_t hz) {
      if(hz >= maxClockHz && runsUntimed(pins_.coreHz())) {
         setHalfCycles(halfPeriodCycles(pins_.coreHz(), 0), false);
      } else {
         setHalfCycles(halfPeriodCyclesOfHz(pins_.coreHz(), hz), true);
      }
   }

   void sequenceOut(uint16_t bitCount, art::span<const uint8_t> data) override {
      if(isTimed()) {
         sequenceOut<true>(bitCount, data);
      } else {
         sequenceOut<false>(bitCount, data);
      }
   }

   void sequenceIn(uint16_t bitCount, art::span<uint8_t> data) override {
      if(isTimed()) {
         sequenceIn<true>(bitCount, data);
      } else {
         sequenceIn<false>(bitCount, data);
      }
   }

   uint8_t transfer(uint8_t request, uint32_t &data) override {
      return isTimed() ? transfer<true>(request, data) : transfer<false>(request, data);
   }

   [[nodiscard]] uint32_t halfCycles() const {
      return halfCycles_;
   }

   // False at delay 0 on a slow core: the engine then runs untimed (see
   // the file comment).
   [[nodiscard]] bool isTimed() const {
      return timed_;
   }

   private:
   // Timed: wait for each half period. Untimed: no wait, and no read of the
   // cycle counter.
   template <bool Timed>
   void sequenceOut(uint16_t bitCount, art::span<const uint8_t> data) {
      if(bitCount == 0) {
         return;
      }
      begin();
      auto pacer = startPacer<Timed>();
      pins_.swdioDrive((data[0] & 1u) != 0);
      for(uint32_t i = 0; i < bitCount; ++i) {
         if(i % 8 == 0) {
            background_.poll(pins_);
         }
         writeBit<Timed>(pacer, ((data[i / 8] >> (i % 8)) & 1u) != 0);
      }
   }

   template <bool Timed>
   void sequenceIn(uint16_t bitCount, art::span<uint8_t> data) {
      begin();
      auto pacer = startPacer<Timed>();
      pins_.swdioRelease();
      for(uint32_t i = 0; i < bitCount; ++i) {
         if(i % 8 == 0) {
            background_.poll(pins_);
         }
         if(readBit<Timed>(pacer)) {
            data[i / 8] = static_cast<uint8_t>(data[i / 8] | 1u << (i % 8));
         }
      }
   }

   template <bool Timed>
   uint8_t transfer(uint8_t request, uint32_t &data) {
      begin();
      for(uint32_t attempt = 0;; ++attempt) {
         background_.poll(pins_);
         const uint8_t ack = transferOnce<Timed>(request, data);
         if(ack != ackWait) {
            return ack;
         }
         if(attempt >= retries_ || (stop_ != nullptr && stop_->stopRetries())) {
            return static_cast<uint8_t>(ack | retryOutFlag);
         }
         if(rules_.abortOnWait) {
            // An ABORT write gets OK also with STICKYORUN set.
            uint32_t clear = orunErrClr;
            static_cast<void>(transferOnce<Timed>(abortRequest, clear));
         }
      }
   }

   void setHalfCycles(uint32_t halfCycles, bool timed) {
      halfCycles_ = halfCycles;
      timed_ = timed;
   }

   // The pacer of one operation: a local, so its deadline stays in a
   // register. A member pacer was loaded and stored at each edge, because
   // the compiler assumes that a GPIO store can change it (#117). The first
   // half period starts now.
   template <bool Timed>
   [[gnu::always_inline]] pacing::ClockPacer startPacer() {
      pacing::ClockPacer pacer;
      if constexpr(Timed) {
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
   template <bool Timed>
   uint8_t transferOnce(uint8_t request, uint32_t &data) {
      auto pacer = startPacer<Timed>();
      pins_.swdioDrive(true);
      writeBits<Timed>(pacer, request, 8);
      pins_.swdioRelease();
      clocks<Timed>(pacer, turnaround_);
      const auto ack = static_cast<uint8_t>(readBits<Timed>(pacer, 3));
      uint8_t result = ack;
      const bool ackError = ack != ackOk && ack != ackWait && ack != ackFault;
      if(!rules_.dataPhase && (ack == ackWait || ack == ackFault)) {
         // No data phase: the turnaround back to the host
         clocks<Timed>(pacer, turnaround_);
      } else if((request & readRequest) != 0 || (ackError && rules_.releaseOnAckError)) {
         // A read, or the back-off after a protocol error: SWDIO released
         const uint32_t value = readBits<Timed>(pacer, 32);
         const bool parityBit = readBit<Timed>(pacer);
         clocks<Timed>(pacer, turnaround_);
         if(ack == ackOk) {
            if(parityBit != parity(value)) {
               result |= parityErrorFlag;
            } else {
               data = value;
            }
         }
      } else {
         clocks<Timed>(pacer, turnaround_);
         pins_.swdioDrive((data & 1u) != 0);
         writeBits<Timed>(pacer, data, 32);
         writeBit<Timed>(pacer, parity(data));
      }
      if(ack == ackFault) {
         result |= faultFlag;
      } else if(ackError) {
         result |= ackErrorFlag;
      }
      pins_.swdioDrive(false);
      if(ack == ackOk || !rules_.idleOnlyAfterOk) {
         writeBits<Timed>(pacer, 0, rules_.idleCycles);
      }
      return result;
   }

   // Waits for the end of the half period. The SWCLK edge must follow.
   template <bool Timed>
   [[gnu::always_inline]] void waitHalfPeriod(pacing::ClockPacer &pacer) {
      if constexpr(Timed) {
         pacer.wait(pins_);
      } else {
         static_assert(untimedPadNops == 6, "Change the NOP count below too");
         asm volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop");
      }
   }

   // The SWCLK edge after waitHalfPeriod()
   template <bool Timed>
   [[gnu::always_inline]] void swclk(pacing::ClockPacer &pacer, bool level) {
      pins_.swclk(level);
      if constexpr(Timed) {
         pacer.edge(pins_);
      }
   }

   template <bool Timed>
   [[gnu::always_inline]] void writeBit(pacing::ClockPacer &pacer, bool level) {
      waitHalfPeriod<Timed>(pacer);
      swclk<Timed>(pacer, false);
      pins_.swdioWrite(level);
      waitHalfPeriod<Timed>(pacer);
      swclk<Timed>(pacer, true);
   }

   template <bool Timed>
   [[gnu::always_inline]] bool readBit(pacing::ClockPacer &pacer) {
      waitHalfPeriod<Timed>(pacer);
      swclk<Timed>(pacer, false);
      waitHalfPeriod<Timed>(pacer);
      const bool level = pins_.swdio();
      swclk<Timed>(pacer, true);
      return level;
   }

   template <bool Timed>
   [[gnu::always_inline]] void writeBits(pacing::ClockPacer &pacer, uint32_t value, uint32_t count) {
      for(uint32_t i = 0; i < count; ++i) {
         writeBit<Timed>(pacer, (value & 1u) != 0);
         value >>= 1;
      }
   }

   template <bool Timed>
   [[gnu::always_inline]] uint32_t readBits(pacing::ClockPacer &pacer, uint32_t count) {
      uint32_t value = 0;
      for(uint32_t i = 0; i < count; ++i) {
         value |= static_cast<uint32_t>(readBit<Timed>(pacer)) << i;
      }
      return value;
   }

   // Clock cycles with SWDIO released, for a turnaround
   template <bool Timed>
   [[gnu::always_inline]] void clocks(pacing::ClockPacer &pacer, uint32_t count) {
      for(uint32_t i = 0; i < count; ++i) {
         static_cast<void>(readBit<Timed>(pacer));
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
};

} // namespace swd
