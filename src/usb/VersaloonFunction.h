#pragma once

// The Versaloon vendor interface (interface 0, bulk OUT 0x03, bulk IN 0x82),
// which OpenOCD (vsllink) talks to. Hardware-independent: the host unit
// tests run it on a fake controller.
//
// The function joins the OUT packets of a transfer into one command
// (protocol/CommandAssembler.h), runs it (protocol/CommandProcessor.h) and
// sends the reply as one IN transfer. The command buffer and the reply
// buffer are separate, so a reply can be longer than its command. While
// the reply is on its way, the OUT endpoint NAKs: the next command waits.
//
// A reply whose size is a multiple of the packet size ends with a ZLP, so
// that the host read ends. A reply of protocol::bufferSize bytes has no
// ZLP: the host reads that size, so its read ends without one, and the ZLP
// would end the next read.
//
// A wrong command (length error or overflow) gets an empty reply (a ZLP).
// The host then sees a reply of the wrong size at once, without a timeout.

#include <array>
#include <cstdint>
#include <usb/VendorBulkFunction.h>

#include "protocol/CommandAssembler.h"
#include "protocol/CommandProcessor.h"
#include "protocol/Versaloon.h"
#include "usb/UsbDescriptors.h"

namespace usb {

class VersaloonFunction final : public art::usb::VendorBulkFunction {
   public:
   // See protocol::CommandProcessor for bootloader and usbToXxx.
   explicit VersaloonFunction(const protocol::DeviceInfo &info,
                              boot::BootloaderEntry *bootloader = nullptr,
                              protocol::UsbToXxxHandler *usbToXxx = nullptr);

   protected:
   bool dataReceived(art::span<const uint8_t> packet) override;
   void transferSent() override;
   void dataPathClosed() override;

   private:
   // Starts the IN transfer of the reply. Returns the value for
   // dataReceived(): false holds the OUT endpoint until the reply is sent.
   bool sendReply(art::span<const uint8_t> reply);

   std::array<uint8_t, protocol::bufferSize> command_{};
   std::array<uint8_t, protocol::bufferSize> reply_{};
   protocol::CommandAssembler assembler_;
   protocol::CommandProcessor processor_;
};

} // namespace usb
