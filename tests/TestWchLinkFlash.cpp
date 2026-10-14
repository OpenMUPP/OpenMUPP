// The flash commands of the WCH-Link personality through its USB function,
// on the wire engine with mock pins, a mock DM and a mock flash controller
// (MockWchFlash.h). The command sequences come from the WCH-LinkE captures
// in docs/wch-link-captures (docs/wch-link-protocol.md, "Flash").

#include "MockWchFlash.h"
#include "WchLinkBench.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using rvswd::Wire;
using vsltest::MockWchFlash;
using vsltest::WchLinkBench;
using Bytes = std::vector<uint8_t>;

namespace dm = rvswd::dm;

constexpr uint32_t v208ChipId = 0x2080'053c;

const Bytes errorFrame{ 0x81, 0x55, 0x01, 0x01 };
const Bytes unknownReply{ 0x82, 0x81, 0x01, 0xff };

Bytes request(uint8_t cmd, const Bytes &payload) {
   Bytes packet{ 0x81, cmd, static_cast<uint8_t>(payload.size()) };
   packet.insert(packet.end(), payload.begin(), payload.end());
   return packet;
}

Bytes reply(uint8_t cmd, uint8_t value) {
   return { 0x82, cmd, 0x01, value };
}

// A pattern that is not erased flash
Bytes pattern(std::size_t size, uint8_t seed) {
   Bytes bytes(size);
   for(std::size_t i = 0; i < size; ++i) {
      bytes[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8));
   }
   return bytes;
}

// A CH32V003 on one wire or a CH32V208 on two wires, with its flash
// controller, enumerated and connected as wlink does.
struct FlashBench {
   explicit FlashBench(Wire wire)
      : wire(wire)
      , bench(wire)
      , flash(bench.dm, wire == Wire::One ? MockWchFlash::v003 : MockWchFlash::v208) {
      if(wire == Wire::Two) {
         bench.pins.registers[dm::chipId] = v208ChipId;
      }
      bench.enumerate();
      REQUIRE(bench.transact(request(0x0c, { 0x01, 0x01 })) == reply(0x0c, 0x01));
      REQUIRE(bench.transact(request(0x0d, { 0x02 })).size() == 8);
      // The connect halted the hart, as on the WCH-LinkE.
      bench.dm.csrs[dm::dpc] = 0x0800'0123;
      bench.dm.csrs[0x300] = 0x0000'1888;
      bench.pins.accesses.clear();
   }

   void checkClean() {
      for(const auto &error : flash.errors) {
         FAIL_CHECK(error);
      }
      CHECK(bench.pins.protocolErrors == 0);
      CHECK(bench.pins.contentions == 0);
   }

   Wire wire;
   WchLinkBench bench;
   MockWchFlash flash;
};

} // namespace

TEST_CASE("WCH-Link flash: mass erase with the own algorithm (wlink erase)") {
   for(const auto wire : { Wire::One, Wire::Two }) {
      const int wireIndex = static_cast<int>(wire);
      CAPTURE(wireIndex);
      FlashBench fb(wire);
      fb.flash.fill(MockWchFlash::flashBase, pattern(512, 1));
      // v003-wlink-erase.pcapng: 06 01, 06 04, then 02 01
      CHECK(fb.bench.transact(request(0x06, { 0x01 })) == reply(0x06, 0x02));
      CHECK(fb.bench.transact(request(0x06, { 0x04 })) == reply(0x06, 0x00));
      CHECK(fb.bench.transact(request(0x02, { 0x01 })) == reply(0x02, 0x01));
      CHECK(fb.flash.massErases == 1);
      CHECK(fb.flash.erased(MockWchFlash::flashBase, 512));
      // The reset before it, and the controller is locked again.
      CHECK(fb.bench.dm.systemResets == 1);
      CHECK(fb.flash.locked);
      CHECK(fb.bench.dm.halted);
      fb.checkClean();
   }
}

TEST_CASE("WCH-Link flash: 06 04 replies 00 without a target access") {
   FlashBench fb(Wire::Two);
   CHECK(fb.bench.transact(request(0x06, { 0x04 })) == reply(0x06, 0x00));
   CHECK(fb.bench.pins.accesses.empty());
}

TEST_CASE("WCH-Link flash: a controller that stays busy fails the erase") {
   FlashBench fb(Wire::Two);
   fb.flash.busyReads = 100'000;
   CHECK(fb.bench.transact(request(0x02, { 0x01 })) == errorFrame);
   // One read for each poll interval, until the timeout
   CHECK(fb.bench.platform.delays.size() > wchlink::Flash::operationTimeoutUs / wchlink::Flash::pollIntervalUs - 2);
}

TEST_CASE("WCH-Link flash: unprotect writes the option bytes and resets (wlink unprotect)") {
   for(const auto wire : { Wire::One, Wire::Two }) {
      const int wireIndex = static_cast<int>(wire);
      CAPTURE(wireIndex);
      FlashBench fb(wire);
      const std::size_t count = wire == Wire::One ? wchlink::fpec::optionBytesV00x : wchlink::fpec::optionBytesV20x;
      // Protected, with USER and DATA values, and a write protection
      fb.flash.setOption(0, 0x3c);
      fb.flash.setOption(1, 0xe7);
      fb.flash.setOption(2, 0x12);
      fb.flash.setOption(3, 0x34);
      fb.flash.setOption(4, 0xfe);
      fb.bench.dm.halted = false;
      CHECK(fb.bench.transact(request(0x0b, { 0x01 })) == reply(0x0b, 0x01));
      REQUIRE(fb.flash.readProtected);
      fb.flash.fill(MockWchFlash::flashBase, pattern(256, 9));
      fb.bench.dm.halted = true;

      // v003-wlink-flash-protected.pcapng
      CHECK(fb.bench.transact(request(0x06, { 0x01 })) == reply(0x06, 0x01));
      CHECK(fb.bench.transact(request(0x06, { 0x02 })) == reply(0x06, 0x02));
      CHECK(fb.flash.option(0) == 0xa5);
      CHECK(fb.flash.option(1) == 0xe7);
      CHECK(fb.flash.option(2) == 0x12);
      CHECK(fb.flash.option(3) == 0x34);
      for(std::size_t i = 4; i < count; ++i) {
         CHECK(fb.flash.option(i) == 0xff);
      }
      CHECK(fb.flash.optionErases == 1);
      // The chip erased the main flash.
      CHECK(fb.flash.autoErases == 1);
      CHECK(fb.flash.erased(MockWchFlash::flashBase, 256));
      // The reset after it loaded the new state.
      CHECK(fb.bench.transact(request(0x06, { 0x01 })) == reply(0x06, 0x02));
      CHECK(fb.flash.locked);
      CHECK_FALSE(fb.flash.obwre);
      fb.checkClean();
   }
}

TEST_CASE("WCH-Link flash: protect takes effect at the next reset (wlink protect)") {
   for(const auto wire : { Wire::One, Wire::Two }) {
      const int wireIndex = static_cast<int>(wire);
      CAPTURE(wireIndex);
      FlashBench fb(wire);
      fb.flash.setOption(1, 0xe7);
      fb.flash.setOption(4, 0xf0);
      // v208-wlink-protect.pcapng
      CHECK(fb.bench.transact(request(0x06, { 0x01 })) == reply(0x06, 0x02));
      CHECK(fb.bench.transact(request(0x06, { 0x03 })) == reply(0x06, 0x03));
      CHECK(fb.flash.option(0) != 0xa5);
      CHECK(fb.flash.option(0) != 0xcc);
      // USER and the write protection stay.
      CHECK(fb.flash.option(1) == 0xe7);
      CHECK(fb.flash.option(4) == 0xf0);
      CHECK(fb.flash.autoErases == 0);
      CHECK(fb.bench.transact(request(0x06, { 0x01 })) == reply(0x06, 0x02));
      // v208-wlink-status-protected.pcapng: after a reset
      CHECK(fb.bench.transact(request(0x0b, { 0x01 })) == reply(0x0b, 0x01));
      CHECK(fb.bench.transact(request(0x06, { 0x01 })) == reply(0x06, 0x01));
      fb.checkClean();
   }
}

TEST_CASE("WCH-Link flash: erase and protection need a connect") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();
   CHECK(bench.transact(request(0x02, { 0x01 })) == errorFrame);
   CHECK(bench.transact(request(0x06, { 0x02 })) == errorFrame);
   CHECK(bench.transact(request(0x06, { 0x03 })) == errorFrame);
   CHECK(bench.pins.accesses.empty());
}

TEST_CASE("WCH-Link flash: other sub-commands get the unknown-command reply") {
   FlashBench fb(Wire::Two);
   // The verify sub-command is not captured.
   for(const uint8_t sub : { 0x00, 0x03, 0x04, 0x06, 0x09, 0x0b, 0x0d, 0xff }) {
      CHECK(fb.bench.transact(request(0x02, { sub })) == unknownReply);
   }
   for(const uint8_t sub : { 0x00, 0x05, 0xff }) {
      CHECK(fb.bench.transact(request(0x06, { sub })) == unknownReply);
   }
   CHECK(fb.bench.pins.accesses.empty());
}
