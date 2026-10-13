#pragma once

// RISC-V debug wire engine of the WCH-Link personality: DMI accesses (debug
// module interface, RISC-V debug spec 0.13) on the two-wire interface
// (RVSWD, CH32V20x and CH32V30x) and the one-wire interface (SDI,
// CH32V00x). Hardware-independent, as the SWD engine (swd/SwdBitBang.h): a
// Pins type gives the pin access, so the host unit tests run the frames on
// mock pins with a mock target (tests/MockRvSwdTarget.h). On target,
// board/BoardRvSwd.h gives FastPin access. The frame formats and the
// timing come from the captures of a WCH-LinkE (docs/wch-link-protocol.md,
// "Wire frame format").
//
// The Pins type has the members of the SWD engine, with other idle levels,
// and an interrupt lock:
//
//   void init();               SWCLK output low, SWDIO driven high
//   void fini();               SWCLK and SWDIO inputs
//   void begin();              before each operation: make now() count
//   void swclk(bool);
//   void swdioDrive(bool);     SWDIO output at the level
//   void swdioRelease();       SWDIO input (with pull-up)
//   void swdioWrite(bool);     SWDIO level while it is an output
//   bool swdio();              SWDIO level
//   uint32_t now();            free-running core cycle counter
//   uint32_t coreHz();         core clock frequency
//   uint32_t lockIrq();        interrupts off, returns the old state
//   void unlockIrq(uint32_t);  restores the state of lockIrq()
//
// Two wires: a frame is a start condition, 53 clocks and a stop condition,
// as on an I2C bus. SWDIO is stable at the rising edge of SWCLK. Idle is
// SWCLK low and SWDIO high.
//
//   start      SWCLK high, then SWDIO falls
//   1-7        DM register address, MSB first             host
//   8          1: write, 0: read                          host
//   9          parity of bits 1-8 (1: odd number of 1s)   host
//   10         the level of bit 9, then SWDIO released    host
//   11-14      response bits                              target
//   15-46      data, MSB first                            host (write), target (read)
//   47         parity of the data                         host (write), target (read)
//   48         the level of bit 47, then released         host (write), target (read)
//   49-52      tail bits                                  target
//   53         0                                          host
//   stop       SWCLK high, then SWDIO rises
//
// The low phase before bits 1, 15 and 53 is two half periods, as on the
// WCH-LinkE: SWDIO changes direction there. The meaning of the response
// and the tail bits is open. head() and tail() give them, and W3 decides
// what they mean.
//
// Each half period waits from its SWCLK edge, as in the SWD engine: an
// interrupt makes a half period longer, never shorter. The interrupts stay
// on.
//
// One wire: a frame is 41 bits, MSB first: a start bit 1, the 7-bit
// address, 1 for a write or 0 for a read, and 32 data bits. A bit is a low
// pulse and a high level. A short low pulse is a 1, a long low pulse is a
// 0. Idle is SWDIO driven high: a pull-up is too slow for the high level
// of 370 ns (docs/wch-link-protocol.md, "Pins and USB ID").
//
// - Write bits: the host drives the low pulse and the high level.
// - Read bits: the host drives a short low pulse and releases SWDIO. The
//   target holds the line low for a 0. The host samples SWDIO at
//   oneWireSampleNs after the falling edge. When the line is high again,
//   the host drives it high for the high level. A line that stays low for
//   oneWireReleaseTimeoutNs ends the frame with lineLowFlag, and SWDIO
//   stays released.
// - After bit 8 of a read, the high level is oneWireTurnaroundHighNs.
//
// An interrupt in a one-wire bit makes the low pulse of a 1 long enough
// for a 0. Thus the interrupts are off for each one-wire frame (about
// 45 us). Each phase waits from its edge, so the pulses are longer than
// the constants by the time of the loop code. The constants are the
// WCH-LinkE values. They are not measured on a dongle.
//
// Line reset: two wires, lineResetClocks clocks with SWDIO high. One
// wire, oneWireResetLowUs with SWDIO low. The WCH-LinkE sends both before
// its first frame. That they reset the line is not confirmed. The probe
// also sends other patterns at connect (the 85-clock frames). Their
// meaning is open, and the engine does not send them.
//
// With a protocol::Background, the engine calls Background::service()
// before each frame and in the reset, at most once per
// backgroundIntervalMs (protocol/Background.h). The interrupts are on
// then: the service can run USB (UsbStack::poll()).

#include <bit>
#include <cstdint>

#include "protocol/Background.h"

namespace rvswd {

enum class Wire : uint8_t {
   // SDI, CH32V00x
   One,
   // RVSWD, CH32V20x and CH32V30x
   Two,
};

// The flags of a DMI access. 0 is no error.
// Two wires: the parity bit of the read data is wrong.
inline constexpr uint8_t parityErrorFlag = 0x01;
// One wire: the line stays low after a read bit.
inline constexpr uint8_t lineLowFlag = 0x02;

// A DM register address has 7 bits.
inline constexpr uint8_t addressMask = 0x7F;

// Two wires: the measured SWCLK rates of the WCH-LinkE. All frames at low
// speed and the reads at the default speed use a period of 2.09 us. The
// writes at the default speed use 1.10 us.
inline constexpr uint32_t lowClockHz = 478'000;
inline constexpr uint32_t highClockHz = 907'000;
inline constexpr uint32_t defaultClockHz = lowClockHz;

// Two wires: the clocks of a line reset. The WCH-LinkE sends 90 before its
// first frame.
inline constexpr uint32_t lineResetClocks = 100;

// Two wires: the core cycles of the loop code in one half period. The
// value of the SWD engine (swd/SwdBitBang.h). Not measured for this engine.
inline constexpr uint32_t loopOverheadCycles = 24;

// One wire: the times of the WCH-LinkE (docs/wch-link-protocol.md)
inline constexpr uint32_t oneWireShortLowNs = 250;
inline constexpr uint32_t oneWireLongLowNs = 860;
inline constexpr uint32_t oneWireHighNs = 370;
inline constexpr uint32_t oneWireTurnaroundHighNs = 750;
// Between the longest 1 (374 ns) and the shortest 0 (824 ns) of the target
inline constexpr uint32_t oneWireSampleNs = 600;
inline constexpr uint32_t oneWireReleaseTimeoutNs = 5'000;
// The shortest gap is 3.7 us.
inline constexpr uint32_t oneWireFrameGapNs = 4'000;
inline constexpr uint32_t oneWireResetLowUs = 2'000;

// Core cycles of a time, rounded up
constexpr uint32_t cyclesOfNs(uint32_t coreHz, uint32_t ns) {
   return static_cast<uint32_t>((static_cast<uint64_t>(coreHz) * ns + 999'999'999u) / 1'000'000'000u);
}

// Core cycles of a half SWCLK period, rounded up: SWCLK is never faster
// than the frequency. 0 Hz gives the slowest clock (1 Hz).
constexpr uint32_t halfPeriodCycles(uint32_t coreHz, uint32_t hz) {
   const uint64_t cyclesPerPeriod = 2ull * (hz == 0 ? 1u : hz);
   return static_cast<uint32_t>((coreHz + cyclesPerPeriod - 1) / cyclesPerPeriod);
}

// 1 for an odd number of 1 bits
constexpr bool parity(uint32_t value) {
   return (std::popcount(value) & 1) != 0;
}

template <typename Pins>
class RvSwdEngine {
   public:
   // background: serviced before the frames, nullptr for none.
   explicit RvSwdEngine(Pins &pins, protocol::Background *background = nullptr)
      : pins_(pins)
      , background_(background) {
   }

   // The pins at their idle levels, and the wire of the next frames
   void init(Wire wire) {
      wire_ = wire;
      pins_.init();
      setClockHz(clockHz_);
      const uint32_t coreHz = pins_.coreHz();
      shortLow_ = cyclesOfNs(coreHz, oneWireShortLowNs);
      longLow_ = cyclesOfNs(coreHz, oneWireLongLowNs);
      high_ = cyclesOfNs(coreHz, oneWireHighNs);
      turnaroundHigh_ = cyclesOfNs(coreHz, oneWireTurnaroundHighNs);
      sample_ = cyclesOfNs(coreHz, oneWireSampleNs);
      releaseTimeout_ = cyclesOfNs(coreHz, oneWireReleaseTimeoutNs);
      frameGap_ = cyclesOfNs(coreHz, oneWireFrameGapNs);
   }

   void fini() {
      pins_.fini();
   }

   [[nodiscard]] Wire wire() const {
      return wire_;
   }

   // Two wires: SWCLK in Hz, rounded down to the nearest available rate.
   // One wire: no effect (the WCH-LinkE uses one rate for all speeds).
   void setClockHz(uint32_t hz) {
      clockHz_ = hz;
      halfCycles_ = halfPeriodCycles(pins_.coreHz(), hz);
      waitCycles_ = halfCycles_ > loopOverheadCycles ? halfCycles_ - loopOverheadCycles : 0;
   }

   [[nodiscard]] uint32_t halfCycles() const {
      return halfCycles_;
   }

   void lineReset() {
      begin();
      background_.poll(pins_);
      if(wire_ == Wire::Two) {
         pins_.swdioDrive(true);
         for(uint32_t i = 0; i < lineResetClocks; ++i) {
            pins_.swclk(true);
            waitHalfPeriod();
            pins_.swclk(false);
            waitHalfPeriod();
         }
      } else {
         pins_.swdioDrive(false);
         const uint32_t edge = pins_.now();
         const uint32_t low = cyclesOfNs(pins_.coreHz(), oneWireResetLowUs * 1000u);
         while(pins_.now() - edge < low) {
            background_.poll(pins_);
         }
         pins_.swdioWrite(true);
         waitFrom(pins_.now(), frameGap_);
      }
   }

   // DMI write. Returns the flags (0: no error).
   uint8_t write(uint8_t address, uint32_t data) {
      return frame(address, true, data);
   }

   // DMI read. The data changes only without an error.
   uint8_t read(uint8_t address, uint32_t &data) {
      uint32_t value = 0;
      const uint8_t flags = frame(address, false, value);
      if(flags == 0) {
         data = value;
      }
      return flags;
   }

   // Two wires: the response bits (11-14) and the tail bits (49-52) of the
   // last frame, the first bit in bit 3. One wire: 0.
   [[nodiscard]] uint8_t head() const {
      return head_;
   }
   [[nodiscard]] uint8_t tail() const {
      return tail_;
   }

   private:
   void begin() {
      pins_.begin();
      background_.begin(pins_);
   }

   uint8_t frame(uint8_t address, bool write, uint32_t &data) {
      begin();
      background_.poll(pins_);
      head_ = 0;
      tail_ = 0;
      const uint32_t request = static_cast<uint32_t>(address & addressMask) << 1 | (write ? 1u : 0u);
      return wire_ == Wire::Two ? frameTwo(request, write, data) : frameOne(request, write, data);
   }

   // Two wires

   uint8_t frameTwo(uint32_t request, bool write, uint32_t &data) {
      // Start condition, from idle (SWCLK low, SWDIO high)
      pins_.swclk(true);
      waitFullHalfPeriod();
      pins_.swdioWrite(false);
      waitFullHalfPeriod();
      // Bits 1-9: a longer low phase before bit 1
      lowPhase();
      writeBits(request << 1 | (parity(request) ? 1u : 0u), 9);
      // Bit 10: the level of bit 9, then the target drives.
      holdAndRelease();
      head_ = static_cast<uint8_t>(readBits(4));
      uint8_t flags = 0;
      // Bits 15-48: the data phase, with a longer low phase before bit 15
      lowPhase();
      if(write) {
         pins_.swdioDrive((data & 0x8000'0000u) != 0);
         writeBits(data, 32);
         writeBit(parity(data));
         holdAndRelease();
      } else {
         data = readBits(32);
         if(readBit() != parity(data)) {
            flags |= parityErrorFlag;
         }
         static_cast<void>(readBit());
      }
      tail_ = static_cast<uint8_t>(readBits(4));
      // Bit 53 and the stop condition, with a longer low phase before bit 53
      lowPhase();
      pins_.swdioDrive(false);
      waitFullHalfPeriod();
      pins_.swclk(true);
      waitFullHalfPeriod();
      pins_.swdioWrite(true);
      waitFullHalfPeriod();
      pins_.swclk(false);
      return flags;
   }

   // Waits for the rest of the half period after an edge. Unsigned
   // difference: correct across the wrap of the counter.
   [[gnu::always_inline]] void waitHalfPeriod() {
      const uint32_t edge = pins_.now();
      while(pins_.now() - edge < waitCycles_) {
      }
   }

   // The half periods of the start and the stop condition and of
   // lowPhase() have no loop code: they wait for the full half period.
   void waitFullHalfPeriod() {
      waitFrom(pins_.now(), halfCycles_);
   }

   // SWCLK low for an extra half period, before the bit that follows
   void lowPhase() {
      pins_.swclk(false);
      waitFullHalfPeriod();
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

   // A clock with SWDIO unchanged, then SWDIO released after the rising
   // edge
   void holdAndRelease() {
      pins_.swclk(false);
      waitHalfPeriod();
      pins_.swclk(true);
      pins_.swdioRelease();
      waitHalfPeriod();
   }

   // MSB first
   void writeBits(uint32_t value, uint32_t count) {
      for(uint32_t i = count; i > 0; --i) {
         writeBit(((value >> (i - 1)) & 1u) != 0);
      }
   }

   // MSB first
   uint32_t readBits(uint32_t count) {
      uint32_t value = 0;
      for(uint32_t i = 0; i < count; ++i) {
         value = value << 1 | (readBit() ? 1u : 0u);
      }
      return value;
   }

   // One wire

   uint8_t frameOne(uint32_t request, bool write, uint32_t &data) {
      const uint32_t irq = pins_.lockIrq();
      // The start bit drives SWDIO: it can be released after an error.
      pins_.swdioDrive(true);
      sendBit(true, high_);
      for(uint32_t i = 8; i > 1; --i) {
         sendBit(((request >> (i - 1)) & 1u) != 0, high_);
      }
      uint8_t flags = 0;
      if(write) {
         sendBit(true, high_);
         for(uint32_t i = 32; i > 0; --i) {
            sendBit(((data >> (i - 1)) & 1u) != 0, high_);
         }
      } else {
         sendBit(false, turnaroundHigh_);
         uint32_t value = 0;
         for(uint32_t i = 0; i < 32 && flags == 0; ++i) {
            bool level = false;
            flags = receiveBit(level);
            value = value << 1 | (level ? 1u : 0u);
         }
         data = value;
      }
      pins_.unlockIrq(irq);
      waitFrom(pins_.now(), frameGap_);
      return flags;
   }

   void sendBit(bool level, uint32_t high) {
      pins_.swdioWrite(false);
      waitFrom(pins_.now(), level ? shortLow_ : longLow_);
      pins_.swdioWrite(true);
      waitFrom(pins_.now(), high);
   }

   // Returns the flags.
   uint8_t receiveBit(bool &level) {
      pins_.swdioWrite(false);
      const uint32_t edge = pins_.now();
      waitFrom(edge, shortLow_);
      pins_.swdioRelease();
      waitFrom(edge, sample_);
      level = pins_.swdio();
      while(!pins_.swdio()) {
         if(pins_.now() - edge >= releaseTimeout_) {
            return lineLowFlag;
         }
      }
      pins_.swdioDrive(true);
      waitFrom(pins_.now(), high_);
      return 0;
   }

   // Waits until `cycles` after `edge`.
   [[gnu::always_inline]] void waitFrom(uint32_t edge, uint32_t cycles) {
      while(pins_.now() - edge < cycles) {
      }
   }

   Pins &pins_;
   protocol::BackgroundTimer background_;
   Wire wire_{ Wire::Two };
   uint32_t clockHz_{ defaultClockHz };
   uint32_t halfCycles_{ 0 };
   // halfCycles_ without the loop code
   uint32_t waitCycles_{ 0 };
   // One wire, in core cycles
   uint32_t shortLow_{ 0 };
   uint32_t longLow_{ 0 };
   uint32_t high_{ 0 };
   uint32_t turnaroundHigh_{ 0 };
   uint32_t sample_{ 0 };
   uint32_t releaseTimeout_{ 0 };
   uint32_t frameGap_{ 0 };
   uint8_t head_{ 0 };
   uint8_t tail_{ 0 };
};

} // namespace rvswd
