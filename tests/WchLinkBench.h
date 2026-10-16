#pragma once

// The WCH-Link personality on the libArT USB device stack, with the fake
// controller of libArT as the host: the descriptors, and the WCH-Link
// function on a processor with the wire engine, mock pins and a mock DM.
// With a personality switch, also the switch request on EP0
// (usb/SwitchRequest.h). For the WCH-Link USB tests.

#include "MockDebugModule.h"
#include "MockRvSwdTarget.h"
#include "protocol/wchlink/WchLinkProcessor.h"
#include "protocol/wchlink/WchLinkTarget.h"
#include "usb/SwitchRequest.h"
#include "usb/WchLinkDescriptors.h"
#include "usb/WchLinkFunction.h"

#include <FakeDeviceController.h>
#include <doctest/doctest.h>
#include <usb/Device.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace vsltest {

class FakeWchPlatform final : public wchlink::Platform {
   public:
   void delayUs(uint32_t microseconds) override {
      delays.push_back(microseconds);
   }

   std::vector<uint32_t> delays;
};

class WchLinkBench {
   public:
   static constexpr uint8_t standardDeviceIn = 0x80;
   static constexpr uint8_t standardDeviceOut = 0x00;
   static constexpr uint8_t getDescriptor = 6;
   static constexpr uint8_t setAddress = 5;
   static constexpr uint8_t setConfiguration = 9;

   // wire: the wire of the target. The pins see the frames of the other
   // wire as noise. personalitySwitch: the switch request on EP0, nullptr
   // for an image with one personality.
   explicit WchLinkBench(rvswd::Wire wire = rvswd::Wire::Two, personality::PersonalitySwitch *personalitySwitch = nullptr)
      : pins(wire)
      , dm(pins)
      , target(pins)
      , processor(target, platform)
      , function(processor)
      , device_(controller_, descriptors_) {
      const std::array<uint8_t, usb::uidBytes> uid{ 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,
                                                    0xDE, 0xF0, 0x01, 0x02, 0x03, 0x04 };
      serial_.set(uid);
      if(personalitySwitch != nullptr) {
         switchRequest_.emplace(*personalitySwitch);
         device_.setVendorRequestHandler(&*switchRequest_);
      }
      REQUIRE(device_.addFunction(function));
      device_.start();
   }

   art::usb::test::FakeDeviceController &controller() {
      return controller_;
   }

   art::usb::test::FakeDeviceController::ControlResult descriptor(art::usb::DescriptorType type, uint8_t index, uint16_t length) {
      bool ok = false;
      auto result =
      controller_.controlIn(art::usb::test::setupPacket(standardDeviceIn, getDescriptor,
                                                        static_cast<uint16_t>(static_cast<uint8_t>(type) << 8 | index), 0, length),
                            &ok);
      CHECK(ok);
      return result;
   }

   void request(uint8_t request, uint16_t value) {
      bool ok = false;
      const auto result = controller_.controlOut(art::usb::test::setupPacket(standardDeviceOut, request, value, 0, 0), {}, &ok);
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

   // The host writes one request packet, and the main loop runs it.
   // Returns false if the OUT endpoint NAKs.
   bool write(const std::vector<uint8_t> &packet) {
      const bool sent = send(packet);
      mainLoop();
      return sent;
   }

   // One iteration of the main loop, as UsbStack::poll(): the USB events,
   // then the function until it has nothing to do, with the USB events
   // after each step.
   void mainLoop() {
      controller_.deliverQueuedEvents();
      while(function.poll()) {
         controller_.deliverQueuedEvents();
      }
   }

   // The host writes one packet, and the main loop does not run. Returns
   // false if the OUT endpoint NAKs.
   bool send(const std::vector<uint8_t> &packet) {
      return controller_.sendOut(usb::wch::commandOutEndpoint, art::span<const uint8_t>(packet.data(), packet.size()));
   }

   // The host reads one response packet. No value if the IN endpoint NAKs.
   std::optional<std::vector<uint8_t>> read() {
      return controller_.takeIn(usb::wch::commandInEndpoint);
   }

   // The host reads one data packet, then the main loop runs.
   std::optional<std::vector<uint8_t>> readData() {
      auto packet = controller_.takeIn(usb::wch::dataInEndpoint);
      mainLoop();
      return packet;
   }

   // A request, the main loop, and the response
   std::vector<uint8_t> transact(const std::vector<uint8_t> &packet) {
      REQUIRE(write(packet));
      auto response = read();
      REQUIRE(response.has_value());
      mainLoop();
      return *response;
   }

   MockRvSwdTarget pins;
   MockDebugModule dm;
   wchlink::RvSwdTarget<MockRvSwdTarget> target;
   FakeWchPlatform platform;
   wchlink::Processor processor;
   usb::WchLinkFunction function;

   private:
   art::usb::test::FakeDeviceController controller_;
   usb::SerialNumber serial_;
   const std::array<art::span<const uint8_t>, usb::wch::stringCount> strings_{
      usb::desc::asSpan(usb::wch::languageDescriptor),
      usb::desc::asSpan(usb::wch::manufacturerDescriptor),
      usb::desc::asSpan(usb::wch::productDescriptor),
      serial_.bytes(),
   };
   const art::usb::Descriptors descriptors_{
      .device = usb::desc::asSpan(usb::wch::deviceDescriptor),
      .configuration = usb::desc::asSpan(usb::wch::configurationDescriptor),
      .strings = art::span<const art::span<const uint8_t>>(strings_.data(), strings_.size()),
   };
   std::optional<usb::SwitchRequestHandler> switchRequest_;
   art::usb::Device device_;
};

} // namespace vsltest
