// SPDX-License-Identifier: GPL-3.0-or-later
// ID_DAP_Vendor6 of the CMSIS-DAP processor (DapProcessor.h): the wire
// mode of the UART, on a fake UART.

#include "CmsisDapBench.h"
#include "protocol/cmsisdap/DapProcessor.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

class FakeUartWire final : public cmsisdap::UartWire {
   public:
   void setSingleWire(bool on) override {
      ++changes;
      value = on;
   }
   [[nodiscard]] bool singleWire() const override {
      return value;
   }

   bool value{ false };
   int changes{ 0 };
};

struct Dap {
   Dap()
      : info(mupptest::dapTestInfo())
      , dap(swd, gpio, platform, info) {
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
};

constexpr uint8_t vendor6 = 0x86;

} // namespace

TEST_CASE("UART wire: read, single wire, normal") {
   Dap d;
   FakeUartWire wire;
   d.dap.setUartWire(wire);
   CHECK(d.run({ vendor6, 2 }) == Bytes{ vendor6, 0x00, 0 });
   CHECK(wire.changes == 0);
   CHECK(d.run({ vendor6, 1 }) == Bytes{ vendor6, 0x00, 1 });
   CHECK(wire.value);
   CHECK(d.run({ vendor6, 2 }) == Bytes{ vendor6, 0x00, 1 });
   CHECK(d.run({ vendor6, 0 }) == Bytes{ vendor6, 0x00, 0 });
   CHECK_FALSE(wire.value);
}

TEST_CASE("UART wire: another op replies DAP_ERROR with the mode") {
   Dap d;
   FakeUartWire wire;
   d.dap.setUartWire(wire);
   wire.value = true;
   CHECK(d.run({ vendor6, 3 }) == Bytes{ vendor6, 0xFF, 1 });
   CHECK(wire.changes == 0);
   // No op byte
   CHECK(d.run({ vendor6 }) == Bytes{ 0xFF });
   CHECK(wire.changes == 0);
}

TEST_CASE("UART wire: hostGone keeps the mode") {
   Dap d;
   FakeUartWire wire;
   d.dap.setUartWire(wire);
   CHECK(d.run({ vendor6, 1 }) == Bytes{ vendor6, 0x00, 1 });
   d.dap.hostGone();
   CHECK(wire.value);
   CHECK(wire.changes == 1);
}

TEST_CASE("UART wire: in DAP_ExecuteCommands") {
   Dap d;
   FakeUartWire wire;
   d.dap.setUartWire(wire);
   CHECK(d.run({ 0x7F, 2, vendor6, 1, vendor6, 2 }) == Bytes{ 0x7F, 2, vendor6, 0x00, 1, vendor6, 0x00, 1 });
}

TEST_CASE("UART wire: without a UART wire the command is invalid") {
   Dap d;
   CHECK(d.run({ vendor6, 2 }) == Bytes{ 0xFF });
}
