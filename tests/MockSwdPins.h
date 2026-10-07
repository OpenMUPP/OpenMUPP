#pragma once

// Mock pins for the SWD bit-bang engine (swd/SwdBitBang.h), with a mock
// target. The target samples SWDIO at the rising edge of SWCLK, and drives
// its bits right after the rising edge, as ADIv5 specifies. It decodes the
// requests of the host, replies with the acknowledges of a queue, and
// records the written data.
//
// SWDIO has a pull-up: it reads 1 when nobody drives it. The mock counts
// the times that the host and the target drive SWDIO at the same time, and
// the data bits that the target samples while the host does not drive.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "protocol/SwdBlock.h"

namespace vsltest {

class MockSwdPins {
   public:
   // A transfer that the target saw
   struct Transfer {
      uint8_t request;
      uint8_t ack;
      // Write: the data and the parity bit of the host
      uint32_t written{ 0 };
      bool writtenParity{ false };
   };

   // The level of SWDIO at a rising edge of SWCLK, and its time
   struct RisingEdge {
      uint32_t time;
      bool hostDrives;
      bool level;
   };

   explicit MockSwdPins(uint8_t turnaround = 1)
      : turnaround_(turnaround) {
   }

   void init() {
      ++inits;
      swclk_ = true;
      hostDrives_ = true;
      hostLevel_ = true;
   }
   void fini() {
      ++finis;
      hostDrives_ = false;
   }
   void begin() {
      ++begins;
   }

   void swclk(bool level) {
      if(level == swclk_) {
         return;
      }
      swclk_ = level;
      if(edges.size() == stallEdge) {
         time += stallCycles;
      }
      edges.push_back(time);
      if(level) {
         rising();
      }
   }
   void swdioDrive(bool level) {
      if(targetDrives_) {
         ++contentions;
      }
      hostDrives_ = true;
      hostLevel_ = level;
   }
   void swdioRelease() {
      hostDrives_ = false;
   }
   void swdioWrite(bool level) {
      REQUIRE(hostDrives_);
      hostLevel_ = level;
   }
   [[nodiscard]] bool swdio() const {
      if(hostDrives_) {
         return hostLevel_;
      }
      if(targetDrives_) {
         return targetLevel_;
      }
      return true;
   }

   // Each call is one time step.
   uint32_t now() {
      return time++;
   }
   [[nodiscard]] uint32_t coreHz() const {
      return 72'000'000;
   }

   [[nodiscard]] bool swclkLevel() const {
      return swclk_;
   }
   [[nodiscard]] bool hostDrives() const {
      return hostDrives_;
   }

   // The target: the acknowledges of the next requests (OK when empty),
   // the data of a read, and a wrong parity bit for a read. An ACK of 0
   // means no reply: the target does not drive SWDIO.
   std::deque<uint8_t> acks;
   uint32_t readData{ 0 };
   bool badParity{ false };
   // False: the target ignores SWDIO (for the sequence tests).
   bool decodes{ true };

   std::vector<Transfer> transfers;
   std::vector<RisingEdge> risingEdges;
   std::vector<uint32_t> edges;
   int contentions{ 0 };
   int undrivenSamples{ 0 };
   int inits{ 0 };
   int finis{ 0 };
   int begins{ 0 };
   // An interrupt: the time jumps by stallCycles at SWCLK edge stallEdge.
   std::size_t stallEdge{ SIZE_MAX };
   uint32_t stallCycles{ 0 };
   // Starts near the wrap of the counter.
   uint32_t time{ 0xFFFFFF00u };

   private:
   enum class Action : uint8_t { Release, Drive, Sample };
   struct Step {
      Action action;
      bool level;
   };

   void rising() {
      const bool level = swdio();
      if(hostDrives_ && targetDrives_) {
         ++contentions;
      }
      risingEdges.push_back({ time, hostDrives_, level });
      if(!decodes) {
         return;
      }
      if(!plan_.empty()) {
         const Step step = plan_.front();
         plan_.pop_front();
         if(step.action == Action::Sample) {
            sample(level);
         }
      } else if(requestBits_ > 0 || (hostDrives_ && level)) {
         request_ = static_cast<uint8_t>(request_ | (level ? 1u : 0u) << requestBits_);
         if(++requestBits_ == 8) {
            planReply();
         }
      }
      // The target drives the next cycle right after the rising edge.
      targetDrives_ = !plan_.empty() && plan_.front().action == Action::Drive;
      targetLevel_ = targetDrives_ && plan_.front().level;
   }

   void planReply() {
      const uint8_t request = request_;
      request_ = 0;
      requestBits_ = 0;
      const bool valid = (request & 0xC1) == 0x81 && ((request & 0x20) != 0) == protocol::swd::parity(request & 0x1E);
      if(!valid) {
         transfers.push_back({ request, 0 });
         return;
      }
      uint8_t ack = protocol::swd::ackOk;
      if(!acks.empty()) {
         ack = acks.front();
         acks.pop_front();
      }
      transfers.push_back({ request, ack });
      if(ack == 0) {
         return;
      }
      addRelease(turnaround_);
      for(unsigned i = 0; i < 3; ++i) {
         plan_.push_back({ Action::Drive, ((ack >> i) & 1u) != 0 });
      }
      if(ack != protocol::swd::ackOk) {
         addRelease(turnaround_);
         return;
      }
      if((request & protocol::swd::readRequest) != 0) {
         for(unsigned i = 0; i < 32; ++i) {
            plan_.push_back({ Action::Drive, ((readData >> i) & 1u) != 0 });
         }
         plan_.push_back({ Action::Drive, protocol::swd::parity(readData) != badParity });
         addRelease(turnaround_);
      } else {
         addRelease(turnaround_);
         samples_ = 0;
         for(unsigned i = 0; i < 33; ++i) {
            plan_.push_back({ Action::Sample, false });
         }
      }
   }

   void addRelease(unsigned count) {
      for(unsigned i = 0; i < count; ++i) {
         plan_.push_back({ Action::Release, false });
      }
   }

   void sample(bool level) {
      Transfer &transfer = transfers.back();
      if(!hostDrives_) {
         ++undrivenSamples;
      }
      if(samples_ < 32) {
         transfer.written |= static_cast<uint32_t>(level) << samples_;
      } else {
         transfer.writtenParity = level;
      }
      ++samples_;
   }

   uint8_t turnaround_;
   bool swclk_{ false };
   bool hostDrives_{ false };
   bool hostLevel_{ false };
   bool targetDrives_{ false };
   bool targetLevel_{ false };
   std::deque<Step> plan_;
   uint8_t request_{ 0 };
   unsigned requestBits_{ 0 };
   unsigned samples_{ 0 };
};

} // namespace vsltest
