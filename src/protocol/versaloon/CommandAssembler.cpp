#include "protocol/versaloon/CommandAssembler.h"

#include <algorithm>

#include "protocol/versaloon/Versaloon.h"

namespace protocol {

CommandAssembler::CommandAssembler(art::span<uint8_t> buffer, std::size_t packetSize)
   : buffer_(buffer)
   , packetSize_(packetSize) {
}

CommandAssembler::Result CommandAssembler::add(art::span<const uint8_t> packet) {
   switch(state_) {
   case State::Idle:
      return start(packet);
   case State::Draining:
      return drain(packet);
   case State::Collecting:
   case State::Overflow:
      break;
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
      if(packet.size() >= packetSize_) {
         // One packet, but the transfer goes on.
         return failTransfer(packet);
      }
      // One packet. buffer_ is larger than a packet.
      received_ = std::min(packet.size(), buffer_.size());
      std::copy_n(packet.begin(), received_, buffer_.begin());
      return Result::Complete;
   }
   if(packet.size() < usbToXxxHeaderSize) {
      return failTransfer(packet);
   }
   expected_ = static_cast<std::size_t>(packet[1] | packet[2] << 8);
   if(expected_ < usbToXxxHeaderSize) {
      return failTransfer(packet);
   }
   state_ = expected_ > buffer_.size() ? State::Overflow : State::Collecting;
   return proceed(packet);
}

CommandAssembler::Result CommandAssembler::proceed(art::span<const uint8_t> packet) {
   if(packet.size() > expected_ - received_) {
      return failTransfer(packet);
   }
   if(state_ == State::Collecting) {
      std::copy(packet.begin(), packet.end(), buffer_.begin() + static_cast<std::ptrdiff_t>(received_));
   }
   received_ += packet.size();
   if(received_ == expected_) {
      if(state_ == State::Overflow) {
         // The command ends at its length.
         reset();
         return Result::Error;
      }
      state_ = State::Idle;
      return Result::Complete;
   }
   if(packet.size() < packetSize_) {
      // A short packet ends the transfer of the host.
      return failTransfer(packet);
   }
   return Result::Incomplete;
}

CommandAssembler::Result CommandAssembler::drain(art::span<const uint8_t> packet) {
   if(packet.size() < packetSize_) {
      reset();
      return Result::Error;
   }
   return Result::Incomplete;
}

CommandAssembler::Result CommandAssembler::failTransfer(art::span<const uint8_t> packet) {
   if(packet.size() < packetSize_) {
      // The packet ends the transfer.
      reset();
      return Result::Error;
   }
   state_ = State::Draining;
   return Result::Incomplete;
}

} // namespace protocol
