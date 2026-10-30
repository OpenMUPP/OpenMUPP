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
//   bool oneWireFrame(uint64_t bits, uint32_t count, uint32_t readFrom,
//                     uint32_t &samples);
//                              one wire: runs a frame (see below)
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
//   11-14      handshake: 11 low, 12 released,            11: target
//              13 low, 14 released                        13: host
//   15-46      data, MSB first                            host (write), target (read)
//   47         parity of the data                         host (write), target (read)
//   48         the level of bit 47, then released         host (write), target (read)
//   49-52      handshake: 49 low, 50 released,            49: target
//              51 high, 52 released                       51: host
//   53         0                                          host
//   stop       SWCLK high, then SWDIO rises
//
// The low phase before bits 1, 15 and 53 is two half periods, as on the
// WCH-LinkE: SWDIO changes direction there. A target that sees bit 13
// high drops the frame: it drives no more bits (CH32V208). head() and
// tail() give the line levels of the handshake bits.
//
// Each SWCLK edge and each SWDIO change of a condition comes at a deadline
// of a ClockPacer (ClockPacer.h), as in the SWD engine: the loop code does
// not change the rate, and an interrupt makes a half period longer, never
// shorter. The interrupts stay on.
//
// One wire: a frame is 41 bits, MSB first: a start bit 1, the 7-bit
// address, 1 for a write or 0 for a read, and 32 data bits. A bit is a low
// pulse and a high level. A short low pulse is a 1, a long low pulse is a
// 0. Idle is SWDIO driven high: a pull-up is too slow for the high level
// of 370 ns (docs/wch-link.md, "One-wire timer").
//
// The width of a low pulse is the bit value, so code timing cannot make
// the bits: on a dongle, waits on the cycle counter gave low pulses of
// 120 to 530 ns for a 1, and the CH32V003 did not answer. Thus the Pins
// type runs each frame with a hardware timer (board/BoardRvSwd.cpp), and
// the engine gives it only the bits. oneWireFrame() sends `count` bits,
// the first in bit count-1 of `bits`:
//
// - Write bits: a low pulse of oneWireShortLowNs for a 1 or
//   oneWireLongLowNs for a 0, then oneWireHighNs high. The host drives
//   both levels.
// - The bit before readFrom has a high level of oneWireTurnaroundHighNs.
// - Read bits (from readFrom on): the host drives a low pulse of
//   oneWireShortLowNs and then releases SWDIO (open drain). The target
//   holds the line low for a 0. The host samples SWDIO at oneWireSampleNs
//   after the falling edge. A read bit lasts oneWireReadBitNs.
// - Before the first bit, SWDIO is high for oneWireLeadNs.
//
// samples gets the read bits, the first in the MSB. SWDIO is driven high
// before the frame. After a frame without read bits, it is driven high.
// After read bits, it is released. false: the timer did not end the
// frame, or the core clock is below oneWireMinClockHz and no bit went out
// (frameFlag). SWDIO is then released.
//
// While SWDIO is released (after read bits, or after a reset with the
// line held low), the engine drives SWDIO again only when the line is
// high. A line that stays low for oneWireReleaseTimeoutNs gives
// lineLowFlag, and SWDIO stays released. Thus the host never drives the
// line high against a target that holds it low. The interrupts are off
// for each frame (about 50 us): the switch to open drain after the bit
// before readFrom is code, with a window of about 1 us.
//
// Line reset: two wires, the clocks of the WCH-LinkE before its first
// frame: lineResetHighClocks clocks with SWDIO high, one clock with
// SWDIO low, lineResetTailClocks clocks with SWDIO high. SWDIO changes
// while SWCLK is low. One wire, oneWireResetLowUs with SWDIO low. That
// these sequences reset the line is not confirmed. The probe also sends
// other patterns at connect (the 85-clock frames). Their meaning is
// open, and the engine does not send them.
//
// With a protocol::Background, the engine calls Background::service()
// before each frame (two wires: SWCLK low), and in the low level of the
// one-wire reset, at most once per backgroundIntervalMs
// (protocol/Background.h). The interrupts are on then. The engine runs
// in a command, inside UsbStack::poll(): the service must not start a
// command, and must return within some microseconds.

#include <bit>
#include <cstdint>

#include "ClockPacer.h"
#include "IntDiv.h"

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
// One wire: the timer did not end the frame, or the core clock is below
// oneWireMinClockHz.
inline constexpr uint8_t frameFlag = 0x04;

// A DM register address has 7 bits.
inline constexpr uint8_t addressMask = 0x7F;

// Two wires: the measured SWCLK rates of the WCH-LinkE. All frames at low
// speed and the reads at the default speed use a period of 2.09 us. The
// writes at the default speed use 1.10 us.
inline constexpr uint32_t lowClockHz = 478'000;
inline constexpr uint32_t highClockHz = 907'000;
inline constexpr uint32_t defaultClockHz = lowClockHz;

// Two wires: the clocks of a line reset, as the WCH-LinkE sends them
// before its first frame: 9 with SWDIO high, 1 with SWDIO low, 90 with
// SWDIO high.
inline constexpr uint32_t lineResetHighClocks = 9;
inline constexpr uint32_t lineResetTailClocks = 90;
inline constexpr uint32_t lineResetClocks = lineResetHighClocks + 1 + lineResetTailClocks;

// One wire: the times of the WCH-LinkE (docs/wch-link-protocol.md)
inline constexpr uint32_t oneWireShortLowNs = 250;
inline constexpr uint32_t oneWireLongLowNs = 860;
inline constexpr uint32_t oneWireHighNs = 370;
inline constexpr uint32_t oneWireTurnaroundHighNs = 750;
// The sample compare. The DMA reads the line 80 to 210 ns later (measured,
// board/BoardRvSwd.cpp): between the longest 1 (374 ns) and the shortest
// 0 (824 ns) of the target.
inline constexpr uint32_t oneWireSampleNs = 450;
// The longest 0 of the target (868 ns) and a high level. The WCH-LinkE
// waits for the line instead; the CH32V003 accepted highs up to 1.46 us.
inline constexpr uint32_t oneWireReadBitNs = 1'300;
inline constexpr uint32_t oneWireLeadNs = 1'000;
// A frame: start bit, 7 address bits and the write bit, then 32 data bits
inline constexpr uint32_t oneWireHeadBits = 9;
inline constexpr uint32_t oneWireFrameBits = oneWireHeadBits + 32;
inline constexpr uint32_t oneWireReleaseTimeoutNs = 5'000;
// The shortest gap is 3.7 us.
inline constexpr uint32_t oneWireFrameGapNs = 4'000;
inline constexpr uint32_t oneWireResetLowUs = 2'000;

// Core cycles of a time, rounded up. The result must fit in 32 bits.
constexpr uint32_t cyclesOfNs(uint32_t coreHz, uint32_t ns) {
   return intdiv::mulDivCeil(coreHz, ns, 1'000'000'000u);
}

// Core cycles of a half SWCLK period, rounded up: SWCLK is never faster
// than the frequency. 0 Hz gives the slowest clock (1 Hz). coreHz / (2 * hz)
// rounded up, in two 32-bit steps: ceil(ceil(a / b) / c) is ceil(a / (b * c)).
constexpr uint32_t halfPeriodCycles(uint32_t coreHz, uint32_t hz) {
   return intdiv::ceilDiv(intdiv::ceilDiv(coreHz, 2u), hz == 0 ? 1u : hz);
}

// 1 for an odd number of 1 bits
constexpr bool parity(uint32_t value) {
   return (std::popcount(value) & 1) != 0;
}

// One wire: the layout of a frame in timer ticks, for the board timer
// (board/BoardRvSwd.cpp) and the mock target. The times are above.
struct OneWireTicks {
   uint32_t shortLow;
   uint32_t longLow;
   uint32_t high;
   uint32_t turnaroundHigh;
   uint32_t readBit;
   uint32_t sample;
   uint32_t lead;
};

constexpr OneWireTicks oneWireTicks(uint32_t hz) {
   return { cyclesOfNs(hz, oneWireShortLowNs), cyclesOfNs(hz, oneWireLongLowNs),
            cyclesOfNs(hz, oneWireHighNs),     cyclesOfNs(hz, oneWireTurnaroundHighNs),
            cyclesOfNs(hz, oneWireReadBitNs),  cyclesOfNs(hz, oneWireSampleNs),
            cyclesOfNs(hz, oneWireLeadNs) };
}

// The clock of the measured setup (Mini, TIM3). The DMA burst that
// loads a bit must end within the bit before; at a slower clock it can
// end late, and each later bit then has the times of the bit before it.
inline constexpr uint32_t oneWireMinClockHz = 72'000'000;

// The low pulse and the period of bit i
struct OneWireBit {
   uint32_t low;
   uint32_t period;
};

constexpr OneWireBit oneWireBit(const OneWireTicks &t, uint64_t bits, uint32_t count, uint32_t readFrom, uint32_t i) {
   if(i >= readFrom) {
      return { t.shortLow, t.readBit };
   }
   const uint32_t low = (bits >> (count - 1 - i) & 1u) != 0 ? t.shortLow : t.longLow;
   const bool turnaround = readFrom < count && i + 1 == readFrom;
   return { low, low + (turnaround ? t.turnaroundHigh : t.high) };
}

// From the counter start (the start of the lead-in): the switch to open
// drain, in the high level after bit readFrom-1 (0 without read bits), and
// the end, after the last bit and before the sample compare of the end
// period.
struct OneWireTimes {
   uint32_t switchAt;
   uint32_t endAt;
};

constexpr OneWireTimes oneWireTimes(const OneWireTicks &t, uint64_t bits, uint32_t count, uint32_t readFrom) {
   uint32_t start = t.lead;
   uint32_t switchAt = 0;
   for(uint32_t i = 0; i < count; ++i) {
      const OneWireBit bit = oneWireBit(t, bits, count, readFrom, i);
      if(readFrom < count && i + 1 == readFrom) {
         switchAt = start + bit.low + t.high / 2u;
      }
      start += bit.period;
   }
   return { switchAt, start + t.sample / 2u };
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
      pacer_.setHalfCycles(halfCycles_);
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
            pins_.swdioWrite(i != lineResetHighClocks);
            waitHalfPeriod();
            swclk(true);
            waitHalfPeriod();
            swclk(false);
         }
         pins_.swdioWrite(true);
         halfPeriod();
      } else {
         // Driving low is safe also when the target holds the line low.
         pins_.swdioDrive(false);
         const uint32_t edge = pins_.now();
         const uint32_t low = cyclesOfNs(pins_.coreHz(), oneWireResetLowUs * 1000u);
         while(pins_.now() - edge < low) {
            background_.poll(pins_);
         }
         pins_.swdioRelease();
         released_ = true;
         static_cast<void>(driveIfHigh(pins_.now()));
         waitFrom(pins_.now(), frameGap_);
      }
   }

   // DMI write. Returns the flags (0: no error). The flags do not check
   // the target: a two-wire write without a target also returns 0 (with
   // bit 11 and bit 49 high). The caller must check head() and tail().
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

   // Two wires: the handshake bits 11-14 and 49-52 of the last frame,
   // the first bit in bit 3. One wire: 0.
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

   // Each bit ends with SWCLK low: a wait before each edge, and the loop
   // code after it.
   uint8_t frameTwo(uint32_t request, bool write, uint32_t &data) {
      // Start condition, from idle (SWCLK low, SWDIO high)
      waitHalfPeriod();
      swclk(true);
      waitHalfPeriod();
      pins_.swdioWrite(false);
      pacer_.edge(pins_);
      waitHalfPeriod();
      swclk(false);
      // Bits 1-9: a longer low phase before bit 1
      halfPeriod();
      writeBits(request << 1 | (parity(request) ? 1u : 0u), 9);
      // Bit 10: the level of bit 9, then the target drives.
      holdAndRelease(parity(request));
      head_ = handshake(false);
      uint8_t flags = 0;
      // Bits 15-48: the data phase, with a longer low phase before bit 15
      halfPeriod();
      if(write) {
         pins_.swdioDrive((data & 0x8000'0000u) != 0);
         writeBits(data, 32);
         writeBit(parity(data));
         holdAndRelease(parity(data));
      } else {
         data = readBits(32);
         if(readBit() != parity(data)) {
            flags |= parityErrorFlag;
         }
         static_cast<void>(readBit());
      }
      tail_ = handshake(true);
      // Bit 53 and the stop condition, with a longer low phase before bit 53
      halfPeriod();
      pins_.swdioDrive(false);
      waitHalfPeriod();
      swclk(true);
      waitHalfPeriod();
      pins_.swdioWrite(true);
      pacer_.edge(pins_);
      waitHalfPeriod();
      swclk(false);
      return flags;
   }

   // Waits for the end of the half period. An edge must follow.
   [[gnu::always_inline]] void waitHalfPeriod() {
      pacer_.wait(pins_);
   }

   // The SWCLK edge after waitHalfPeriod()
   [[gnu::always_inline]] void swclk(bool level) {
      pins_.swclk(level);
      pacer_.edge(pins_);
   }

   // A half period without an edge: the longer low phases
   void halfPeriod() {
      waitHalfPeriod();
      pacer_.edge(pins_);
   }

   // SWCLK is low before each bit.
   [[gnu::always_inline]] void writeBit(bool level) {
      pins_.swdioWrite(level);
      waitHalfPeriod();
      swclk(true);
      waitHalfPeriod();
      swclk(false);
   }

   [[gnu::always_inline]] bool readBit() {
      waitHalfPeriod();
      const bool level = pins_.swdio();
      swclk(true);
      waitHalfPeriod();
      swclk(false);
      return level;
   }

   // Bits 11-14 and 49-52, SWDIO released before. The target drives the
   // first bit, the probe drives the third bit (13: low, 51: high), and
   // SWDIO is released for the second and the fourth bit (analog capture
   // of the WCH-LinkE). Returns the line levels, the first bit in bit 3.
   uint8_t handshake(bool probeLevel) {
      uint32_t value = readBits(2);
      pins_.swdioDrive(probeLevel);
      value = value << 1 | (readBit() ? 1u : 0u);
      pins_.swdioRelease();
      value = value << 1 | (readBit() ? 1u : 0u);
      return static_cast<uint8_t>(value);
   }

   // A clock with SWDIO unchanged at the level, then SWDIO released. A
   // high level is released after the rising edge, as on the WCH-LinkE. A
   // low level is released after the falling edge: a strong pull-up
   // (2.2 kOhm) raises the line at once, and a rise while SWCLK is high is
   // a stop condition. The target drives the next bit low.
   void holdAndRelease(bool level) {
      waitHalfPeriod();
      swclk(true);
      if(level) {
         pins_.swdioRelease();
      }
      waitHalfPeriod();
      swclk(false);
      if(!level) {
         pins_.swdioRelease();
      }
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
      if(!driveIfHigh(pins_.now())) {
         pins_.unlockIrq(irq);
         return lineLowFlag;
      }
      // Start bit, address, write bit. A read sends 1s (short pulses) in
      // the data bits and samples them.
      const uint64_t head = 1u << 8 | request;
      const uint64_t bits = head << 32 | (write ? data : 0xFFFF'FFFFu);
      uint32_t samples = 0;
      uint8_t flags = 0;
      if(!pins_.oneWireFrame(bits, oneWireFrameBits, write ? oneWireFrameBits : oneWireHeadBits, samples)) {
         released_ = true;
         flags = frameFlag;
      } else if(!write) {
         released_ = true;
         data = samples;
         if(!driveIfHigh(pins_.now())) {
            flags = lineLowFlag;
         }
      }
      pins_.unlockIrq(irq);
      waitFrom(pins_.now(), frameGap_);
      return flags;
   }

   // While SWDIO is released: drives it high when the line is high.
   // Returns false when the line stays low (SWDIO stays released).
   bool driveIfHigh(uint32_t start) {
      if(!released_) {
         return true;
      }
      while(!pins_.swdio()) {
         if(pins_.now() - start >= releaseTimeout_) {
            return false;
         }
      }
      pins_.swdioDrive(true);
      released_ = false;
      return true;
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
   pacing::ClockPacer pacer_;
   // One wire, in core cycles
   uint32_t releaseTimeout_{ 0 };
   uint32_t frameGap_{ 0 };
   uint8_t head_{ 0 };
   uint8_t tail_{ 0 };
   // One wire: SWDIO is released after an error.
   bool released_{ false };
};

} // namespace rvswd
