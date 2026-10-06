// The descriptors and the Versaloon function on the libArT USB device stack,
// with the fake controller of libArT as the host.

#include "boot/BootloaderEntry.h"
#include "usb/UsbDescriptors.h"
#include "usb/VersaloonFunction.h"

#include <FakeDeviceController.h>
#include <doctest/doctest.h>
#include <usb/Device.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

using art::usb::DescriptorType;
using art::usb::EndpointType;
using art::usb::test::FakeDeviceController;
using art::usb::test::setupPacket;

constexpr uint8_t standardDeviceIn = 0x80;
constexpr uint8_t standardDeviceOut = 0x00;
constexpr uint8_t getDescriptor = 6;
constexpr uint8_t setAddress = 5;
constexpr uint8_t setConfiguration = 9;

std::vector<uint8_t> bytes(const auto &descriptor) {
   return { descriptor.begin(), descriptor.end() };
}

// The device with the descriptors of the firmware. The CDC-ACM function is a
// placeholder that only owns interfaces 1-2: the descriptors open its
// endpoints.
class Bench {
   public:
   // bootloader: the bootloader entry of FW_UPDATE, nullptr for none.
   explicit Bench(boot::BootloaderEntry *bootloader = nullptr)
      : versaloon(bootloader)
      , device_(controller_, descriptors_)
      , cdc_(usb::cdcControlInterface, 2) {
      const std::array<uint8_t, usb::uidBytes> uid{ 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,
                                                    0xDE, 0xF0, 0x01, 0x02, 0x03, 0x04 };
      serial_.set(uid);
      REQUIRE(device_.addFunction(versaloon));
      REQUIRE(device_.addFunction(cdc_));
      device_.start();
   }

   FakeDeviceController &controller() {
      return controller_;
   }

   art::usb::Device &device() {
      return device_;
   }

   FakeDeviceController::ControlResult descriptor(DescriptorType type, uint8_t index, uint16_t length) {
      bool ok = false;
      auto result = controller_.controlIn(setupPacket(standardDeviceIn, getDescriptor,
                                                      static_cast<uint16_t>(static_cast<uint8_t>(type) << 8 | index), 0, length),
                                          &ok);
      CHECK(ok);
      return result;
   }

   void request(uint8_t request, uint16_t value) {
      bool ok = false;
      const auto result = controller_.controlOut(setupPacket(standardDeviceOut, request, value, 0, 0), {}, &ok);
      CHECK(ok);
      CHECK_FALSE(result.stalled);
   }

   // Bus reset, SET_ADDRESS 5 and SET_CONFIGURATION 1.
   void enumerate() {
      controller_.busReset();
      request(setAddress, 5);
      request(setConfiguration, 1);
      REQUIRE(device_.isConfigured());
   }

   usb::VersaloonFunction versaloon;

   private:
   FakeDeviceController controller_;
   usb::SerialNumber serial_;
   const std::array<art::span<const uint8_t>, usb::stringCount> strings_{
      usb::desc::asSpan(usb::languageDescriptor),   usb::desc::asSpan(usb::manufacturerDescriptor),
      usb::desc::asSpan(usb::productDescriptor),    serial_.bytes(),
      usb::desc::asSpan(usb::serialPortDescriptor),
   };
   const art::usb::Descriptors descriptors_{
      .device = usb::desc::asSpan(usb::deviceDescriptor),
      .configuration = usb::desc::asSpan(usb::configurationDescriptor),
      .strings = art::span<const art::span<const uint8_t>>(strings_.data(), strings_.size()),
   };
   art::usb::Device device_;
   art::usb::Function cdc_;
};

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

TEST_CASE("The Versaloon function echoes each OUT packet") {
   Bench bench;
   bench.enumerate();
   auto &controller = bench.controller();

   const std::vector<uint8_t> command{ 0x00, 0x01, 0x02, 0x03 };
   REQUIRE(controller.sendOut(0x03, art::span<const uint8_t>(command.data(), command.size())));
   // The OUT endpoint NAKs while the echo is on its way.
   CHECK_FALSE(controller.isArmed(0x03));
   CHECK(controller.takeIn(0x82) == command);
   CHECK(controller.isArmed(0x03));
}

TEST_CASE("The Versaloon function echoes a full packet without a ZLP, and a ZLP as a ZLP") {
   Bench bench;
   bench.enumerate();
   auto &controller = bench.controller();

   std::vector<uint8_t> full(64);
   for(std::size_t i = 0; i < full.size(); ++i) {
      full[i] = static_cast<uint8_t>(i);
   }
   REQUIRE(controller.sendOut(0x03, art::span<const uint8_t>(full.data(), full.size())));
   CHECK(controller.takeIn(0x82) == full);
   CHECK_FALSE(controller.takeIn(0x82).has_value());

   REQUIRE(controller.sendOut(0x03, art::span<const uint8_t>()));
   CHECK(controller.takeIn(0x82) == std::vector<uint8_t>{});
   CHECK(controller.isArmed(0x03));
}

TEST_CASE("FW_UPDATE with the key replies and requests the bootloader") {
   boot::BootloaderEntry bootloader;
   Bench bench(&bootloader);
   bench.enumerate();
   auto &controller = bench.controller();

   const std::vector<uint8_t> command{ usb::versaloonFwUpdate, usb::versaloonFwUpdateKey };
   REQUIRE(controller.sendOut(0x03, art::span<const uint8_t>(command.data(), command.size())));
   CHECK(bootloader.isRequested());
   CHECK(controller.takeIn(0x82) == std::vector<uint8_t>{ usb::fwUpdateAccepted });
   CHECK(controller.isArmed(0x03));
}

TEST_CASE("FW_UPDATE requests the bootloader also when the host does not read the reply") {
   boot::BootloaderEntry bootloader;
   Bench bench(&bootloader);
   bench.enumerate();
   auto &controller = bench.controller();

   const std::vector<uint8_t> command{ usb::versaloonFwUpdate, usb::versaloonFwUpdateKey };
   REQUIRE(controller.sendOut(0x03, art::span<const uint8_t>(command.data(), command.size())));
   CHECK(bootloader.isRequested());
   CHECK_FALSE(bootloader.isDue(0));
   CHECK(bootloader.isDue(boot::BootloaderEntry::replyDelayMs));
}

TEST_CASE("FW_UPDATE without the key is refused") {
   boot::BootloaderEntry bootloader;
   Bench bench(&bootloader);
   bench.enumerate();
   auto &controller = bench.controller();

   const std::vector<uint8_t> wrongKey{ usb::versaloonFwUpdate, 0x55 };
   REQUIRE(controller.sendOut(0x03, art::span<const uint8_t>(wrongKey.data(), wrongKey.size())));
   CHECK(controller.takeIn(0x82) == std::vector<uint8_t>{ usb::fwUpdateRefused });

   const std::vector<uint8_t> noKey{ usb::versaloonFwUpdate };
   REQUIRE(controller.sendOut(0x03, art::span<const uint8_t>(noKey.data(), noKey.size())));
   CHECK(controller.takeIn(0x82) == std::vector<uint8_t>{ usb::fwUpdateRefused });

   CHECK_FALSE(bootloader.isRequested());
}

TEST_CASE("FW_UPDATE is refused without a bootloader") {
   Bench bench;
   bench.enumerate();
   auto &controller = bench.controller();

   const std::vector<uint8_t> command{ usb::versaloonFwUpdate, usb::versaloonFwUpdateKey };
   REQUIRE(controller.sendOut(0x03, art::span<const uint8_t>(command.data(), command.size())));
   CHECK(controller.takeIn(0x82) == std::vector<uint8_t>{ usb::fwUpdateRefused });
   CHECK(controller.isArmed(0x03));
}
