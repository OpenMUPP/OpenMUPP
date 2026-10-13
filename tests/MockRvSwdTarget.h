#pragma once

// Mock pins for the RISC-V debug wire engine (rvswd/RvSwdEngine.h), with a
// mock target that has 128 DM registers. The target decodes the frames of
// the host as the WCH-LinkE captures show them (docs/wch-link-protocol.md),
// replies to reads, and records each access.
//
// Two wires: the target sees a start condition and a stop condition only
// when the host drives SWDIO while SWCLK is high. It samples SWDIO at the
// rising edges of SWCLK. It drives its bits from the falling edge before
// the bit and releases SWDIO after the rising edge of its last bit, as the
// captures show. Each other change of SWDIO by the host while SWCLK is
// high, a wrong bit count, a wrong address parity, a wrong repeat bit and
// a bit 53 that is not 0 count as a protocol error. 50 clocks with SWDIO
// high from the host outside a frame are a line reset.
//
// One wire: the target measures the low pulses of the host. A pulse
// shorter than 500 ns is a 1. In the 32 data bits of a read, the target
// holds the line low after the falling edge of the host for a 0 bit,
// until 860 ns after that edge. A low level longer than 1.5 us is a line
// reset. The host must change the line in a frame only with the
// interrupts locked.
//
// SWDIO has a pull-up: it reads 1 when nobody drives it. The mock counts
// the times that the host and the target drive SWDIO at the same time, and
// the bits that the target samples while the host does not drive.
//
// onRead and onWrite connect a DM model (tests/MockDebugModule.h) to the
// registers.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "rvswd/RvSwdEngine.h"

namespace vsltest {

class MockRvSwdTarget {
   public:
   // An access that the target decoded
   struct Access {
      uint8_t address;
      bool write;
      uint32_t data;
   };

   // A change of the SWDIO line level, and its time
   struct LineEdge {
      uint32_t time;
      bool level;
      bool swclk;
      // The host drives SWDIO after the change.
      bool host;
   };

   explicit MockRvSwdTarget(rvswd::Wire wire)
      : wire_(wire) {
   }

   // Pins

   void init() {
      ++inits;
      swclk_ = false;
      hostDrives_ = true;
      hostLevel_ = true;
      lineChanged();
   }
   void fini() {
      ++finis;
      hostDrives_ = false;
      lineChanged();
   }
   void begin() {
      ++begins;
   }

   void swclk(bool level) {
      if(level == swclk_) {
         return;
      }
      updateHold();
      time += swclkEdgeCycles;
      swclk_ = level;
      if(level) {
         risingEdges.push_back(time);
         rising();
      } else {
         falling();
      }
      lineChanged();
   }
   void swdioDrive(bool level) {
      hostAction();
      const bool before = line();
      hostDrives_ = true;
      hostLevel_ = level;
      hostChanged(before);
   }
   void swdioRelease() {
      hostAction();
      const bool before = line();
      hostDrives_ = false;
      lineChanged();
      // One wire: the target sees the line rise also from the pull-up.
      // Two wires: a release is no start or stop condition.
      if(wire_ == rvswd::Wire::One && before != line()) {
         oneWireEdge(line());
      }
   }
   void swdioWrite(bool level) {
      REQUIRE(hostDrives_);
      time += swdioWriteCycles;
      hostAction();
      const bool before = line();
      hostLevel_ = level;
      hostChanged(before);
   }
   bool swdio() {
      updateHold();
      return line();
   }

   // Each call is one time step.
   uint32_t now() {
      return time++;
   }
   [[nodiscard]] uint32_t coreHz() const {
      return 72'000'000;
   }
   uint32_t lockIrq() {
      ++locks;
      const uint32_t old = irqLocked_ ? 1 : 0;
      irqLocked_ = true;
      return old;
   }
   void unlockIrq(uint32_t old) {
      irqLocked_ = old != 0;
   }

   // State for the tests

   [[nodiscard]] bool swclkLevel() const {
      return swclk_;
   }
   [[nodiscard]] bool hostDrives() const {
      return hostDrives_;
   }
   [[nodiscard]] bool hostLevel() const {
      return hostLevel_;
   }
   [[nodiscard]] bool irqLocked() const {
      return irqLocked_;
   }
   [[nodiscard]] bool inFrame() const {
      return bit_ > 0 || started_;
   }

   // The target
   std::array<uint32_t, 128> registers{};
   // Two wires: the response and tail bits, first bit in bit 3. The
   // values of the captures.
   uint8_t readHead{ 0b0100 };
   uint8_t readTail{ 0b0111 };
   uint8_t writeHead{ 0b0000 };
   uint8_t writeTail{ 0b0011 };
   // Two wires: a wrong parity bit for the next read
   bool badParity{ false };
   // False: the target does not drive SWDIO (no target).
   bool present{ true };
   // One wire: the target holds the line low from the next read bit on.
   bool stuckLow{ false };
   // Called with the address before the target answers a read, and after
   // it stores a write
   std::function<void(uint8_t)> onRead;
   std::function<void(uint8_t)> onWrite;

   std::vector<Access> accesses;
   // Two wires: the bits that the target sampled in the last frame, bit 1
   // first (the numbering of the document)
   std::vector<bool> frameBits;
   // Two wires: SWDIO at the rising edges outside a frame
   std::vector<bool> idleBits;
   std::vector<uint32_t> risingEdges;
   std::vector<LineEdge> lineEdges;
   int starts{ 0 };
   int stops{ 0 };
   int protocolErrors{ 0 };
   int contentions{ 0 };
   int undrivenSamples{ 0 };
   int lineResets{ 0 };
   // One wire: line changes in a frame with the interrupts on
   int unlockedChanges{ 0 };
   int inits{ 0 };
   int finis{ 0 };
   int begins{ 0 };
   int locks{ 0 };
   // The loop code of the engine: time added at each SWCLK edge, and at
   // each SWDIO write
   uint32_t swclkEdgeCycles{ 0 };
   uint32_t swdioWriteCycles{ 0 };
   // Starts near the wrap of the counter.
   uint32_t time{ 0xFFFFF000u };

   private:
   static constexpr uint32_t frameBitCount = 53;
   static constexpr uint32_t resetClocks = 50;
   // One wire, in core cycles at 72 MHz
   static constexpr uint32_t oneThresholdCycles = 36; // 500 ns
   static constexpr uint32_t zeroHoldCycles = 62;     // 860 ns
   static constexpr uint32_t resetLowCycles = 108;    // 1.5 us
   // No host change for this time ends a frame (2 us).
   static constexpr uint32_t abortCycles = 144;
   static constexpr uint32_t oneWireFrameBits = 41;

   [[nodiscard]] bool line() const {
      if(hostDrives_ && !hostLevel_) {
         return false;
      }
      if(targetDrives_ && !targetLevel_) {
         return false;
      }
      return true;
   }

   // Two wires: each overlap of the drivers. One wire: the host drives
   // high while the target holds the line low (both low is no fight).
   void lineChanged() {
      if(hostDrives_ && targetDrives_ && (wire_ == rvswd::Wire::Two || hostLevel_ != targetLevel_)) {
         ++contentions;
      }
      const bool level = line();
      if(level != lineLevel_) {
         lineLevel_ = level;
         lineEdges.push_back({ time, level, swclk_, hostDrives_ });
      }
   }

   // Before each SWDIO change of the host
   void hostAction() {
      updateHold();
      if(wire_ == rvswd::Wire::One && started_ && time - lastHost_ > abortCycles) {
         endOneWireFrame();
      }
      lastHost_ = time;
   }

   void hostChanged(bool before) {
      lineChanged();
      const bool after = line();
      if(wire_ == rvswd::Wire::Two) {
         if(swclk_ && before != after) {
            condition(after);
         }
      } else if(before != after) {
         oneWireEdge(after);
      }
   }

   // Two wires

   // The target drives the bit.
   [[nodiscard]] bool targetBit(uint32_t bit) const {
      return present && targetOwns(bit);
   }

   // The bit belongs to the target, also without a target.
   [[nodiscard]] bool targetOwns(uint32_t bit) const {
      if(bit >= 11 && bit <= 14) {
         return true;
      }
      if(bit >= 49 && bit <= 52) {
         return true;
      }
      return !write_ && bit >= 15 && bit <= 48;
   }

   // The level that the target drives in a bit
   [[nodiscard]] bool targetLevel(uint32_t bit) const {
      if(bit <= 14) {
         return ((write_ ? writeHead : readHead) >> (14 - bit) & 1u) != 0;
      }
      if(bit <= 46) {
         return (registers[address_] >> (46 - bit) & 1u) != 0;
      }
      if(bit <= 48) {
         return parity(registers[address_]) != readBadParity_;
      }
      return ((write_ ? writeTail : readTail) >> (52 - bit) & 1u) != 0;
   }

   static bool parity(uint32_t value) {
      return rvswd::parity(value);
   }

   // SWDIO changes while SWCLK is high.
   void condition(bool level) {
      if(!level && !started_ && bit_ == 0) {
         ++starts;
         started_ = true;
         frameBits.clear();
         address_ = 0;
         write_ = false;
         data_ = 0;
         readBadParity_ = false;
         highClocks_ = 0;
      } else if(level && started_ && bit_ == frameBitCount) {
         ++stops;
         endFrame();
      } else {
         ++protocolErrors;
      }
   }

   void endFrame() {
      started_ = false;
      bit_ = 0;
      targetDrives_ = false;
   }

   void falling() {
      if(wire_ != rvswd::Wire::Two || !started_) {
         return;
      }
      const uint32_t next = bit_ + 1;
      if(targetBit(next)) {
         targetDrives_ = true;
         targetLevel_ = targetLevel(next);
      }
   }

   void rising() {
      if(wire_ != rvswd::Wire::Two) {
         return;
      }
      const bool level = line();
      if(!started_) {
         idleBits.push_back(level);
         // A line reset: clocks with SWDIO high from the host
         highClocks_ = hostDrives_ && level ? highClocks_ + 1 : 0;
         if(highClocks_ == resetClocks) {
            ++lineResets;
         }
         return;
      }
      if(bit_ == frameBitCount) {
         // A clock after bit 53: no stop condition
         ++protocolErrors;
         endFrame();
         return;
      }
      const uint32_t bit = ++bit_;
      frameBits.push_back(level);
      const bool target = targetBit(bit);
      if(!targetOwns(bit) && !hostDrives_) {
         ++undrivenSamples;
      }
      sampleTwo(bit, level);
      if(target && !targetBit(bit + 1)) {
         // Released after the rising edge
         targetDrives_ = false;
      }
   }

   void sampleTwo(uint32_t bit, bool level) {
      const uint32_t value = level ? 1u : 0u;
      if(bit <= 7) {
         address_ = static_cast<uint8_t>(address_ << 1 | value);
      } else if(bit == 8) {
         write_ = level;
         // Bits 1-8 so far
      } else if(bit == 9) {
         const uint32_t request = static_cast<uint32_t>(address_) << 1 | (write_ ? 1u : 0u);
         if(level != parity(request)) {
            ++protocolErrors;
         }
         repeat_ = level;
         if(!write_) {
            readBadParity_ = badParity;
            badParity = false;
            notifyRead();
            accesses.push_back({ address_, false, registers[address_] });
         }
      } else if(bit == 10 || (bit == 48 && write_)) {
         if(level != repeat_) {
            ++protocolErrors;
         }
      } else if(bit >= 15 && bit <= 46 && write_) {
         data_ = data_ << 1 | value;
      } else if(bit == 47 && write_) {
         repeat_ = level;
         if(level != parity(data_)) {
            ++protocolErrors;
         } else {
            registers[address_] = data_;
            accesses.push_back({ address_, true, data_ });
            notifyWrite();
         }
      } else if(bit == 53 && level) {
         ++protocolErrors;
      }
   }

   // One wire

   void updateHold() {
      if(holding_ && !stuckLow && time - holdStart_ >= zeroHoldCycles) {
         holding_ = false;
         targetDrives_ = false;
         lineChanged();
      }
   }

   void oneWireEdge(bool level) {
      if(started_ && !irqLocked_) {
         ++unlockedChanges;
      }
      if(!level) {
         lowStart_ = time;
         if(started_ && readSlot()) {
            // A read bit: the target holds the line low for a 0.
            const uint32_t bit = oneBit_ - 9;
            if(present && (stuckLow || (registers[address_] >> (31 - bit) & 1u) == 0)) {
               holding_ = true;
               holdStart_ = time;
               targetDrives_ = true;
               targetLevel_ = false;
            }
            if(++oneBit_ == oneWireFrameBits) {
               endOneWireFrame();
            }
         }
         return;
      }
      const uint32_t width = time - lowStart_;
      if(width >= resetLowCycles) {
         ++lineResets;
         started_ = false;
         oneBit_ = 0;
         return;
      }
      if(!hostDrives_) {
         // The end of a read bit: the host released the line.
         return;
      }
      sampleOne(width < oneThresholdCycles);
   }

   [[nodiscard]] bool readSlot() const {
      return oneBit_ >= 9 && !write_;
   }

   void sampleOne(bool level) {
      const uint32_t value = level ? 1u : 0u;
      if(!started_) {
         if(!level) {
            ++protocolErrors;
            return;
         }
         started_ = true;
         oneBit_ = 1;
         address_ = 0;
         data_ = 0;
         return;
      }
      if(oneBit_ <= 7) {
         address_ = static_cast<uint8_t>(address_ << 1 | value);
      } else if(oneBit_ == 8) {
         write_ = level;
         if(!write_) {
            notifyRead();
            accesses.push_back({ address_, false, registers[address_] });
         }
      } else {
         data_ = data_ << 1 | value;
      }
      if(++oneBit_ == oneWireFrameBits) {
         registers[address_] = data_;
         accesses.push_back({ address_, true, data_ });
         notifyWrite();
         endOneWireFrame();
      }
   }

   void notifyRead() {
      if(onRead) {
         onRead(address_);
      }
   }
   void notifyWrite() {
      if(onWrite) {
         onWrite(address_);
      }
   }

   void endOneWireFrame() {
      started_ = false;
      oneBit_ = 0;
   }

   rvswd::Wire wire_;
   bool swclk_{ false };
   bool hostDrives_{ false };
   bool hostLevel_{ false };
   bool targetDrives_{ false };
   bool targetLevel_{ false };
   bool lineLevel_{ true };
   bool irqLocked_{ false };
   bool started_{ false };
   // Two wires: the rising edges since the start condition
   uint32_t bit_{ 0 };
   uint32_t highClocks_{ 0 };
   uint8_t address_{ 0 };
   bool write_{ false };
   bool repeat_{ false };
   bool readBadParity_{ false };
   uint32_t data_{ 0 };
   // One wire
   uint32_t oneBit_{ 0 };
   uint32_t lowStart_{ 0 };
   uint32_t lastHost_{ 0 };
   bool holding_{ false };
   uint32_t holdStart_{ 0 };
};

} // namespace vsltest
