#include "usb/UsbDescriptors.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {

using art::usb::DescriptorType;

constexpr uint8_t descriptorType(DescriptorType type) {
   return static_cast<uint8_t>(type);
}

uint16_t word(const uint8_t *bytes) {
   return static_cast<uint16_t>(bytes[0] | bytes[1] << 8);
}

// One descriptor of the configuration set: its offset and its bytes.
struct Entry {
   std::size_t offset;
   std::vector<uint8_t> bytes;
};

// Splits the configuration set into its descriptors.
std::vector<Entry> split(const auto &set) {
   std::vector<Entry> entries;
   std::size_t offset = 0;
   while(offset < set.size()) {
      const uint8_t length = set[offset];
      REQUIRE(length >= 2);
      REQUIRE(offset + length <= set.size());
      entries.push_back({ offset, std::vector<uint8_t>(set.begin() + offset, set.begin() + offset + length) });
      offset += length;
   }
   CHECK(offset == set.size());
   return entries;
}

std::vector<Entry> ofType(const std::vector<Entry> &entries, DescriptorType type) {
   std::vector<Entry> result;
   for(const auto &entry : entries) {
      if(entry.bytes[1] == descriptorType(type)) {
         result.push_back(entry);
      }
   }
   return result;
}

// The class-specific (CS_INTERFACE) descriptor with the subtype.
Entry cdcFunctional(const std::vector<Entry> &entries, uint8_t subtype) {
   for(const auto &entry : entries) {
      if(entry.bytes[1] == usb::desc::cdc::kCsInterface && entry.bytes[2] == subtype) {
         return entry;
      }
   }
   FAIL("no CDC functional descriptor with subtype ", int(subtype));
   return {};
}

// The text of a string descriptor; the descriptors use only ASCII.
std::string text(const auto &descriptor) {
   REQUIRE(descriptor[0] == descriptor.size());
   REQUIRE(descriptor[1] == descriptorType(DescriptorType::String));
   std::string result;
   for(std::size_t i = 2; i + 1 < descriptor.size(); i += 2) {
      CHECK(descriptor[i + 1] == 0);
      result.push_back(static_cast<char>(descriptor[i]));
   }
   return result;
}

} // namespace

TEST_CASE("The device descriptor has the Versaloon identity and the IAD device class") {
   const auto &d = usb::deviceDescriptor;

   CHECK(d.size() == 18);
   CHECK(d[0] == 18);
   CHECK(d[1] == descriptorType(DescriptorType::Device));
   CHECK(word(&d[2]) == 0x0200);
   CHECK(d[4] == 0xEF);
   CHECK(d[5] == 0x02);
   CHECK(d[6] == 0x01);
   CHECK(d[7] == 64);
   CHECK(word(&d[8]) == 0x0483);
   CHECK(word(&d[10]) == 0xA038);
   CHECK(word(&d[12]) == (usb::toBcd(VERSION_MAJOR) << 8 | usb::toBcd(VERSION_MINOR)));
   CHECK(d[14] == usb::stringManufacturer);
   CHECK(d[15] == usb::stringProduct);
   CHECK(d[16] == usb::stringSerialNumber);
   CHECK(d[17] == 1);
}

TEST_CASE("toBcd gives two BCD digits") {
   CHECK(usb::toBcd(0) == 0x00);
   CHECK(usb::toBcd(7) == 0x07);
   CHECK(usb::toBcd(42) == 0x42);
   CHECK(usb::toBcd(142) == 0x42);
}

TEST_CASE("The configuration descriptor has three interfaces and the right lengths") {
   const auto &c = usb::configurationDescriptor;
   const auto entries = split(c);

   CHECK(c[0] == 9);
   CHECK(c[1] == descriptorType(DescriptorType::Configuration));
   CHECK(word(&c[2]) == c.size());
   CHECK(c[4] == 3);
   CHECK(c[5] == 1);
   // Bus-powered, no remote wakeup, 200 mA
   CHECK(c[7] == 0x80);
   CHECK(c[8] == 100);

   // Configuration, 3 interfaces, IAD, 4 CDC functional, 5 endpoints
   CHECK(entries.size() == 1 + 3 + 1 + 4 + 5);
   CHECK(c.size() == 9 + 3 * 9 + 8 + (5 + 5 + 4 + 5) + 5 * 7);

   for(const auto &entry : ofType(entries, DescriptorType::Interface)) {
      CHECK(entry.bytes.size() == 9);
   }
   for(const auto &entry : ofType(entries, DescriptorType::Endpoint)) {
      CHECK(entry.bytes.size() == 7);
   }
}

TEST_CASE("Interface 0 is the vendor interface with the OpenOCD endpoints") {
   const auto entries = split(usb::configurationDescriptor);
   const auto interfaces = ofType(entries, DescriptorType::Interface);
   REQUIRE(interfaces.size() == 3);

   const auto &vendor = interfaces[0].bytes;
   CHECK(vendor[2] == 0);
   CHECK(vendor[3] == 0);
   CHECK(vendor[4] == 2);
   CHECK(vendor[5] == 0xFF);

   // The two endpoints follow the interface descriptor.
   const auto endpoints = ofType(entries, DescriptorType::Endpoint);
   REQUIRE(endpoints.size() == 5);
   const std::vector<uint8_t> out{ 7, 0x05, 0x03, 0x02, 64, 0, 0 };
   const std::vector<uint8_t> in{ 7, 0x05, 0x82, 0x02, 64, 0, 0 };
   CHECK(endpoints[0].bytes == out);
   CHECK(endpoints[1].bytes == in);
   CHECK(endpoints[0].offset == interfaces[0].offset + 9);
}

TEST_CASE("An IAD groups the CDC-ACM interfaces 1 and 2") {
   const auto entries = split(usb::configurationDescriptor);
   const auto iads = ofType(entries, DescriptorType::InterfaceAssociation);
   const auto interfaces = ofType(entries, DescriptorType::Interface);
   REQUIRE(iads.size() == 1);
   REQUIRE(interfaces.size() == 3);

   const auto &iad = iads[0].bytes;
   CHECK(iad.size() == 8);
   CHECK(iad[2] == 1);
   CHECK(iad[3] == 2);
   CHECK(iad[4] == 0x02);
   CHECK(iad[5] == 0x02);
   CHECK(iad[7] == usb::stringSerialPort);
   // The IAD comes right before the first interface of the function.
   CHECK(iads[0].offset + 8 == interfaces[1].offset);

   const auto &control = interfaces[1].bytes;
   CHECK(control[2] == 1);
   CHECK(control[4] == 1);
   CHECK(control[5] == 0x02);
   CHECK(control[6] == 0x02);

   const auto &data = interfaces[2].bytes;
   CHECK(data[2] == 2);
   CHECK(data[4] == 2);
   CHECK(data[5] == 0x0A);
}

TEST_CASE("The CDC functional descriptors point at data interface 2") {
   const auto entries = split(usb::configurationDescriptor);
   using namespace usb::desc::cdc;

   const auto header = cdcFunctional(entries, kSubtypeHeader).bytes;
   CHECK(word(&header[3]) == 0x0120);

   // The original firmware has data interface 1 here.
   const auto callManagement = cdcFunctional(entries, kSubtypeCallManagement).bytes;
   CHECK(callManagement == std::vector<uint8_t>{ 5, kCsInterface, kSubtypeCallManagement, 0, 2 });

   const auto acm = cdcFunctional(entries, kSubtypeAcm).bytes;
   CHECK(acm == std::vector<uint8_t>{ 4, kCsInterface, kSubtypeAcm, kAcmLineCoding });

   const auto union_ = cdcFunctional(entries, kSubtypeUnion).bytes;
   CHECK(union_ == std::vector<uint8_t>{ 5, kCsInterface, kSubtypeUnion, 1, 2 });
}

TEST_CASE("The CDC endpoints are the notification and the bulk data endpoints") {
   const auto endpoints = ofType(split(usb::configurationDescriptor), DescriptorType::Endpoint);
   REQUIRE(endpoints.size() == 5);

   CHECK(endpoints[2].bytes == std::vector<uint8_t>{ 7, 0x05, 0x81, 0x03, 16, 0, 16 });
   CHECK(endpoints[3].bytes == std::vector<uint8_t>{ 7, 0x05, 0x04, 0x02, 64, 0, 0 });
   CHECK(endpoints[4].bytes == std::vector<uint8_t>{ 7, 0x05, 0x84, 0x02, 64, 0, 0 });
}

TEST_CASE("The endpoints fit the STM32F1 USB peripheral") {
   const auto endpoints = ofType(split(usb::configurationDescriptor), DescriptorType::Endpoint);

   std::set<uint8_t> addresses;
   std::array<int, 8> typeOfNumber{};
   typeOfNumber.fill(-1);
   // Buffer descriptor table (8 endpoints x 8 bytes) and EP0 (64 bytes in
   // each direction)
   unsigned packetMemory = 64 + 2 * 64;
   for(const auto &entry : endpoints) {
      const uint8_t address = entry.bytes[2];
      const uint8_t number = address & 0x0F;
      const uint8_t type = entry.bytes[3];
      const uint16_t maxPacketSize = word(&entry.bytes[4]);

      CHECK(addresses.insert(address).second);
      // Endpoint registers 1-7; both directions of a number have one type.
      CHECK(number >= 1);
      CHECK(number <= 7);
      if(number < typeOfNumber.size()) {
         CHECK((typeOfNumber[number] == -1 || typeOfNumber[number] == type));
         typeOfNumber[number] = type;
      }
      // An even size: the OUT buffers are allocated in 2-byte (up to 62
      // bytes) or 32-byte blocks.
      CHECK((maxPacketSize == 64 || maxPacketSize % 2 == 0));
      packetMemory += maxPacketSize;
   }
   CHECK(packetMemory <= 512);
}

TEST_CASE("The strings name the device and the serial port") {
   const auto &languages = usb::languageDescriptor;
   CHECK(languages.size() == 4);
   CHECK(languages[1] == descriptorType(DescriptorType::String));
   CHECK(word(&languages[2]) == 0x0409);

   CHECK(text(usb::manufacturerDescriptor) == "Versaloon");
   CHECK(text(usb::productDescriptor).find("Versaloon") != std::string::npos);
   CHECK(text(usb::serialPortDescriptor) == "Versaloon serial port");
   CHECK(usb::stringCount == 5);
}

TEST_CASE("The serial number is the chip UID in hexadecimal") {
   usb::SerialNumber serial;
   const std::array<uint8_t, usb::uidBytes> uid{ 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x10, 0x9F, 0xFF };
   serial.set(uid);

   const auto bytes = serial.bytes();
   CHECK(bytes.size() == 2 + 4 * 12);
   CHECK(text(bytes) == "000123456789ABCDEF109FFF");
}
