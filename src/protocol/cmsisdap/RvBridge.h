#pragma once

// The JTAG port of the CMSIS-DAP processor in an image with the RISC-V
// bridge (VSL_CMSIS_DAP_RV_BRIDGE): the real JTAG port of the board, or the
// emulated TAP (RvDtmTap.h). The vendor command ID_DAP_Vendor1
// (DapProcessor.h) switches between them. The real port is the default, so
// the bridge is off at power-up and after hostGone() (a USB reset).
//
// A switch while the processor uses the port (after init(), before fini())
// ends the old port with fini() and starts the new one with init(). The
// bridge off thus releases the pins of the wire. setSpeed() always goes to
// the real port, so it has the TCK of the host after the bridge. The TAP
// ignores the speed.

#include <cstdint>
#include <utils/compat/span.h>

#include "protocol/JtagPort.h"
#include "protocol/cmsisdap/DapProcessor.h"
#include "protocol/cmsisdap/RvDtmTap.h"

namespace cmsisdap {

class RvBridge final : public protocol::JtagPort, public Bridge {
   public:
   RvBridge(protocol::JtagPort &jtag, RvDtmTap &tap)
      : jtag_(jtag)
      , tap_(tap) {
   }

   void init() override {
      active_ = true;
      port().init();
   }
   void fini() override {
      active_ = false;
      port().fini();
   }
   // The TAP takes each speed: the wire has its own clock.
   [[nodiscard]] bool setSpeed(uint32_t kHz) override {
      const bool real = jtag_.setSpeed(kHz);
      return on_ || real;
   }
   [[nodiscard]] bool
   shift(uint32_t bitCount, art::span<const uint8_t> tdi, art::span<const uint8_t> tms, art::span<uint8_t> tdo) override {
      return port().shift(bitCount, tdi, tms, tdo);
   }

   void enable(bool on) override {
      if(on == on_) {
         return;
      }
      if(active_) {
         port().fini();
      }
      on_ = on;
      if(active_) {
         port().init();
      }
   }
   void pinsChanged() override {
      tap_.pinsChanged();
   }

   [[nodiscard]] bool enabled() const {
      return on_;
   }

   private:
   protocol::JtagPort &port() {
      return on_ ? static_cast<protocol::JtagPort &>(tap_) : jtag_;
   }

   protocol::JtagPort &jtag_;
   RvDtmTap &tap_;
   bool on_{ false };
   // The processor uses the port: init() without fini().
   bool active_{ false };
};

} // namespace cmsisdap
