#pragma once

// The JTAG port of the CMSIS-DAP processor in an image with the RISC-V
// bridge (MUPP_CMSIS_DAP_RV_BRIDGE): the real JTAG port of the board, or the
// emulated TAP (RvDtmTap.h). The vendor command ID_DAP_Vendor1
// (DapProcessor.h) sets the mode:
//
// - Off: the real port.
// - On: the TAP.
// - Auto: init() (DAP_Connect to JTAG) finds the port, see detect(). A
//   change to auto keeps the port until then.
//
// The stored mode is the mode at power-up and after hostGone() (a USB
// reset). storeMode() writes it to the BridgeModeStore: the settings page
// of the image in the firmware (personality/PersonalityStore.h). Auto
// when the page holds no mode.
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

// The storage of the stored mode
class BridgeModeStore {
   public:
   // False when the store failed
   virtual bool store(BridgeMode mode) = 0;

   protected:
   ~BridgeModeStore() = default;
};

class RvBridge final : public protocol::JtagPort, public Bridge {
   public:
   // stored: the stored mode, the mode of the start
   RvBridge(protocol::JtagPort &jtag, DapSwd &swd, RvDtmTap &tap, BridgeModeStore &store, BridgeMode stored)
      : jtag_(jtag)
      , swd_(swd)
      , tap_(tap)
      , store_(store)
      , mode_(stored)
      , stored_(stored)
      , on_(stored == BridgeMode::On) {
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
   bool storeMode(BridgeMode mode) override;
   [[nodiscard]] BridgeMode storedMode() const override {
      return stored_;
   }
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
   BridgeModeStore &store_;
   BridgeMode mode_;
   BridgeMode stored_;
   bool on_;
   // The processor uses the port: init() without fini().
   bool active_{ false };
};

} // namespace cmsisdap
