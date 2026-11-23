// SPDX-License-Identifier: GPL-3.0-or-later
// The EDBG transport of the AVR commands (protocol/avr/EdbgTransport.h)
// through the HID function: the fragments of AVR_CMD and AVR_RSP, the
// frames, the scopes, the target pins and the target power, with a mock
// AVR on the ISP engine (EdbgBench.h). TestEdbgDebugWire.cpp has the
// debugWIRE sessions.

#include "EdbgBench.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace edbgtest;

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
   // A frame that is not complete yet: the old response is gone too.
   b.sendFrame({ avr::edbg::sof, 0x00, 0x01, 0x00, handler::housekeeping, 0x10, 0x00 });
   CHECK(b.fragment(0x12, { avr::edbg::sof, 0x00, 0x02, 0x00, handler::housekeeping }) == avr::edbg::fragmentMore);
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

TEST_CASE("EDBG: the AVR8 scope stores the parameters of the host") {
   EdbgBench b;
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x00, 0x00, 0x02, 0x02, 0x01 }) == ok);
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x01, 0x00, 0x01, 0x01 }) == ok);
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x02, 0x1D, 0x02, 0x57, 0x00 }) == ok);
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x03, 0x00, 0x01, 0x01 }) == ok);
   // jtag3_print_parms1(): connection and arch
   CHECK(b.transact(handler::avr8, { 0x02, 0x00, 0x01, 0x00, 0x01 }) == data({ 0x01 }));
   CHECK(b.transact(handler::avr8, { 0x02, 0x00, 0x00, 0x00, 0x02 }) == data({ 0x02, 0x01 }));
   CHECK(b.transact(handler::avr8, { 0x02, 0x00, 0x02, 0x1C, 0x03 }) == data({ 0x00, 0x57, 0x00 }));
   // Not stored: other addresses and contexts, a SET without its data
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00 }) == failed(0x90));
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x01, 0x05, 0x01, 0x10 }) == failed(0x90));
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x02, 0x1F, 0x02, 0x00, 0x00 }) == failed(0x90));
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x04, 0x00, 0x01, 0x00 }) == failed(0x90));
   CHECK(b.transact(handler::avr8, { 0x01, 0x00, 0x00, 0x00, 0x02, 0x05 }) == failed(0x90));
   CHECK(b.transact(handler::avr8, { 0x02, 0x00, 0x00, 0x00, 0x00 }) == failed(0x90));
   CHECK(b.transact(handler::avr8, { 0x02, 0x00, 0x00, 0x00, 0x02 }) == data({ 0x02, 0x01 }));
   // The debug commands need an active physical. Deactivate without one: OK
   // (avrdude ISP).
   for(const uint8_t command : { uint8_t{ 0x12 }, uint8_t{ 0x21 }, uint8_t{ 0x32 }, uint8_t{ 0x35 } }) {
      CHECK(b.transact(handler::avr8, { command, 0x00 }) == failed(0x31));
   }
   CHECK(b.transact(handler::avr8, { 0x11, 0x00 }) == ok);
   CHECK(b.transact(handler::avr8, { 0x20, 0x00, 0x00, 0, 0, 0, 0 }) == failed(0x90));
   CHECK(b.transact(handler::avr8, { 0x00, 0x00 }) == failed(0x31));
   CHECK(b.transact(handler::avr8, {}) == failed(0x91));
   CHECK(b.dwPins.inits == 0);
   // A new host starts with zeros.
   b.enumerate();
   CHECK(b.transact(handler::avr8, { 0x02, 0x00, 0x00, 0x00, 0x02 }) == data({ 0x00, 0x00 }));
   CHECK(b.transact(handler::avr8, { 0x02, 0x00, 0x02, 0x1D, 0x01 }) == data({ 0x00 }));
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
      CHECK(b.bulkCommand({ 0x82, 0x01 }) == Bytes{ 0x82, 0x01, 0x00, 0x00, 0x00 });
      CHECK_FALSE(b.targetPower.isOn());
      CHECK(b.transact(handler::edbgControl, powerOn) == ok);
      // Switch-off always works.
      CHECK(b.bulkCommand({ 0x82, 0x00 }) == Bytes{ 0x82, 0x00, 0x00, 0x00, 0x00 });
      CHECK_FALSE(b.targetPower.isOn());
      CHECK(b.isp(leaveRequest) == Bytes{ isp::command::leaveProgmode, isp::status::ok });
      CHECK(b.bulkCommand({ 0x82, 0x01 }) == Bytes{ 0x82, 0x00, 0xE4, 0x0C, 0x01 });
   }
   SUBCASE("on keeps a 5 V supply at 5 V") {
      b.power.has5vSwitchPin = true;
      b.enterProgmode();
      REQUIRE(b.targetPower.switchOn(protocol::Supply::V5) == protocol::TargetPower::Result::Ok);
      CHECK(b.transact(handler::edbgControl, powerOn) == ok);
      CHECK(b.targetPower.supply() == protocol::Supply::V5);
      CHECK(b.power.selections == std::vector<protocol::Supply>{ protocol::Supply::V5 });
      CHECK(b.transact(handler::edbgControl, powerGet) == data({ 0x01 }));
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
