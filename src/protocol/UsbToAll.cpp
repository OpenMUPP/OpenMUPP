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
   if(!forEachBlock(command, [this](uint8_t type, art::span<const uint8_t> payload) {
         const Block *const handler = find(type);
         return handler == nullptr || handler->isWellFormed(payload);
      })) {
      return 0;
   }
   Reply out(reply);
   static_cast<void>(forEachBlock(command, [this, &out](uint8_t type, art::span<const uint8_t> payload) {
      runBlock(type, payload, out);
      return !out.overflow();
   }));
   if(out.overflow()) {
      return 0;
   }
   return out.size();
}

void UsbToAll::hostGone() {
   for(std::size_t i = 0; i < blockCount_; ++i) {
      blocks_[i]->hostGone();
   }
}

template <typename Visit>
bool UsbToAll::forEachBlock(art::span<const uint8_t> command, Visit visit) {
   // A top-level command other than USB_TO_ALL is one block.
   std::size_t offset = command[0] == block::all ? blockHeaderSize : 0;
   while(offset < command.size()) {
      if(command.size() - offset < blockHeaderSize) {
         return false;
      }
      const uint8_t type = command[offset];
      const std::size_t length = getU16(command, offset + 1);
      if(length < blockHeaderSize || length > command.size() - offset) {
         return false;
      }
      if(!visit(type, command.subspan(offset + blockHeaderSize, length - blockHeaderSize))) {
         return false;
      }
      offset += length;
   }
   return true;
}

void UsbToAll::runBlock(uint8_t type, art::span<const uint8_t> payload, Reply &reply) {
   switch(type) {
   case block::info:
      if(auto slot = reply.reserve(capabilityBytes)) {
         const auto bitmap = capabilities();
         std::copy(bitmap.begin(), bitmap.end(), slot->data().begin());
      }
      return;
   case block::delay:
      runDelay(payload, reply);
      return;
   default:
      break;
   }
   Block *const handler = find(type);
   if(handler == nullptr) {
      reply.add(Status::CmdNotSupport);
      return;
   }
   handler->run(payload, reply);
}

void UsbToAll::runDelay(art::span<const uint8_t> payload, Reply &reply) {
   auto slot = reply.reserve();
   if(!slot) {
      return;
   }
   if(payload.size() != 2) {
      slot->setStatus(Status::InvalidPara);
      return;
   }
   const uint16_t value = getU16(payload, 0);
   if(value & delayInMs) {
      delay_.milliseconds(static_cast<uint16_t>(value & ~delayInMs));
   } else {
      delay_.microseconds(value);
   }
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
