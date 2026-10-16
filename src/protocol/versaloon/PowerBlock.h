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
// INVALID_PARA. OUT 3300 is FAILED, and the supply stays off, when:
//
//   - the target voltage is above externalSupplyMillivolts: the target
//     has its own supply, and two supplies on one rail can do damage;
//   - the target voltage cannot be measured on a board with a sense;
//   - the voltage is not above that level settleMs after the switch-on:
//     the rail has a short circuit or an overload. The block then
//     switches off again.
//
// A board without a power switch gets CMD_NOT_SUPPORT for OUT 3300. OUT 0
// is OK, because the target has no supply of this adapter. A board without
// a Vref sense gets CMD_NOT_SUPPORT for IN. It switches on without the
// checks above, and never switches off by itself.
//
// IN is the voltage at the Vref pin: the supply of the adapter when it is
// on, else the supply of the target. The original firmware ignores the IN
// payload, and so does this block.
//
// poll() switches the supply off when the voltage stays at or below
// externalSupplyMillivolts, as the original firmware does: the target
// pulls the rail down, or an external supply with a lower voltage
// back-feeds it. The supply stays on when the host goes away, so that a
// tool can switch the target on and then end.

#include <cstdint>
#include <optional>

#include "protocol/PowerHardware.h"
#include "protocol/versaloon/UsbToAll.h"
#include "protocol/versaloon/UsbToXxx.h"

namespace protocol {

class PowerBlock final : public SubCommandBlock, public TargetVoltage {
   public:
   // The only voltage that OUT switches on
   static constexpr uint16_t supplyMillivolts = 3300;
   // A target voltage above this level is a live supply
   static constexpr uint16_t externalSupplyMillivolts = 1800;
   // Time from the switch-on to the first check of the voltage
   static constexpr uint16_t settleMs = 10;
   // Time between two checks in poll()
   static constexpr uint32_t pollIntervalMs = 10;
   // Consecutive low checks in poll() that switch the supply off
   static constexpr unsigned lowChecksToOff = 3;

   PowerBlock(PowerHardware &hardware, Delay &delay);

   // The target voltage in mV, 0 without a sense or when it fails.
   [[nodiscard]] uint16_t millivolts() override;

   // Call it in the main loop. nowMs is a millisecond counter. It checks
   // the voltage every pollIntervalMs while the supply is on.
   void poll(uint64_t nowMs);

   // True when this block has switched the supply on.
   [[nodiscard]] bool isOn() const {
      return on_;
   }

   protected:
   void runSubCommand(const SubCommand &command, Reply &reply) override;

   private:
   [[nodiscard]] Status out(art::span<const uint8_t> payload);
   [[nodiscard]] Status switchOn();
   void switchOff();
   // True when the voltage of the sense is above externalSupplyMillivolts.
   // No value when the measurement fails.
   [[nodiscard]] std::optional<bool> isLive();

   PowerHardware &hardware_;
   Delay &delay_;
   bool on_{ false };
   unsigned lowChecks_{ 0 };
   uint64_t lastCheckMs_{ 0 };
};

} // namespace protocol
