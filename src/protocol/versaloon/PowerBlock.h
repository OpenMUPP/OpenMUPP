#pragma once

// The USB_TO_POWER block (0x60): the 3.3 V target supply and the target
// voltage (Vref). Hardware-independent: the block maps the requests to a
// PowerHardware object, and the host unit tests use a fake one.
//
// Sub-commands (port index 0 only):
//
//   INIT    []             -> [status]
//   FINI    []             -> [status]
//   CONFIG  []             -> [status]
//   OUT     [mV u16]       -> [status]
//   IN      [any]          -> [status][mV u16]
//
// OUT accepts only 0 (supply off) and 3300 (supply on). Other values are
// INVALID_PARA. OUT 3300 is FAILED, and the supply stays off, when a
// protection of TargetPower refuses it (protocol/TargetPower.h): the
// target has its own supply, the voltage cannot be measured, or the rail
// does not come up.
//
// A board without a power switch gets CMD_NOT_SUPPORT for OUT 3300. OUT 0
// is OK, because the target has no supply of this adapter. A board without
// a Vref sense gets CMD_NOT_SUPPORT for IN.
//
// IN is the voltage at the Vref pin: the supply of the adapter when it is
// on, else the supply of the target. The original firmware ignores the IN
// payload, and so does this block.
//
// The main loop calls TargetPower::poll(), which switches the supply off
// when the rail drops. The supply stays on when the host goes away, so
// that a tool can switch the target on and then end.

#include <cstdint>

#include "protocol/TargetPower.h"
#include "protocol/versaloon/UsbToAll.h"
#include "protocol/versaloon/UsbToXxx.h"

namespace protocol {

class PowerBlock final : public SubCommandBlock, public TargetVoltage {
   public:
   // The only voltage that OUT switches on
   static constexpr uint16_t supplyMillivolts = 3300;

   explicit PowerBlock(TargetPower &power);

   // The target voltage in mV, 0 without a sense or when it fails.
   [[nodiscard]] uint16_t millivolts() override;

   protected:
   void runSubCommand(const SubCommand &command, Reply &reply) override;

   private:
   [[nodiscard]] Status out(art::span<const uint8_t> payload);

   TargetPower &power_;
};

} // namespace protocol
