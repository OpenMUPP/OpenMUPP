// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The CMSIS-DAP personality on the libArT USB device stack, with the fake
// controller of libArT as the host: the descriptors, the CMSIS-DAP function
// on a processor with fake hardware, and a placeholder for the CDC-ACM
// function. With a personality switch, also the switch request on EP0
// before the MS OS 2.0 request (usb/SwitchRequest.h), as in UsbStack. The
// device has the high-speed descriptors too. For the CMSIS-DAP USB tests.

#include "protocol/cmsisdap/DapProcessor.h"
#include "usb/CmsisDapDescriptors.h"
#include "usb/CmsisDapFunction.h"
#include "usb/SwitchRequest.h"

#include <FakeDeviceController.h>
#include <doctest/doctest.h>
#include <usb/Device.h>
#include <usb/MsOs20.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mupptest {

using art::usb::DescriptorType;
using art::usb::test::FakeDeviceController;
using art::usb::test::setupPacket;

inline constexpr uint8_t standardDeviceIn = 0x80;
inline constexpr uint8_t standardDeviceOut = 0x00;
inline constexpr uint8_t getDescriptor = 6;
inline constexpr uint8_t setAddress = 5;
inline constexpr uint8_t setConfiguration = 9;

inline constexpr uint8_t dapOut = usb::dap::outEndpoint;
inline constexpr uint8_t dapIn = usb::dap::inEndpoint;

// An SWD engine that records the calls. A transfer is always OK.
// onTransfer runs in each transfer, as the background service of the
// engine does (protocol/Background.h).
class FakeDapSwd final : public cmsisdap::DapSwd {
   public:
   void init() override {
      ++inits;
   }
   void fini() override {
      ++finis;
   }
   void setClockHz(uint32_t) override {
   }
   void apply(const cmsisdap::SwdSettings &) override {
   }
   void sequenceOut(uint16_t bitCount, art::span<const uint8_t>) override {
      sequences.push_back(bitCount);
   }
   void sequenceIn(uint16_t, art::span<uint8_t>) override {
   }
   uint8_t transfer(uint8_t, uint32_t &data) override {
      ++transfers;
      if(onTransfer) {
         onTransfer();
      }
      data = 0;
      return ack;
   }
   void setAbort(const cmsisdap::TransferAbort &) override {
   }

   int inits{ 0 };
   int finis{ 0 };
   int transfers{ 0 };
   // The bit counts of sequenceOut()
   std::vector<uint16_t> sequences;
   // The acknowledge of each transfer
   uint8_t ack{ cmsisdap::dap::ackOk };
   std::function<void()> onTransfer;
};

class FakeDapGpio final : public protocol::GpioPins {
   public:
   [[nodiscard]] uint16_t available() const override {
      return 0;
   }
   void configure(uint16_t, protocol::PinMode, bool) override {
   }
   void write(uint16_t, uint16_t) override {
   }
   [[nodiscard]] uint16_t read(uint16_t) const override {
      return 0;
   }
};

class FakeDapPlatform final : public cmsisdap::Platform {
   public:
   uint32_t nowUs() override {
      return 0;
   }
   void delayUs(uint32_t microseconds) override {
      delays.push_back(microseconds);
   }
   void setLed(cmsisdap::Led, bool) override {
   }

   std::vector<uint32_t> delays;
};

inline cmsisdap::DeviceInfo dapTestInfo(const char *product = "Product") {
   cmsisdap::DeviceInfo info;
   info.vendor = "Vendor";
   info.product = product;
   info.serialNumber = "0123456789AB";
   info.firmwareVersion = "1.2.3";
   return info;
}

class DapBench {
   // Declared first: the processor copies it.
   cmsisdap::DeviceInfo info_;

   public:
   explicit DapBench(const char *product = "Product",
                     personality::PersonalitySwitch *personalitySwitch = nullptr,
                     boot::BootloaderEntry *bootloader = nullptr)
      : info_(dapTestInfo(product))
      , processor(swd, gpio, platform, info_, nullptr, personalitySwitch, nullptr, bootloader)
      , function(processor)
      , device_(controller_, descriptors_)
      , cdc_(usb::cdcControlInterface, 2) {
      const std::array<uint8_t, usb::uidBytes> uid{ 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,
                                                    0xDE, 0xF0, 0x01, 0x02, 0x03, 0x04 };
      serial_.set(uid);
      if(personalitySwitch != nullptr || bootloader != nullptr) {
         switchRequest_.emplace(personalitySwitch, bootloader, &msOs20Handler_);
         device_.setVendorRequestHandler(&*switchRequest_);
      } else {
         device_.setVendorRequestHandler(&msOs20Handler_);
      }
      REQUIRE(device_.addFunction(function));
      REQUIRE(device_.addFunction(cdc_));
      device_.start();
   }

   FakeDeviceController &controller() {
      return controller_;
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

   // The host writes one command packet, and the main loop runs it.
   // Returns false if the OUT endpoint NAKs.
   bool write(const std::vector<uint8_t> &command) {
      const bool sent = send(command);
      mainLoop();
      return sent;
   }

   // One iteration of the main loop, as UsbStack::poll(): the USB events,
   // then the stored commands one at a time, with the USB events after
   // each one.
   void mainLoop() {
      controller_.deliverQueuedEvents();
      while(function.poll()) {
         controller_.deliverQueuedEvents();
      }
   }

   // The host writes one command packet, and the main loop does not run.
   // Returns false if the OUT endpoint NAKs.
   bool send(const std::vector<uint8_t> &command) {
      return controller_.sendOut(dapOut, art::span<const uint8_t>(command.data(), command.size()));
   }

   // The host reads one response packet. No value if the IN endpoint NAKs.
   std::optional<std::vector<uint8_t>> read() {
      return controller_.takeIn(dapIn);
   }

   FakeDapSwd swd;
   FakeDapGpio gpio;
   FakeDapPlatform platform;
   cmsisdap::Processor processor;
   usb::CmsisDapFunction function;

   private:
   FakeDeviceController controller_;
   usb::SerialNumber serial_;
   const std::array<art::span<const uint8_t>, usb::dap::stringCount> strings_{
      usb::desc::asSpan(usb::dap::languageDescriptor),   usb::desc::asSpan(usb::dap::manufacturerDescriptor),
      usb::desc::asSpan(usb::dap::productDescriptor),    serial_.bytes(),
      usb::desc::asSpan(usb::dap::serialPortDescriptor), usb::desc::asSpan(usb::dap::interfaceStringDescriptor),
   };
   const art::usb::Descriptors descriptors_{
      .device = usb::desc::asSpan(usb::dap::deviceDescriptor),
      .configuration = usb::desc::asSpan(usb::dap::configurationDescriptor),
      .strings = art::span<const art::span<const uint8_t>>(strings_.data(), strings_.size()),
      .bos = usb::desc::asSpan(usb::dap::bosDescriptor),
      // As on the CH32V307. The fake controller is at full speed until
      // controller().setHighSpeed(true).
      .highSpeedConfiguration = usb::desc::asSpan(usb::dap::highSpeedConfigurationDescriptor),
      .deviceQualifier = usb::desc::asSpan(usb::dap::deviceQualifierDescriptor),
   };
   art::usb::msos20::DescriptorSetHandler msOs20Handler_{ usb::dap::msOs20VendorCode,
                                                          usb::desc::asSpan(usb::dap::msOs20DescriptorSet) };
   std::optional<usb::SwitchRequestHandler> switchRequest_;
   art::usb::Device device_;
   art::usb::Function cdc_;
};

} // namespace mupptest
