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
constexpr uint8_t dataOut = 0x02;
constexpr uint8_t dataIn = 0x82;

const Bytes errorFrame{ 0x81, 0x55, 0x01, 0x01 };
const Bytes unknownReply{ 0x82, 0x81, 0x01, 0xff };
const Bytes blockAck{ 0x41, 0x01, 0x01, 0x04 };
const Bytes blockFailed{ 0x41, 0x01, 0x01, 0xff };

Bytes request(uint8_t cmd, const Bytes &payload) {
   Bytes packet{ 0x81, cmd, static_cast<uint8_t>(payload.size()) };
   packet.insert(packet.end(), payload.begin(), payload.end());
   return packet;
}

Bytes reply(uint8_t cmd, uint8_t value) {
   return { 0x82, cmd, 0x01, value };
}

Bytes u32(uint32_t address, uint32_t length) {
   return { static_cast<uint8_t>(address >> 24), static_cast<uint8_t>(address >> 16),
            static_cast<uint8_t>(address >> 8),  static_cast<uint8_t>(address),
            static_cast<uint8_t>(length >> 24),  static_cast<uint8_t>(length >> 16),
            static_cast<uint8_t>(length >> 8),   static_cast<uint8_t>(length) };
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

   [[nodiscard]] const wchlink::StubLayout &layout() const {
      return wire == Wire::One ? wchlink::layoutV00x : wchlink::layoutV20x;
   }

   // The host sends bytes on the data OUT endpoint in packets of 64 bytes.
   // Returns false when a packet got a NAK.
   bool send(const Bytes &bytes) {
      for(std::size_t offset = 0; offset < bytes.size(); offset += 64) {
         const std::size_t size = std::min<std::size_t>(64, bytes.size() - offset);
         if(!bench.controller().sendOut(dataOut, art::span<const uint8_t>(bytes.data() + offset, size))) {
            return false;
         }
         bench.mainLoop();
      }
      return true;
   }

   std::optional<Bytes> ack() {
      return bench.readData();
   }

   // 01, 02 05, the stub, 02 07, 02 02 (v003-wlink-flash.pcapng)
   void start(uint32_t address, uint32_t length) {
      flash.stub = pattern(wchlink::stubSize, 0x13);
      REQUIRE(bench.transact(request(0x01, u32(address, length))) == reply(0x01, 0x01));
      REQUIRE(bench.transact(request(0x02, { 0x05 })) == reply(0x02, 0x05));
      REQUIRE(send(flash.stub));
      REQUIRE(bench.transact(request(0x02, { 0x07 })) == reply(0x02, 0x07));
      REQUIRE(bench.transact(request(0x02, { 0x02 })) == reply(0x02, 0x02));
   }

   // The image in blocks, with the acknowledge after each block, as wlink.
   void program(const Bytes &image) {
      const std::size_t block = layout().blockSize;
      for(std::size_t offset = 0; offset < image.size(); offset += block) {
         const std::size_t size = std::min(block, image.size() - offset);
         CAPTURE(offset);
         REQUIRE(send(Bytes(image.begin() + static_cast<std::ptrdiff_t>(offset),
                            image.begin() + static_cast<std::ptrdiff_t>(offset + size))));
         CHECK(ack() == blockAck);
      }
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

TEST_CASE("WCH-Link flash: unprotect keeps USER and DATA when the chip hides the option bytes") {
   for(const auto wire : { Wire::One, Wire::Two }) {
      const int wireIndex = static_cast<int>(wire);
      CAPTURE(wireIndex);
      FlashBench fb(wire);
      // USER with the reserved bits of the series at 1
      const uint8_t user = wire == Wire::One ? 0xe7 : 0xdd;
      fb.flash.setOption(0, 0x3c);
      fb.flash.setOption(1, user);
      fb.flash.setOption(2, 0x12);
      fb.flash.setOption(3, 0x34);
      fb.flash.setOption(4, 0xfe);
      fb.bench.dm.halted = false;
      REQUIRE(fb.bench.transact(request(0x0b, { 0x01 })) == reply(0x0b, 0x01));
      REQUIRE(fb.flash.readProtected);
      fb.bench.dm.halted = true;
      SUBCASE("The reads fault") {
         for(uint32_t i = 0; i < 16; ++i) {
            fb.bench.dm.faultLoadAddresses.insert(wchlink::fpec::optionBytes + i);
         }
      }
      SUBCASE("The reads give a fixed word") {
         fb.flash.hiddenOptionWord = 0x0000'0300;
      }
      CHECK(fb.bench.transact(request(0x06, { 0x02 })) == reply(0x06, 0x02));
      CHECK(fb.flash.option(0) == 0xa5);
      // From FLASH_OBR. The CH32V20x OBR has no DATA bytes.
      CHECK(fb.flash.option(1) == user);
      CHECK(fb.flash.option(2) == (wire == Wire::One ? 0x12 : 0xff));
      CHECK(fb.flash.option(3) == (wire == Wire::One ? 0x34 : 0xff));
      CHECK(fb.flash.option(4) == 0xff);
      CHECK(fb.bench.transact(request(0x06, { 0x01 })) == reply(0x06, 0x02));
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

TEST_CASE("WCH-Link flash: program sequence of wlink with the host stub") {
   for(const auto wire : { Wire::One, Wire::Two }) {
      const int wireIndex = static_cast<int>(wire);
      CAPTURE(wireIndex);
      FlashBench fb(wire);
      auto &controller = fb.bench.controller();
      const auto &layout = fb.layout();
      // Three blocks, the flash had other data.
      const Bytes image = pattern(3 * layout.blockSize, 0x42);
      fb.flash.fill(MockWchFlash::flashBase, pattern(layout.blockSize, 0x99));
      CHECK_FALSE(controller.isArmed(dataOut));

      // v003-wlink-flash.pcapng
      fb.flash.stub = pattern(wchlink::stubSize, 0x13);
      CHECK(fb.bench.transact(request(0x01, u32(0x0800'0000, static_cast<uint32_t>(image.size())))) == reply(0x01, 0x01));
      CHECK(fb.bench.transact(request(0x02, { 0x05 })) == reply(0x02, 0x05));
      CHECK(fb.bench.dm.systemResets == 1);
      CHECK(controller.isArmed(dataOut));
      REQUIRE(fb.send(fb.flash.stub));
      // The stub is complete: no more data until 02 02.
      CHECK_FALSE(controller.isArmed(dataOut));
      CHECK(fb.bench.transact(request(0x02, { 0x07 })) == reply(0x02, 0x07));
      CHECK_FALSE(fb.flash.locked);
      CHECK_FALSE(fb.flash.flocked);
      CHECK(fb.bench.transact(request(0x02, { 0x02 })) == reply(0x02, 0x02));
      CHECK(controller.isArmed(dataOut));
      fb.program(image);
      // Armed until 02 08: a host can send more than the area.
      CHECK(controller.isArmed(dataOut));
      CHECK(fb.bench.transact(request(0x02, { 0x08 })) == reply(0x02, 0x08));
      CHECK_FALSE(fb.bench.processor.wantsData());

      for(std::size_t i = 0; i < image.size(); i += 4) {
         CAPTURE(i);
         const uint32_t expected = image[i] | image[i + 1] << 8 | image[i + 2] << 16 | static_cast<uint32_t>(image[i + 3]) << 24;
         REQUIRE(fb.flash.word(MockWchFlash::flashBase + static_cast<uint32_t>(i)) == expected);
      }
      // One unlock, then one call for each block: erase, program, verify
      REQUIRE(fb.flash.calls.size() == 4);
      CHECK(fb.flash.calls[0].operation == wchlink::stubUnlock);
      for(uint32_t block = 0; block < 3; ++block) {
         const auto &call = fb.flash.calls[block + 1];
         CHECK(call.operation == (wchlink::stubErasePages | wchlink::stubProgram | wchlink::stubVerify));
         CHECK(call.address == 0x0800'0000 + block * layout.blockSize);
         CHECK(call.length == layout.blockSize);
      }
      // Locked, and the hart state of the reset is back.
      CHECK(fb.flash.locked);
      CHECK(fb.flash.flocked);
      CHECK(fb.bench.dm.csrs[dm::dpc] == 0x0800'0123);
      CHECK(fb.bench.dm.csrs[0x300] == 0x0000'1888);
      CHECK(fb.bench.dm.halted);
      // wlink then resets and quits.
      CHECK(fb.bench.transact(request(0x0b, { 0x01 })) == reply(0x0b, 0x01));
      CHECK(fb.bench.transact(request(0x0d, { 0xff })) == reply(0x0d, 0xff));
      fb.checkClean();
   }
}

TEST_CASE("WCH-Link flash: the last block can be short, with an odd length") {
   for(const auto wire : { Wire::One, Wire::Two }) {
      const int wireIndex = static_cast<int>(wire);
      CAPTURE(wireIndex);
      FlashBench fb(wire);
      const auto &layout = fb.layout();
      const uint32_t base = 0x0800'0000 + 2 * layout.pageSize;
      const Bytes image = pattern(layout.blockSize + layout.pageSize + 5, 0x21);
      // The rest of the last page had other data.
      fb.flash.fill(base, pattern(2 * layout.blockSize, 0x77));
      fb.start(base, static_cast<uint32_t>(image.size()));
      // The bytes after the area are ignored.
      Bytes sent = image;
      sent.resize(image.size() + 3, 0x00);
      fb.program(sent);
      CHECK(fb.bench.transact(request(0x02, { 0x08 })) == reply(0x02, 0x08));

      REQUIRE(fb.flash.calls.size() == 3);
      CHECK(fb.flash.calls[2].address == base + layout.blockSize);
      // Two pages
      CHECK(fb.flash.calls[2].length == 2 * layout.pageSize);
      for(std::size_t i = 0; i < image.size(); ++i) {
         CAPTURE(i);
         REQUIRE(fb.flash.flash[base - MockWchFlash::flashBase + i] == image[i]);
      }
      // The rest of the last page is erased flash.
      for(std::size_t i = image.size(); i < layout.blockSize + 2 * layout.pageSize; ++i) {
         CAPTURE(i);
         REQUIRE(fb.flash.flash[base - MockWchFlash::flashBase + i] == 0xff);
      }
      fb.checkClean();
   }
}

TEST_CASE("WCH-Link flash: packets after the end of the area are taken and dropped") {
   FlashBench fb(Wire::One);
   const Bytes image = pattern(1024 + 100, 0x31);
   fb.start(0x0800'0000, static_cast<uint32_t>(image.size()));
   fb.program(image);
   // A host that pads the image to whole blocks: no NAK, no acknowledge
   CHECK(fb.bench.controller().isArmed(dataOut));
   REQUIRE(fb.send(Bytes(1024 - 100, 0xff)));
   CHECK_FALSE(fb.ack().has_value());
   CHECK(fb.flash.calls.size() == 3);
   CHECK(fb.bench.transact(request(0x02, { 0x08 })) == reply(0x02, 0x08));
   CHECK_FALSE(fb.bench.processor.wantsData());
   fb.checkClean();
}

TEST_CASE("WCH-Link flash: minichlink sends 256-byte transfers, reads no acknowledge, and quits") {
   // v208-mc-write.pcapng: one block of 4 KB, no 02 08
   FlashBench fb(Wire::Two);
   const Bytes image = pattern(4096, 0x5a);
   // minichlink resets with haltreq before the transfer.
   REQUIRE(fb.bench.transact(request(0x08, { 0x10, 0x80, 0x00, 0x00, 0x03, 0x02 })).back() == 0x00);
   REQUIRE(fb.bench.transact(request(0x08, { 0x10, 0x80, 0x00, 0x00, 0x01, 0x02 })).back() == 0x00);
   fb.start(0x0800'0000, 4096);
   REQUIRE(fb.send(image));
   // The host sends the quit at once. The block is written first.
   CHECK(fb.bench.transact(request(0x0d, { 0xff })) == reply(0x0d, 0xff));
   CHECK(fb.flash.word(0x0800'0000) == static_cast<uint32_t>(image[0] | image[1] << 8 | image[2] << 16 | image[3] << 24));
   CHECK(fb.flash.calls.size() == 2);
   // The quit ended the sequence: locked, and the hart runs from the reset
   // state.
   CHECK(fb.flash.locked);
   CHECK(fb.bench.dm.csrs[dm::dpc] == 0x0800'0123);
   CHECK_FALSE(fb.bench.dm.halted);
   CHECK_FALSE(fb.bench.processor.connected());
   fb.checkClean();
}

TEST_CASE("WCH-Link flash: the program sequence in the wrong order gives the error frame") {
   FlashBench fb(Wire::Two);
   fb.flash.stub = pattern(wchlink::stubSize, 0x13);
   CHECK(fb.bench.transact(request(0x01, u32(0x0800'0000, 0x1000))) == reply(0x01, 0x01));
   // No sequence
   CHECK(fb.bench.transact(request(0x02, { 0x07 })) == errorFrame);
   CHECK(fb.bench.transact(request(0x02, { 0x02 })) == errorFrame);
   CHECK(fb.bench.transact(request(0x02, { 0x08 })) == errorFrame);
   CHECK_FALSE(fb.bench.controller().isArmed(dataOut));
   CHECK(fb.bench.pins.accesses.empty());

   SUBCASE("Unlock before the stub is complete") {
      CHECK(fb.bench.transact(request(0x02, { 0x05 })) == reply(0x02, 0x05));
      REQUIRE(fb.send(Bytes(fb.flash.stub.begin(), fb.flash.stub.begin() + 256)));
      CHECK(fb.bench.transact(request(0x02, { 0x07 })) == errorFrame);
      CHECK(fb.flash.calls.empty());
      CHECK(fb.bench.transact(request(0x02, { 0x08 })) == errorFrame);
   }
   SUBCASE("Data before the unlock") {
      CHECK(fb.bench.transact(request(0x02, { 0x05 })) == reply(0x02, 0x05));
      REQUIRE(fb.send(fb.flash.stub));
      CHECK(fb.bench.transact(request(0x02, { 0x02 })) == errorFrame);
   }
   SUBCASE("A second unlock") {
      CHECK(fb.bench.transact(request(0x02, { 0x05 })) == reply(0x02, 0x05));
      REQUIRE(fb.send(fb.flash.stub));
      CHECK(fb.bench.transact(request(0x02, { 0x07 })) == reply(0x02, 0x07));
      CHECK(fb.bench.transact(request(0x02, { 0x07 })) == errorFrame);
   }
   SUBCASE("End before all data") {
      fb.start(0x0800'0000, 0x1000);
      REQUIRE(fb.send(pattern(64, 1)));
      CHECK(fb.bench.transact(request(0x02, { 0x08 })) == errorFrame);
      CHECK(fb.flash.locked);
   }
   SUBCASE("An area that does not start at a page") {
      CHECK(fb.bench.transact(request(0x01, u32(0x0800'0080, 0x1000))) == reply(0x01, 0x01));
      CHECK(fb.bench.transact(request(0x02, { 0x05 })) == reply(0x02, 0x05));
      REQUIRE(fb.send(fb.flash.stub));
      CHECK(fb.bench.transact(request(0x02, { 0x07 })) == reply(0x02, 0x07));
      CHECK(fb.bench.transact(request(0x02, { 0x02 })) == errorFrame);
   }
   CHECK_FALSE(fb.bench.processor.wantsData());
   // An armed endpoint stays armed. Its next packet is dropped.
   if(fb.bench.controller().isArmed(dataOut)) {
      CHECK(fb.send(pattern(64, 0)));
   }
   CHECK_FALSE(fb.bench.controller().isArmed(dataOut));
   fb.checkClean();
}

TEST_CASE("WCH-Link flash: the program sequence needs a connect") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();
   for(const uint8_t sub : { 0x05, 0x07, 0x02, 0x08 }) {
      CHECK(bench.transact(request(0x02, { sub })) == errorFrame);
   }
   CHECK_FALSE(bench.controller().isArmed(dataOut));
   CHECK(bench.pins.accesses.empty());
}

TEST_CASE("WCH-Link flash: a failed block gives a failed acknowledge and ends the data") {
   FlashBench fb(Wire::Two);
   fb.start(0x0800'0000, 3 * 4096);
   SUBCASE("Verify error") {
      fb.flash.corruptNextProgram = true;
   }
   SUBCASE("The stub does not reach its ebreak") {
      fb.flash.stubHangs = true;
   }
   REQUIRE(fb.send(pattern(4096, 3)));
   CHECK(fb.ack() == blockFailed);
   CHECK_FALSE(fb.bench.controller().isArmed(dataOut));
   // A hung stub is halted again.
   CHECK(fb.bench.dm.halted);
   CHECK(fb.bench.transact(request(0x02, { 0x08 })) == errorFrame);
   CHECK(fb.flash.locked);
   // A new sequence works.
   fb.flash.stubHangs = false;
   fb.flash.calls.clear();
   fb.start(0x0800'0000, 4096);
   fb.program(pattern(4096, 4));
   CHECK(fb.bench.transact(request(0x02, { 0x08 })) == reply(0x02, 0x08));
}

TEST_CASE("WCH-Link flash: a stub that differs from the host stub never halts") {
   // The model checks the stub bytes in RAM: a wrong word order would fail.
   FlashBench fb(Wire::One);
   fb.flash.stub = pattern(wchlink::stubSize, 0x13);
   CHECK(fb.bench.transact(request(0x01, u32(0x0800'0000, 1024))) == reply(0x01, 0x01));
   CHECK(fb.bench.transact(request(0x02, { 0x05 })) == reply(0x02, 0x05));
   Bytes other = fb.flash.stub;
   other[100] ^= 0xff;
   REQUIRE(fb.send(other));
   CHECK(fb.bench.transact(request(0x02, { 0x07 })) == errorFrame);
   CHECK(fb.flash.errors.size() == 1);
}

TEST_CASE("WCH-Link flash: a bus reset drops the program sequence") {
   FlashBench fb(Wire::Two);
   fb.start(0x0800'0000, 4096);
   fb.bench.enumerate();
   CHECK_FALSE(fb.bench.controller().isArmed(dataOut));
   CHECK_FALSE(fb.bench.processor.wantsData());
}
