// ID_DAP_Vendor2 of the CMSIS-DAP processor (DapProcessor.h): the target
// supply, on a fake power switch and voltage sense (protocol/TargetPower.h).

#include "CmsisDapBench.h"
#include "FakePower.h"
#include "protocol/TargetPower.h"
#include "protocol/cmsisdap/DapProcessor.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
using protocol::TargetPower;

struct Dap {
   explicit Dap(bool withPower = true)
      : delay(hardware)
      , power(hardware, delay)
      , info(vsltest::dapTestInfo())
      , dap(swd, gpio, platform, info, nullptr, nullptr, nullptr, nullptr, nullptr, withPower ? &power : nullptr) {
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

   FakePower hardware;
   FakeDelay delay;
   TargetPower power;
   vsltest::FakeDapSwd swd;
   vsltest::FakeDapGpio gpio;
   vsltest::FakeDapPlatform platform;
   cmsisdap::DeviceInfo info;
   cmsisdap::Processor dap;
};

constexpr uint8_t vendor2 = 0x82;
constexpr uint8_t off = 0;
constexpr uint8_t on = 1;
constexpr uint8_t read = 2;
constexpr uint8_t done = 0;
constexpr uint8_t refused = 1;
constexpr uint8_t notSupported = 0xFF;

} // namespace

TEST_CASE("Target power: on, then off, replies the voltage after each") {
   Dap d;
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, done, 0xE4, 0x0C });
   CHECK(d.hardware.isOn);
   CHECK(d.power.isOn());
   CHECK(d.run({ vendor2, off }) == Bytes{ vendor2, done, 0x00, 0x00 });
   CHECK_FALSE(d.hardware.isOn);
   CHECK(d.hardware.switches == std::vector<bool>{ true, false });
}

TEST_CASE("Target power: read changes nothing") {
   Dap d;
   d.hardware.externalMv = 0x1234;
   CHECK(d.run({ vendor2, read }) == Bytes{ vendor2, done, 0x34, 0x12 });
   CHECK(d.hardware.switches.empty());
}

TEST_CASE("Target power: on is refused above 1.8 V, and the supply stays off") {
   Dap d;
   d.hardware.externalMv = TargetPower::externalSupplyMillivolts + 1;
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, refused, 0x09, 0x07 });
   CHECK(d.hardware.switches.empty());
   CHECK_FALSE(d.power.isOn());
}

TEST_CASE("Target power: on is refused when the voltage cannot be measured") {
   Dap d;
   d.hardware.failSense = true;
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, refused, 0x00, 0x00 });
   CHECK(d.hardware.switches.empty());
}

TEST_CASE("Target power: on is refused, and the supply goes off, when the rail does not come up") {
   Dap d;
   d.hardware.loadedMv = 200;
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, refused, 0x00, 0x00 });
   CHECK(d.hardware.switches == std::vector<bool>{ true, false });
}

TEST_CASE("Target power: a board without a switch replies not supported to on and off") {
   Dap d;
   d.hardware.hasSwitchPin = false;
   d.hardware.externalMv = 3300;
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, notSupported, 0xE4, 0x0C });
   CHECK(d.run({ vendor2, off }) == Bytes{ vendor2, notSupported, 0xE4, 0x0C });
   // The sense still works.
   CHECK(d.run({ vendor2, read }) == Bytes{ vendor2, done, 0xE4, 0x0C });
}

TEST_CASE("Target power: a board without a sense switches without the checks and replies 0 mV") {
   Dap d;
   d.hardware.hasSensePin = false;
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, done, 0x00, 0x00 });
   CHECK(d.hardware.isOn);
   CHECK(d.run({ vendor2, read }) == Bytes{ vendor2, done, 0x00, 0x00 });
}

TEST_CASE("Target power: another op replies DAP_ERROR and changes nothing") {
   Dap d;
   CHECK(d.run({ vendor2, 3 }) == Bytes{ vendor2, notSupported, 0x00, 0x00 });
   CHECK(d.hardware.switches.empty());
   // Too short
   CHECK(d.run({ vendor2 }) == Bytes{ 0xFF });
}

TEST_CASE("Target power: without a TargetPower, ID_DAP_Vendor2 is an unknown command") {
   Dap d(false);
   CHECK(d.run({ vendor2, on }) == Bytes{ 0xFF });
   CHECK(d.hardware.switches.empty());
}

TEST_CASE("Target power: the host going away keeps the supply on") {
   Dap d;
   REQUIRE(d.run({ vendor2, on }) == Bytes{ vendor2, done, 0xE4, 0x0C });
   d.dap.hostGone();
   CHECK(d.hardware.isOn);
}

TEST_CASE("Target power: in DAP_ExecuteCommands, a response that does not fit switches nothing") {
   Dap d;
   // 15 DAP_Info packet size responses (4 bytes each) after the 2-byte
   // header fill 62 bytes: the 4-byte power response does not fit.
   Bytes request{ 0x7F, 16 };
   for(int i = 0; i < 15; ++i) {
      request.insert(request.end(), { 0x00, 0xFF });
   }
   request.insert(request.end(), { vendor2, on });
   const Bytes response = d.run(request);
   CHECK(response[1] == 15);
   CHECK(d.hardware.switches.empty());
}
