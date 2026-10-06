#pragma once

// The Versaloon vendor interface (interface 0, bulk OUT 0x03, bulk IN 0x82),
// which OpenOCD (vsllink) talks to.
//
// Stub until the command protocol (phase C): it sends each OUT packet back
// unchanged on the IN endpoint, with the same size (a zero-length packet
// too). While the echo is on its way, the OUT endpoint NAKs, so no packet
// is lost. Hardware-independent: the host unit tests run it on a fake
// controller.

#include <array>
#include <cstddef>
#include <cstdint>
#include <usb/VendorBulkFunction.h>

#include "usb/UsbDescriptors.h"

namespace usb {

class VersaloonFunction final : public art::usb::VendorBulkFunction {
   public:
   VersaloonFunction();

   protected:
   bool dataReceived(art::span<const uint8_t> packet) override;
   void transferSent() override;

   private:
   std::array<uint8_t, versaloonPacketSize> echo_{};
};

} // namespace usb
