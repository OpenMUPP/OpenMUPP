// SPDX-License-Identifier: GPL-3.0-or-later
// ID_DAP_Vendor4 of the CMSIS-DAP processor (DapProcessor.h): the board ID
// and the adapter ID, on a fake source.

#include "CmsisDapBench.h"
#include "protocol/cmsisdap/DapProcessor.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

class FakeIdSource final : public cmsisdap::IdSource {
   public:
   [[nodiscard]] cmsisdap::IdReport read() override {
      ++reads;
      return report;
   }

   cmsisdap::IdReport report{ .boardKind = 1, .boardSlot = 1, .adapterKind = 1, .adapterSlot = 5, .allowed = true, .adapterRead = true };
   int reads{ 0 };
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

constexpr uint8_t vendor4 = 0x84;

} // namespace

TEST_CASE("ID report: board revision 1 and adapter slot 5, connect allowed") {
   Dap d;
   FakeIdSource source;
   d.dap.setIdSource(source);
   CHECK(d.run({ vendor4 }) == Bytes{ vendor4, 0x00, 1, 1, 1, 5, 1 });
   CHECK(source.reads == 1);
}

TEST_CASE("ID report: an adapter fault refuses the connect") {
   Dap d;
   FakeIdSource source;
   source.report.adapterKind = 2;
   source.report.adapterSlot = 0;
   source.report.allowed = false;
   d.dap.setIdSource(source);
   CHECK(d.run({ vendor4 }) == Bytes{ vendor4, 0x00, 1, 1, 2, 0, 0 });
}

TEST_CASE("ID report: a failed adapter read gives DAP_ERROR, the board ID and no adapter fields") {
   Dap d;
   FakeIdSource source;
   source.report.adapterRead = false;
   d.dap.setIdSource(source);
   CHECK(d.run({ vendor4 }) == Bytes{ vendor4, 0xFF, 1, 1, 0, 0, 0 });
}

TEST_CASE("ID report: without a source, the command replies ID_DAP_Invalid") {
   Dap d;
   CHECK(d.run({ vendor4 }) == Bytes{ 0xFF });
}

TEST_CASE("ID report: the command works in DAP_ExecuteCommands") {
   Dap d;
   FakeIdSource source;
   d.dap.setIdSource(source);
   CHECK(d.run({ 0x7F, 1, vendor4 }) == Bytes{ 0x7F, 1, vendor4, 0x00, 1, 1, 1, 5, 1 });
}
