// SPDX-License-Identifier: GPL-3.0-or-later

#include "protocol/versaloon/GpioBlock.h"

namespace protocol {

namespace {

PinMode modeOf(bool output, bool pullEnable, bool pull) {
   if(output) {
      return pullEnable ? PinMode::OpenDrain : PinMode::PushPull;
   }
   if(!pullEnable) {
      return PinMode::Input;
   }
   return pull ? PinMode::InputPullUp : PinMode::InputPullDown;
}

} // namespace

GpioBlock::GpioBlock(GpioPins &pins)
   : SubCommandBlock(block::gpio)
   , pins_(pins) {
}

void GpioBlock::runSubCommand(const SubCommand &command, Reply &reply) {
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
      return;
   case subcommand::config:
      slot->setStatus(config(command.payload));
      return;
   case subcommand::out:
      slot->setStatus(out(command.payload));
      return;
   case subcommand::in: {
      if(command.payload.size() != 2 || (getU16(command.payload, 0) & ~gpio::all) != 0) {
         slot->setStatus(Status::InvalidPara);
         return;
      }
      const uint16_t value = pins_.read(getU16(command.payload, 0) & pins_.available());
      slot->data()[0] = static_cast<uint8_t>(value);
      slot->data()[1] = static_cast<uint8_t>(value >> 8);
      return;
   }
   default:
      slot->setStatus(Status::CmdNotSupport);
      return;
   }
}

void GpioBlock::hostGone() {
   for(const uint16_t bit : { gpio::srst, gpio::trst }) {
      if((pins_.available() & bit) != 0) {
         pins_.configure(bit, PinMode::InputPullUp, true);
      }
   }
}

Status GpioBlock::config(art::span<const uint8_t> payload) {
   if(payload.size() != 8) {
      return Status::InvalidPara;
   }
   const uint16_t mask = getU16(payload, 0);
   const uint16_t io = getU16(payload, 2) & mask;
   const uint16_t pullEnable = getU16(payload, 4);
   const uint16_t pull = getU16(payload, 6);
   if((mask & ~gpio::all) != 0 || (io & gpio::inputOnly) != 0) {
      return Status::InvalidPara;
   }
   const uint16_t selected = mask & pins_.available();
   for(uint16_t bit = 1; bit <= gpio::tms; bit = static_cast<uint16_t>(bit << 1)) {
      if((selected & bit) != 0) {
         pins_.configure(bit, modeOf((io & bit) != 0, (pullEnable & bit) != 0, (pull & bit) != 0), (pull & bit) != 0);
      }
   }
   return Status::Ok;
}

Status GpioBlock::out(art::span<const uint8_t> payload) {
   if(payload.size() != 4) {
      return Status::InvalidPara;
   }
   const uint16_t mask = getU16(payload, 0);
   if((mask & ~gpio::all) != 0) {
      return Status::InvalidPara;
   }
   pins_.write(mask & pins_.available(), getU16(payload, 2));
   return Status::Ok;
}

} // namespace protocol
