// SPDX-License-Identifier: GPL-3.0-or-later
// ID_DAP_Vendor3 of the CMSIS-DAP processor (DapProcessor.h): the setting
// "VPP without an adapter", on a fake store.

#include "CmsisDapBench.h"
#include "protocol/cmsisdap/DapProcessor.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

class FakeVppSetting final : public cmsisdap::VppSetting {
   public:
   [[nodiscard]] bool load() const override {
      return value;
   }
   bool store(bool allowed) override {
      ++stores;
      if(failStore) {
         return false;
      }
      value = allowed;
      return true;
   }

   bool value{ false };
   bool failStore{ false };
   int stores{ 0 };
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

constexpr uint8_t vendor3 = 0x83;

} // namespace

TEST_CASE("VPP setting: read, store on, store off") {
   Dap d;
   FakeVppSetting setting;
   d.dap.setVppSetting(setting);
   CHECK(d.run({ vendor3, 2 }) == Bytes{ vendor3, 0x00, 0 });
   CHECK(setting.stores == 0);
   CHECK(d.run({ vendor3, 1 }) == Bytes{ vendor3, 0x00, 1 });
   CHECK(setting.value);
   CHECK(d.run({ vendor3, 2 }) == Bytes{ vendor3, 0x00, 1 });
   CHECK(d.run({ vendor3, 0 }) == Bytes{ vendor3, 0x00, 0 });
   CHECK_FALSE(setting.value);
}

TEST_CASE("VPP setting: another op and a failed store reply DAP_ERROR with the stored value") {
   Dap d;
   FakeVppSetting setting;
   d.dap.setVppSetting(setting);
   CHECK(d.run({ vendor3, 3 }) == Bytes{ vendor3, 0xFF, 0 });
   CHECK(setting.stores == 0);
   setting.failStore = true;
   CHECK(d.run({ vendor3, 1 }) == Bytes{ vendor3, 0xFF, 0 });
   // No op byte
   CHECK(d.run({ vendor3 }) == Bytes{ 0xFF });
}

TEST_CASE("VPP setting: without a store the command is invalid") {
   Dap d;
   CHECK(d.run({ vendor3, 2 }) == Bytes{ 0xFF });
}
