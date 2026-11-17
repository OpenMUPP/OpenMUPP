// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A mock debugWIRE target for the dW engine (avr/DebugWireEngine.h), with
// mock pins and a mock UART receiver.
//
// The line is low when the probe or the target pulls it low. The probe
// drive is a list of edges with their times. The target drive is a list of
// low intervals, which the target makes when it sends.
//
// The target decodes the probe drive at its own bit time (bitCycles):
// sample in the middle of each bit. A low of breakBits bit times or more is
// a break: the target sends 0x55 syncDelayBits after the end of the break.
// A byte that is in replies gets its reply one bit time after the stop bit:
// a break first if the reply has one, then the bytes.
//
// The receiver of the pins decodes the line at the bit time of the baud
// rate of rxEnable(): the echo of the probe and the bytes of the target.
//
// Each now() call is one time step. The target and the receiver work
// lazily: each pins call first processes the time until now.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace mupptest {

class MockDwTarget {
   public:
   struct Reply {
      bool breakFirst{ false };
      std::vector<uint8_t> bytes;
   };

   explicit MockDwTarget(uint32_t targetBitCycles)
      : bitCycles(targetBitCycles) {
   }

   // The probe drive, from the pins
   void drive(uint64_t time, bool low) {
      if(low == probeLow_) {
         return;
      }
      probeLow_ = low;
      ++version;
      // Two changes at one time step: no edge
      if(!probeEdges_.empty() && probeEdges_.back().first == time) {
         probeEdges_.pop_back();
         return;
      }
      probeEdges_.emplace_back(time, low);
   }

   // The line level at time (true: high)
   [[nodiscard]] bool level(uint64_t time) const {
      return !probeLowAt(time) && !targetLowAt(time);
   }

   // Processes the probe frames until time.
   void advance(uint64_t time) {
      now_ = time;
      if(decodedVersion_ == version && now_ < decodeWake_) {
         return;
      }
      while(decodeOne()) {
      }
      decodedVersion_ = version;
   }

   // The target sends a byte at time or later (after its last byte).
   void send(uint8_t value, uint64_t time) {
      uint64_t start = std::max({ time, txFree_, now_ + 1 });
      // Start bit, data LSB first, stop bit
      const uint32_t frame = (uint32_t{ value } << 1) | 0x200u;
      for(uint32_t bit = 0; bit < 10; ++bit) {
         if(((frame >> bit) & 1u) == 0) {
            pullLow(start + bit * bitCycles, start + (bit + 1u) * bitCycles);
         }
      }
      txFree_ = start + 10u * bitCycles;
   }

   // The break of a target that stops, then 0x55
   void sendBreak(uint64_t time) {
      const uint64_t start = std::max({ time, txFree_, now_ + 1 });
      pullLow(start, start + breakBits * bitCycles);
      txFree_ = start + (breakBits + syncDelayBits) * bitCycles;
      send(0x55, txFree_);
   }

   // The target holds the line low in [start, end).
   void pullLow(uint64_t start, uint64_t end) {
      lows_.emplace_back(start, end);
      std::sort(lows_.begin(), lows_.end());
      std::vector<std::pair<uint64_t, uint64_t>> merged;
      for(const auto &low : lows_) {
         if(!merged.empty() && low.first <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, low.second);
         } else {
            merged.push_back(low);
         }
      }
      lows_ = std::move(merged);
      ++version;
   }

   // The change times of the line at time or later, in order
   [[nodiscard]] std::vector<uint64_t> changesFrom(uint64_t time) const {
      std::vector<uint64_t> times;
      auto probe = std::lower_bound(probeEdges_.begin(), probeEdges_.end(), std::make_pair(time, false));
      for(; probe != probeEdges_.end(); ++probe) {
         times.push_back(probe->first);
      }
      for(const auto &[start, end] : lows_) {
         if(start >= time) {
            times.push_back(start);
         }
         if(end >= time) {
            times.push_back(end);
         }
      }
      std::sort(times.begin(), times.end());
      return times;
   }

   // The target bit time in time steps
   uint32_t bitCycles;
   uint32_t breakBits{ 12 };
   uint32_t syncDelayBits{ 2 };
   // Off: the target does not answer (no target, or dW not enabled).
   bool present{ true };
   std::map<uint8_t, Reply> replies;
   // A model of the target behind the line (MockAvrDw.h): gets each byte
   // with the end time of its stop bit, and each probe break with the time
   // of its end, in place of replies and the 0x55 of a break.
   std::function<void(uint8_t, uint64_t)> onByte;
   std::function<void(uint64_t)> onBreak;
   // The bytes of the probe that the target decoded
   std::vector<uint8_t> received;
   // The breaks of the probe: their start time and length
   std::vector<std::pair<uint64_t, uint64_t>> breaks;
   int framingErrors{ 0 };
   // Changes at each new probe edge or target low: the caches of the
   // decoders depend on it.
   uint64_t version{ 0 };

   private:
   [[nodiscard]] bool probeLowAt(uint64_t time) const {
      auto it = std::upper_bound(probeEdges_.begin(), probeEdges_.end(), std::make_pair(time, true));
      return it != probeEdges_.begin() && std::prev(it)->second;
   }

   [[nodiscard]] bool targetLowAt(uint64_t time) const {
      auto it = std::upper_bound(lows_.begin(), lows_.end(), std::make_pair(time, UINT64_MAX));
      return it != lows_.begin() && time < std::prev(it)->second;
   }

   // The first probe edge to low at searchFrom_ or later
   [[nodiscard]] const std::pair<uint64_t, bool> *nextProbeFall() const {
      for(auto it = std::lower_bound(probeEdges_.begin(), probeEdges_.end(), std::make_pair(searchFrom_, false));
          it != probeEdges_.end(); ++it) {
         if(it->second) {
            return &*it;
         }
      }
      return nullptr;
   }

   // The probe edge to high after time, 0 for none yet
   [[nodiscard]] uint64_t probeRiseAfter(uint64_t time) const {
      for(auto it = std::upper_bound(probeEdges_.begin(), probeEdges_.end(), std::make_pair(time, true));
          it != probeEdges_.end(); ++it) {
         if(!it->second) {
            return it->first;
         }
      }
      return 0;
   }

   // Decodes one probe frame. False when the next frame is not complete.
   bool decodeOne() {
      // Without a new edge, the next decode is at decodeWake_ or later.
      decodeWake_ = UINT64_MAX;
      const auto *fall = nextProbeFall();
      if(fall == nullptr) {
         return false;
      }
      const uint64_t start = fall->first;
      const uint64_t stopSample = start + 9u * bitCycles + bitCycles / 2u;
      if(now_ < stopSample) {
         decodeWake_ = stopSample;
         return false;
      }
      uint32_t value = 0;
      for(uint32_t bit = 0; bit < 8; ++bit) {
         const uint64_t sample = start + (bit + 1u) * bitCycles + bitCycles / 2u;
         value |= (probeLowAt(sample) ? 0u : 1u) << bit;
      }
      if(!probeLowAt(stopSample)) {
         searchFrom_ = stopSample;
         received.push_back(static_cast<uint8_t>(value));
         answer(static_cast<uint8_t>(value), start + 10u * bitCycles);
         return true;
      }
      const uint64_t rise = probeRiseAfter(stopSample);
      if(rise == 0 || rise > now_) {
         decodeWake_ = rise == 0 ? UINT64_MAX : rise;
         return false;
      }
      searchFrom_ = rise;
      if(value == 0 && rise - start >= uint64_t{ breakBits } * bitCycles) {
         breaks.emplace_back(start, rise - start);
         if(present && onBreak) {
            onBreak(rise);
         } else if(present) {
            send(0x55, rise + syncDelayBits * bitCycles);
         }
      } else {
         ++framingErrors;
      }
      return true;
   }

   void answer(uint8_t value, uint64_t stopEnd) {
      if(present && onByte) {
         onByte(value, stopEnd);
         return;
      }
      const auto it = replies.find(value);
      if(!present || it == replies.end()) {
         return;
      }
      const uint64_t at = stopEnd + bitCycles;
      if(it->second.breakFirst) {
         sendBreak(at);
      }
      for(const uint8_t byte : it->second.bytes) {
         send(byte, at);
      }
   }

   std::vector<std::pair<uint64_t, bool>> probeEdges_;
   std::vector<std::pair<uint64_t, uint64_t>> lows_;
   bool probeLow_{ false };
   uint64_t now_{ 0 };
   uint64_t searchFrom_{ 0 };
   uint64_t txFree_{ 0 };
   uint64_t decodeWake_{ 0 };
   uint64_t decodedVersion_{ UINT64_MAX };
};

// The pins of the engine on a MockDwTarget. The time is 64 bits inside the
// mock; now() gives the low 32 bits, as the cycle counter.
class MockDwPins {
   public:
   static constexpr uint32_t clockHz = 72'000'000;

   explicit MockDwPins(MockDwTarget &target)
      : target_(target) {
   }

   void init() {
      ++inits;
      driven = true;
      drive(false);
      rxOn_ = false;
   }
   void fini() {
      ++finis;
      drive(false);
      driven = false;
      rxOn_ = false;
   }
   void begin() {
      ++begins;
   }

   void drive(bool low) {
      target_.advance(time_);
      target_.drive(time_, low);
   }
   bool sense() {
      target_.advance(time_);
      return target_.level(time_);
   }

   uint32_t now() {
      target_.advance(time_);
      const auto value = static_cast<uint32_t>(time_);
      ++time_;
      return value;
   }
   [[nodiscard]] uint32_t coreHz() const {
      return clockHz;
   }

   uint32_t lockIrq() {
      const uint32_t old = locked ? 1u : 0u;
      if(!locked) {
         lockedAt_ = time_;
      }
      locked = true;
      ++locks;
      return old;
   }
   void unlockIrq(uint32_t old) {
      if(old == 0 && locked) {
         locked = false;
         maxLockedCycles = std::max(maxLockedCycles, time_ - lockedAt_);
      }
   }

   // False outside [rxMinBaud, rxMaxBaud], as the BRR range of a USART
   bool rxEnable(uint32_t baud) {
      if(baud < rxMinBaud || baud > rxMaxBaud) {
         rxOn_ = false;
         return false;
      }
      rxBaud = baud;
      rxBitCycles_ = (clockHz + baud / 2u) / baud;
      rxFrom_ = time_ + rxBlindBits * rxBitCycles_;
      rxWake_ = 0;
      rxOn_ = true;
      return true;
   }
   void rxDisable() {
      rxOn_ = false;
   }

   // The UART receiver: the next frame on the line from rxFrom_, when its
   // stop bit sample is in the past (the RXNE time of a USART).
   int32_t rxRead() {
      target_.advance(time_);
      if(!rxOn_ || (rxVersion_ == target_.version && time_ < rxWake_)) {
         return -1;
      }
      // Without a change of the line, no frame ends before rxWake_.
      rxVersion_ = target_.version;
      rxWake_ = UINT64_MAX;
      const auto start = nextFrame(rxFrom_);
      if(!start) {
         return -1;
      }
      const uint64_t stopSample = *start + 9u * rxBitCycles_ + rxBitCycles_ / 2u;
      if(time_ < stopSample) {
         rxWake_ = stopSample;
         return -1;
      }
      int32_t value = 0;
      for(uint32_t bit = 0; bit < 8; ++bit) {
         const uint64_t sample = *start + (bit + 1u) * rxBitCycles_ + rxBitCycles_ / 2u;
         value |= (target_.level(sample) ? 1 : 0) << bit;
      }
      if(!target_.level(stopSample)) {
         value |= 0x100;
      }
      rxFrom_ = stopSample;
      // The USART holds one byte: a frame that ended while this one waited
      // for the read is lost (overrun).
      for(auto next = nextFrame(rxFrom_); next && *next + 9u * rxBitCycles_ + rxBitCycles_ / 2u <= time_;
          next = nextFrame(rxFrom_)) {
         rxFrom_ = *next + 9u * rxBitCycles_ + rxBitCycles_ / 2u;
         ++rxOverruns;
      }
      rxWake_ = 0;
      rxBytes.push_back(value);
      return value;
   }

   // The 64-bit time, for the tests
   [[nodiscard]] uint64_t time() const {
      return time_;
   }
   void setTime(uint64_t time) {
      time_ = time;
   }

   bool driven{ false };
   bool locked{ false };
   int locks{ 0 };
   uint64_t maxLockedCycles{ 0 };
   uint32_t rxBaud{ 0 };
   // The baud rate range of the receiver. Default: no limit.
   uint32_t rxMinBaud{ 0 };
   uint32_t rxMaxBaud{ UINT32_MAX };
   // After rxEnable(), the receiver misses a start bit for this many bit
   // times, as the CH32V307 USART (debugWIRE hardware test).
   uint32_t rxBlindBits{ 10 };
   std::vector<int32_t> rxBytes;
   // Frames that the receiver lost, because the byte before was not read
   int rxOverruns{ 0 };
   int inits{ 0 };
   int finis{ 0 };
   int begins{ 0 };

   private:
   // The start bit (falling edge) of the first frame from the time
   std::optional<uint64_t> nextFrame(uint64_t from) const {
      for(const uint64_t change : target_.changesFrom(from)) {
         if(change != 0 && target_.level(change - 1) && !target_.level(change)) {
            return change;
         }
      }
      return std::nullopt;
   }

   MockDwTarget &target_;
   // Starts near the wrap of the 32-bit counter.
   uint64_t time_{ 0xFFFFF000u };
   uint64_t lockedAt_{ 0 };
   uint64_t rxBitCycles_{ 1 };
   uint64_t rxFrom_{ 0 };
   uint64_t rxWake_{ 0 };
   uint64_t rxVersion_{ UINT64_MAX };
   bool rxOn_{ false };
};

} // namespace mupptest
