#include "protocol/versaloon/PowerBlock.h"

namespace protocol {

PowerBlock::PowerBlock(PowerHardware &hardware, Delay &delay)
   : SubCommandBlock(block::power)
   , hardware_(hardware)
   , delay_(delay) {
}

uint16_t PowerBlock::millivolts() {
   if(!hardware_.hasSense()) {
      return 0;
   }
   return hardware_.senseMillivolts().value_or(0);
}

void PowerBlock::runSubCommand(const SubCommand &command, Reply &reply) {
   const bool in = command.command == subcommand::in;
   auto slot = reply.reserve(in ? 2 : 0);
   if(!slot) {
      return;
   }
   if(command.index != 0) {
      slot->setStatus(Status::InvalidIndex);
      return;
   }
   switch(command.command) {
   case subcommand::init:
   case subcommand::fini:
   case subcommand::config:
      return;
   case subcommand::out:
      slot->setStatus(out(command.payload));
      return;
   case subcommand::in: {
      if(!hardware_.hasSense()) {
         slot->setStatus(Status::CmdNotSupport);
         return;
      }
      const auto value = hardware_.senseMillivolts();
      if(!value) {
         slot->setStatus(Status::Failed);
         return;
      }
      slot->data()[0] = static_cast<uint8_t>(*value);
      slot->data()[1] = static_cast<uint8_t>(*value >> 8);
      return;
   }
   default:
      slot->setStatus(Status::CmdNotSupport);
      return;
   }
}

void PowerBlock::poll(uint64_t nowMs) {
   if(!on_ || !hardware_.hasSense() || nowMs - lastCheckMs_ < pollIntervalMs) {
      return;
   }
   lastCheckMs_ = nowMs;
   const auto live = isLive();
   // A failed measurement says nothing about the rail.
   if(!live) {
      return;
   }
   lowChecks_ = *live ? 0 : lowChecks_ + 1;
   if(lowChecks_ >= lowChecksToOff) {
      switchOff();
   }
}

Status PowerBlock::out(art::span<const uint8_t> payload) {
   if(payload.size() != 2) {
      return Status::InvalidPara;
   }
   switch(getU16(payload, 0)) {
   case 0:
      switchOff();
      return Status::Ok;
   case supplyMillivolts:
      return switchOn();
   default:
      return Status::InvalidPara;
   }
}

Status PowerBlock::switchOn() {
   if(!hardware_.hasSwitch()) {
      return Status::CmdNotSupport;
   }
   if(on_) {
      return Status::Ok;
   }
   if(!hardware_.hasSense()) {
      hardware_.setSwitch(true);
      on_ = true;
      return Status::Ok;
   }
   // The voltage at the Vref pin is the target supply, as long as the
   // switch is off.
   const auto external = isLive();
   if(!external || *external) {
      return Status::Failed;
   }
   hardware_.setSwitch(true);
   on_ = true;
   lowChecks_ = 0;
   delay_.milliseconds(settleMs);
   const auto live = isLive();
   if(!live || !*live) {
      switchOff();
      return Status::Failed;
   }
   return Status::Ok;
}

void PowerBlock::switchOff() {
   if(on_) {
      hardware_.setSwitch(false);
      on_ = false;
   }
}

std::optional<bool> PowerBlock::isLive() {
   const auto value = hardware_.senseMillivolts();
   if(!value) {
      return std::nullopt;
   }
   return *value > externalSupplyMillivolts;
}

} // namespace protocol
