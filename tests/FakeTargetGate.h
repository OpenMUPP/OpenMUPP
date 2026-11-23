// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A target gate (protocol/TargetGate.h) for the tests: check() records the
// protocol and returns `allow`.

#include "protocol/TargetGate.h"

#include <vector>

namespace mupptest {

class FakeTargetGate final : public protocol::TargetGate {
   public:
   bool check(protocol::TargetProtocol protocol) override {
      checks.push_back(protocol);
      allowed_ = allow;
      return allowed_;
   }
   [[nodiscard]] bool isAllowed() const override {
      return allowed_;
   }

   // The result of the next check()
   bool allow{ true };
   std::vector<protocol::TargetProtocol> checks;

   private:
   bool allowed_{ true };
};

} // namespace mupptest
