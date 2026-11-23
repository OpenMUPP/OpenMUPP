// SPDX-License-Identifier: GPL-3.0-or-later
// The CMSIS-DAP v1 (HID) interface next to the v2 interface
// (MUPP_CMSIS_DAP_HID): the descriptors, the HID function on its own
// processor, and the target pins that the two processors share. With the
// fake controller of libArT as the host.

#include "CmsisDapBench.h"

#include <doctest/doctest.h>
#include <usb/HidFunction.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

using art::usb::DescriptorType;
using art::usb::EndpointType;
using mupptest::FakeDapGpio;
using mupptest::FakeDapPlatform;
using mupptest::FakeDapSwd;
using mupptest::FakeDeviceController;
using mupptest::setupPacket;
using Bytes = std::vector<uint8_t>;

constexpr uint8_t hidOut = usb::dap::hidOutEndpoint;
constexpr uint8_t hidIn = usb::dap::hidInEndpoint;
constexpr auto configurationWithHid = usb::dap::makeConfiguration<true>(usb::dap::packetSize, usb::cdcNotificationInterval);
constexpr auto highSpeedConfigurationWithHid =
usb::dap::makeConfiguration<true>(usb::dap::highSpeedPacketSize, usb::dap::cdcNotificationHighSpeedInterval);

Bytes bytes(const auto &descriptor) {
   return { descriptor.begin(), descriptor.end() };
}

// The CMSIS-DAP device with the HID interface: two processors on one
// engine, sharing the target pins.
class HidBench {
   cmsisdap::DeviceInfo info_{ mupptest::dapTestInfo() };

   public:
   HidBench()
      : bulkProcessor(swd, gpio, platform, info_)
      , hidProcessor(swd, gpio, platform, info_)
      , bulk(bulkProcessor)
      , hid(hidProcessor)
      , device_(controller_, descriptors_)
      , cdc_(usb::cdcControlInterface, 2) {
      bulkProcessor.sharePins(owner, protocol::PinClient::CmsisDapBulk);
      hidProcessor.sharePins(owner, protocol::PinClient::CmsisDapHid);
      REQUIRE(device_.addFunction(bulk));
      REQUIRE(device_.addFunction(cdc_));
      REQUIRE(device_.addFunction(hid));
      device_.start();
   }

   FakeDeviceController &controller() {
      return controller_;
   }

   void request(uint8_t request, uint16_t value) {
      bool ok = false;
      const auto result = controller_.controlOut(setupPacket(mupptest::standardDeviceOut, request, value, 0, 0), {}, &ok);
      CHECK(ok);
      CHECK_FALSE(result.stalled);
   }

   void enumerate() {
      controller_.busReset();
      request(mupptest::setAddress, 5);
      request(mupptest::setConfiguration, 1);
      REQUIRE(device_.isConfigured());
   }

   FakeDeviceController::ControlResult getInterfaceDescriptor(uint8_t type, uint16_t length) {
      bool ok = false;
      auto result = controller_.controlIn(setupPacket(0x81, mupptest::getDescriptor, static_cast<uint16_t>(type << 8),
                                                      usb::dap::hidInterface, length),
                                          &ok);
      CHECK(ok);
      return result;
   }

   // As UsbStack::poll(): the USB events, then one command of each
   // interface per round.
   void mainLoop() {
      controller_.deliverQueuedEvents();
      while(true) {
         const bool bulkRan = bulk.poll();
         const bool hidRan = hid.poll();
         if(!bulkRan && !hidRan) {
            break;
         }
         controller_.deliverQueuedEvents();
      }
   }

   // One command report on the HID interface, padded to 64 bytes as the
   // host does, and its response report.
   std::optional<Bytes> hidCommand(Bytes command) {
      command.resize(usb::dap::hidReportSize, 0);
      if(!controller_.sendOut(hidOut, art::span<const uint8_t>(command.data(), command.size()))) {
         return std::nullopt;
      }
      mainLoop();
      return controller_.takeIn(hidIn);
   }

   std::optional<Bytes> bulkCommand(const Bytes &command) {
      if(!controller_.sendOut(mupptest::dapOut, art::span<const uint8_t>(command.data(), command.size()))) {
         return std::nullopt;
      }
      mainLoop();
      return controller_.takeIn(mupptest::dapIn);
   }

   FakeDapSwd swd;
   FakeDapGpio gpio;
   FakeDapPlatform platform;
   protocol::TargetPinOwner owner;
   cmsisdap::Processor bulkProcessor;
   cmsisdap::Processor hidProcessor;
   usb::CmsisDapFunction bulk;
   usb::CmsisDapHidFunction hid;

   private:
   FakeDeviceController controller_;
   const art::usb::Descriptors descriptors_{
      .device = usb::desc::asSpan(usb::dap::deviceDescriptor),
      .configuration = usb::desc::asSpan(configurationWithHid),
      .highSpeedConfiguration = usb::desc::asSpan(highSpeedConfigurationWithHid),
      .deviceQualifier = usb::desc::asSpan(usb::dap::deviceQualifierDescriptor),
   };
   art::usb::Device device_;
   art::usb::Function cdc_;
};

// The report padded with zeros to 64 bytes
Bytes report(Bytes start) {
   start.resize(usb::dap::hidReportSize, 0);
   return start;
}

} // namespace

TEST_CASE("HID: the configuration has the CMSIS-DAP v1 interface after the other three") {
   const auto check = [](const Bytes &configuration, const Bytes &without) {
      // bNumInterfaces
      CHECK(configuration[4] == 4);
      // The other interfaces are as without the HID interface: only the
      // header (wTotalLength, bNumInterfaces) and the HID interface differ.
      REQUIRE(configuration.size() == without.size() + usb::dap::hidInterfaceDescriptor.size());
      const auto end = configuration.begin() + static_cast<std::ptrdiff_t>(without.size());
      CHECK(Bytes(configuration.begin() + 9, end) == Bytes(without.begin() + 9, without.end()));
      // The HID interface is the same at both speeds.
      CHECK(Bytes(end, configuration.end()) == bytes(usb::dap::hidInterfaceDescriptor));
   };
   check(bytes(configurationWithHid), bytes(usb::dap::makeConfiguration<false>(usb::dap::packetSize, usb::cdcNotificationInterval)));
   check(bytes(highSpeedConfigurationWithHid),
         bytes(usb::dap::makeConfiguration<false>(usb::dap::highSpeedPacketSize, usb::dap::cdcNotificationHighSpeedInterval)));
   const Bytes hidInterface = bytes(usb::dap::hidInterfaceDescriptor);
   // Interface 3, two endpoints, HID class
   CHECK(Bytes(hidInterface.begin(), hidInterface.begin() + 9) == Bytes{ 9, 4, 3, 0, 2, 0x03, 0, 0, 0 });
   // HID descriptor: HID 1.11, one report descriptor of 27 bytes
   CHECK(Bytes(hidInterface.begin() + 9, hidInterface.begin() + 18) == Bytes{ 9, 0x21, 0x11, 0x01, 0, 1, 0x22, 27, 0 });
   // Interrupt IN 0x85 and OUT 0x05, 64 bytes, bInterval 1
   CHECK(Bytes(hidInterface.begin() + 18, hidInterface.end()) == Bytes{ 7, 5, 0x85, 3, 64, 0, 1, 7, 5, 0x05, 3, 64, 0, 1 });
   CHECK(usb::dap::hidEndpointCount == 6);
}

TEST_CASE("HID: the host reads the report descriptor and opens the interrupt endpoints") {
   for(const bool highSpeed : { false, true }) {
      CAPTURE(highSpeed);
      HidBench bench;
      bench.controller().setHighSpeed(highSpeed);
      bench.enumerate();
      CHECK(bench.getInterfaceDescriptor(art::usb::hid::kDescriptorTypeReport, 255).data == bytes(usb::dap::hidReportDescriptor));
      for(const uint8_t address : { hidOut, hidIn }) {
         const auto endpoint = bench.controller().endpoint(address);
         REQUIRE(endpoint.has_value());
         CHECK(endpoint->type == EndpointType::Interrupt);
         CHECK(endpoint->maxPacketSize == 64);
      }
      CHECK(bench.hid.isConfigured());
   }
}

TEST_CASE("HID: DAP_Info reports 64-byte packets, also at high speed, and each response is one full report") {
   HidBench bench;
   bench.controller().setHighSpeed(true);
   bench.enumerate();
   // DAP_Info packet size: 64 on HID, 512 on bulk
   CHECK(bench.hidCommand({ 0x00, 0xFF }) == report({ 0x00, 0x02, 0x40, 0x00 }));
   CHECK(bench.bulkCommand({ 0x00, 0xFF }) == Bytes{ 0x00, 0x02, 0x00, 0x02 });
   // A short response is padded with zeros to the report size.
   CHECK(bench.hidCommand({ 0x09, 0x00, 0x00 }) == report({ 0x09, 0x00 }));
   CHECK(bench.hid.commandCount() == 2);
}

TEST_CASE("HID: the first interface that connects owns the target pins") {
   HidBench bench;
   bench.enumerate();
   // avrdude: DAP_Connect (SWD) on the HID interface
   CHECK(bench.hidCommand({ 0x02, 0x01 }) == report({ 0x02, 0x01 }));
   CHECK(bench.owner.owner() == protocol::PinClient::CmsisDapHid);
   // A second tool on the bulk interface gets port 0.
   CHECK(bench.bulkCommand({ 0x02, 0x01 }) == Bytes{ 0x02, 0x00 });
   // DAP_Disconnect on HID frees the pins.
   CHECK(bench.hidCommand({ 0x03 }) == report({ 0x03, 0x00 }));
   CHECK(bench.bulkCommand({ 0x02, 0x01 }) == Bytes{ 0x02, 0x01 });
   CHECK(bench.owner.owner() == protocol::PinClient::CmsisDapBulk);
   // A bus reset: both hosts are gone, the pins are free.
   bench.enumerate();
   CHECK(bench.owner.owner() == protocol::PinClient::None);
}

TEST_CASE("HID: DAP_Connect does not drive the pins before the first transfer") {
   HidBench bench;
   bench.enumerate();
   // avrdude: DAP_Connect, then its own commands (#45)
   CHECK(bench.hidCommand({ 0x02, 0x01 }) == report({ 0x02, 0x01 }));
   CHECK(bench.owner.owner() == protocol::PinClient::CmsisDapHid);
   CHECK(bench.swd.inits == 0);
   // DAP_Disconnect without a transfer: nothing to release.
   CHECK(bench.hidCommand({ 0x03 }) == report({ 0x03, 0x00 }));
   CHECK(bench.swd.inits == 0);
   CHECK(bench.swd.finis == 0);
   CHECK(bench.owner.owner() == protocol::PinClient::None);
   // The first transfer drives the pins, once.
   CHECK(bench.hidCommand({ 0x02, 0x01 }) == report({ 0x02, 0x01 }));
   CHECK(bench.hidCommand({ 0x05, 0x00, 0x01, 0x02 })->at(2) == 0x01);
   CHECK(bench.swd.inits == 1);
   CHECK(bench.swd.transfers == 1);
   CHECK(bench.hidCommand({ 0x05, 0x00, 0x01, 0x02 })->at(2) == 0x01);
   CHECK(bench.swd.inits == 1);
   // A second DAP_Connect releases the pins that were driven.
   CHECK(bench.hidCommand({ 0x02, 0x01 }) == report({ 0x02, 0x01 }));
   CHECK(bench.swd.finis == 1);
   CHECK(bench.hidCommand({ 0x03 }) == report({ 0x03, 0x00 }));
   CHECK(bench.swd.finis == 1);
   // A bulk interface drives the pins at its first sequence.
   CHECK(bench.bulkCommand({ 0x02, 0x01 }) == Bytes{ 0x02, 0x01 });
   CHECK(bench.swd.inits == 1);
   CHECK(bench.bulkCommand({ 0x12, 0x08, 0xFF }) == Bytes{ 0x12, 0x00 });
   CHECK(bench.swd.inits == 2);
   CHECK(bench.bulkCommand({ 0x03 }) == Bytes{ 0x03, 0x00 });
   CHECK(bench.swd.finis == 2);
}

TEST_CASE("HID: no interface gets the pins while the board keeps them") {
   HidBench bench;
   // A board without debug outputs (board::Definition::debugOutputs, main.cpp)
   REQUIRE(bench.owner.acquire(protocol::PinClient::Board));
   bench.enumerate();
   CHECK(bench.hidCommand({ 0x02, 0x01 }) == report({ 0x02, 0x00 }));
   CHECK(bench.bulkCommand({ 0x02, 0x01 }) == Bytes{ 0x02, 0x00 });
   CHECK(bench.bulkCommand({ 0x02, 0x02 }) == Bytes{ 0x02, 0x00 });
   CHECK(bench.swd.inits == 0);
   // Neither a disconnect nor a bus reset frees the pins.
   CHECK(bench.bulkCommand({ 0x03 }) == Bytes{ 0x03, 0x00 });
   bench.enumerate();
   CHECK(bench.owner.owner() == protocol::PinClient::Board);
   CHECK(bench.bulkCommand({ 0x02, 0x01 }) == Bytes{ 0x02, 0x00 });
}

TEST_CASE("HID: DAP_TransferAbort stops a running DAP_TransferBlock") {
   HidBench bench;
   bench.enumerate();
   CHECK(bench.hidCommand({ 0x02, 0x01 }) == report({ 0x02, 0x01 }));
   bool sent = false;
   bench.swd.onTransfer = [&bench, &sent] {
      if(bench.swd.transfers == 2) {
         // A padded report, as the host sends it
         Bytes abort = report({ 0x07 });
         sent = bench.controller().sendOut(hidOut, art::span<const uint8_t>(abort.data(), abort.size()));
      }
   };
   // DAP_TransferBlock: 6 reads of DP CTRL/STAT
   CHECK(bench.hidCommand({ 0x06, 0x00, 0x06, 0x00, 0x06 }) == report({ 0x06, 0x02, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0 }));
   CHECK(sent);
   CHECK(bench.swd.transfers == 2);
   // The abort has no response.
   CHECK_FALSE(bench.controller().takeIn(hidIn).has_value());
}
