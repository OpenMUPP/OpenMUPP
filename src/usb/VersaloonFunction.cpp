#include "usb/VersaloonFunction.h"

#include <algorithm>

#include "boot/BootloaderEntry.h"

namespace usb {

VersaloonFunction::VersaloonFunction(boot::BootloaderEntry *bootloader)
   : VendorBulkFunction(versaloonInterface,
                        art::usb::VendorBulkEndpoints{ .out = versaloonOutEndpoint, .in = versaloonInEndpoint, .maxPacketSize = versaloonPacketSize })
   , bootloader_(bootloader) {
}

bool VersaloonFunction::dataReceived(art::span<const uint8_t> packet) {
   if(!packet.empty() && packet[0] == versaloonFwUpdate) {
      return firmwareUpdate(packet);
   }
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

bool VersaloonFunction::firmwareUpdate(art::span<const uint8_t> packet) {
   const bool accept = bootloader_ != nullptr && packet.size() >= 2 && packet[1] == versaloonFwUpdateKey;
   echo_[0] = accept ? fwUpdateAccepted : fwUpdateRefused;
   const bool queued = sendInTransfer(art::span<const uint8_t>(echo_.data(), 1), ZeroLengthPacket::Never);
   if(accept) {
      // The main loop resets after the reply delay, also when the host
      // does not read the reply.
      bootloader_->request();
   }
   return !queued;
}

void VersaloonFunction::transferSent() {
   resumeOut();
}

} // namespace usb
