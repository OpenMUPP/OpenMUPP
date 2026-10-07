#include "usb/VersaloonFunction.h"

namespace usb {

VersaloonFunction::VersaloonFunction(const protocol::DeviceInfo &info, boot::BootloaderEntry *bootloader, protocol::UsbToXxxHandler *usbToXxx)
   : VendorBulkFunction(versaloonInterface,
                        art::usb::VendorBulkEndpoints{ .out = versaloonOutEndpoint, .in = versaloonInEndpoint, .maxPacketSize = versaloonPacketSize })
   , assembler_(art::span<uint8_t>(command_.data(), command_.size()), versaloonPacketSize)
   , processor_(info, bootloader, usbToXxx) {
}

bool VersaloonFunction::dataReceived(art::span<const uint8_t> packet) {
   using Result = protocol::CommandAssembler::Result;
   switch(assembler_.add(packet)) {
   case Result::Incomplete:
   case Result::Ignored:
      return true;
   case Result::Error:
      return sendReply(art::span<const uint8_t>());
   case Result::Complete:
      break;
   }
   const auto size = processor_.process(assembler_.command(), art::span<uint8_t>(reply_.data(), reply_.size()));
   if(!size) {
      return true;
   }
   if(*size > reply_.size()) {
      // A handler error: do not send the RAM after the buffer.
      return sendReply(art::span<const uint8_t>());
   }
   return sendReply(art::span<const uint8_t>(reply_.data(), *size));
}

bool VersaloonFunction::sendReply(art::span<const uint8_t> reply) {
   const auto zeroLengthPacket = reply.size() < reply_.size() ? ZeroLengthPacket::Auto : ZeroLengthPacket::Never;
   // Not configured: drop the reply.
   return !sendInTransfer(reply, zeroLengthPacket);
}

void VersaloonFunction::transferSent() {
   resumeOut();
}

void VersaloonFunction::dataPathClosed() {
   assembler_.reset();
}

} // namespace usb
