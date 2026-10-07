#include "protocol/CommandAssembler.h"

#include <algorithm>

#include "protocol/Versaloon.h"

namespace protocol {

CommandAssembler::CommandAssembler(art::span<uint8_t> buffer, std::size_t packetSize)
   : buffer_(buffer)
   , packetSize_(packetSize) {
}

CommandAssembler::Result CommandAssembler::add(art::span<const uint8_t> packet) {
   if(state_ == State::Idle) {
      return start(packet);
   }
   return proceed(packet);
}

void CommandAssembler::reset() {
   state_ = State::Idle;
   received_ = 0;
   expected_ = 0;
}

CommandAssembler::Result CommandAssembler::start(art::span<const uint8_t> packet) {
   received_ = 0;
   if(packet.empty()) {
      return Result::Ignored;
   }
   if(!isUsbToXxx(packet[0])) {
      // One packet. buffer_ is larger than a packet.
      received_ = std::min(packet.size(), buffer_.size());
      std::copy_n(packet.begin(), received_, buffer_.begin());
      return Result::Complete;
   }
   if(packet.size() < usbToXxxHeaderSize) {
      return fail();
   }
   expected_ = static_cast<std::size_t>(packet[1] | packet[2] << 8);
   if(expected_ < usbToXxxHeaderSize) {
      return fail();
   }
   state_ = expected_ > buffer_.size() ? State::Discarding : State::Collecting;
   return proceed(packet);
}

CommandAssembler::Result CommandAssembler::proceed(art::span<const uint8_t> packet) {
   if(packet.size() > expected_ - received_) {
      return fail();
   }
   if(state_ == State::Collecting) {
      std::copy(packet.begin(), packet.end(), buffer_.begin() + static_cast<std::ptrdiff_t>(received_));
   }
   received_ += packet.size();
   if(received_ == expected_) {
      if(state_ == State::Discarding) {
         return fail();
      }
      state_ = State::Idle;
      return Result::Complete;
   }
   if(packet.size() < packetSize_) {
      // A short packet ends the transfer of the host.
      return fail();
   }
   return Result::Incomplete;
}

CommandAssembler::Result CommandAssembler::fail() {
   reset();
   return Result::Error;
}

} // namespace protocol
