// The RISC-V bridge of the CMSIS-DAP personality: the switching JTAG port
// (RvBridge.h) on a fake real port and the emulated TAP with mock pins, and
// ID_DAP_Vendor1 of the processor (DapProcessor.h).

#include "protocol/cmsisdap/RvBridge.h"

#include "CmsisDapBench.h"
#include "MockDebugModule.h"
#include "MockRvSwdTarget.h"
#include "protocol/cmsisdap/DapProcessor.h"
#include "protocol/cmsisdap/RvDtmTap.h"
#include "protocol/wchlink/WchLinkTarget.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

// The real JTAG port: records the calls. shift() gives TDO 0.
class FakeJtagPort final : public protocol::JtagPort {
   public:
   void init() override {
      calls.emplace_back("init");
   }
   void fini() override {
      calls.emplace_back("fini");
   }
   bool setSpeed(uint32_t kHz) override {
      calls.push_back("speed " + std::to_string(kHz));
      return true;
   }
   bool shift(uint32_t bitCount, art::span<const uint8_t>, art::span<const uint8_t>, art::span<uint8_t>) override {
      calls.push_back("shift " + std::to_string(bitCount));
      return true;
   }

   std::vector<std::string> calls;
};

// Records the calls of the processor
class FakeBridge final : public cmsisdap::Bridge {
   public:
   void enable(bool on) override {
      enables.push_back(on);
   }
   void pinsChanged() override {
      ++pinChanges;
   }

   std::vector<bool> enables;
   int pinChanges{ 0 };
};

// SWCLK, SWDIO and SRST
class Gpio final : public protocol::GpioPins {
   public:
   [[nodiscard]] uint16_t available() const override {
      return protocol::gpio::tck | protocol::gpio::tms | protocol::gpio::srst;
   }
   void configure(uint16_t, protocol::PinMode, bool) override {
   }
   void write(uint16_t, uint16_t) override {
   }
   [[nodiscard]] uint16_t read(uint16_t) const override {
      return 0;
   }
};

// The TAP on the wire engine, with mock pins and a mock DM, on two wires
struct Wire {
   Wire()
      : pins(rvswd::Wire::Two)
      , dm(pins)
      , target(pins)
      , tap(target) {
   }

   vsltest::MockRvSwdTarget pins;
   vsltest::MockDebugModule dm;
   wchlink::RvSwdTarget<vsltest::MockRvSwdTarget> target;
   cmsisdap::RvDtmTap tap;
};

using Bits = std::vector<bool>;

Bits valueBits(uint64_t v, std::size_t count) {
   Bits result;
   for(std::size_t i = 0; i < count; ++i) {
      result.push_back(((v >> i) & 1u) != 0);
   }
   return result;
}

Bits operator+(Bits a, const Bits &b) {
   a.insert(a.end(), b.begin(), b.end());
   return a;
}

// One shift, TDI low when tdi is empty. Returns the TDO bits.
Bits shift(protocol::JtagPort &port, const Bits &tms, Bits tdi = {}) {
   tdi.resize(tms.size(), false);
   Bytes tmsBytes((tms.size() + 7) / 8);
   Bytes tdiBytes(tmsBytes.size());
   Bytes tdoBytes(tmsBytes.size());
   for(std::size_t i = 0; i < tms.size(); ++i) {
      tmsBytes[i / 8] = static_cast<uint8_t>(tmsBytes[i / 8] | (tms[i] ? 1u << (i % 8) : 0u));
      tdiBytes[i / 8] = static_cast<uint8_t>(tdiBytes[i / 8] | (tdi[i] ? 1u << (i % 8) : 0u));
   }
   REQUIRE(port.shift(static_cast<uint32_t>(tms.size()), tdiBytes, tmsBytes, tdoBytes));
   Bits tdo;
   for(std::size_t i = 0; i < tms.size(); ++i) {
      tdo.push_back(((tdoBytes[i / 8] >> (i % 8)) & 1u) != 0);
   }
   return tdo;
}

// From Run-Test/Idle: a DR or IR scan, back to Run-Test/Idle. Returns the
// TDO bits of the scan.
Bits scan(protocol::JtagPort &port, bool ir, const Bits &tdi) {
   const Bits head = ir ? valueBits(0b0011, 4) : valueBits(0b001, 3);
   Bits tms = head + Bits(tdi.size() - 1, false) + valueBits(0b011, 3);
   const Bits tdo = shift(port, tms, Bits(head.size(), false) + tdi);
   return Bits(tdo.begin() + static_cast<std::ptrdiff_t>(head.size()),
               tdo.begin() + static_cast<std::ptrdiff_t>(head.size() + tdi.size()));
}

uint32_t value(const Bits &b) {
   uint32_t result = 0;
   for(std::size_t i = 0; i < b.size() && i < 32; ++i) {
      result |= (b[i] ? 1u : 0u) << i;
   }
   return result;
}

// Test-Logic-Reset, Run-Test/Idle, then a DR scan: the IDCODE
uint32_t readIdcode(protocol::JtagPort &port) {
   shift(port, valueBits(0b011111, 6));
   return value(scan(port, false, Bits(32, false)));
}

// From Run-Test/Idle: IR DMI, then a DMI read of dmstatus
void dmiRead(protocol::JtagPort &port) {
   scan(port, true, valueBits(cmsisdap::rvDtm::irDmi, 5));
   scan(port, false, valueBits(cmsisdap::rvDtm::opRead, 2) + Bits(32, false) + valueBits(rvswd::dm::dmstatus, 7));
}

} // namespace

TEST_CASE("RV bridge: the real JTAG port is the default") {
   FakeJtagPort jtag;
   Wire w;
   cmsisdap::RvBridge bridge{ jtag, w.tap };
   CHECK_FALSE(bridge.enabled());
   bridge.init();
   shift(bridge, Bits(8, false));
   bridge.fini();
   CHECK(jtag.calls == std::vector<std::string>{ "init", "shift 8", "fini" });
   CHECK(w.pins.inits == 0);
}

TEST_CASE("RV bridge: the switch ends the port in use and starts the other one") {
   FakeJtagPort jtag;
   Wire w;
   cmsisdap::RvBridge bridge{ jtag, w.tap };
   bridge.init();
   bridge.enable(true);
   CHECK(bridge.enabled());
   CHECK(jtag.calls == std::vector<std::string>{ "init", "fini" });
   CHECK(readIdcode(bridge) == cmsisdap::rvDtm::idcode);
   CHECK(jtag.calls.size() == 2);

   // A DMI access attaches the wire. The switch off releases the pins.
   dmiRead(bridge);
   REQUIRE(w.pins.accesses.size() == 4);
   const int finis = w.pins.finis;
   bridge.enable(false);
   CHECK(w.pins.finis == finis + 1);
   CHECK(jtag.calls == std::vector<std::string>{ "init", "fini", "init" });
   shift(bridge, Bits(8, false));
   CHECK(jtag.calls.back() == "shift 8");
}

TEST_CASE("RV bridge: a switch without a connection starts no port") {
   FakeJtagPort jtag;
   Wire w;
   cmsisdap::RvBridge bridge{ jtag, w.tap };
   bridge.enable(true);
   bridge.enable(true);
   CHECK(jtag.calls.empty());
   bridge.init();
   CHECK(jtag.calls.empty());
   CHECK(readIdcode(bridge) == cmsisdap::rvDtm::idcode);
   bridge.fini();
   bridge.enable(false);
   CHECK(jtag.calls.empty());
}

TEST_CASE("RV bridge: the real port gets each speed") {
   FakeJtagPort jtag;
   Wire w;
   cmsisdap::RvBridge bridge{ jtag, w.tap };
   bridge.enable(true);
   CHECK(bridge.setSpeed(500));
   bridge.enable(false);
   CHECK(jtag.calls == std::vector<std::string>{ "speed 500" });
}

TEST_CASE("RV bridge: pinsChanged() makes the TAP connect again") {
   FakeJtagPort jtag;
   Wire w;
   cmsisdap::RvBridge bridge{ jtag, w.tap };
   bridge.enable(true);
   bridge.init();
   readIdcode(bridge);
   dmiRead(bridge);
   REQUIRE(w.pins.accesses.size() == 4);
   dmiRead(bridge);
   REQUIRE(w.pins.accesses.size() == 5);
   bridge.pinsChanged();
   dmiRead(bridge);
   // The unlock writes, dmstatus, then the read
   REQUIRE(w.pins.accesses.size() == 9);
   CHECK(w.pins.accesses[5].address == 0x7e);
}

namespace {

// The processor with an optional bridge
struct Dap {
   explicit Dap(cmsisdap::Bridge *bridge, protocol::JtagPort *jtag = nullptr)
      : info(vsltest::dapTestInfo())
      , dap(swd, gpio, platform, info, jtag, nullptr, nullptr, nullptr, bridge) {
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

   vsltest::FakeDapSwd swd;
   Gpio gpio;
   vsltest::FakeDapPlatform platform;
   cmsisdap::DeviceInfo info;
   cmsisdap::Processor dap;
};

} // namespace

TEST_CASE("RV bridge: ID_DAP_Vendor1 without a bridge is an unknown command") {
   Dap d(nullptr);
   CHECK(d.run({ 0x81, 0x01 }) == Bytes{ 0xFF });
}

TEST_CASE("RV bridge: ID_DAP_Vendor1 switches the bridge") {
   FakeBridge bridge;
   Dap d(&bridge);
   CHECK(d.run({ 0x81, 0x01 }) == Bytes{ 0x81, 0x00 });
   CHECK(d.run({ 0x81, 0x00 }) == Bytes{ 0x81, 0x00 });
   CHECK(bridge.enables == std::vector<bool>{ true, false });
   // Another value: DAP_ERROR, no switch
   CHECK(d.run({ 0x81, 0x02 }) == Bytes{ 0x81, 0xFF });
   CHECK(bridge.enables.size() == 2);
   // Too short
   CHECK(d.run({ 0x81 }) == Bytes{ 0xFF });
   CHECK(bridge.enables.size() == 2);
}

TEST_CASE("RV bridge: hostGone() switches the bridge off") {
   FakeBridge bridge;
   Dap d(&bridge);
   d.dap.hostGone();
   CHECK(bridge.enables == std::vector<bool>{ false });
   // A suspend keeps the bridge.
   d.dap.release();
   CHECK(bridge.enables.size() == 1);
}

TEST_CASE("RV bridge: DAP_SWJ_Pins on SWCLK or SWDIO tells the bridge") {
   FakeBridge bridge;
   Dap d(&bridge);
   // SRST only
   d.run({ 0x10, 0x80, 0x80, 0, 0, 0, 0 });
   CHECK(bridge.pinChanges == 0);
   // SWCLK, then SWDIO
   d.run({ 0x10, 0x00, 0x01, 0, 0, 0, 0 });
   d.run({ 0x10, 0x00, 0x02, 0, 0, 0, 0 });
   CHECK(bridge.pinChanges == 2);
}

TEST_CASE("RV bridge: the processor reads the IDCODE of the TAP after the switch") {
   FakeJtagPort jtag;
   Wire w;
   cmsisdap::RvBridge bridge{ jtag, w.tap };
   Dap d(&bridge, &bridge);
   // DAP_Connect to JTAG on the real port
   CHECK(d.run({ 0x02, 0x02 }) == Bytes{ 0x02, 0x02 });
   REQUIRE(jtag.calls.back() == "init");
   CHECK(d.run({ 0x81, 0x01 }) == Bytes{ 0x81, 0x00 });
   CHECK(jtag.calls.back() == "fini");
   // DAP_JTAG_Sequence: Test-Logic-Reset, Run-Test/Idle, Shift-DR, then
   // 32 bits with TDO capture
   const Bytes sequence{ 0x14, 0x05, 0x45, 0x00, 0x01, 0x00, 0x41, 0x00, 0x02, 0x00, 0xA0, 0x00, 0x00, 0x00, 0x00 };
   const Bytes idcode{ 0x01, 0x31, 0x65, 0x15 };
   CHECK(d.run(sequence) == Bytes{ 0x14, 0x00, idcode[0], idcode[1], idcode[2], idcode[3] });
   // Off: DAP_Disconnect, then the real port again after DAP_Connect
   CHECK(d.run({ 0x81, 0x00 }) == Bytes{ 0x81, 0x00 });
   CHECK(jtag.calls.back() == "init");
   CHECK(d.run({ 0x03 }) == Bytes{ 0x03, 0x00 });
   CHECK(jtag.calls.back() == "fini");
}
