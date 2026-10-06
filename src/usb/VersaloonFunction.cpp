#include "usb/VersaloonFunction.h"

#include <algorithm>

namespace usb {

VersaloonFunction::VersaloonFunction()
   : VendorBulkFunction(versaloonInterface,
                        art::usb::VendorBulkEndpoints{ .out = versaloonOutEndpoint, .in = versaloonInEndpoint, .maxPacketSize = versaloonPacketSize }) {
}

bool VersaloonFunction::dataReceived(art::span<const uint8_t> packet) {
   const std::size_t size = std::min(packet.size(), echo_.size());
   std::copy_n(packet.begin(), size, echo_.begin());
   // One IN packet per OUT packet: no ZLP after a full packet.
   if(!sendInTransfer(art::span<const uint8_t>(echo_.data(), size), ZeroLengthPacket::Never)) {
      // Not configured: drop the packet.
      return true;
   }
   // Hold the OUT endpoint until the host has read the echo.
   return false;
}

void VersaloonFunction::transferSent() {
   resumeOut();
}

} // namespace usb
