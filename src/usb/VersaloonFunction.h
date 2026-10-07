#pragma once

// The Versaloon vendor interface (interface 0, bulk OUT 0x03, bulk IN 0x82),
// which OpenOCD (vsllink) talks to.
//
// Stub until the command protocol (phase C): it sends each OUT packet back
// unchanged on the IN endpoint, with the same size (a zero-length packet
// too). While the echo is on its way, the OUT endpoint NAKs, so no packet
// is lost. Hardware-independent: the host unit tests run it on a fake
// controller.
//
// The one real command is VERSALOON_FW_UPDATE: [0x0F][0xAA]. The function
// replies with one status byte and requests the bootloader entry
// (boot/BootloaderEntry.h). Without a bootloader, or with a wrong key, it
// replies fwUpdateRefused and the firmware keeps running.

#include <array>
#include <cstddef>
#include <cstdint>
#include <usb/VendorBulkFunction.h>

#include "usb/UsbDescriptors.h"

namespace boot {
class BootloaderEntry;
}

namespace usb {

// VERSALOON_FW_UPDATE and its key, as in OpenOCD (versaloon_internal.h)
inline constexpr uint8_t versaloonFwUpdate = 0x0F;
inline constexpr uint8_t versaloonFwUpdateKey = 0xAA;

// Status byte of the FW_UPDATE reply
inline constexpr uint8_t fwUpdateAccepted = 0x00;
inline constexpr uint8_t fwUpdateRefused = 0x01;

class VersaloonFunction final : public art::usb::VendorBulkFunction {
   public:
   // bootloader: the bootloader entry of FW_UPDATE. nullptr: the firmware
   // has no bootloader and refuses FW_UPDATE.
   explicit VersaloonFunction(boot::BootloaderEntry *bootloader = nullptr);

   protected:
   bool dataReceived(art::span<const uint8_t> packet) override;
   void transferSent() override;

   private:
   // Queues the FW_UPDATE reply and requests the bootloader entry.
   bool firmwareUpdate(art::span<const uint8_t> packet);

   boot::BootloaderEntry *bootloader_;
   std::array<uint8_t, versaloonPacketSize> echo_{};
};

} // namespace usb
