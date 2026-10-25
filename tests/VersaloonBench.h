#pragma once

// The firmware's USB device on the libArT USB device stack, with the fake
// controller of libArT as the host: the descriptors, the Versaloon function
// and a placeholder for the CDC-ACM function. With a personality switch,
// also the switch request on EP0 (usb/SwitchRequest.h). For the USB tests
// and the replay tests.

#include "boot/BootloaderEntry.h"
#include "protocol/versaloon/CommandProcessor.h"
#include "usb/SwitchRequest.h"
#include "usb/UsbDescriptors.h"
#include "usb/VersaloonFunction.h"

#include <FakeDeviceController.h>
#include <doctest/doctest.h>
#include <usb/Device.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace vsltest {

using art::usb::DescriptorType;
using art::usb::test::FakeDeviceController;
using art::usb::test::setupPacket;

inline constexpr uint8_t standardDeviceIn = 0x80;
inline constexpr uint8_t standardDeviceOut = 0x00;
inline constexpr uint8_t getDescriptor = 6;
inline constexpr uint8_t setAddress = 5;
inline constexpr uint8_t setConfiguration = 9;

inline constexpr uint8_t versaloonOut = 0x03;
inline constexpr uint8_t versaloonIn = 0x82;

inline constexpr protocol::DeviceInfo testInfo{ .version = "v1.2.3", .board = "mini", .hardwareId = 0x15 };

// The device with the descriptors of the firmware. The CDC-ACM function is a
// placeholder that only owns interfaces 1-2: the descriptors open its
// endpoints.
class Bench {
   public:
   // bootloader: the bootloader entry of FW_UPDATE, nullptr for none.
   // usbToXxx: the USB_TO_XXX handler, nullptr for none.
   // personalitySwitch: SWITCH_PERSONALITY and the switch request on EP0,
   // nullptr for an image with one personality. The bench has the switch
   // request on EP0 when it has a personality switch or a bootloader.
   explicit Bench(boot::BootloaderEntry *bootloader = nullptr,
                  protocol::UsbToXxxHandler *usbToXxx = nullptr,
                  personality::PersonalitySwitch *personalitySwitch = nullptr)
      : versaloon(testInfo, bootloader, usbToXxx, personalitySwitch)
      , device_(controller_, descriptors_)
      , cdc_(usb::cdcControlInterface, 2) {
      const std::array<uint8_t, usb::uidBytes> uid{ 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,
                                                    0xDE, 0xF0, 0x01, 0x02, 0x03, 0x04 };
      serial_.set(uid);
      if(personalitySwitch != nullptr || bootloader != nullptr) {
         switchRequest_.emplace(personalitySwitch, bootloader);
         device_.setVendorRequestHandler(&*switchRequest_);
      }
      REQUIRE(device_.addFunction(versaloon));
      REQUIRE(device_.addFunction(cdc_));
      device_.start();
   }

   FakeDeviceController &controller() {
      return controller_;
   }

   art::usb::Device &device() {
      return device_;
   }

   FakeDeviceController::ControlResult descriptor(DescriptorType type, uint8_t index, uint16_t length) {
      bool ok = false;
      auto result = controller_.controlIn(setupPacket(standardDeviceIn, getDescriptor,
                                                      static_cast<uint16_t>(static_cast<uint8_t>(type) << 8 | index), 0, length),
                                          &ok);
      CHECK(ok);
      return result;
   }

   void request(uint8_t request, uint16_t value) {
      bool ok = false;
      const auto result = controller_.controlOut(setupPacket(standardDeviceOut, request, value, 0, 0), {}, &ok);
      CHECK(ok);
      CHECK_FALSE(result.stalled);
   }

   // Bus reset, SET_ADDRESS 5 and SET_CONFIGURATION 1.
   void enumerate() {
      controller_.busReset();
      request(setAddress, 5);
      request(setConfiguration, 1);
      REQUIRE(device_.isConfigured());
   }

   // The host writes data as one bulk transfer: packets of 64 bytes, no ZLP
   // after a full packet (libusb). Each packet waits while the OUT endpoint
   // NAKs; returns false if it NAKs for good.
   bool write(const std::vector<uint8_t> &data) {
      std::size_t offset = 0;
      do {
         const std::size_t size = std::min<std::size_t>(usb::versaloonPacketSize, data.size() - offset);
         if(!controller_.sendOut(versaloonOut, art::span<const uint8_t>(data.data() + offset, size))) {
            return false;
         }
         offset += size;
      } while(offset < data.size());
      return true;
   }

   // The host reads one bulk transfer: packets until a short packet (a ZLP
   // included). The sizes of the packets go to packetSizes. No value if the
   // IN endpoint NAKs at the start.
   std::optional<std::vector<uint8_t>> read(std::vector<std::size_t> *packetSizes = nullptr) {
      std::vector<uint8_t> data;
      for(;;) {
         const auto packet = controller_.takeIn(versaloonIn);
         if(!packet) {
            if(data.empty()) {
               return std::nullopt;
            }
            // The host reads exactly the size it asked for, without a ZLP.
            return data;
         }
         if(packetSizes != nullptr) {
            packetSizes->push_back(packet->size());
         }
         data.insert(data.end(), packet->begin(), packet->end());
         if(packet->size() < usb::versaloonPacketSize) {
            return data;
         }
      }
   }

   usb::VersaloonFunction versaloon;

   private:
   FakeDeviceController controller_;
   usb::SerialNumber serial_;
   const std::array<art::span<const uint8_t>, usb::stringCount> strings_{
      usb::desc::asSpan(usb::languageDescriptor),   usb::desc::asSpan(usb::manufacturerDescriptor),
      usb::desc::asSpan(usb::productDescriptor),    serial_.bytes(),
      usb::desc::asSpan(usb::serialPortDescriptor),
   };
   const art::usb::Descriptors descriptors_{
      .device = usb::desc::asSpan(usb::deviceDescriptor),
      .configuration = usb::desc::asSpan(usb::configurationDescriptor),
      .strings = art::span<const art::span<const uint8_t>>(strings_.data(), strings_.size()),
      // No BOS: the Versaloon personality is a USB 2.00 device.
      .bos = {},
   };
   std::optional<usb::SwitchRequestHandler> switchRequest_;
   art::usb::Device device_;
   art::usb::Function cdc_;
};

} // namespace vsltest
