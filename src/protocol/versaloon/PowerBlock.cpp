// SPDX-License-Identifier: GPL-3.0-or-later

#include "protocol/versaloon/PowerBlock.h"

namespace protocol {

PowerBlock::PowerBlock(TargetPower &power)
   : SubCommandBlock(block::power)
   , power_(power) {
}

uint16_t PowerBlock::millivolts() {
   return power_.millivolts();
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
      auto &hardware = power_.hardware();
      if(!hardware.hasSense()) {
         slot->setStatus(Status::CmdNotSupport);
         return;
      }
      const auto value = hardware.senseMillivolts();
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

Status PowerBlock::out(art::span<const uint8_t> payload) {
   if(payload.size() != 2) {
      return Status::InvalidPara;
   }
   switch(getU16(payload, 0)) {
   case 0:
      power_.switchOff();
      return Status::Ok;
   case supplyMillivolts:
      switch(power_.switchOn()) {
      case TargetPower::Result::Ok:
         return Status::Ok;
      case TargetPower::Result::Refused:
         return Status::Failed;
      case TargetPower::Result::NotSupported:
         return Status::CmdNotSupport;
      }
      return Status::Failed;
   default:
      return Status::InvalidPara;
   }
}

} // namespace protocol
