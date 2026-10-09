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
// OpenOCD table. At the fastest setting the loop code is slower than the
// half period, the wait is 0, and SWCLK stays below the table.
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
// STM32F103 at 72 MHz with 2 flash wait states). Measured with a logic
// analyzer on the Versaloon Mini (the ST-Link/V2 board has other SWD pins
// and is not measured). With the value 12, SWCLK was 220 kHz at 235 kHz.
// That is 163.6 cycles per half period, thus 21.6 cycles of loop code. The
// high half has about 33 cycles and the low half about 14. SWCLK must never
// be faster than the table, thus the value is rounded down. Too small makes
// SWCLK slower than the table, too large makes it faster. Results with 21:
// 230 kHz at 235 kHz (53 % high), and 1.68 MHz at 1.85 MHz (71 % high),
// where the wait is 0.
inline constexpr uint32_t loopOverheadCycles = 21;

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
   }

   void sequenceOut(uint16_t bitCount, art::span<const uint8_t> data) override {
      if(bitCount == 0) {
         return;
      }
      begin();
      pins_.swdioDrive((data[0] & 1u) != 0);
      for(uint32_t i = 0; i < bitCount; ++i) {
         if(i % 8 == 0) {
            background_.poll(pins_);
         }
         writeBit(((data[i / 8] >> (i % 8)) & 1u) != 0);
      }
   }

   void sequenceIn(uint16_t bitCount, art::span<uint8_t> data) override {
      begin();
      pins_.swdioRelease();
      for(uint32_t i = 0; i < bitCount; ++i) {
         if(i % 8 == 0) {
            background_.poll(pins_);
         }
         if(readBit()) {
            data[i / 8] = static_cast<uint8_t>(data[i / 8] | 1u << (i % 8));
         }
      }
   }

   uint8_t transfer(uint8_t request, uint32_t &data) override {
      begin();
      for(uint32_t attempt = 0;; ++attempt) {
         background_.poll(pins_);
         const uint8_t ack = transferOnce(request, data);
         if(ack != ackWait) {
            return ack;
         }
         if(attempt >= retries_) {
            return static_cast<uint8_t>(ack | retryOutFlag);
         }
         // An ABORT write gets OK also with STICKYORUN set.
         uint32_t clear = orunErrClr;
         static_cast<void>(transferOnce(abortRequest, clear));
      }
   }

   [[nodiscard]] uint32_t halfCycles() const {
      return halfCycles_;
   }

   private:
   void begin() {
      pins_.begin();
      background_.begin(pins_);
   }

   // One transfer. Returns the ACK and the FAULT, ACK error and parity
   // flags.
   uint8_t transferOnce(uint8_t request, uint32_t &data) {
      pins_.swdioDrive(true);
      writeBits(request, 8);
      pins_.swdioRelease();
      clocks(turnaround_);
      const auto ack = static_cast<uint8_t>(readBits(3));
      uint8_t result = ack;
      if((request & readRequest) != 0) {
         const uint32_t value = readBits(32);
         const bool parityBit = readBit();
         clocks(turnaround_);
         if(ack == ackOk) {
            if(parityBit != parity(value)) {
               result |= parityErrorFlag;
            } else {
               data = value;
            }
         }
      } else {
         clocks(turnaround_);
         pins_.swdioDrive((data & 1u) != 0);
         writeBits(data, 32);
         writeBit(parity(data));
      }
      if(ack == ackFault) {
         result |= faultFlag;
      } else if(ack != ackOk && ack != ackWait) {
         result |= ackErrorFlag;
      }
      pins_.swdioDrive(false);
      writeBits(0, idleCycles);
      return result;
   }

   // Waits for the rest of the half period after an edge. Unsigned
   // difference: correct across the wrap of the counter.
   [[gnu::always_inline]] void waitHalfPeriod() {
      const uint32_t edge = pins_.now();
      while(pins_.now() - edge < waitCycles_) {
      }
   }

   [[gnu::always_inline]] void writeBit(bool level) {
      pins_.swclk(false);
      pins_.swdioWrite(level);
      waitHalfPeriod();
      pins_.swclk(true);
      waitHalfPeriod();
   }

   [[gnu::always_inline]] bool readBit() {
      pins_.swclk(false);
      waitHalfPeriod();
      const bool level = pins_.swdio();
      pins_.swclk(true);
      waitHalfPeriod();
      return level;
   }

   void writeBits(uint32_t value, uint32_t count) {
      for(uint32_t i = 0; i < count; ++i) {
         writeBit((value & 1u) != 0);
         value >>= 1;
      }
   }

   uint32_t readBits(uint32_t count) {
      uint32_t value = 0;
      for(uint32_t i = 0; i < count; ++i) {
         value |= static_cast<uint32_t>(readBit()) << i;
      }
      return value;
   }

   // Clock cycles with SWDIO released, for a turnaround
   void clocks(uint32_t count) {
      for(uint32_t i = 0; i < count; ++i) {
         static_cast<void>(readBit());
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
};

} // namespace swd
