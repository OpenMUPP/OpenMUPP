#pragma once

// The JTAG port of the CMSIS-DAP processor in an image with the RISC-V
// bridge (VSL_CMSIS_DAP_RV_BRIDGE): the real JTAG port of the board, or the
// emulated TAP (RvDtmTap.h). The vendor command ID_DAP_Vendor1
// (DapProcessor.h) sets the mode:
//
// - Off: the real port.
// - On: the TAP.
// - Auto: init() (DAP_Connect to JTAG) finds the port, see detect(). Auto
//   is the mode at power-up and after hostGone() (a USB reset).
//
// A mode change while the processor uses the port (after init(), before
// fini()) ends the old port with fini() and starts the new one with init().
// The change to the real port thus releases the pins of the wire.
// setSpeed() always goes to the real port, so it has the TCK of the host
// after the bridge. The TAP ignores the speed.

#include <cstdint>
#include <utils/compat/span.h>

#include "protocol/JtagPort.h"
#include "protocol/cmsisdap/DapProcessor.h"
#include "protocol/cmsisdap/DapSwd.h"
#include "protocol/cmsisdap/RvDtmTap.h"

namespace cmsisdap {

class RvBridge final : public protocol::JtagPort, public Bridge {
   public:
   RvBridge(protocol::JtagPort &jtag, DapSwd &swd, RvDtmTap &tap)
      : jtag_(jtag)
      , swd_(swd)
      , tap_(tap) {
   }

   void init() override {
      active_ = true;
      if(mode_ == BridgeMode::Auto) {
         on_ = detect();
      }
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

   void setMode(BridgeMode mode) override;
   void pinsChanged() override {
      tap_.pinsChanged();
   }

   [[nodiscard]] BridgeMode mode() const {
      return mode_;
   }
   // The TAP is the port.
   [[nodiscard]] bool enabled() const {
      return on_;
   }

   private:
   protocol::JtagPort &port() {
      return on_ ? static_cast<protocol::JtagPort &>(tap_) : jtag_;
   }
   // Auto: true when the TAP is the port. In order: a JTAG chain on the
   // real port, an SWD target, then a WCH target on one wire or two wires
   // (RvDtmTap::probe()). Only a WCH target selects the TAP. Each step
   // releases its pins.
   bool detect();
   // A JTAG chain: Test-Logic-Reset, then 64 bits of Shift-DR with TDI
   // high. TDO that stays high (the pull-up) or low is no chain.
   bool jtagChain();
   // An SWD target: the JTAG-to-SWD switch and a read of DPIDR with ACK OK.
   // It then switches an SWJ-DP back to JTAG.
   bool swdTarget();

   protocol::JtagPort &jtag_;
   DapSwd &swd_;
   RvDtmTap &tap_;
   BridgeMode mode_{ BridgeMode::Auto };
   bool on_{ false };
   // The processor uses the port: init() without fini().
   bool active_{ false };
};

} // namespace cmsisdap
