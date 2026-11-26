// SPDX-License-Identifier: GPL-3.0-or-later
// ID_DAP_Vendor5 of the CMSIS-DAP processor (DapProcessor.h): the VPP
// control for the hardware test, on the VPP driver with a fake boost.

#include "CmsisDapBench.h"
#include "FakeVpp.h"
#include "protocol/cmsisdap/DapProcessor.h"
#include "protocol/cmsisdap/DapVpp.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

struct Dap {
   Dap()
      : info(mupptest::dapTestInfo())
      , dap(swd, gpio, platform, info) {
      dap.setVpp(dapVpp);
   }

   Bytes run(const Bytes &request) {
      REQUIRE(dap.canSubmit());
      dap.submit(request);
      dap.run();
      REQUIRE(dap.hasResponse());
      const auto span = dap.response();
      Bytes response(span.begin(), span.end());
      dap.popResponse();
      return response;
   }

   mupptest::FakeDapSwd swd;
   mupptest::FakeDapGpio gpio;
   mupptest::FakeDapPlatform platform;
   cmsisdap::DeviceInfo info;
   cmsisdap::Processor dap;
   FakeVppBoard board;
   FakeVppGate gate;
   protocol::Vpp vpp{ board, board, gate };
   cmsisdap::DapVpp dapVpp{ vpp };
};

constexpr uint8_t vendor5 = 0x85;

uint16_t field(const Bytes &response, std::size_t offset) {
   return static_cast<uint16_t>(response[offset] | (response[offset + 1] << 8));
}

} // namespace

TEST_CASE("VPP control: without a VppCommand the command replies ID_DAP_Invalid") {
   mupptest::FakeDapSwd swd;
   mupptest::FakeDapGpio gpio;
   mupptest::FakeDapPlatform platform;
   const cmsisdap::DeviceInfo info = mupptest::dapTestInfo();
   cmsisdap::Processor dap(swd, gpio, platform, info);
   const Bytes request{ vendor5, 2 };
   dap.submit(request);
   dap.run();
   REQUIRE(dap.hasResponse());
   CHECK(dap.response()[0] == 0xFF);
}

TEST_CASE("VPP control: on, read and off") {
   Dap d;
   // 12000 mV
   const Bytes on = d.run({ vendor5, 1, 0xE0, 0x2E });
   REQUIRE(on.size() == 8);
   CHECK(on[0] == vendor5);
   CHECK(on[1] == 0);
   CHECK(field(on, 2) == 12000);
   CHECK(field(on, 4) >= 11900);
   CHECK(field(on, 4) <= 12100);
   CHECK(field(on, 6) + 100 >= field(on, 4));
   CHECK(d.board.on);
   const Bytes read = d.run({ vendor5, 2 });
   CHECK(read[1] == 0);
   CHECK(field(read, 2) == 12000);
   const Bytes off = d.run({ vendor5, 0 });
   CHECK(off[1] == 0);
   CHECK(field(off, 2) == 0);
   // VPP_PD pulls the target pin to 0 V
   CHECK(field(off, 6) == 0);
   CHECK_FALSE(d.board.on);
   CHECK_FALSE(d.board.enable);
}

TEST_CASE("VPP control: a refused level, no level and a short have their status") {
   Dap d;
   // 14701 mV: above the hardware maximum
   CHECK(d.run({ vendor5, 1, 0x6D, 0x39 })[1] == 1);
   d.gate.limit.reset();
   CHECK(d.run({ vendor5, 1, 0xE0, 0x2E })[1] == 1);
   d.gate.limit = protocol::Vpp::hardwareMaxMillivolts;
   d.board.failSense = true;
   const Bytes noLevel = d.run({ vendor5, 1, 0xE0, 0x2E });
   CHECK(noLevel[1] == 2);
   CHECK(field(noLevel, 2) == 0);
   CHECK(field(noLevel, 4) == 0);
   d.board.failSense = false;
   d.board.shortedPin = true;
   const Bytes fault = d.run({ vendor5, 1, 0xE0, 0x2E });
   CHECK(fault[1] == 3);
   CHECK(field(fault, 2) == 0);
   CHECK_FALSE(d.board.on);
}

TEST_CASE("VPP control: another op replies DAP_ERROR, a short request ID_DAP_Invalid") {
   Dap d;
   CHECK(d.run({ vendor5, 3 })[1] == 0xFF);
   CHECK(d.run({ vendor5, 1, 0xE0 }) == Bytes{ 0xFF });
   CHECK(d.run({ vendor5 }) == Bytes{ 0xFF });
   CHECK_FALSE(d.board.enable);
}

TEST_CASE("VPP control: in DAP_ExecuteCommands the command replies ID_DAP_Invalid") {
   Dap d;
   // DAP_ExecuteCommands with one command
   CHECK(d.run({ 0x7F, 1, vendor5, 1, 0xE0, 0x2E }) == Bytes{ 0x7F, 1, 0xFF });
   CHECK_FALSE(d.board.enable);
}

TEST_CASE("VPP control: the host gone and a suspend switch VPP off") {
   Dap d;
   REQUIRE(d.run({ vendor5, 1, 0xE0, 0x2E })[1] == 0);
   d.dap.hostGone();
   CHECK_FALSE(d.vpp.isOn());
   CHECK_FALSE(d.board.on);
   REQUIRE(d.run({ vendor5, 1, 0xE0, 0x2E })[1] == 0);
   d.dap.release();
   CHECK_FALSE(d.vpp.isOn());
   CHECK(d.board.pullDown);
}

TEST_CASE("VPP control: the host gone without VPP on writes no pin") {
   Dap d;
   d.dap.hostGone();
   d.dap.release();
   // VPP_PD stays low: the target pin stays at the run level
   CHECK(d.board.events.empty());
   CHECK_FALSE(d.board.pullDown);
}
