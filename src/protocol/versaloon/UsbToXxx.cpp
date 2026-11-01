// SPDX-License-Identifier: GPL-3.0-or-later

#include "protocol/versaloon/UsbToXxx.h"

#include <algorithm>

namespace protocol {

namespace {

// Calls visit(command) for each sub-command of payload. Returns false when
// the framing is wrong, or when visit() returns false.
template <typename Visit>
bool forEachSubCommand(art::span<const uint8_t> payload, Visit visit) {
   std::size_t offset = 0;
   while(offset < payload.size()) {
      if(payload.size() - offset < blockHeaderSize) {
         return false;
      }
      const uint8_t first = payload[offset];
      const std::size_t length = getU16(payload, offset + 1);
      offset += blockHeaderSize;
      if(length > payload.size() - offset) {
         return false;
      }
      const SubCommand command{
         .command = static_cast<uint8_t>(first & subcommand::commandMask),
         .index = static_cast<uint8_t>(first & subcommand::indexMask),
         .payload = payload.subspan(offset, length),
      };
      if(!visit(command)) {
         return false;
      }
      offset += length;
   }
   return true;
}

} // namespace

std::optional<Reply::Slot> Reply::reserve(std::size_t dataSize) {
   if(overflow_ || buffer_.size() - size_ < 1 + dataSize) {
      overflow_ = true;
      return std::nullopt;
   }
   buffer_[size_] = static_cast<uint8_t>(Status::Ok);
   const auto data = buffer_.subspan(size_ + 1, dataSize);
   std::fill(data.begin(), data.end(), 0);
   Slot slot(buffer_[size_], data);
   size_ += 1 + dataSize;
   return slot;
}

void Reply::add(Status status) {
   if(auto slot = reserve()) {
      slot->setStatus(status);
   }
}

bool SubCommandBlock::isWellFormed(art::span<const uint8_t> payload) const {
   return forEachSubCommand(payload, [](const SubCommand &) { return true; });
}

void SubCommandBlock::run(art::span<const uint8_t> payload, Reply &reply) {
   static_cast<void>(forEachSubCommand(payload, [this, &reply](const SubCommand &command) {
      runSubCommand(command, reply);
      return !reply.overflow();
   }));
}

} // namespace protocol
