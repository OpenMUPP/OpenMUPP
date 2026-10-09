#pragma once

// SWD bit-bang engine: runs the sequences and transfers of the SWD block
// (protocol/SwdBlock.h) on GPIO pins. Hardware-independent: a Pins type
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
// SWCLK is high between bits. One bit (ADIv5, B4.2):
//
//   1. SWCLK low. The host writes SWDIO. Wait for the low half period.
//   2. The host reads SWDIO. The target changed it after the last rising
//      edge.
//   3. SWCLK high: the target samples SWDIO, or drives its next bit. Wait
//      for the high half period.
//
// Each half period waits from its SWCLK edge, as in the JTAG engine
// (jtag/JtagBitBang.h): an interrupt makes a half period longer, never
// shorter. The wait is halfCycles minus loopOverheadCycles, the cycles of
// the loop code in each half period. Thus SWCLK is not faster than the
// OpenOCD table, except at delay 0.
//
// At delay 0 (1.85 MHz), the engine runs untimed: it does not read the
// cycle counter, and each half period is the loop code and untimedPadNops NOP
// instructions. SWCLK is then about 3 MHz, as with the original firmware
// (2.25 to 3 MHz), and faster than 1.85 MHz.
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
// After each transfer, the host drives SWDIO low for idleCycles cycles, as
// the original firmware does. OpenOCD does not send idle cycles to this
// adapter, and the target needs idle cycles to complete the last write.
//
// A WAIT repeats the transfer up to `retries` times. With ORUNDETECT, a
// WAIT sets STICKYORUN, and the DP replies FAULT until ABORT.ORUNERRCLR
// clears it. Thus, before each retry, the host writes ABORT with
// ORUNERRCLR, as the OpenOCD bitbang driver does.
//
// With a protocol::Background, the engine calls Background::service()
// before each transfer and each retry, and between the bytes of a
// sequence, at most once per backgroundIntervalMs (protocol/Background.h).
// SWCLK is then high: the call makes that high half period longer.

#include <array>
#include <cstdint>

#include "protocol/Background.h"
#include "protocol/SwdBlock.h"

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

// The core cycles of the loop code in one half period (Release build,
// STM32F103 at 72 MHz with 2 flash wait states). SWCLK must never be
// faster than the table: too small makes SWCLK slower than the table, too
// large makes it faster.
//
// With the engine at -Os, a logic analyzer on the Versaloon Mini gave 21
// (230 kHz at 235 kHz). The engine is now compiled with -O2, and its loop
// code is shorter. 8 is a lower bound from the -O2 disassembly, so SWCLK
// stays below the table. Measure SWCLK at delay 1 again, and correct the
// value. The ST-Link/V2 board has other SWD pins and is not measured.
inline constexpr uint32_t loopOverheadCycles = 8;

// Untimed (delay 0): NOP instructions in each half period. They keep a
// minimum low half period, so SWDIO has a setup time before the rising
// edge, and the target has time to drive SWDIO before the read. With the
// loop code (about 10 cycles per bit at -O2), one bit takes about 24
// cycles: about 3 MHz, as with the original firmware. Not measured yet.
inline constexpr uint32_t untimedPadNops = 6;

// The DP ABORT write (encoded request) and its ORUNERRCLR bit
inline constexpr uint8_t abortRequest = 0x81;
inline constexpr uint32_t orunErrClr = 0x10;

// SWDIO low cycles after each transfer
inline constexpr uint32_t idleCycles = 8;

// Core cycles of a half SWCLK period for an OpenOCD delay value, rounded
// up: SWCLK is never faster than OpenOCD expects.
constexpr uint32_t halfPeriodCycles(uint32_t coreHz, uint16_t delay) {
   if(delay < fixedClockHz.size()) {
      const uint64_t cyclesPerPeriod = 2ull * fixedClockHz[delay];
      return static_cast<uint32_t>((coreHz + cyclesPerPeriod - 1) / cyclesPerPeriod);
   }
   // Half a period is (delay + 1) us.
   return static_cast<uint32_t>((static_cast<uint64_t>(coreHz) * (delay + 1u) + 999'999u) / 1'000'000u);
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

   void configure(uint8_t turnaround, uint16_t retries, uint16_t delay) override {
      turnaround_ = turnaround;
      retries_ = retries;
      halfCycles_ = halfPeriodCycles(pins_.coreHz(), delay);
      waitCycles_ = halfCycles_ > loopOverheadCycles ? halfCycles_ - loopOverheadCycles : 0;
      timed_ = delay != 0;
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

   // False at delay 0: the engine then runs untimed (see the file
   // comment).
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
      pins_.swdioDrive((data[0] & 1u) != 0);
      for(uint32_t i = 0; i < bitCount; ++i) {
         if(i % 8 == 0) {
            background_.poll(pins_);
         }
         writeBit<Timed>(((data[i / 8] >> (i % 8)) & 1u) != 0);
      }
   }

   template <bool Timed>
   void sequenceIn(uint16_t bitCount, art::span<uint8_t> data) {
      begin();
      pins_.swdioRelease();
      for(uint32_t i = 0; i < bitCount; ++i) {
         if(i % 8 == 0) {
            background_.poll(pins_);
         }
         if(readBit<Timed>()) {
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
         if(attempt >= retries_) {
            return static_cast<uint8_t>(ack | retryOutFlag);
         }
         // An ABORT write gets OK also with STICKYORUN set.
         uint32_t clear = orunErrClr;
         static_cast<void>(transferOnce<Timed>(abortRequest, clear));
      }
   }

   void begin() {
      pins_.begin();
      background_.begin(pins_);
   }

   // One transfer. Returns the ACK and the FAULT, ACK error and parity
   // flags.
   template <bool Timed>
   uint8_t transferOnce(uint8_t request, uint32_t &data) {
      pins_.swdioDrive(true);
      writeBits<Timed>(request, 8);
      pins_.swdioRelease();
      clocks<Timed>(turnaround_);
      const auto ack = static_cast<uint8_t>(readBits<Timed>(3));
      uint8_t result = ack;
      if((request & readRequest) != 0) {
         const uint32_t value = readBits<Timed>(32);
         const bool parityBit = readBit<Timed>();
         clocks<Timed>(turnaround_);
         if(ack == ackOk) {
            if(parityBit != parity(value)) {
               result |= parityErrorFlag;
            } else {
               data = value;
            }
         }
      } else {
         clocks<Timed>(turnaround_);
         pins_.swdioDrive((data & 1u) != 0);
         writeBits<Timed>(data, 32);
         writeBit<Timed>(parity(data));
      }
      if(ack == ackFault) {
         result |= faultFlag;
      } else if(ack != ackOk && ack != ackWait) {
         result |= ackErrorFlag;
      }
      pins_.swdioDrive(false);
      writeBits<Timed>(0, idleCycles);
      return result;
   }

   // Waits for the rest of the half period after an edge. Unsigned
   // difference: correct across the wrap of the counter.
   template <bool Timed>
   [[gnu::always_inline]] void waitHalfPeriod() {
      if constexpr(Timed) {
         const uint32_t edge = pins_.now();
         while(pins_.now() - edge < waitCycles_) {
         }
      } else {
         static_assert(untimedPadNops == 6, "Change the NOP count below too");
         asm volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop");
      }
   }

   template <bool Timed>
   [[gnu::always_inline]] void writeBit(bool level) {
      pins_.swclk(false);
      pins_.swdioWrite(level);
      waitHalfPeriod<Timed>();
      pins_.swclk(true);
      waitHalfPeriod<Timed>();
   }

   template <bool Timed>
   [[gnu::always_inline]] bool readBit() {
      pins_.swclk(false);
      waitHalfPeriod<Timed>();
      const bool level = pins_.swdio();
      pins_.swclk(true);
      waitHalfPeriod<Timed>();
      return level;
   }

   template <bool Timed>
   void writeBits(uint32_t value, uint32_t count) {
      for(uint32_t i = 0; i < count; ++i) {
         writeBit<Timed>((value & 1u) != 0);
         value >>= 1;
      }
   }

   template <bool Timed>
   uint32_t readBits(uint32_t count) {
      uint32_t value = 0;
      for(uint32_t i = 0; i < count; ++i) {
         value |= static_cast<uint32_t>(readBit<Timed>()) << i;
      }
      return value;
   }

   // Clock cycles with SWDIO released, for a turnaround
   template <bool Timed>
   void clocks(uint32_t count) {
      for(uint32_t i = 0; i < count; ++i) {
         static_cast<void>(readBit<Timed>());
      }
   }

   Pins &pins_;
   protocol::BackgroundTimer background_;
   // INIT of the SWD block sets the configuration.
   uint8_t turnaround_{ defaultTurnaround };
   uint16_t retries_{ defaultRetries };
   uint32_t halfCycles_{ 0 };
   // halfCycles_ without the loop code
   uint32_t waitCycles_{ 0 };
   bool timed_{ true };
};

} // namespace swd
