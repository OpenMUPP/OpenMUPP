#include "protocol/UsbToXxx.h"

#include <algorithm>

namespace protocol {

art::span<uint8_t> Reply::add(Status status, std::size_t dataSize) {
   if(overflow_ || buffer_.size() - size_ < 1 + dataSize) {
      overflow_ = true;
      return {};
   }
   buffer_[size_] = static_cast<uint8_t>(status);
   const auto data = buffer_.subspan(size_ + 1, dataSize);
   std::fill(data.begin(), data.end(), 0);
   size_ += 1 + dataSize;
   return data;
}

bool SubCommandBlock::run(art::span<const uint8_t> payload, Reply &reply) {
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
      runSubCommand(command, reply);
      offset += length;
   }
   return true;
}

} // namespace protocol
