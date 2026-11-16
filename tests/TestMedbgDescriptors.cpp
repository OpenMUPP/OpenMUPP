// SPDX-License-Identifier: GPL-3.0-or-later
// The descriptors of the medbg build (MUPP_AVR_USB_ID=medbg): the ID of the
// Xplained Mini, and the HID interface at number 0 (Bloom opens it by the
// number).

#include "usb/CmsisDapDescriptors.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

constexpr uint8_t interfaceDescriptorType = 4;
constexpr uint8_t hidClass = 0x03;

struct Interface {
   uint8_t number;
   uint8_t classCode;
};

// The interface descriptors in the order of the configuration, with the
// alternate setting 0 only
std::vector<Interface> interfacesOf(const auto &configuration) {
   std::vector<Interface> result;
   for(std::size_t offset = 0; offset < configuration.size(); offset += configuration[offset]) {
      REQUIRE(configuration[offset] >= 2);
      if(configuration[offset + 1] == interfaceDescriptorType && configuration[offset + 3] == 0) {
         result.push_back({ configuration[offset + 2], configuration[offset + 5] });
      }
   }
   return result;
}

} // namespace

TEST_CASE("medbg: the device has the ID of the Xplained Mini") {
   const auto &device = usb::dap::deviceDescriptor;
   CHECK((device[8] | device[9] << 8) == 0x03EB);
   CHECK((device[10] | device[11] << 8) == 0x2145);
}

TEST_CASE("medbg: the HID interface is interface 0, the others follow") {
   for(const auto &configuration : { usb::desc::asSpan(usb::dap::configurationDescriptor),
                                     usb::desc::asSpan(usb::dap::highSpeedConfigurationDescriptor) }) {
      CHECK(configuration[4] == 4); // bNumInterfaces
      const auto interfaces = interfacesOf(configuration);
      REQUIRE(interfaces.size() == 4);
      CHECK(interfaces[0].number == 0);
      CHECK(interfaces[0].classCode == hidClass);
      // v2 bulk (vendor class), CDC control, CDC data
      CHECK(interfaces[1].number == 1);
      CHECK(interfaces[1].classCode == 0xFF);
      CHECK(interfaces[2].number == 2);
      CHECK(interfaces[3].number == 3);
      CHECK(usb::dap::hidInterface == 0);
      CHECK(usb::dap::dapInterface == 1);
      CHECK(usb::dap::cdcControlInterface == 2);
      CHECK(usb::dap::cdcDataInterface == 3);
   }
}

TEST_CASE("medbg: WinUSB binds the v2 interface at its new number") {
   const auto &set = usb::dap::msOs20DescriptorSet;
   // Set header 10 bytes, then the function subset header: its first interface
   CHECK(set[10 + 4] == usb::dap::dapInterface);
}
