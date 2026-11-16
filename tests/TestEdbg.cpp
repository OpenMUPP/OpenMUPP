// SPDX-License-Identifier: GPL-3.0-or-later
// The EDBG transport of the AVR commands (protocol/avr/EdbgTransport.h)
// through the HID function: the fragments of AVR_CMD and AVR_RSP, the
// frames, the scopes, the target pins and the target power, with a mock
// AVR on the ISP engine. The host is the fake controller of libArT.

#include "CmsisDapBench.h"
#include "FakePower.h"
#include "MockIspTarget.h"
#include "protocol/avr/EdbgTransport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
using Engine = avr::IspEngine<mupptest::MockIspPins, mupptest::MockIspSpi>;
using mupptest::FakeDeviceController;
using protocol::PinClient;
namespace isp = avr::isp;
namespace handler = avr::edbg::handler;

constexpr uint8_t hidOut = usb::dap::hidOutEndpoint;
constexpr uint8_t hidIn = usb::dap::hidInEndpoint;
constexpr std::size_t reportSize = usb::dap::hidReportSize;
constexpr auto configurationWithHid = usb::dap::makeConfiguration<true>(usb::dap::packetSize, usb::cdcNotificationInterval);
constexpr auto highSpeedConfigurationWithHid =
usb::dap::makeConfiguration<true>(usb::dap::highSpeedPacketSize, usb::dap::cdcNotificationHighSpeedInterval);

// The scope responses
const Bytes ok{ 0x80, 0x00 };
Bytes failed(uint8_t code) {
   return { 0xA0, 0x00, code };
}
Bytes data(Bytes values) {
   values.insert(values.begin(), { 0x84, 0x01 });
   values.push_back(0x00);
   return values;
}

// The values of avrdude.conf for the ATmega2560
const Bytes enterRequest{ isp::command::enterProgmode, 200, 1, 1, 32, 0, 0x53, 3, 0xAC, 0x53, 0x00, 0x00 };
const Bytes leaveRequest{ isp::command::leaveProgmode, 1, 1 };

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
      , edbg(tool, scope, hidProcessor, &targetPower, &owner)
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

Bytes pattern(std::size_t bytes, uint8_t seed) {
   Bytes values(bytes);
   for(std::size_t i = 0; i < bytes; ++i) {
      values[i] = static_cast<uint8_t>(seed + i * 7u);
   }
   return values;
}

} // namespace

TEST_CASE("EDBG: avrdude reads the signature with jtagice3_isp") {
   EdbgBench b;
   // jtag3_edbg_prepare(): DAP_Connect (SWD) and DAP_HostStatus
   CHECK(b.hidCommand({ 0x02, 0x01 })[1] == 0x01);
   CHECK(b.hidCommand({ 0x01, 0x00, 0x01 })[1] == 0x00);
   // Sign-on
   CHECK(b.transact(handler::housekeeping, { 0x10, 0x00 }) == ok);
   // stk500v2_jtag3_initialize(): arch mega, session programming, connection ISP
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x00, 0x00, 0x01, 0x02 }) == ok);
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x00, 0x01, 0x01, 0x01 }) == ok);
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x01, 0x00, 0x01, 0x01 }) == ok);
   // GET_BAUD: the default of the engine
   CHECK(b.isp({ isp::command::getBaud }) == Bytes{ isp::command::getBaud, 100, 0, isp::status::ok });
   b.enterProgmode();
   CHECK(b.target.enabled);
   for(uint8_t i = 0; i < 3; ++i) {
      CHECK(b.isp({ isp::command::readSignature, 4, 0x30, 0x00, i, 0x00 }) ==
            Bytes{ isp::command::readSignature, isp::status::ok, b.target.signature[i], isp::status::ok });
   }
   CHECK(b.isp(leaveRequest) == Bytes{ isp::command::leaveProgmode, isp::status::ok });
   CHECK_FALSE(b.pins.driven);
   // jtag3_close(): AVR sign-off (deactivate physical), sign-off
   CHECK(b.transact(handler::avr8, { 0x11, 0x00, 0x00 }) == ok);
   CHECK(b.transact(handler::housekeeping, { 0x11, 0x00, 0x00 }) == ok);
   // DAP_Connect took the pins: DAP_Disconnect gives them back.
   CHECK(b.owner.owner() == PinClient::CmsisDapHid);
   CHECK(b.hidCommand({ 0x03 })[1] == 0x00);
   CHECK(b.owner.owner() == PinClient::None);
}

TEST_CASE("EDBG: a flash page in 5 command fragments, and a read in 5 response fragments") {
   EdbgBench b;
   b.enterProgmode();
   // avrdude: LOAD_ADDRESS, PROGRAM_FLASH page mode with the page write
   CHECK(b.isp({ isp::command::loadAddress, 0x80, 0x00, 0x00, 0x00 }) == Bytes{ isp::command::loadAddress, isp::status::ok });
   const Bytes page = pattern(256, 3);
   Bytes program{ isp::command::programFlash, 0x01, 0x00, 0xC1, 10, 0x40, 0x4C, 0x20, 0x00, 0x00 };
   program.insert(program.end(), page.begin(), page.end());
   // The 5 bytes of the frame header and 266 bytes: 271 bytes, 5 fragments
   CHECK(b.isp(program) == Bytes{ isp::command::programFlash, isp::status::ok });
   CHECK(std::equal(page.begin(), page.end(), b.target.flash.begin()));
   CHECK(b.isp({ isp::command::loadAddress, 0x80, 0x00, 0x00, 0x00 }) == Bytes{ isp::command::loadAddress, isp::status::ok });
   // [0x0E][seq u16][0x11][0x14][OK][256 bytes][OK]: 263 bytes, 5 fragments
   Bytes expected{ isp::command::readFlash, isp::status::ok };
   expected.insert(expected.end(), page.begin(), page.end());
   expected.push_back(isp::status::ok);
   CHECK(b.isp({ isp::command::readFlash, 0x01, 0x00, 0x20 }) == expected);
}

TEST_CASE("EDBG: the fragment sizes of the host are free, up to the report") {
   EdbgBench b;
   const Bytes frame{ avr::edbg::sof, 0x00, 0x34, 0x12, handler::housekeeping, 0x10, 0x00 };
   // One byte per fragment, and an empty fragment
   CHECK(b.fragment(0x18, { frame[0] }) == avr::edbg::fragmentMore);
   CHECK(b.fragment(0x28, {}) == avr::edbg::fragmentMore);
   for(uint8_t i = 1; i < frame.size(); ++i) {
      CHECK(b.fragment(static_cast<uint8_t>((i + 2) << 4 | 8), { frame[i] }) ==
            (i + 1u == frame.size() ? avr::edbg::fragmentDone : avr::edbg::fragmentMore));
   }
   CHECK(b.receiveFrame() == Bytes{ 0x0E, 0x34, 0x12, handler::housekeeping, 0x80, 0x00 });
   // A full report: 60 bytes of frame
   Bytes full{ avr::edbg::sof, 0x00, 0xFF, 0xFF, handler::housekeeping, 0x10, 0x00 };
   full.resize(reportSize - 4, 0xEE);
   CHECK(b.fragment(0x11, full) == avr::edbg::fragmentDone);
   CHECK(b.receiveFrame() == Bytes{ 0x0E, 0xFF, 0xFF, handler::housekeeping, 0x80, 0x00 });
}

TEST_CASE("EDBG: a bad fragment drops the frame and gives no response") {
   EdbgBench b;
   const Bytes start{ avr::edbg::sof, 0x00, 0x00, 0x00, handler::housekeeping };
   const auto rejected = [&b](uint8_t info, const Bytes &part, std::optional<uint16_t> size = std::nullopt) {
      CHECK(b.fragment(info, part, size) == avr::edbg::fragmentRejected);
      CHECK_FALSE(b.receiveFrame().has_value());
   };
   SUBCASE("fragment 0") {
      rejected(0x01, start);
   }
   SUBCASE("count 0") {
      rejected(0x10, start);
   }
   SUBCASE("number above the count") {
      rejected(0x32, start);
   }
   SUBCASE("a fragment that does not follow") {
      CHECK(b.fragment(0x13, start) == avr::edbg::fragmentMore);
      rejected(0x33, { 0x10, 0x00 });
      // The frame is gone: fragment 2 has no fragment 1 now.
      rejected(0x23, { 0x10, 0x00 });
   }
   SUBCASE("another count") {
      CHECK(b.fragment(0x12, start) == avr::edbg::fragmentMore);
      rejected(0x23, { 0x10, 0x00 });
   }
   SUBCASE("a size above the report") {
      rejected(0x11, start, reportSize - 3);
      rejected(0x11, start, 0xFFFF);
   }
   SUBCASE("a frame above maxFrameBytes") {
      static_assert(avr::EdbgTransport::maxFrameBytes / (reportSize - 4) + 1 <= 15);
      constexpr uint8_t count = avr::EdbgTransport::maxFrameBytes / (reportSize - 4) + 1;
      const Bytes part(reportSize - 4, 0);
      for(uint8_t i = 1; i < count; ++i) {
         CHECK(b.fragment(static_cast<uint8_t>(i << 4 | count), part) == avr::edbg::fragmentMore);
      }
      rejected(static_cast<uint8_t>(count << 4 | count), part);
   }
   // A good frame after it works.
   CHECK(b.transact(handler::housekeeping, { 0x10, 0x00 }) == ok);
}

TEST_CASE("EDBG: a frame with a bad header gets no response") {
   EdbgBench b;
   const auto noResponse = [&b](const Bytes &frame) {
      CHECK(b.fragment(0x11, frame) == avr::edbg::fragmentDone);
      CHECK_FALSE(b.receiveFrame().has_value());
   };
   noResponse({ 0x0F, 0x00, 0x00, 0x00, handler::housekeeping, 0x10, 0x00 });
   noResponse({ avr::edbg::sof, 0x01, 0x00, 0x00, handler::housekeeping, 0x10, 0x00 });
   noResponse({ avr::edbg::sof, 0x00, 0x00, 0x00 });
   noResponse({});
   // The handler alone: a scope gets an empty payload.
   CHECK(b.transact(handler::housekeeping, {}) == failed(0x10));
   CHECK(b.transact(handler::avrIsp, {}) == failed(0x10));
}

TEST_CASE("EDBG: a new fragment 1 drops the response that the host did not read") {
   EdbgBench b;
   b.sendFrame({ avr::edbg::sof, 0x00, 0x01, 0x00, handler::housekeeping, 0x10, 0x00 });
   b.sendFrame({ avr::edbg::sof, 0x00, 0x02, 0x00, handler::housekeeping, 0x11, 0x00, 0x00 });
   CHECK(b.receiveFrame() == Bytes{ 0x0E, 0x02, 0x00, handler::housekeeping, 0x80, 0x00 });
   CHECK_FALSE(b.receiveFrame().has_value());
   // A response that the host read half: the rest is gone too.
   b.enterProgmode();
   b.sendFrame({ avr::edbg::sof, 0x00, 0x03, 0x00, handler::avrIsp, isp::command::readFlash, 0x01, 0x00, 0x20 });
   CHECK(b.hidCommand({ avr::edbg::avrRsp })[1] == 0x15);
   b.sendFrame({ avr::edbg::sof, 0x00, 0x04, 0x00, handler::housekeeping, 0x10, 0x00 });
   CHECK(b.receiveFrame() == Bytes{ 0x0E, 0x04, 0x00, handler::housekeeping, 0x80, 0x00 });
}

TEST_CASE("EDBG: AVR_EVT has no event, and the vendor commands do not nest") {
   EdbgBench b;
   CHECK(b.hidCommand({ avr::edbg::avrEvt }) == [] {
      Bytes report{ avr::edbg::avrEvt, 0x00, 0x00 };
      report.resize(reportSize, 0);
      return report;
   }());
   // DAP_ExecuteCommands: DAP_Info packet count, then AVR_EVT
   const Bytes response = b.hidCommand({ 0x7F, 0x02, 0x00, 0xFE, avr::edbg::avrEvt });
   CHECK(Bytes(response.begin(), response.begin() + 7) == Bytes{ 0x7F, 0x02, 0x00, 0x01, 0x04, 0xFF, 0x00 });
}

TEST_CASE("EDBG: housekeeping versions and the target voltage") {
   EdbgBench b;
   // jtag3_display(): hardware version, firmware major, minor, build
   CHECK(b.transact(handler::housekeeping, { 0x02, 0x00, 0x00, 0x00, 0x05 }) == data({ 1, 2, 3, 0x05, 0x04 }));
   // Bloom: firmware major, minor and build
   CHECK(b.transact(handler::housekeeping, { 0x02, 0x00, 0x00, 0x01, 0x04 }) == data({ 2, 3, 0x05, 0x04 }));
   b.power.externalMv = 3280;
   CHECK(b.transact(handler::housekeeping, { 0x02, 0x00, 0x01, 0x00, 0x02 }) == data({ 0xD0, 0x0C }));
   // Ranges outside the tables, an empty range, a short command
   CHECK(b.transact(handler::housekeeping, { 0x02, 0x00, 0x00, 0x04, 0x02 }) == failed(0x10));
   CHECK(b.transact(handler::housekeeping, { 0x02, 0x00, 0x00, 0xFF, 0xFF }) == failed(0x10));
   CHECK(b.transact(handler::housekeeping, { 0x02, 0x00, 0x00, 0x00, 0x00 }) == failed(0x10));
   CHECK(b.transact(handler::housekeeping, { 0x02, 0x00, 0x01, 0x00, 0x03 }) == failed(0x10));
   CHECK(b.transact(handler::housekeeping, { 0x02, 0x00, 0x02, 0x00, 0x01 }) == failed(0x10));
   CHECK(b.transact(handler::housekeeping, { 0x02, 0x00, 0x00, 0x00 }) == failed(0x10));
   // The parameters are read-only (avrdude -x vtarg sets PARM3_VADJUST).
   CHECK(b.transact(handler::housekeeping, { 0x01, 0x00, 0x01, 0x20, 0x02, 0xE4, 0x0C }) == failed(0x10));
   // FW_UPGRADE with the key of avrdude: a clean error, the probe stays.
   CHECK(b.transact(handler::housekeeping, { 0x50, 0x00, 0x70, 0x6D, 0x6A, 0x00 }) == failed(0x10));
   CHECK(b.transact(handler::housekeeping, { 0x50 }) == failed(0x10));
   CHECK(b.transact(handler::housekeeping, { 0x30, 0x00, 0x00 }) == failed(0x10));
}

TEST_CASE("EDBG: discovery and unknown handlers") {
   EdbgBench b;
   CHECK(b.transact(handler::discovery, { 0x00, 0x00, 0x00 }) == Bytes{ 0x81, 0x00, 0x00, 0x01, 0x11, 0x12, 0x20 });
   CHECK(b.transact(handler::discovery, { 0x00, 0x00, 0x80 }) == Bytes{ 0x81, 0x00, 'O', 'p', 'e', 'n', 'M', 'U', 'P', 'P' });
   const Bytes serial = b.transact(handler::discovery, { 0x00, 0x00, 0x81 });
   CHECK(std::string(serial.begin() + 2, serial.end()) == "0123456789AB");
   CHECK(b.transact(handler::discovery, { 0x00, 0x00, 0x82 }) == failed(0x10));
   CHECK(b.transact(handler::discovery, { 0x00, 0x00 }) == failed(0x10));
   CHECK(b.transact(handler::discovery, { 0x01, 0x00, 0x00 }) == failed(0x10));
   for(const uint8_t other : { uint8_t{ 0x13 }, uint8_t{ 0x14 }, uint8_t{ 0xFF } }) {
      CHECK(b.transact(other, { 0x00, 0x00, 0x00 }) == failed(0x10));
   }
}

TEST_CASE("EDBG: the AVR8 scope stores the parameters of avrdude and has no other command") {
   EdbgBench b;
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x00, 0x00, 0x02, 0x02, 0x01 }) == ok);
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x01, 0x00, 0x01, 0x01 }) == ok);
   // jtag3_print_parms1(): connection and arch
   CHECK(b.transact(handler::avr8, { 0x02, 0x00, 0x01, 0x00, 0x01 }) == data({ 0x01 }));
   CHECK(b.transact(handler::avr8, { 0x02, 0x00, 0x00, 0x00, 0x02 }) == data({ 0x02, 0x01 }));
   // Not stored: other addresses and contexts, a SET without its data
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00 }) == failed(0x90));
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x01, 0x20, 0x02, 0x10, 0x00 }) == failed(0x90));
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x02, 0x00, 0x01, 0x00 }) == failed(0x90));
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x00, 0x00, 0x02, 0x05 }) == failed(0x90));
   CHECK(b.transact(handler::avr8, { 0x02, 0x00, 0x00, 0x00, 0x02 }) == data({ 0x02, 0x01 }));
   // The debug commands come with debugWIRE.
   for(const uint8_t command : { uint8_t{ 0x00 }, uint8_t{ 0x10 }, uint8_t{ 0x15 }, uint8_t{ 0x21 } }) {
      CHECK(b.transact(handler::avr8, { command, 0x00 }) == failed(0x90));
   }
   CHECK(b.transact(handler::avr8, {}) == failed(0x90));
   // A new host starts with zeros.
   b.enumerate();
   CHECK(b.transact(handler::avr8, { 0x02, 0x00, 0x00, 0x00, 0x02 }) == data({ 0x00, 0x00 }));
}

TEST_CASE("EDBG: ISP programming mode and the pins of the other interface") {
   EdbgBench b;
   SUBCASE("the v2 interface owns the pins: ENTER_PROGMODE fails without a pin change") {
      CHECK(b.bulkCommand({ 0x02, 0x01 }) == Bytes{ 0x02, 0x01 });
      CHECK(b.isp(enterRequest) == Bytes{ isp::command::enterProgmode, isp::status::failed });
      CHECK(b.pins.inits == 0);
      CHECK(b.target.resets == 0);
      CHECK(b.isp({ isp::command::readSignature, 4, 0x30, 0x00, 0x00, 0x00 }) ==
            Bytes{ isp::command::readSignature, isp::status::failed });
      CHECK(b.owner.owner() == PinClient::CmsisDapBulk);
   }
   SUBCASE("ENTER_PROGMODE takes the pins, LEAVE_PROGMODE gives them back") {
      b.enterProgmode();
      CHECK(b.owner.owner() == PinClient::CmsisDapHid);
      CHECK(b.bulkCommand({ 0x02, 0x01 }) == Bytes{ 0x02, 0x00 });
      CHECK(b.isp(leaveRequest) == Bytes{ isp::command::leaveProgmode, isp::status::ok });
      CHECK(b.owner.owner() == PinClient::None);
      CHECK(b.bulkCommand({ 0x02, 0x01 }) == Bytes{ 0x02, 0x01 });
   }
   SUBCASE("a failed ENTER_PROGMODE gives the pins back") {
      b.target.syncOffset = 5;
      Bytes request = enterRequest;
      request[4] = 1;
      CHECK(b.isp(request) == Bytes{ isp::command::enterProgmode, isp::status::failed });
      CHECK_FALSE(b.pins.driven);
      CHECK(b.owner.owner() == PinClient::None);
   }
   SUBCASE("other ISP commands do not touch the owner") {
      CHECK(b.hidCommand({ 0x02, 0x01 })[1] == 0x01);
      CHECK(b.isp({ isp::command::getBaud }).size() == 4);
      b.enterProgmode();
      CHECK(b.isp(leaveRequest) == Bytes{ isp::command::leaveProgmode, isp::status::ok });
      // The HID processor is connected: its DAP_Disconnect gives them back.
      CHECK(b.owner.owner() == PinClient::CmsisDapHid);
      CHECK(b.hidCommand({ 0x03 })[1] == 0x00);
      CHECK(b.owner.owner() == PinClient::None);
   }
}

TEST_CASE("EDBG: the host goes away during the programming mode") {
   EdbgBench b;
   b.enterProgmode();
   REQUIRE(b.pins.driven);
   SUBCASE("bus reset") {
      b.enumerate();
   }
   SUBCASE("DAP_Disconnect on the HID interface") {
      CHECK(b.hidCommand({ 0x03 })[1] == 0x00);
   }
   SUBCASE("USB suspend") {
      b.controller().busSuspend();
      b.controller().busResume();
   }
   CHECK_FALSE(b.pins.driven);
   CHECK_FALSE(b.target.inReset());
   CHECK_FALSE(b.scope.programming());
   CHECK(b.owner.owner() == PinClient::None);
   CHECK(b.bulkCommand({ 0x02, 0x01 }) == Bytes{ 0x02, 0x01 });
}

TEST_CASE("EDBG: a bus reset drops a frame and a response of the old host") {
   EdbgBench b;
   CHECK(b.fragment(0x12, { avr::edbg::sof, 0x00, 0x00, 0x00, handler::housekeeping }) == avr::edbg::fragmentMore);
   b.enumerate();
   CHECK(b.fragment(0x22, { 0x10, 0x00 }) == avr::edbg::fragmentRejected);
   b.sendFrame({ avr::edbg::sof, 0x00, 0x00, 0x00, handler::housekeeping, 0x10, 0x00 });
   b.enumerate();
   CHECK_FALSE(b.receiveFrame().has_value());
}

TEST_CASE("EDBG: target power: switch-on for the owner or while nobody owns the pins, switch-off always") {
   EdbgBench b;
   const Bytes powerOn{ 0x01, 0x00, 0x00, 0x10, 0x01, 0x01 };
   const Bytes powerOff{ 0x01, 0x00, 0x00, 0x10, 0x01, 0x00 };
   const Bytes powerGet{ 0x02, 0x00, 0x00, 0x10, 0x01 };
   SUBCASE("EDBG control on the HID interface") {
      CHECK(b.transact(handler::edbgControl, powerGet) == data({ 0x00 }));
      CHECK(b.transact(handler::edbgControl, powerOn) == ok);
      CHECK(b.transact(handler::edbgControl, powerGet) == data({ 0x01 }));
      CHECK(b.transact(handler::edbgControl, powerOff) == ok);
      CHECK_FALSE(b.targetPower.isOn());
      // The v2 interface owns the pins.
      CHECK(b.bulkCommand({ 0x02, 0x01 }) == Bytes{ 0x02, 0x01 });
      CHECK(b.transact(handler::edbgControl, powerOn) == failed(0x16));
      CHECK_FALSE(b.targetPower.isOn());
      CHECK(b.bulkCommand({ 0x82, 0x01 })[1] == 0x00);
      CHECK(b.transact(handler::edbgControl, powerOff) == ok);
      CHECK_FALSE(b.targetPower.isOn());
      // The HID interface owns the pins.
      CHECK(b.bulkCommand({ 0x03 }) == Bytes{ 0x03, 0x00 });
      b.enterProgmode();
      CHECK(b.transact(handler::edbgControl, powerOn) == ok);
   }
   SUBCASE("ID_DAP_Vendor2 on the v2 interface") {
      b.enterProgmode();
      CHECK(b.bulkCommand({ 0x82, 0x01 }) == Bytes{ 0x82, 0x01, 0x00, 0x00 });
      CHECK_FALSE(b.targetPower.isOn());
      CHECK(b.transact(handler::edbgControl, powerOn) == ok);
      // Switch-off always works.
      CHECK(b.bulkCommand({ 0x82, 0x00 }) == Bytes{ 0x82, 0x00, 0x00, 0x00 });
      CHECK_FALSE(b.targetPower.isOn());
      CHECK(b.isp(leaveRequest) == Bytes{ isp::command::leaveProgmode, isp::status::ok });
      CHECK(b.bulkCommand({ 0x82, 0x01 }) == Bytes{ 0x82, 0x00, 0xE4, 0x0C });
   }
   SUBCASE("a protection refuses the switch-on") {
      b.power.externalMv = 3300;
      CHECK(b.transact(handler::edbgControl, powerOn) == failed(0x16));
      CHECK_FALSE(b.targetPower.isOn());
   }
   SUBCASE("a board without a power switch") {
      b.power.hasSwitchPin = false;
      CHECK(b.transact(handler::edbgControl, powerGet) == failed(0x01));
      CHECK(b.transact(handler::edbgControl, powerOn) == failed(0x01));
      CHECK(b.transact(handler::edbgControl, powerOff) == failed(0x01));
   }
   SUBCASE("other values and parameters") {
      CHECK(b.transact(handler::edbgControl, { 0x01, 0x00, 0x00, 0x10, 0x01, 0x02 }) == failed(0x01));
      CHECK(b.transact(handler::edbgControl, { 0x01, 0x00, 0x00, 0x10, 0x02, 0x01, 0x00 }) == failed(0x01));
      CHECK(b.transact(handler::edbgControl, { 0x01, 0x00, 0x00, 0x10, 0x01 }) == failed(0x01));
      CHECK(b.transact(handler::edbgControl, { 0x02, 0x00, 0x00, 0x00, 0x01 }) == failed(0x01));
      CHECK(b.transact(handler::edbgControl, { 0x02, 0x00, 0x11, 0x20, 0x01 }) == failed(0x01));
      CHECK(b.transact(handler::edbgControl, { 0x00, 0x00, 0x00 }) == failed(0x01));
      CHECK(b.transact(handler::edbgControl, {}) == failed(0x01));
      CHECK_FALSE(b.targetPower.isOn());
   }
}
