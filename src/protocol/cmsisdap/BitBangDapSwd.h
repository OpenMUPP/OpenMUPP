// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The CMSIS-DAP transfer rules on the SWD bit-bang engine. Hardware-
// independent: the engine runs on mock pins in the host unit tests.
//
// The adapter sets the CMSIS-DAP rules of the engine (see TransferRules in
// swd/SwdBitBang.h) and never calls configure(). Thus the order of
// DAP_SWJ_Clock, DAP_SWD_Configure and DAP_TransferConfigure does not
// matter.
//
// The adapter is the RetryStop of the engine: after a WAIT, it asks the
// processor for DAP_TransferAbort.

#include "protocol/cmsisdap/DapSwd.h"
#include "swd/SwdBitBang.h"

namespace cmsisdap {

template <typename Port>
class BitBangDapSwd final : public DapSwd, private swd::RetryStop {
   public:
   explicit BitBangDapSwd(Port &port)
      : port_(port) {
   }

   void init() override {
      port_.init();
   }

   void fini() override {
      port_.fini();
   }

   void setClockHz(uint32_t hz) override {
      port_.setClockHz(hz);
   }

   void apply(const SwdSettings &settings) override {
      swd::TransferRules rules;
      rules.idleCycles = settings.idleCycles;
      rules.abortOnWait = false;
      rules.dataPhase = settings.dataPhase;
      rules.idleOnlyAfterOk = true;
      rules.releaseOnAckError = true;
      port_.setRules(rules);
      port_.setTurnaround(settings.turnaround);
      port_.setRetries(settings.waitRetries);
   }

   void sequenceOut(uint16_t bitCount, art::span<const uint8_t> data) override {
      port_.sequenceOut(bitCount, data);
   }

   void sequenceIn(uint16_t bitCount, art::span<uint8_t> data) override {
      port_.sequenceIn(bitCount, data);
   }

   uint8_t transfer(uint8_t request, uint32_t &data) override {
      // The engine takes APnDP, RnW, A2 and A3 in bits 1-4.
      const uint8_t ack = port_.transfer(protocol::swd::encodeRequest(static_cast<uint8_t>((request & 0x0F) << 1)), data);
      if((ack & swd::parityErrorFlag) != 0) {
         return dap::protocolError;
      }
      return ack & protocol::swd::ackMask;
   }

   void setAbort(const TransferAbort &abort) override {
      abort_ = &abort;
      port_.setRetryStop(this);
   }

   private:
   bool stopRetries() override {
      return abort_->aborted();
   }

   Port &port_;
   const TransferAbort *abort_{ nullptr };
};

} // namespace cmsisdap
