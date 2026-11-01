#pragma once

// The personality switch request on EP0 (docs/usb-device.md, "Switch request
// on EP0"). Hardware-independent: the host unit tests check it.
//
// An image with more than one personality (MUPP_MULTI_PERSONALITY) accepts
// the request in each personality, so a host can leave a personality that
// has no switch command of its own, for example WCH-Link. An image with a
// bootloader accepts the bootloader code (boot::bootloaderCode), also with
// one personality. The request is a USB vendor request to the device, with
// no data stage:
//
//   bmRequestType  0x40 (vendor, host-to-device, device)
//   bRequest       switchRequestCode
//   wValue         the personality code (personality/Personality.h), or
//                  boot::bootloaderCode
//   wIndex         0
//   wLength        0
//
// The handler calls PersonalitySwitch::request() or BootloaderEntry::request(),
// as SWITCH_PERSONALITY and ID_DAP_Vendor0 do. The status stage of the
// request comes before the reset (replyDelayMs). A code that is not a
// personality of the image, the bootloader code without a bootloader, or a
// request in another form, stalls.
//
// usb::Device has one handler for the vendor requests to the device. The
// CMSIS-DAP personality also needs it for the MS OS 2.0 descriptor set.
// Thus the handler sends each other request to the next handler.

#include <cstdint>
#include <optional>
#include <usb/UsbTypes.h>
#include <usb/VendorRequestHandler.h>
#include <utils/compat/span.h>

#include "boot/BootloaderEntry.h"
#include "personality/Personality.h"
#include "personality/PersonalitySwitch.h"

namespace usb {

// bmRequestType of the switch request
inline constexpr uint8_t switchRequestType = 0x40;
// bRequest of the switch request: the code of SWITCH_PERSONALITY of the
// Versaloon protocol. It must differ from the MS OS 2.0 vendor code of the
// CMSIS-DAP personality (usb/UsbStack.cpp checks it).
inline constexpr uint8_t switchRequestCode = 0x0E;

class SwitchRequestHandler final : public art::usb::VendorRequestHandler {
   public:
   // personalitySwitch: nullptr in an image with one personality.
   // bootloader: nullptr in an image without a bootloader. next: the
   // handler of the other vendor requests to the device, nullptr for none.
   // personalities: the codes that the request accepts, normally the
   // personalities of the image. The objects must outlive the handler.
   explicit SwitchRequestHandler(personality::PersonalitySwitch *personalitySwitch,
                                 boot::BootloaderEntry *bootloader = nullptr,
                                 art::usb::VendorRequestHandler *next = nullptr,
                                 art::span<const personality::Personality> personalities =
                                 art::span<const personality::Personality>(personality::all.data(), personality::all.size()))
      : personalitySwitch_(personalitySwitch)
      , bootloader_(bootloader)
      , next_(next)
      , personalities_(personalities) {
   }

   std::optional<art::span<const uint8_t>> vendorRequest(const art::usb::SetupPacket &setup, art::span<uint8_t> buffer) override {
      if(setup.bRequest != switchRequestCode) {
         if(next_ == nullptr) {
            return std::nullopt;
         }
         return next_->vendorRequest(setup, buffer);
      }
      if(setup.bmRequestType != switchRequestType || setup.wIndex != 0 || setup.wLength != 0 || setup.wValue > UINT8_MAX) {
         return std::nullopt;
      }
      if(setup.wValue == boot::bootloaderCode && bootloader_ != nullptr) {
         // The main loop enters the bootloader after the reply delay
         bootloader_->request();
         return art::span<const uint8_t>();
      }
      const auto target =
      personalitySwitch_ != nullptr ? personality::fromCodeIn(personalities_, static_cast<uint8_t>(setup.wValue)) : std::nullopt;
      if(!target) {
         return std::nullopt;
      }
      // The main loop stores the personality and resets after the reply
      // delay: the device sends the status stage first.
      personalitySwitch_->request(*target);
      return art::span<const uint8_t>();
   }

   private:
   personality::PersonalitySwitch *personalitySwitch_;
   boot::BootloaderEntry *bootloader_;
   art::usb::VendorRequestHandler *next_;
   art::span<const personality::Personality> personalities_;
};

} // namespace usb
