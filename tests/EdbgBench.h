// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The CMSIS-DAP device with the HID interface and the EDBG transport of the
// AVR commands (protocol/avr/EdbgTransport.h), as in src/main.cpp, for the
// EDBG tests: a mock AVR on the ISP engine and a mock AVR with debugWIRE on
// the dW engine. The host is the fake controller of libArT.

#include "CmsisDapBench.h"
#include "FakePower.h"
#include "MockAvrDw.h"
#include "MockDebugWireTarget.h"
#include "MockIspTarget.h"
#include "avr/DebugWireTarget.h"
#include "protocol/avr/Avr8Scope.h"
#include "protocol/avr/EdbgTransport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace edbgtest {

using Bytes = std::vector<uint8_t>;
using Engine = avr::IspEngine<mupptest::MockIspPins, mupptest::MockIspSpi>;
using DwEngine = avr::DebugWireEngine<mupptest::MockDwPins>;
using DwTarget = avr::DebugWireTarget<DwEngine>;

// The dW target: 16 MHz, dW at 125 kbaud
inline constexpr uint32_t dwBitCycles = mupptest::MockDwPins::clockHz / (16'000'000 / 128);
using mupptest::FakeDeviceController;
using protocol::PinClient;
namespace isp = avr::isp;
namespace handler = avr::edbg::handler;

inline constexpr uint8_t hidOut = usb::dap::hidOutEndpoint;
inline constexpr uint8_t hidIn = usb::dap::hidInEndpoint;
inline constexpr std::size_t reportSize = usb::dap::hidReportSize;
inline constexpr auto configurationWithHid = usb::dap::makeConfiguration<true>(usb::dap::packetSize, usb::cdcNotificationInterval);
inline constexpr auto highSpeedConfigurationWithHid =
usb::dap::makeConfiguration<true>(usb::dap::highSpeedPacketSize, usb::dap::cdcNotificationHighSpeedInterval);

// The scope responses
inline const Bytes ok{ 0x80, 0x00 };
inline Bytes failed(uint8_t code) {
   return { 0xA0, 0x00, code };
}
inline Bytes data(Bytes values) {
   values.insert(values.begin(), { 0x84, 0x01 });
   values.push_back(0x00);
   return values;
}

// The values of avrdude.conf for the ATmega2560
inline const Bytes enterRequest{ isp::command::enterProgmode, 200, 1, 1, 32, 0, 0x53, 3, 0xAC, 0x53, 0x00, 0x00 };
inline const Bytes leaveRequest{ isp::command::leaveProgmode, 1, 1 };

// The CMSIS-DAP device with the HID interface and the EDBG transport on
// the HID processor, as in src/main.cpp.
class EdbgBench {
   cmsisdap::DeviceInfo info_{ mupptest::dapTestInfo() };

   public:
   EdbgBench()
      : delay(power)
      , targetPower(power, delay)
      , bulkProcessor(swd, gpio, platform, info_, nullptr, nullptr, nullptr, nullptr, nullptr, &targetPower)
      , hidProcessor(swd, gpio, platform, info_)
      , edbg(tool, scope, avr8Scope, hidProcessor, &targetPower, &owner)
      , bulk(bulkProcessor)
      , hid(hidProcessor)
      , device_(controller_, descriptors_)
      , cdc_(usb::cdcControlInterface, 2) {
      bulkProcessor.sharePins(owner, PinClient::CmsisDapBulk);
      hidProcessor.sharePins(owner, PinClient::CmsisDapHid);
      hidProcessor.setVendorCommands(edbg);
      REQUIRE(device_.addFunction(bulk));
      REQUIRE(device_.addFunction(cdc_));
      REQUIRE(device_.addFunction(hid));
      device_.start();
      enumerate();
   }

   void request(uint8_t request, uint16_t value) {
      bool done = false;
      const auto result =
      controller_.controlOut(mupptest::setupPacket(mupptest::standardDeviceOut, request, value, 0, 0), {}, &done);
      CHECK(done);
      CHECK_FALSE(result.stalled);
   }

   void enumerate() {
      controller_.busReset();
      request(mupptest::setAddress, 5);
      request(mupptest::setConfiguration, 1);
      REQUIRE(hid.isConfigured());
   }

   FakeDeviceController &controller() {
      return controller_;
   }

   // As UsbStack::poll()
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

   // One HID report, padded with zeros as the host does, and the response
   // report.
   Bytes hidCommand(Bytes command) {
      command.resize(reportSize, 0);
      REQUIRE(controller_.sendOut(hidOut, art::span<const uint8_t>(command.data(), command.size())));
      mainLoop();
      const auto report = controller_.takeIn(hidIn);
      REQUIRE(report.has_value());
      REQUIRE(report->size() == reportSize);
      return *report;
   }

   Bytes bulkCommand(const Bytes &command) {
      REQUIRE(controller_.sendOut(mupptest::dapOut, art::span<const uint8_t>(command.data(), command.size())));
      mainLoop();
      const auto response = controller_.takeIn(mupptest::dapIn);
      REQUIRE(response.has_value());
      return *response;
   }

   // One AVR_CMD fragment: the response code
   uint8_t fragment(uint8_t info, const Bytes &part, std::optional<uint16_t> size = std::nullopt) {
      const uint16_t length = size.value_or(static_cast<uint16_t>(part.size()));
      Bytes report{ avr::edbg::avrCmd, info, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length) };
      report.insert(report.end(), part.begin(), part.end());
      const Bytes response = hidCommand(report);
      CHECK(response[0] == avr::edbg::avrCmd);
      return response[1];
   }

   // Sends a frame in fragments of `chunk` bytes, as avrdude and Bloom do.
   void sendFrame(const Bytes &frame, std::size_t chunk = reportSize - 4) {
      const std::size_t count = (frame.size() + chunk - 1) / chunk;
      REQUIRE(count <= 15);
      for(std::size_t i = 0; i < count; ++i) {
         const auto begin = frame.begin() + static_cast<std::ptrdiff_t>(i * chunk);
         const auto end = frame.begin() + static_cast<std::ptrdiff_t>(std::min(frame.size(), (i + 1) * chunk));
         const uint8_t code = fragment(static_cast<uint8_t>((i + 1) << 4 | count), Bytes(begin, end));
         CHECK(code == (i + 1 == count ? avr::edbg::fragmentDone : avr::edbg::fragmentMore));
      }
   }

   // The response frame from AVR_RSP, in fragments. No value: no response.
   std::optional<Bytes> receiveFrame() {
      Bytes frame;
      uint8_t count = 0;
      for(uint8_t number = 1;; ++number) {
         const Bytes report = hidCommand({ avr::edbg::avrRsp });
         REQUIRE(report[0] == avr::edbg::avrRsp);
         if(report[1] == 0) {
            REQUIRE(number == 1);
            CHECK(report[2] == 0);
            CHECK(report[3] == 0);
            return std::nullopt;
         }
         if(number == 1) {
            count = report[1] & 0x0F;
         }
         REQUIRE(report[1] == (number << 4 | count));
         const std::size_t size = (std::size_t{ report[2] } << 8) | report[3];
         REQUIRE(size <= reportSize - 4);
         frame.insert(frame.end(), report.begin() + 4, report.begin() + 4 + static_cast<std::ptrdiff_t>(size));
         if(number == count) {
            return frame;
         }
         CHECK(size == reportSize - 4);
      }
   }

   // One command frame to a scope, and the payload of the response frame
   // (after the check of SOF, sequence and handler).
   Bytes transact(uint8_t handlerId, const Bytes &payload) {
      Bytes frame{ avr::edbg::sof, 0x00, static_cast<uint8_t>(sequence), static_cast<uint8_t>(sequence >> 8), handlerId };
      frame.insert(frame.end(), payload.begin(), payload.end());
      sendFrame(frame);
      const auto response = receiveFrame();
      REQUIRE(response.has_value());
      REQUIRE(response->size() >= 4);
      CHECK((*response)[0] == avr::edbg::sof);
      CHECK((*response)[1] == static_cast<uint8_t>(sequence));
      CHECK((*response)[2] == static_cast<uint8_t>(sequence >> 8));
      CHECK((*response)[3] == handlerId);
      ++sequence;
      return Bytes(response->begin() + 4, response->end());
   }

   Bytes isp(const Bytes &command) {
      return transact(handler::avrIsp, command);
   }

   void enterProgmode() {
      REQUIRE(isp({ isp::command::setBaud, 0xA0, 0x0F }) == Bytes{ isp::command::setBaud, isp::status::ok });
      REQUIRE(isp(enterRequest) == Bytes{ isp::command::enterProgmode, isp::status::ok });
   }

   FakePower power;
   FakeDelay delay;
   protocol::TargetPower targetPower;
   mupptest::MockIspTarget target;
   mupptest::MockIspPins pins{ target };
   Engine engine{ pins };
   avr::IspScope<Engine> scope{ engine };
   mupptest::MockDwTarget dwLine{ dwBitCycles };
   mupptest::MockDwPins dwPins{ dwLine };
   DwEngine dwEngine{ dwPins };
   mupptest::MockAvrDw avr{ dwLine, mupptest::MockAvrDw::atmega328p };
   DwTarget dwTarget{ dwEngine, avr::DwDevice{} };
   avr::Avr8Scope<DwTarget> avr8Scope{ dwTarget };
   avr::ToolInfo tool{ .name = "OpenMUPP",
                       .serialNumber = "0123456789AB",
                       .hardwareVersion = 1,
                       .firmwareMajor = 2,
                       .firmwareMinor = 3,
                       .firmwareBuild = 0x0405 };
   mupptest::FakeDapSwd swd;
   mupptest::FakeDapGpio gpio;
   mupptest::FakeDapPlatform platform;
   protocol::TargetPinOwner owner;
   cmsisdap::Processor bulkProcessor;
   cmsisdap::Processor hidProcessor;
   avr::EdbgTransport edbg;
   usb::CmsisDapFunction bulk;
   usb::CmsisDapHidFunction hid;
   uint16_t sequence{ 0 };

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

inline Bytes pattern(std::size_t bytes, uint8_t seed) {
   Bytes values(bytes);
   for(std::size_t i = 0; i < bytes; ++i) {
      values[i] = static_cast<uint8_t>(seed + i * 7u);
   }
   return values;
}


} // namespace edbgtest
