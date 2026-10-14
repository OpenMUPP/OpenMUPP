// The WCH-Link personality through its USB function: the descriptors, the
// endpoints, and the commands of the processor on the wire engine with mock
// pins and a mock DM. The request and response bytes come from the
// WCH-LinkE captures in docs/wch-link-captures (docs/wch-link-protocol.md).

#include "WchLinkBench.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using art::usb::DescriptorType;
using art::usb::EndpointType;
using rvswd::Wire;
using vsltest::WchLinkBench;
using Bytes = std::vector<uint8_t>;

namespace dm = rvswd::dm;

constexpr uint32_t v208ChipId = 0x2080'053c;
constexpr uint32_t v003ChipId = 0x0030'0500;

Bytes bytes(const auto &descriptor) {
   return { descriptor.begin(), descriptor.end() };
}

// [0x81][cmd][len][payload]
Bytes request(uint8_t cmd, const Bytes &payload) {
   Bytes packet{ 0x81, cmd, static_cast<uint8_t>(payload.size()) };
   packet.insert(packet.end(), payload.begin(), payload.end());
   return packet;
}

const Bytes unknownReply{ 0x82, 0x81, 0x01, 0xff };
const Bytes connectError{ 0x81, 0x55, 0x01, 0x01 };

struct Access {
   uint8_t address;
   bool write;
   uint32_t data;

   bool operator==(const Access &other) const {
      return address == other.address && write == other.write && data == other.data;
   }
};

Access W(uint8_t address, uint32_t data) {
   return { address, true, data };
}
Access R(uint8_t address, uint32_t data) {
   return { address, false, data };
}

// The accesses since the last call
std::vector<Access> take(WchLinkBench &bench) {
   std::vector<Access> result;
   for(const auto &access : bench.pins.accesses) {
      result.push_back({ access.address, access.write, access.data });
   }
   bench.pins.accesses.clear();
   return result;
}

// Words of little-endian memory
void putWord(vsltest::MockDebugModule &dm, uint32_t address, uint32_t value) {
   dm.store(address, value, 4);
}

// The family code and the chip ID of the connect response
Bytes connectReply(uint8_t family, uint32_t chipId) {
   return { 0x82,
            0x0d,
            0x05,
            family,
            static_cast<uint8_t>(chipId >> 24),
            static_cast<uint8_t>(chipId >> 16),
            static_cast<uint8_t>(chipId >> 8),
            static_cast<uint8_t>(chipId) };
}

// wlink: configure with the generic chip type, then connect
void connect(WchLinkBench &bench, uint8_t chipType = 0x01) {
   REQUIRE(bench.transact(request(0x0c, { chipType, 0x01 })) == Bytes{ 0x82, 0x0c, 0x01, 0x01 });
   const auto reply = bench.transact(request(0x0d, { 0x02 }));
   REQUIRE(reply.size() == 8);
   REQUIRE(reply[0] == 0x82);
   bench.pins.accesses.clear();
   // On two wires, the try of one wire came first.
   bench.pins.inits = 0;
   bench.pins.finis = 0;
}

} // namespace

template <>
struct doctest::StringMaker<std::vector<Access>> {
   static String convert(const std::vector<Access> &accesses) {
      std::string text;
      for(const auto &access : accesses) {
         char item[32];
         std::snprintf(item, sizeof(item), "%s %02x %08x, ", access.write ? "W" : "R", access.address, access.data);
         text += item;
      }
      return text.c_str();
   }
};

TEST_CASE("WCH-Link descriptors: one vendor interface with two bulk pairs") {
   CHECK(usb::wch::deviceDescriptor[0] == 18);
   // The own ID is the default. The WCH ID is a build option.
   CHECK(usb::wch::vendorId == 0x1209);
   CHECK(usb::wch::productId == 0x0001);
   CHECK((usb::wch::deviceDescriptor[8] | usb::wch::deviceDescriptor[9] << 8) == 0x1209);
   CHECK((usb::wch::deviceDescriptor[10] | usb::wch::deviceDescriptor[11] << 8) == 0x0001);

   const auto &set = usb::wch::configurationDescriptor;
   // Configuration, one interface, four endpoints
   REQUIRE(set.size() == 9 + 9 + 4 * 7);
   CHECK(set[4] == 1);
   CHECK(set[9 + 4] == 4);
   CHECK(set[9 + 5] == 0xFF);
   const std::vector<uint8_t> addresses{ 0x01, 0x81, 0x02, 0x82 };
   for(std::size_t i = 0; i < addresses.size(); ++i) {
      const auto *endpoint = &set[18 + 7 * i];
      CHECK(endpoint[2] == addresses[i]);
      CHECK(endpoint[3] == static_cast<uint8_t>(EndpointType::Bulk));
      CHECK((endpoint[4] | endpoint[5] << 8) == 64);
   }
   // Buffer descriptor table (3 endpoint numbers), EP0, four bulk endpoints
   CHECK(usb::wch::packetMemoryUsed == 3 * 8 + 128 + 256);
}

TEST_CASE("WCH-Link: the host reads the descriptors and opens the endpoints") {
   WchLinkBench bench;
   bench.controller().busReset();
   CHECK(bench.descriptor(DescriptorType::Device, 0, 18).data == bytes(usb::wch::deviceDescriptor));
   CHECK(bench.descriptor(DescriptorType::Configuration, 0, 255).data == bytes(usb::wch::configurationDescriptor));
   CHECK(bench.descriptor(DescriptorType::String, usb::wch::stringProduct, 255).data == bytes(usb::wch::productDescriptor));
   CHECK(bench.descriptor(DescriptorType::String, usb::wch::stringSerialNumber, 255).data.size() == 50);

   bench.request(WchLinkBench::setAddress, 5);
   bench.request(WchLinkBench::setConfiguration, 1);
   auto &controller = bench.controller();
   for(const uint8_t endpoint : { 0x01, 0x81, 0x02, 0x82 }) {
      CHECK(controller.isOpen(endpoint));
   }
   CHECK(controller.isArmed(0x01));
   // The flash data endpoint is for W5.
   CHECK_FALSE(controller.isArmed(0x02));
}

TEST_CASE("WCH-Link: probe information, without a target access") {
   WchLinkBench bench;
   bench.enumerate();
   CHECK(bench.transact(request(0x0d, { 0x01 })) == Bytes{ 0x82, 0x0d, 0x04, 0x02, 0x13, 0x12, 0x00 });
   CHECK(bench.pins.inits == 0);
   CHECK(bench.pins.accesses.empty());
   CHECK(bench.function.commandCount() == 1);
}

TEST_CASE("WCH-Link: unknown and refused commands get the unknown-command reply") {
   WchLinkBench bench;
   bench.enumerate();
   // v003-raw-errors.pcapng
   CHECK(bench.transact(request(0x7f, { 0x00 })) == unknownReply);
   // The flash commands (W5): set area, read protection, program sub-commands
   CHECK(bench.transact(request(0x01, { 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00 })) == unknownReply);
   for(const uint8_t sub : { 0x02, 0x03, 0x04 }) {
      CHECK(bench.transact(request(0x06, { sub })) == unknownReply);
   }
   for(const uint8_t sub : { 0x01, 0x02, 0x05, 0x07, 0x08 }) {
      CHECK(bench.transact(request(0x02, { sub })) == unknownReply);
   }
   // Other command numbers, for example firmware update or mode switch
   for(const uint8_t cmd : { 0x00, 0x0f, 0x10, 0x12, 0x80, 0xff }) {
      CHECK(bench.transact(request(cmd, { 0x01 })) == unknownReply);
   }
   // Not a request: a wrong header, or no command
   CHECK(bench.transact({ 0x82, 0x0d, 0x01, 0x01 }) == unknownReply);
   CHECK(bench.transact({ 0x81, 0x0d }) == unknownReply);
   CHECK(bench.pins.inits == 0);
   CHECK(bench.pins.accesses.empty());

   // The probe still works.
   CHECK(bench.transact(request(0x0d, { 0x01 })) == Bytes{ 0x82, 0x0d, 0x04, 0x02, 0x13, 0x12, 0x00 });
}

TEST_CASE("WCH-Link: control sub-commands without a target") {
   WchLinkBench bench;
   bench.enumerate();
   // minichlink sends 0d 03 first.
   CHECK(bench.transact(request(0x0d, { 0x03 })) == Bytes{ 0x82, 0x0d, 0x01, 0x03 });
   CHECK(bench.transact(request(0x0d, { 0x04 })) == Bytes{ 0x82, 0x0d, 0x01, 0x00 });
   // The actions of the WCH-LinkE are refused: RAM and flash split, erase,
   // target power, reset pin, SDI print. 0d 05 echoes on the WCH-LinkE.
   for(const uint8_t sub : { 0x05, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0f, 0x13, 0x14, 0x15, 0xee }) {
      CHECK(bench.transact(request(0x0d, { sub, 0x00 })) == unknownReply);
   }
   // Quit without a connect
   CHECK(bench.transact(request(0x0d, { 0xff })) == Bytes{ 0x82, 0x0d, 0x01, 0xff });
   CHECK(bench.pins.accesses.empty());
}

TEST_CASE("WCH-Link: a missing payload reads 0, as on the WCH-LinkE") {
   WchLinkBench bench;
   bench.enumerate();
   // v003-raw-errors.pcapng: 81 03 01 00 -> 82 03 01 01
   CHECK(bench.transact(request(0x03, { 0x00 })) == Bytes{ 0x82, 0x03, 0x01, 0x01 });
   // A length byte beyond the packet
   CHECK(bench.transact({ 0x81, 0x0c, 0x20, 0x01 }) == Bytes{ 0x82, 0x0c, 0x01, 0x01 });
}

TEST_CASE("WCH-Link: connect on two wires does the probe actions (CH32V208)") {
   WchLinkBench bench(Wire::Two);
   bench.pins.registers[dm::chipId] = v208ChipId;
   bench.enumerate();
   CHECK(bench.transact(request(0x0c, { 0x01, 0x01 })) == Bytes{ 0x82, 0x0c, 0x01, 0x01 });
   // v208-wlink-status.pcapng
   CHECK(bench.transact(request(0x0d, { 0x02 })) == connectReply(0x05, v208ChipId));
   CHECK(bench.processor.connected());
   CHECK(bench.dm.halted);
   // The try of one wire, then two wires
   CHECK(bench.pins.inits == 2);
   CHECK(bench.pins.protocolErrors == 0);
   CHECK(bench.pins.contentions == 0);

   const std::vector<Access> expected{
      W(0x7e, 0x5aa5'0400),
      W(0x7d, 0x5aa5'0400),
      R(dm::dmstatus, 0x0000'0c82),
      W(dm::dmcontrol, 0x8000'0001),
      W(dm::dmcontrol, 0x8000'0001),
      R(dm::dmstatus, 0x0000'0382),
      R(dm::chipId, v208ChipId),
      // CSR 0x7c0 = 0x300. The module clears cmderr before its first command.
      W(dm::data0, 0x0000'0300),
      W(dm::abstractcs, dm::cmderrClear),
      W(dm::command, 0x0023'07c0),
      R(dm::abstractcs, 0x0800'0002),
   };
   CHECK(take(bench) == expected);
   CHECK(bench.dm.csrs[0x7c0] == 0x300);

   // wlink then configures with the family code.
   CHECK(bench.transact(request(0x0c, { 0x05, 0x01 })) == Bytes{ 0x82, 0x0c, 0x01, 0x01 });
   CHECK(bench.pins.accesses.empty());
}

TEST_CASE("WCH-Link: connect on one wire with chip type 0x09 (CH32V003)") {
   WchLinkBench bench(Wire::One);
   bench.enumerate();
   // minichlink: 0c 09 01, then connect
   CHECK(bench.transact(request(0x0c, { 0x09, 0x01 })) == Bytes{ 0x82, 0x0c, 0x01, 0x01 });
   // v003-wlink-status.pcapng
   CHECK(bench.transact(request(0x0d, { 0x02 })) == connectReply(0x09, v003ChipId));
   // One wire only
   CHECK(bench.pins.inits == 1);
   CHECK(bench.pins.unlockedChanges == 0);
   CHECK(bench.pins.contentions == 0);
   CHECK(bench.dm.halted);
}

TEST_CASE("WCH-Link: connect with the generic chip type finds the target on either wire") {
   for(const auto wire : { Wire::One, Wire::Two }) {
      const int wireIndex = static_cast<int>(wire);
      CAPTURE(wireIndex);
      WchLinkBench bench(wire);
      if(wire == Wire::Two) {
         bench.pins.registers[dm::chipId] = v208ChipId;
      }
      bench.enumerate();
      // wlink sends 0c 01 01 before the connect also for the CH32V003.
      CHECK(bench.transact(request(0x0c, { 0x01, 0x01 })) == Bytes{ 0x82, 0x0c, 0x01, 0x01 });
      CHECK(bench.transact(request(0x0d, { 0x02 })) ==
            (wire == Wire::One ? connectReply(0x09, v003ChipId) : connectReply(0x05, v208ChipId)));
      // One wire first, then two wires
      CHECK(bench.pins.inits == (wire == Wire::One ? 1 : 2));
      CHECK(bench.pins.finis == (wire == Wire::One ? 0 : 1));
      CHECK(bench.processor.connected());
      CHECK(bench.dm.halted);
      // The frames of the other wire are no access and no error for the
      // target, and nobody fights over the line.
      CHECK(bench.pins.contentions == 0);
      CHECK(bench.pins.protocolErrors == 0);
      CHECK(bench.pins.unlockedChanges == 0);
      const auto accesses = take(bench);
      REQUIRE_FALSE(accesses.empty());
      CHECK(accesses.front() == W(0x7e, 0x5aa5'0400));
      int unlocks = 0;
      for(const auto &access : accesses) {
         unlocks += access.address == 0x7e ? 1 : 0;
      }
      CHECK(unlocks == 1);
   }
}

TEST_CASE("WCH-Link: the family code comes from a table of chip ID prefixes") {
   // Captured
   CHECK(wchlink::familyOf(v003ChipId) == 0x09);
   CHECK(wchlink::familyOf(v208ChipId) == 0x05);
   // The CH32V20x series, not captured
   CHECK(wchlink::familyOf(0x2030'0500) == 0x05);
   CHECK(wchlink::familyOf(0x2070'050c) == 0x05);
   // Unknown chips: no family code
   CHECK(wchlink::familyOf(0x3050'0508) == 0);
   CHECK(wchlink::familyOf(0x2500'0410) == 0);
   CHECK(wchlink::familyOf(0x0020'0600) == 0);
}

TEST_CASE("WCH-Link: connect to a chip without a family code fails and lets the hart run") {
   // A one-wire chip that is not in the table
   WchLinkBench bench(Wire::One);
   bench.pins.registers[dm::chipId] = 0x0020'0600;
   bench.enumerate();
   CHECK(bench.transact(request(0x0c, { 0x01, 0x01 })) == Bytes{ 0x82, 0x0c, 0x01, 0x01 });
   CHECK(bench.transact(request(0x0d, { 0x02 })) == connectError);
   CHECK_FALSE(bench.processor.connected());
   // No try on two wires
   CHECK(bench.pins.inits == 1);
   CHECK(bench.pins.finis == 1);
   CHECK_FALSE(bench.dm.halted);
   CHECK_FALSE(bench.dm.active());
   CHECK_FALSE(bench.pins.hostDrives());
}

TEST_CASE("WCH-Link: protection query reads FLASH_OBR at the command") {
   for(const auto wire : { Wire::Two, Wire::One }) {
      const int wireIndex = static_cast<int>(wire);
      CAPTURE(wireIndex);
      WchLinkBench bench(wire);
      bench.enumerate();
      // Before a connect
      CHECK(bench.transact(request(0x06, { 0x01 })) == connectError);
      connect(bench);
      // Unprotected: the values of v208-wlink-status-timing.csv
      putWord(bench.dm, 0x4002'201c, 0x0000'00fc);
      putWord(bench.dm, 0x4002'2020, 0xffff'ffff);
      bench.dm.commands.clear();
      CHECK(bench.transact(request(0x06, { 0x01 })) == Bytes{ 0x82, 0x06, 0x01, 0x02 });
      CHECK(bench.dm.commands.size() == 2);
      // Protected: RDPRT (bit 1)
      putWord(bench.dm, 0x4002'201c, 0x0000'00fe);
      CHECK(bench.transact(request(0x06, { 0x01 })) == Bytes{ 0x82, 0x06, 0x01, 0x01 });
      // A failed read
      bench.dm.faultAddresses.insert(0x4002'2020);
      CHECK(bench.transact(request(0x06, { 0x01 })) == connectError);
   }
}

TEST_CASE("WCH-Link: connect without a target gives the error frame") {
   for(const auto wire : { Wire::Two, Wire::One }) {
      const int wireIndex = static_cast<int>(wire);
      CAPTURE(wireIndex);
      WchLinkBench bench(wire);
      bench.pins.present = false;
      bench.enumerate();
      CHECK(bench.transact(request(0x0c, { 0x01, 0x01 })) == Bytes{ 0x82, 0x0c, 0x01, 0x01 });
      // v208-wlink-connect-fail.pcapng
      CHECK(bench.transact(request(0x0d, { 0x02 })) == connectError);
      CHECK_FALSE(bench.processor.connected());
      // Both wires were tried, and the pins are released.
      CHECK(bench.pins.inits == 2);
      CHECK(bench.pins.finis == 2);
      CHECK_FALSE(bench.pins.hostDrives());

      // The commands that need a target fail.
      CHECK(bench.transact(request(0x11, { 0x06 })) == connectError);
      CHECK(bench.transact(request(0x0b, { 0x01 })) == connectError);
      CHECK(bench.transact(request(0x02, { 0x0c })) == connectError);
      CHECK(bench.transact(request(0x08, { 0x11, 0, 0, 0, 0, 0x01 })) == Bytes{ 0x82, 0x08, 0x06, 0x11, 0, 0, 0, 0, 0x02 });
   }
}

TEST_CASE("WCH-Link: DMI access") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();

   // Before a connect: status 2, no access
   CHECK(bench.transact(request(0x08, { 0x11, 0, 0, 0, 0, 0x01 })) == Bytes{ 0x82, 0x08, 0x06, 0x11, 0, 0, 0, 0, 0x02 });
   CHECK(bench.pins.inits == 0);

   connect(bench);
   // Read dmstatus (v003-wlink-status.pcapng: 81 08 06 11 00000000 01)
   CHECK(bench.transact(request(0x08, { 0x11, 0, 0, 0, 0, 0x01 })) == Bytes{ 0x82, 0x08, 0x06, 0x11, 0x00, 0x00, 0x03, 0x82, 0x00 });
   // Write data0, then read it back
   CHECK(bench.transact(request(0x08, { 0x04, 0x12, 0x34, 0x56, 0x78, 0x02 })) ==
         Bytes{ 0x82, 0x08, 0x06, 0x04, 0x12, 0x34, 0x56, 0x78, 0x00 });
   CHECK(bench.transact(request(0x08, { 0x04, 0, 0, 0, 0, 0x01 })) == Bytes{ 0x82, 0x08, 0x06, 0x04, 0x12, 0x34, 0x56, 0x78, 0x00 });
   // op 0: no access
   CHECK(bench.transact(request(0x08, { 0x10, 0xaa, 0xbb, 0xcc, 0xdd, 0x00 })) ==
         Bytes{ 0x82, 0x08, 0x06, 0x10, 0xaa, 0xbb, 0xcc, 0xdd, 0x00 });
   // An unknown op fails.
   CHECK(bench.transact(request(0x08, { 0x10, 0, 0, 0, 0, 0x03 })) == Bytes{ 0x82, 0x08, 0x06, 0x10, 0, 0, 0, 0, 0x02 });
   const std::vector<Access> expected{
      R(dm::dmstatus, 0x0000'0382),
      W(dm::data0, 0x1234'5678),
      R(dm::data0, 0x1234'5678),
   };
   CHECK(take(bench) == expected);

   // A wire error gives status 2.
   bench.pins.badParity = true;
   CHECK(bench.transact(request(0x08, { 0x04, 0, 0, 0, 0, 0x01 })).back() == 0x02);
}

TEST_CASE("WCH-Link: chip information reads the target at the command") {
   for(const auto wire : { Wire::Two, Wire::One }) {
      const int wireIndex = static_cast<int>(wire);
      CAPTURE(wireIndex);
      WchLinkBench bench(wire);
      bench.enumerate();
      connect(bench);
      // CH32V003 values of the capture
      putWord(bench.dm, 0x1fff'f7e0, 0xffff'0010);
      putWord(bench.dm, 0x1fff'f7e8, 0x6784'abcd);
      putWord(bench.dm, 0x1fff'f7ec, 0xcf99'bc48);
      putWord(bench.dm, 0x1fff'f7f0, 0xffff'ffff);
      // 20 raw bytes, no header
      CHECK(bench.transact(request(0x11, { 0x06 })) == Bytes{ 0xff, 0xff, 0x00, 0x10, 0x67, 0x84, 0xab,
                                                              0xcd, 0xcf, 0x99, 0xbc, 0x48, 0xff, 0xff,
                                                              0xff, 0xff, 0x00, 0x30, 0x05, 0x00 });
      CHECK_FALSE(bench.pins.accesses.empty());

      // A failed read gives the error frame.
      bench.dm.faultAddresses.insert(0x1fff'f7e8);
      CHECK(bench.transact(request(0x11, { 0x05 })) == connectError);
   }
}

TEST_CASE("WCH-Link: memory read sends the words on the data endpoint") {
   for(const auto wire : { Wire::Two, Wire::One }) {
      const int wireIndex = static_cast<int>(wire);
      CAPTURE(wireIndex);
      WchLinkBench bench(wire);
      bench.enumerate();
      connect(bench);
      for(uint32_t i = 0; i < 32; ++i) {
         putWord(bench.dm, 0x0800'0000 + 4 * i, 0x1220'006f + i);
      }
      // v003-wlink-dump.pcapng: 03 08 addr len, then 02 01 0c
      CHECK(bench.transact(request(0x03, { 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48 })) == Bytes{ 0x82, 0x03, 0x01, 0x01 });
      CHECK(bench.transact(request(0x02, { 0x0c })) == Bytes{ 0x82, 0x02, 0x01, 0x0c });

      Bytes data;
      std::vector<std::size_t> sizes;
      while(auto packet = bench.readData()) {
         sizes.push_back(packet->size());
         data.insert(data.end(), packet->begin(), packet->end());
      }
      // 0x48 bytes: one full packet, then 8 bytes, no ZLP
      CHECK(sizes == std::vector<std::size_t>{ 64, 8 });
      REQUIRE(data.size() == 0x48);
      // Most significant byte first: 6f 00 20 12 in memory arrives as 12 20 00 6f.
      for(uint32_t i = 0; i < 0x48 / 4; ++i) {
         const uint32_t value = 0x1220'006f + i;
         CHECK(data[4 * i] == static_cast<uint8_t>(value >> 24));
         CHECK(data[4 * i + 1] == static_cast<uint8_t>(value >> 16));
         CHECK(data[4 * i + 2] == static_cast<uint8_t>(value >> 8));
         CHECK(data[4 * i + 3] == static_cast<uint8_t>(value));
      }
      CHECK_FALSE(bench.processor.hasData());
      CHECK(bench.pins.contentions == 0);
   }
}

TEST_CASE("WCH-Link: a memory read length that is not a multiple of 4 cuts the last word") {
   for(const uint32_t length : { 6u, 0x42u }) {
      CAPTURE(length);
      WchLinkBench bench(Wire::Two);
      bench.enumerate();
      connect(bench);
      for(uint32_t i = 0; i < 17; ++i) {
         putWord(bench.dm, 0x2000'0000 + 4 * i, 0x1122'3300 + i);
      }
      bench.transact(request(0x03, { 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, static_cast<uint8_t>(length) }));
      bench.transact(request(0x02, { 0x0c }));
      Bytes data;
      while(auto packet = bench.readData()) {
         data.insert(data.end(), packet->begin(), packet->end());
      }
      REQUIRE(data.size() == length);
      // The last word: its most significant bytes only
      const uint32_t last = 0x1122'3300 + (length - 1) / 4;
      CHECK(data[length - 2] == static_cast<uint8_t>(last >> 24));
      CHECK(data[length - 1] == static_cast<uint8_t>(last >> 16));
   }
}

TEST_CASE("WCH-Link: a new memory read waits until the host took the old packet") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();
   connect(bench);
   putWord(bench.dm, 0x2000'0000, 0xaaaa'aaaa);
   putWord(bench.dm, 0x2000'1000, 0x5555'5555);
   bench.transact(request(0x03, { 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00 }));
   bench.transact(request(0x02, { 0x0c }));
   // The host gives up: a new range and a new start while the first packet
   // of the old read waits on 0x82.
   CHECK(bench.transact(request(0x03, { 0x20, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x04 })) == Bytes{ 0x82, 0x03, 0x01, 0x01 });
   CHECK(bench.transact(request(0x02, { 0x0c })) == connectError);
   // The host takes the old packet. Then the new read starts.
   const auto old = bench.readData();
   REQUIRE(old.has_value());
   CHECK((*old)[0] == 0xaa);
   CHECK_FALSE(bench.readData().has_value());
   CHECK(bench.transact(request(0x02, { 0x0c })) == Bytes{ 0x82, 0x02, 0x01, 0x0c });
   CHECK(bench.readData() == Bytes{ 0x55, 0x55, 0x55, 0x55 });
}

TEST_CASE("WCH-Link: a failed word ends the target reads, and the rest is 0xff") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();
   connect(bench);
   putWord(bench.dm, 0x2000'0000, 0x0102'0304);
   bench.dm.faultAddresses.insert(0x2000'0004);
   bench.dm.commands.clear();
   bench.transact(request(0x03, { 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10 }));
   bench.transact(request(0x02, { 0x0c }));
   const auto packet = bench.readData();
   REQUIRE(packet.has_value());
   CHECK(*packet == Bytes{ 0x01, 0x02, 0x03, 0x04, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff });
   // Only the first two words were read.
   CHECK(bench.dm.commands.size() == 2);
}

TEST_CASE("WCH-Link: a quit ends a memory read that the host did not take") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();
   connect(bench);
   bench.transact(request(0x03, { 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00 }));
   bench.transact(request(0x02, { 0x0c }));
   CHECK(bench.processor.hasData());
   // Commands still run while the data waits.
   CHECK(bench.transact(request(0x0d, { 0x01 })).size() == 7);
   CHECK(bench.transact(request(0x0d, { 0xff })) == Bytes{ 0x82, 0x0d, 0x01, 0xff });
   CHECK_FALSE(bench.processor.hasData());
   // The packet on its way is the last one.
   CHECK(bench.readData().has_value());
   CHECK_FALSE(bench.readData().has_value());
}

TEST_CASE("WCH-Link: reset pulses ndmreset and lets the hart run") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();
   connect(bench);
   // v003-wlink-reset.pcapng
   CHECK(bench.transact(request(0x0b, { 0x01 })) == Bytes{ 0x82, 0x0b, 0x01, 0x01 });
   const std::vector<Access> expected{
      W(dm::dmcontrol, 0x0000'0003),
      W(dm::dmcontrol, 0x0000'0001),
      W(dm::dmcontrol, 0x1000'0001),
   };
   CHECK(take(bench) == expected);
   CHECK(bench.platform.delays == std::vector<uint32_t>{ wchlink::Processor::resetPulseUs });
   // Another sub-command is not known.
   CHECK(bench.transact(request(0x0b, { 0x02 })) == unknownReply);
   CHECK(bench.pins.accesses.empty());
}

TEST_CASE("WCH-Link: quit resumes the hart, ends dmactive and releases the pins") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();
   connect(bench);
   REQUIRE(bench.dm.halted);
   CHECK(bench.transact(request(0x0d, { 0xff })) == Bytes{ 0x82, 0x0d, 0x01, 0xff });
   // The quit of the probe (v208-wlink-status-timing.csv)
   const std::vector<Access> expected{
      W(dm::dmcontrol, 0x4000'0001),
      R(dm::dmstatus, 0x0003'0c82),
      W(dm::dmcontrol, 0x4000'0000),
   };
   CHECK(take(bench) == expected);
   CHECK_FALSE(bench.dm.halted);
   CHECK_FALSE(bench.dm.active());
   CHECK_FALSE(bench.processor.connected());
   CHECK(bench.pins.finis == 1);
   CHECK_FALSE(bench.pins.hostDrives());

   // A DMI access now fails without an access.
   CHECK(bench.transact(request(0x08, { 0x11, 0, 0, 0, 0, 0x01 })).back() == 0x02);
   CHECK(bench.pins.accesses.empty());
}

TEST_CASE("WCH-Link: disable debug acts as quit (not captured)") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();
   connect(bench);
   CHECK(bench.transact(request(0x0e, { 0x01 })) == Bytes{ 0x82, 0x0e, 0x01, 0x01 });
   CHECK_FALSE(bench.dm.halted);
   CHECK_FALSE(bench.processor.connected());
   CHECK(bench.pins.finis == 1);
}

TEST_CASE("WCH-Link: disable debug before a connect only echoes") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();
   CHECK(bench.transact(request(0x0e, { 0x01 })) == Bytes{ 0x82, 0x0e, 0x01, 0x01 });
   CHECK(bench.pins.inits == 0);
   CHECK(bench.pins.finis == 0);
   CHECK(bench.pins.accesses.empty());
}

TEST_CASE("WCH-Link: one request at a time") {
   WchLinkBench bench;
   bench.enumerate();
   REQUIRE(bench.write(request(0x0d, { 0x01 })));
   // The response is not read yet: the OUT endpoint NAKs.
   CHECK_FALSE(bench.send(request(0x0d, { 0x03 })));
   CHECK(bench.read() == Bytes{ 0x82, 0x0d, 0x04, 0x02, 0x13, 0x12, 0x00 });
   bench.mainLoop();
   CHECK(bench.transact(request(0x0d, { 0x03 })) == Bytes{ 0x82, 0x0d, 0x01, 0x03 });
   // A ZLP is ignored.
   CHECK(bench.send({}));
   bench.mainLoop();
   CHECK_FALSE(bench.read().has_value());
   CHECK(bench.function.commandCount() == 2);
}

TEST_CASE("WCH-Link: a bus reset releases the target and drops the state") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();
   connect(bench);
   bench.transact(request(0x03, { 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00 }));
   bench.transact(request(0x02, { 0x0c }));
   REQUIRE(bench.write(request(0x0d, { 0x01 })));
   bench.enumerate();
   CHECK_FALSE(bench.processor.connected());
   CHECK_FALSE(bench.processor.hasData());
   CHECK_FALSE(bench.processor.hasResponse());
   CHECK(bench.pins.finis == 1);
   CHECK_FALSE(bench.read().has_value());
   // The probe works after the reset.
   CHECK(bench.transact(request(0x0d, { 0x03 })) == Bytes{ 0x82, 0x0d, 0x01, 0x03 });
}

TEST_CASE("WCH-Link: a suspend releases the target, and requests wait for the resume") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();
   connect(bench);
   bench.controller().busSuspend();
   bench.mainLoop();
   CHECK_FALSE(bench.processor.connected());
   CHECK(bench.pins.finis == 1);

   REQUIRE(bench.write(request(0x0d, { 0x01 })));
   CHECK_FALSE(bench.read().has_value());
   bench.controller().busResume();
   bench.mainLoop();
   CHECK(bench.read() == Bytes{ 0x82, 0x0d, 0x04, 0x02, 0x13, 0x12, 0x00 });
}

TEST_CASE("WCH-Link: a cleared halt of the command IN endpoint sends the response again") {
   WchLinkBench bench;
   bench.enumerate();
   auto &controller = bench.controller();
   REQUIRE(bench.write(request(0x0d, { 0x01 })));
   // The host halts and clears the IN endpoint before it reads the response.
   bool ok = false;
   controller.controlOut(art::usb::test::setupPacket(0x02, 3, 0, 0x81, 0), {}, &ok);
   REQUIRE(ok);
   CHECK(controller.isStalled(0x81));
   controller.controlOut(art::usb::test::setupPacket(0x02, 1, 0, 0x81, 0), {}, &ok);
   REQUIRE(ok);
   bench.mainLoop();
   CHECK(bench.read() == Bytes{ 0x82, 0x0d, 0x04, 0x02, 0x13, 0x12, 0x00 });
   bench.mainLoop();
   CHECK(bench.transact(request(0x0d, { 0x03 })) == Bytes{ 0x82, 0x0d, 0x01, 0x03 });
}

TEST_CASE("WCH-Link: SET_INTERFACE sends the packets on their way again") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();
   connect(bench);
   putWord(bench.dm, 0x2000'0000, 0x0102'0304);
   bench.transact(request(0x03, { 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 }));
   REQUIRE(bench.write(request(0x02, { 0x0c })));
   // A response on 0x81 and a data packet on 0x82 wait for the host.
   bool ok = false;
   bench.controller().controlOut(art::usb::test::setupPacket(0x01, 11, 0, 0, 0), {}, &ok);
   REQUIRE(ok);
   bench.mainLoop();
   CHECK(bench.read() == Bytes{ 0x82, 0x02, 0x01, 0x0c });
   bench.mainLoop();
   CHECK(bench.readData() == Bytes{ 0x01, 0x02, 0x03, 0x04 });
   CHECK_FALSE(bench.readData().has_value());
   // The command OUT endpoint is armed again.
   CHECK(bench.transact(request(0x0d, { 0x03 })) == Bytes{ 0x82, 0x0d, 0x01, 0x03 });
}

TEST_CASE("WCH-Link: a cleared halt of the data IN endpoint sends the data packet again") {
   WchLinkBench bench(Wire::Two);
   bench.enumerate();
   connect(bench);
   putWord(bench.dm, 0x2000'0000, 0x0102'0304);
   bench.transact(request(0x03, { 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 }));
   bench.transact(request(0x02, { 0x0c }));
   auto &controller = bench.controller();
   bool ok = false;
   controller.controlOut(art::usb::test::setupPacket(0x02, 3, 0, 0x82, 0), {}, &ok);
   REQUIRE(ok);
   CHECK(controller.isStalled(0x82));
   CHECK_FALSE(bench.readData().has_value());
   controller.controlOut(art::usb::test::setupPacket(0x02, 1, 0, 0x82, 0), {}, &ok);
   REQUIRE(ok);
   bench.mainLoop();
   CHECK(bench.readData() == Bytes{ 0x01, 0x02, 0x03, 0x04 });
   CHECK_FALSE(bench.readData().has_value());
}
