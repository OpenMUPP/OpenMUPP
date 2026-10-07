#include "protocol/UsbToAll.h"

#include <algorithm>

#include "protocol/Versaloon.h"

namespace protocol {

namespace {

void setCapability(std::array<uint8_t, UsbToAll::capabilityBytes> &bitmap, uint8_t type) {
   const unsigned bit = type - usbToXxxFirst;
   bitmap[bit / 8] = static_cast<uint8_t>(bitmap[bit / 8] | 1u << (bit % 8));
}

constexpr uint16_t delayInMs = 0x8000;

} // namespace

UsbToAll::UsbToAll(Delay &delay)
   : delay_(delay) {
}

bool UsbToAll::add(Block &block) {
   const uint8_t type = block.type();
   if(blockCount_ == blocks_.size() || !isUsbToXxx(type) || type == block::delay || type == block::info ||
      type == block::all || find(type) != nullptr) {
      return false;
   }
   blocks_[blockCount_++] = &block;
   return true;
}

std::optional<std::size_t> UsbToAll::process(art::span<const uint8_t> command, art::span<uint8_t> reply) {
   Reply out(reply);
   // A top-level command other than USB_TO_ALL is one block.
   std::size_t offset = command[0] == block::all ? blockHeaderSize : 0;
   while(offset < command.size()) {
      if(command.size() - offset < blockHeaderSize) {
         return 0;
      }
      const uint8_t type = command[offset];
      const std::size_t length = getU16(command, offset + 1);
      if(length < blockHeaderSize || length > command.size() - offset) {
         return 0;
      }
      if(!runBlock(type, command.subspan(offset + blockHeaderSize, length - blockHeaderSize), out)) {
         return 0;
      }
      offset += length;
   }
   if(out.overflow()) {
      return 0;
   }
   return out.size();
}

bool UsbToAll::runBlock(uint8_t type, art::span<const uint8_t> payload, Reply &reply) {
   switch(type) {
   case block::info: {
      const auto bitmap = capabilities();
      const auto data = reply.add(Status::Ok, bitmap.size());
      std::copy_n(bitmap.begin(), data.size(), data.begin());
      return true;
   }
   case block::delay:
      runDelay(payload, reply);
      return true;
   default:
      break;
   }
   Block *const handler = find(type);
   if(handler == nullptr) {
      reply.add(Status::CmdNotSupport);
      return true;
   }
   return handler->run(payload, reply);
}

void UsbToAll::runDelay(art::span<const uint8_t> payload, Reply &reply) {
   if(payload.size() != 2) {
      reply.add(Status::InvalidPara);
      return;
   }
   const uint16_t value = getU16(payload, 0);
   if(value & delayInMs) {
      delay_.milliseconds(static_cast<uint16_t>(value & ~delayInMs));
   } else {
      delay_.microseconds(value);
   }
   reply.add(Status::Ok);
}

std::array<uint8_t, UsbToAll::capabilityBytes> UsbToAll::capabilities() const {
   std::array<uint8_t, capabilityBytes> bitmap{};
   for(std::size_t i = 0; i < blockCount_; ++i) {
      setCapability(bitmap, blocks_[i]->type());
   }
   setCapability(bitmap, block::delay);
   setCapability(bitmap, block::info);
   setCapability(bitmap, block::all);
   return bitmap;
}

Block *UsbToAll::find(uint8_t type) const {
   const auto end = blocks_.begin() + static_cast<std::ptrdiff_t>(blockCount_);
   const auto it = std::find_if(blocks_.begin(), end, [type](const Block *block) { return block->type() == type; });
   return it == end ? nullptr : *it;
}

} // namespace protocol
