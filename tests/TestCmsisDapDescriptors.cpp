// The descriptors of the CMSIS-DAP personality.

#include "usb/CmsisDapDescriptors.h"
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

// The packet memory of a configuration in bytes: the buffer descriptor
// table for the highest endpoint number, EP0 (64 bytes each way), and the
// packet buffers of the endpoints. An OUT endpoint of doubleBufferedOut has
// two buffers.
unsigned packetMemory(const auto &configuration, uint8_t doubleBufferedOut) {
   unsigned highest = 0;
   unsigned buffers = 2 * 64;
   for(const auto &entry : ofType(split(configuration), DescriptorType::Endpoint)) {
      const uint8_t address = entry.bytes[2];
      highest = std::max<unsigned>(highest, address & 0x0F);
      buffers += word(&entry.bytes[4]);
      if(address == doubleBufferedOut) {
         buffers += word(&entry.bytes[4]);
      }
   }
   return (highest + 1) * 8 + buffers;
}

} // namespace

TEST_CASE("The CMSIS-DAP device descriptor has the CMSIS-DAP identity and the IAD device class") {
   const auto &d = usb::dap::deviceDescriptor;

   CHECK(d.size() == 18);
   CHECK(d[0] == 18);
   CHECK(d[1] == descriptorType(DescriptorType::Device));
   // USB 2.01: Windows reads the BOS
   CHECK(word(&d[2]) == 0x0201);
   CHECK(d[4] == 0xEF);
   CHECK(d[5] == 0x02);
   CHECK(d[6] == 0x01);
   CHECK(d[7] == 64);
   CHECK(word(&d[8]) == 0x1209);
   CHECK(word(&d[10]) == 0x0001);
   CHECK(word(&d[12]) == (usb::toBcd(VERSION_MAJOR) << 8 | usb::toBcd(VERSION_MINOR)));
   CHECK(d[14] == usb::dap::stringManufacturer);
   CHECK(d[15] == usb::dap::stringProduct);
   CHECK(d[16] == usb::dap::stringSerialNumber);
   CHECK(d[17] == 1);
}

TEST_CASE("The CMSIS-DAP configuration descriptor has three interfaces and the right lengths") {
   const auto &c = usb::dap::configurationDescriptor;
   const auto entries = split(c);

   CHECK(c[0] == 9);
   CHECK(c[1] == descriptorType(DescriptorType::Configuration));
   CHECK(word(&c[2]) == c.size());
   CHECK(c[4] == 3);
   CHECK(c[5] == 1);
   CHECK(c[7] == 0x80);
   CHECK(c[8] == 100);

   // Configuration, 3 interfaces, IAD, 4 CDC functional, 5 endpoints
   CHECK(entries.size() == 1 + 3 + 1 + 4 + 5);
   for(const auto &entry : ofType(entries, DescriptorType::Interface)) {
      CHECK(entry.bytes.size() == 9);
   }
   for(const auto &entry : ofType(entries, DescriptorType::Endpoint)) {
      CHECK(entry.bytes.size() == 7);
   }
}

TEST_CASE("Interface 0 is the CMSIS-DAP v2 vendor interface with two 64-byte bulk endpoints") {
   const auto entries = split(usb::dap::configurationDescriptor);
   const auto interfaces = ofType(entries, DescriptorType::Interface);
   REQUIRE(interfaces.size() == 3);

   const auto &vendor = interfaces[0].bytes;
   CHECK(vendor[2] == 0);
   CHECK(vendor[3] == 0);
   CHECK(vendor[4] == 2);
   CHECK(vendor[5] == 0xFF);
   CHECK(vendor[8] == usb::dap::stringInterface);

   const auto endpoints = ofType(entries, DescriptorType::Endpoint);
   REQUIRE(endpoints.size() == 5);
   const std::vector<uint8_t> out{ 7, 0x05, 0x03, 0x02, 64, 0, 0 };
   const std::vector<uint8_t> in{ 7, 0x05, 0x82, 0x02, 64, 0, 0 };
   CHECK(endpoints[0].bytes == out);
   CHECK(endpoints[1].bytes == in);
   CHECK(endpoints[0].offset == interfaces[0].offset + 9);
}

TEST_CASE("The CDC-ACM part is the same as in the Versaloon personality") {
   // Both configurations start with the configuration header and the
   // vendor interface (9 + 9 + 2 * 7 bytes). The IAD and the CDC
   // interfaces follow, byte for byte.
   constexpr std::size_t vendorPart = 9 + 9 + 2 * 7;
   const auto &dapConfiguration = usb::dap::configurationDescriptor;
   const auto &versaloonConfiguration = usb::configurationDescriptor;
   REQUIRE(dapConfiguration.size() == versaloonConfiguration.size());

   const std::vector<uint8_t> dapRest(dapConfiguration.begin() + vendorPart, dapConfiguration.end());
   const std::vector<uint8_t> versaloonRest(versaloonConfiguration.begin() + vendorPart, versaloonConfiguration.end());
   CHECK(dapRest == versaloonRest);

   const auto iads = ofType(split(dapConfiguration), DescriptorType::InterfaceAssociation);
   REQUIRE(iads.size() == 1);
   CHECK(iads[0].bytes[2] == usb::cdcControlInterface);
   CHECK(iads[0].bytes[3] == 2);
   CHECK(iads[0].bytes[7] == usb::dap::stringSerialPort);
}

TEST_CASE("The endpoint numbers stay as in the Versaloon personality") {
   const auto dap = ofType(split(usb::dap::configurationDescriptor), DescriptorType::Endpoint);
   const auto versaloon = ofType(split(usb::configurationDescriptor), DescriptorType::Endpoint);
   REQUIRE(dap.size() == versaloon.size());
   for(std::size_t i = 0; i < dap.size(); ++i) {
      CHECK(dap[i].bytes == versaloon[i].bytes);
   }
}

TEST_CASE("The CMSIS-DAP endpoints fit the STM32F1 USB peripheral") {
   const auto endpoints = ofType(split(usb::dap::configurationDescriptor), DescriptorType::Endpoint);

   std::set<uint8_t> addresses;
   std::array<int, 8> typeOfNumber{};
   typeOfNumber.fill(-1);
   for(const auto &entry : endpoints) {
      const uint8_t address = entry.bytes[2];
      const uint8_t number = address & 0x0F;
      const uint8_t type = entry.bytes[3];

      CHECK(addresses.insert(address).second);
      CHECK(number >= 1);
      CHECK(number <= 7);
      CHECK((typeOfNumber[number] == -1 || typeOfNumber[number] == type));
      typeOfNumber[number] = type;
      CHECK((word(&entry.bytes[4]) == 64 || word(&entry.bytes[4]) % 2 == 0));
   }
}

TEST_CASE("The packet memory of the CMSIS-DAP personality fits 512 bytes") {
   const auto &configuration = usb::dap::configurationDescriptor;

   // The 64-byte table of the original layout, EP0 and all endpoints
   unsigned original = 64 + 2 * 64;
   for(const auto &entry : ofType(split(configuration), DescriptorType::Endpoint)) {
      original += word(&entry.bytes[4]);
   }
   CHECK(original == 464);
   CHECK(original <= usb::dap::packetMemorySize);

   // The firmware layout: a 40-byte table and a double-buffered DAP OUT
   // endpoint. This is what usb::dap::packetMemoryUsed counts.
   const unsigned firmware = packetMemory(configuration, usb::dap::outEndpoint);
   CHECK(firmware == usb::dap::packetMemoryUsed);
   CHECK(firmware <= usb::dap::packetMemorySize);

   // The same memory as the Versaloon personality
   CHECK(firmware == packetMemory(usb::configurationDescriptor, usb::versaloonOutEndpoint));
   CHECK(usb::dap::packetMemorySize == 512);
}

TEST_CASE("The CMSIS-DAP strings contain CMSIS-DAP in the product and interface strings") {
   const auto &languages = usb::dap::languageDescriptor;
   CHECK(languages.size() == 4);
   CHECK(word(&languages[2]) == 0x0409);

   CHECK(text(usb::dap::manufacturerDescriptor) == "OpenMUPP");
   CHECK(text(usb::dap::productDescriptor) == "OpenMUPP CMSIS-DAP");
   CHECK(text(usb::dap::productDescriptor).find("CMSIS-DAP") != std::string::npos);
   CHECK(text(usb::dap::interfaceStringDescriptor) == "OpenMUPP CMSIS-DAP v2");
   CHECK(text(usb::dap::interfaceStringDescriptor).find("CMSIS-DAP") != std::string::npos);
   CHECK(text(usb::dap::serialPortDescriptor) == "OpenMUPP serial port");
   CHECK(usb::dap::stringCount == 6);
   CHECK(usb::dap::stringInterface == 5);

   // The indexes of the descriptors point at strings that exist.
   const auto &d = usb::dap::deviceDescriptor;
   CHECK(d[14] < usb::dap::stringCount);
   CHECK(d[15] < usb::dap::stringCount);
   CHECK(d[16] < usb::dap::stringCount);
}

TEST_CASE("The BOS has one device capability: the MS OS 2.0 platform capability") {
   const auto &bos = usb::dap::bosDescriptor;
   const auto &set = usb::dap::msOs20DescriptorSet;

   REQUIRE(bos.size() == 5 + 28);
   CHECK(bos[0] == 5);
   CHECK(bos[1] == descriptorType(DescriptorType::Bos));
   CHECK(word(&bos[2]) == bos.size());
   CHECK(bos[4] == 1);

   const uint8_t *capability = &bos[5];
   CHECK(capability[0] == 28);
   CHECK(capability[1] == descriptorType(DescriptorType::DeviceCapability));
   CHECK(capability[2] == 0x05); // PLATFORM
   // {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F}
   const std::vector<uint8_t> uuid{ 0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
                                    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F };
   CHECK(std::vector<uint8_t>(capability + 4, capability + 20) == uuid);
   // Windows 8.1, the size of the set, the vendor code, no alternate enumeration
   CHECK(std::vector<uint8_t>(capability + 20, capability + 24) == std::vector<uint8_t>{ 0x00, 0x00, 0x03, 0x06 });
   CHECK(word(capability + 24) == set.size());
   CHECK(capability[26] == usb::dap::msOs20VendorCode);
   CHECK(capability[27] == 0);
}

TEST_CASE("The MS OS 2.0 descriptor set gives WinUSB to interface 0 only") {
   const auto &set = usb::dap::msOs20DescriptorSet;
   // Set header, function subset header, compatible ID, registry property
   REQUIRE(set.size() == 10 + 8 + 20 + 132);

   // Set header: Windows 8.1, wTotalLength
   CHECK(word(&set[0]) == 10);
   CHECK(word(&set[2]) == 0);
   CHECK(std::vector<uint8_t>(set.begin() + 4, set.begin() + 8) == std::vector<uint8_t>{ 0x00, 0x00, 0x03, 0x06 });
   CHECK(word(&set[8]) == set.size());

   // One function subset, for interface 0: the CDC interfaces keep the
   // driver of Windows.
   const uint8_t *subset = &set[10];
   CHECK(word(subset) == 8);
   CHECK(word(subset + 2) == 2);
   CHECK(subset[4] == usb::dap::dapInterface);
   CHECK(word(subset + 6) == set.size() - 10);

   const uint8_t *compatibleId = subset + 8;
   CHECK(word(compatibleId) == 20);
   CHECK(word(compatibleId + 2) == 3);
   CHECK(std::string(reinterpret_cast<const char *>(compatibleId + 4), 8) == std::string("WINUSB\0\0", 8));
   CHECK(std::vector<uint8_t>(compatibleId + 12, compatibleId + 20) == std::vector<uint8_t>(8, 0));

   const uint8_t *property = compatibleId + 20;
   CHECK(word(property) == 132);
   CHECK(word(property + 2) == 4);
   CHECK(word(property + 4) == 7); // REG_MULTI_SZ
   CHECK(word(property + 6) == 42);
   std::string name;
   for(int i = 0; i < 20; ++i) {
      CHECK(property[9 + 2 * i] == 0);
      name.push_back(static_cast<char>(property[8 + 2 * i]));
   }
   CHECK(name == "DeviceInterfaceGUIDs");
   CHECK(word(property + 50) == 80);
   std::string guid;
   for(int i = 0; i < 38; ++i) {
      CHECK(property[53 + 2 * i] == 0);
      guid.push_back(static_cast<char>(property[52 + 2 * i]));
   }
   CHECK(guid == "{CDB3B5AD-293B-4663-AA36-1AAE46463776}");
   // The null of the GUID and the null that ends the list
   CHECK(std::vector<uint8_t>(property + 128, property + 132) == std::vector<uint8_t>(4, 0));
}
