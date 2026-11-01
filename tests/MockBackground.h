#pragma once

// A protocol::Background for the engine tests: records the time of each
// service() call, and a sample of the pins (a clock level, an edge count)
// that the test chooses.

#include <cstdint>
#include <functional>
#include <vector>

#include "protocol/Background.h"

namespace mupptest {

class MockBackground final : public protocol::Background {
   public:
   struct Call {
      uint32_t time;
      // Clock level at the call
      bool clock;
      // Clock edges or bits before the call
      std::size_t count;
   };

   // sample: the state of the pins at a call
   explicit MockBackground(std::function<Call()> sample)
      : sample_(std::move(sample)) {
   }

   void service() override {
      calls.push_back(sample_());
      if(onService) {
         onService();
      }
   }

   std::vector<Call> calls;
   // Extra work at each call, for example the UART bridge
   std::function<void()> onService;

   private:
   std::function<Call()> sample_;
};

} // namespace mupptest
