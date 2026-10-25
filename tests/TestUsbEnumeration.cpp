// The descriptors and the Versaloon function on the libArT USB device stack,
// with the fake controller of libArT as the host.

#include "boot/BootloaderEntry.h"
#include "protocol/versaloon/Versaloon.h"
#include "usb/CmsisDapDescriptors.h"
#include "usb/UsbDescriptors.h"
#include "usb/VersaloonFunction.h"

#include "VersaloonBench.h"

#include <FakeDeviceController.h>
#include <doctest/doctest.h>
#include <usb/Device.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using art::usb::DescriptorType;
using art::usb::EndpointType;
using vsltest::Bench;
using vsltest::versaloonIn;
using vsltest::versaloonOut;

// Records the USB_TO_XXX commands and replies with replySize bytes that
// count up.
class FakeUsbToXxx final : public protocol::UsbToXxxHandler {
   public:
   std::optional<std::size_t> process(art::span<const uint8_t> command, art::span<uint8_t> reply) override {
      commands.emplace_back(command.begin(), command.end());
      // A handler error can return more than the buffer: fill only the buffer.
      for(std::size_t i = 0; i < std::min(replySize, reply.size()); ++i) {
         reply[i] = static_cast<uint8_t>(i);
      }
      return replySize;
   }

   std::vector<std::vector<uint8_t>> commands;
   std::size_t replySize{ 0 };
};

// A USB_TO_XXX command of total size length; the bytes after the header
// count up.
std::vector<uint8_t> usbToXxx(std::size_t length, uint16_t lengthField) {
   std::vector<uint8_t> command(length);
   command[0] = protocol::usbToXxxLast;
   command[1] = static_cast<uint8_t>(lengthField);
   command[2] = static_cast<uint8_t>(lengthField >> 8);
   for(std::size_t i = 3; i < length; ++i) {
      command[i] = static_cast<uint8_t>(i);
   }
   return command;
}

std::vector<uint8_t> usbToXxx(std::size_t length) {
   return usbToXxx(length, static_cast<uint16_t>(length));
}

std::vector<uint8_t> bytes(const auto &descriptor) {
   return { descriptor.begin(), descriptor.end() };
}

} // namespace

TEST_CASE("The host reads the descriptors of the composite device") {
   Bench bench;
   bench.controller().busReset();

   CHECK(bench.descriptor(DescriptorType::Device, 0, 18).data == bytes(usb::deviceDescriptor));

   // The host reads the 9-byte header first, then wTotalLength bytes.
   const auto header = bench.descriptor(DescriptorType::Configuration, 0, 9).data;
   REQUIRE(header.size() == 9);
   const uint16_t totalLength = static_cast<uint16_t>(header[2] | header[3] << 8);
   CHECK(bench.descriptor(DescriptorType::Configuration, 0, totalLength).data == bytes(usb::configurationDescriptor));

   CHECK(bench.descriptor(DescriptorType::String, usb::stringProduct, 255).data == bytes(usb::productDescriptor));
   const auto serial = bench.descriptor(DescriptorType::String, usb::stringSerialNumber, 255).data;
   REQUIRE(serial.size() == 50);
   CHECK(serial[2] == '1');
   CHECK(serial[4] == '2');
   CHECK(serial[48] == '4');
}

TEST_CASE("The Versaloon personality has no BOS and no MS OS 2.0 vendor request") {
   // In a dual image, the BOS and the MS OS 2.0 descriptor set exist only
   // in the CMSIS-DAP personality (usb/UsbStack.cpp).
   CHECK((usb::deviceDescriptor[2] | usb::deviceDescriptor[3] << 8) == 0x0200);

   Bench bench;
   auto &controller = bench.controller();
   bench.enumerate();
   const auto bosValue = static_cast<uint16_t>(static_cast<uint8_t>(DescriptorType::Bos) << 8);
   CHECK(controller.controlIn(art::usb::test::setupPacket(0x80, 6, bosValue, 0, 5)).stalled);
   CHECK(controller.controlIn(art::usb::test::setupPacket(0xC0, usb::dap::msOs20VendorCode, 0, 7, 170)).stalled);
}

TEST_CASE("SET_CONFIGURATION opens the endpoints of all three interfaces") {
   Bench bench;
   bench.enumerate();
   auto &controller = bench.controller();

   struct Expected {
      uint8_t address;
      EndpointType type;
      uint16_t maxPacketSize;
   };
   for(const Expected &expected : { Expected{ 0x03, EndpointType::Bulk, 64 }, Expected{ 0x82, EndpointType::Bulk, 64 },
                                    Expected{ 0x81, EndpointType::Interrupt, 16 },
                                    Expected{ 0x04, EndpointType::Bulk, 64 }, Expected{ 0x84, EndpointType::Bulk, 64 } }) {
      CAPTURE(int(expected.address));
      const auto endpoint = controller.endpoint(expected.address);
      REQUIRE(endpoint.has_value());
      CHECK(endpoint->type == expected.type);
      CHECK(endpoint->maxPacketSize == expected.maxPacketSize);
   }
   CHECK(bench.versaloon.isConfigured());
   CHECK(controller.isArmed(0x03));
}

TEST_CASE("GET_INFO over USB") {
   Bench bench;
   bench.enumerate();
   auto &controller = bench.controller();

   REQUIRE(bench.write({ protocol::getInfo }));
   // The OUT endpoint NAKs while the reply is on its way.
   CHECK_FALSE(controller.isArmed(versaloonOut));
   const auto reply = bench.read();
   REQUIRE(reply.has_value());
   REQUIRE(reply->size() > 3);
   CHECK((*reply)[0] == 0x00);
   CHECK((*reply)[1] == 0x10);
   CHECK(std::string(reply->begin() + 2, reply->end()) == std::string("versaloon-ng v1.2.3, board mini") + '\0');
   CHECK(controller.isArmed(versaloonOut));
}

TEST_CASE("GET_TVCC and GET_HARDWARE over USB") {
   Bench bench;
   bench.enumerate();

   REQUIRE(bench.write({ protocol::getTvcc }));
   CHECK(bench.read() == std::vector<uint8_t>{ 0x00, 0x00 });
   REQUIRE(bench.write({ protocol::getHardware }));
   CHECK(bench.read() == std::vector<uint8_t>{ 0x15 });
}

TEST_CASE("A command without a reply keeps the OUT endpoint armed") {
   Bench bench;
   bench.enumerate();
   auto &controller = bench.controller();

   REQUIRE(bench.write({ 0x03 }));
   CHECK_FALSE(bench.read().has_value());
   CHECK(controller.isArmed(versaloonOut));
   // A ZLP between commands is ignored.
   REQUIRE(controller.sendOut(versaloonOut, art::span<const uint8_t>()));
   CHECK_FALSE(bench.read().has_value());
   CHECK(controller.isArmed(versaloonOut));
}

TEST_CASE("A multi-packet command gets a multi-packet reply that is longer than the command") {
   FakeUsbToXxx handler;
   Bench bench(nullptr, &handler);
   bench.enumerate();
   auto &controller = bench.controller();

   const auto command = usbToXxx(150);
   handler.replySize = 1000;
   REQUIRE(bench.write(command));
   REQUIRE(handler.commands.size() == 1);
   CHECK(handler.commands[0] == command);

   std::vector<std::size_t> packetSizes;
   const auto reply = bench.read(&packetSizes);
   REQUIRE(reply.has_value());
   REQUIRE(reply->size() == 1000);
   for(std::size_t i = 0; i < reply->size(); ++i) {
      REQUIRE((*reply)[i] == static_cast<uint8_t>(i));
   }
   CHECK(packetSizes.size() == 16);
   CHECK(packetSizes.back() == 1000 % 64);
   CHECK(controller.isArmed(versaloonOut));
}

TEST_CASE("A reply that is a multiple of the packet size ends with a ZLP") {
   FakeUsbToXxx handler;
   Bench bench(nullptr, &handler);
   bench.enumerate();

   for(const std::size_t size : { std::size_t{ 64 }, std::size_t{ 128 }, protocol::bufferSize - 64 }) {
      CAPTURE(size);
      handler.replySize = size;
      REQUIRE(bench.write(usbToXxx(3)));
      std::vector<std::size_t> packetSizes;
      const auto reply = bench.read(&packetSizes);
      REQUIRE(reply.has_value());
      CHECK(reply->size() == size);
      CHECK(packetSizes.size() == size / 64 + 1);
      CHECK(packetSizes.back() == 0);
   }
}

TEST_CASE("A reply of the full buffer size ends without a ZLP") {
   FakeUsbToXxx handler;
   Bench bench(nullptr, &handler);
   bench.enumerate();
   auto &controller = bench.controller();

   handler.replySize = protocol::bufferSize;
   REQUIRE(bench.write(usbToXxx(3)));
   std::vector<std::size_t> packetSizes;
   const auto reply = bench.read(&packetSizes);
   REQUIRE(reply.has_value());
   CHECK(reply->size() == protocol::bufferSize);
   CHECK(packetSizes.size() == protocol::bufferSize / 64);
   CHECK_FALSE(controller.takeIn(versaloonIn).has_value());
   CHECK(controller.isArmed(versaloonOut));
}

TEST_CASE("A command of the full buffer size") {
   FakeUsbToXxx handler;
   Bench bench(nullptr, &handler);
   bench.enumerate();

   const auto command = usbToXxx(protocol::bufferSize);
   handler.replySize = 1;
   REQUIRE(bench.write(command));
   REQUIRE(handler.commands.size() == 1);
   CHECK(handler.commands[0] == command);
   CHECK(bench.read() == std::vector<uint8_t>{ 0x00 });
}

TEST_CASE("A length error gets an empty reply, and the next command works") {
   FakeUsbToXxx handler;
   Bench bench(nullptr, &handler);
   bench.enumerate();

   SUBCASE("The length is smaller than the header") {
      REQUIRE(bench.write(usbToXxx(3, 2)));
   }
   SUBCASE("The transfer is shorter than the length") {
      REQUIRE(bench.write(usbToXxx(100, 120)));
   }
   SUBCASE("The transfer is longer than the length") {
      REQUIRE(bench.write(usbToXxx(100, 80)));
   }
   SUBCASE("The error is on a packet that is not the last") {
      // The second of four packets goes past the length.
      REQUIRE(bench.write(usbToXxx(200, 80)));
   }
   SUBCASE("A common command in a transfer of many packets") {
      std::vector<uint8_t> transfer(150, 0x00);
      transfer[0] = protocol::getHardware;
      REQUIRE(bench.write(transfer));
   }
   CHECK(bench.read() == std::vector<uint8_t>{});
   CHECK_FALSE(bench.read().has_value());
   CHECK(handler.commands.empty());

   REQUIRE(bench.write({ protocol::getHardware }));
   CHECK(bench.read() == std::vector<uint8_t>{ 0x15 });
}

TEST_CASE("A packet after a length error does not start a command") {
   boot::BootloaderEntry bootloader;
   Bench bench(&bootloader);
   bench.enumerate();

   // The third packet starts with the FW_UPDATE command and its key.
   auto transfer = usbToXxx(140, 80);
   transfer[128] = protocol::fwUpdate;
   transfer[129] = protocol::fwUpdateKey;
   REQUIRE(bench.write(transfer));
   CHECK(bench.read() == std::vector<uint8_t>{});
   CHECK_FALSE(bench.read().has_value());
   CHECK_FALSE(bootloader.isRequested());
}

TEST_CASE("A handler reply larger than the reply buffer gets an empty reply") {
   FakeUsbToXxx handler;
   Bench bench(nullptr, &handler);
   bench.enumerate();
   auto &controller = bench.controller();

   handler.replySize = protocol::bufferSize + 1;
   REQUIRE(bench.write(usbToXxx(3)));
   CHECK(bench.read() == std::vector<uint8_t>{});
   CHECK(controller.isArmed(versaloonOut));

   handler.replySize = 1;
   REQUIRE(bench.write(usbToXxx(3)));
   CHECK(bench.read() == std::vector<uint8_t>{ 0x00 });
}

TEST_CASE("An overflow is read to its end, gets an empty reply, and the next command works") {
   FakeUsbToXxx handler;
   Bench bench(nullptr, &handler);
   bench.enumerate();

   const auto tooLarge = usbToXxx(protocol::bufferSize + 100);
   REQUIRE(bench.write(tooLarge));
   CHECK(bench.read() == std::vector<uint8_t>{});
   CHECK(handler.commands.empty());

   handler.replySize = 2;
   REQUIRE(bench.write(usbToXxx(5)));
   CHECK(bench.read() == std::vector<uint8_t>{ 0x00, 0x01 });
}

TEST_CASE("A bus reset drops a partial command") {
   FakeUsbToXxx handler;
   Bench bench(nullptr, &handler);
   bench.enumerate();

   const auto command = usbToXxx(200);
   // The first packet only
   REQUIRE(bench.controller().sendOut(versaloonOut, art::span<const uint8_t>(command.data(), 64)));
   bench.enumerate();
   REQUIRE(bench.write({ protocol::getHardware }));
   CHECK(bench.read() == std::vector<uint8_t>{ 0x15 });
   CHECK(handler.commands.empty());
}

TEST_CASE("FW_UPDATE over USB with the key replies and requests the bootloader") {
   boot::BootloaderEntry bootloader;
   Bench bench(&bootloader);
   bench.enumerate();
   auto &controller = bench.controller();

   REQUIRE(bench.write({ protocol::fwUpdate, protocol::fwUpdateKey }));
   CHECK(bootloader.isRequested());
   CHECK(bench.read() == std::vector<uint8_t>{ protocol::fwUpdateAccepted });
   CHECK(controller.isArmed(versaloonOut));
}

TEST_CASE("FW_UPDATE requests the bootloader also when the host does not read the reply") {
   boot::BootloaderEntry bootloader;
   Bench bench(&bootloader);
   bench.enumerate();

   REQUIRE(bench.write({ protocol::fwUpdate, protocol::fwUpdateKey }));
   CHECK(bootloader.isRequested());
   CHECK_FALSE(bootloader.isDue(0));
   CHECK(bootloader.isDue(boot::BootloaderEntry::replyDelayMs));
}

TEST_CASE("FW_UPDATE over USB is refused without a bootloader") {
   Bench bench;
   bench.enumerate();

   REQUIRE(bench.write({ protocol::fwUpdate, protocol::fwUpdateKey }));
   CHECK(bench.read() == std::vector<uint8_t>{ protocol::fwUpdateRefused });
}
