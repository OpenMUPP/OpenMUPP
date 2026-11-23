// SPDX-License-Identifier: GPL-3.0-or-later
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
      , info(mupptest::dapTestInfo())
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
   mupptest::FakeDapSwd swd;
   mupptest::FakeDapGpio gpio;
   mupptest::FakeDapPlatform platform;
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
constexpr uint8_t opSelect = 3;
// The state byte: the supply, and bit 7 for a fault
constexpr uint8_t sOff = 0;
constexpr uint8_t s3v3 = 1;
constexpr uint8_t s5v = 2;
constexpr uint8_t sFault = 0x80;

} // namespace

TEST_CASE("Target power: on, then off, replies the voltage after each") {
   Dap d;
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, done, 0xE4, 0x0C, s3v3 });
   CHECK(d.hardware.isOn);
   CHECK(d.power.isOn());
   CHECK(d.run({ vendor2, off }) == Bytes{ vendor2, done, 0x00, 0x00, sOff });
   CHECK_FALSE(d.hardware.isOn);
   CHECK(d.hardware.switches == std::vector<bool>{ true, false });
}

TEST_CASE("Target power: read changes nothing") {
   Dap d;
   d.hardware.externalMv = 0x1234;
   CHECK(d.run({ vendor2, read }) == Bytes{ vendor2, done, 0x34, 0x12, sOff });
   CHECK(d.hardware.switches.empty());
}

TEST_CASE("Target power: on is refused above 1.8 V, and the supply stays off") {
   Dap d;
   d.hardware.externalMv = TargetPower::externalSupplyMillivolts + 1;
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, refused, 0x09, 0x07, sOff });
   CHECK(d.hardware.switches.empty());
   CHECK_FALSE(d.power.isOn());
}

TEST_CASE("Target power: on is refused when the voltage cannot be measured") {
   Dap d;
   d.hardware.failSense = true;
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, refused, 0x00, 0x00, sOff });
   CHECK(d.hardware.switches.empty());
}

TEST_CASE("Target power: on is refused, and the supply goes off, when the rail does not come up") {
   Dap d;
   d.hardware.loadedMv = 200;
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, refused, 0x00, 0x00, sOff });
   CHECK(d.hardware.switches == std::vector<bool>{ true, false });
}

TEST_CASE("Target power: a board without a switch replies not supported to on and off") {
   Dap d;
   d.hardware.hasSwitchPin = false;
   d.hardware.externalMv = 3300;
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, notSupported, 0xE4, 0x0C, sOff });
   CHECK(d.run({ vendor2, off }) == Bytes{ vendor2, notSupported, 0xE4, 0x0C, sOff });
   // The sense still works.
   CHECK(d.run({ vendor2, read }) == Bytes{ vendor2, done, 0xE4, 0x0C, sOff });
}

TEST_CASE("Target power: a board without a sense switches without the checks and replies 0 mV") {
   Dap d;
   d.hardware.hasSensePin = false;
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, done, 0x00, 0x00, s3v3 });
   CHECK(d.hardware.isOn);
   CHECK(d.run({ vendor2, read }) == Bytes{ vendor2, done, 0x00, 0x00, s3v3 });
}

TEST_CASE("Target power: another op replies DAP_ERROR and changes nothing") {
   Dap d;
   CHECK(d.run({ vendor2, 4 }) == Bytes{ vendor2, notSupported, 0x00, 0x00, sOff });
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
   REQUIRE(d.run({ vendor2, on }) == Bytes{ vendor2, done, 0xE4, 0x0C, s3v3 });
   d.dap.hostGone();
   CHECK(d.hardware.isOn);
}

TEST_CASE("Target power: in DAP_ExecuteCommands, a response that does not fit switches nothing") {
   Dap d;
   // 15 DAP_Info packet size responses (4 bytes each) after the 2-byte
   // header fill 62 bytes: the 5-byte power response does not fit.
   Bytes request{ 0x7F, 16 };
   for(int i = 0; i < 15; ++i) {
      request.insert(request.end(), { 0x00, 0xFF });
   }
   request.insert(request.end(), { vendor2, on });
   const Bytes response = d.run(request);
   CHECK(response[1] == 15);
   CHECK(d.hardware.switches.empty());
}

TEST_CASE("Target power: with a pin owner, only the owner or anyone while the pins are free switches on") {
   Dap d;
   protocol::TargetPinOwner owner;
   d.dap.sharePins(owner, protocol::PinClient::CmsisDapBulk);
   // Nobody owns the pins.
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, done, 0xE4, 0x0C, s3v3 });
   // The other interface owns them: switch-off works, switch-on does not.
   REQUIRE(owner.acquire(protocol::PinClient::CmsisDapHid));
   CHECK(d.run({ vendor2, off }) == Bytes{ vendor2, done, 0x00, 0x00, sOff });
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, refused, 0x00, 0x00, sOff });
   CHECK(d.hardware.switches == std::vector<bool>{ true, false });
   // This interface owns them.
   owner.release(protocol::PinClient::CmsisDapHid);
   REQUIRE(d.run({ 0x02, 0x01 }) == Bytes{ 0x02, 0x01 });
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, done, 0xE4, 0x0C, s3v3 });
}

TEST_CASE("Target power: op 3 selects 3.3 V or 5 V, and the state shows the supply") {
   Dap d;
   d.hardware.has5vSwitchPin = true;
   // 5000 mV
   CHECK(d.run({ vendor2, opSelect, 0x88, 0x13 }) == Bytes{ vendor2, done, 0x88, 0x13, s5v });
   CHECK(d.power.supply() == protocol::Supply::V5);
   // 3300 mV: the change sequence
   CHECK(d.run({ vendor2, opSelect, 0xE4, 0x0C }) == Bytes{ vendor2, done, 0xE4, 0x0C, s3v3 });
   CHECK(d.hardware.selections ==
         std::vector<protocol::Supply>{ protocol::Supply::V5, protocol::Supply::Off, protocol::Supply::V3v3 });
   // The old request means 3.3 V: no change
   CHECK(d.run({ vendor2, on }) == Bytes{ vendor2, done, 0xE4, 0x0C, s3v3 });
   CHECK(d.run({ vendor2, read }) == Bytes{ vendor2, done, 0xE4, 0x0C, s3v3 });
}

TEST_CASE("Target power: op 3 with another voltage, or without the voltage, changes nothing") {
   Dap d;
   d.hardware.has5vSwitchPin = true;
   CHECK(d.run({ vendor2, opSelect, 0xB8, 0x0B }) == Bytes{ vendor2, notSupported, 0x00, 0x00, sOff });
   CHECK(d.run({ vendor2, opSelect, 0x00, 0x00 }) == Bytes{ vendor2, notSupported, 0x00, 0x00, sOff });
   CHECK(d.run({ vendor2, opSelect, 0x88 }) == Bytes{ 0xFF });
   CHECK(d.hardware.selections.empty());
}

TEST_CASE("Target power: op 3 at 5 V on a board with a 3.3 V switch only is not supported") {
   Dap d;
   CHECK(d.run({ vendor2, opSelect, 0x88, 0x13 }) == Bytes{ vendor2, notSupported, 0x00, 0x00, sOff });
   CHECK(d.run({ vendor2, opSelect, 0xE4, 0x0C }) == Bytes{ vendor2, done, 0xE4, 0x0C, s3v3 });
}

TEST_CASE("Target power: 5 V is refused on a weak USB supply") {
   Dap d;
   d.hardware.has5vSwitchPin = true;
   d.hardware.inputMv = 4300;
   CHECK(d.run({ vendor2, opSelect, 0x88, 0x13 }) == Bytes{ vendor2, refused, 0x00, 0x00, sOff });
   CHECK(d.hardware.selections.empty());
}

TEST_CASE("Target power: op 3 obeys the pin owner") {
   Dap d;
   d.hardware.has5vSwitchPin = true;
   protocol::TargetPinOwner owner;
   d.dap.sharePins(owner, protocol::PinClient::CmsisDapBulk);
   REQUIRE(owner.acquire(protocol::PinClient::CmsisDapHid));
   CHECK(d.run({ vendor2, opSelect, 0x88, 0x13 }) == Bytes{ vendor2, refused, 0x00, 0x00, sOff });
   CHECK(d.hardware.selections.empty());
}

TEST_CASE("Target power: the next response reports a fault once") {
   Dap d;
   d.hardware.has5vSwitchPin = true;
   REQUIRE(d.run({ vendor2, opSelect, 0x88, 0x13 }) == Bytes{ vendor2, done, 0x88, 0x13, s5v });
   // The interrupt switched off; the main loop did not poll yet.
   d.hardware.fault();
   CHECK(d.run({ vendor2, read }) == Bytes{ vendor2, done, 0x00, 0x00, static_cast<uint8_t>(sOff | sFault) });
   CHECK(d.run({ vendor2, read }) == Bytes{ vendor2, done, 0x00, 0x00, sOff });
   CHECK(d.power.faultCount() == 1);
}
