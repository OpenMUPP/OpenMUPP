// The personality switch request on EP0 (usb/SwitchRequest.h) on the
// libArT USB device stack, with the fake controller of libArT as the host.
// The tests in the other personalities are in TestCmsisDapEnumeration.cpp
// and TestWchLink.cpp.

#include "personality/Personality.h"
#include "personality/PersonalitySwitch.h"
#include "protocol/Versaloon.h"
#include "usb/CmsisDapDescriptors.h"
#include "usb/SwitchRequest.h"

#include "SwitchRequestHost.h"
#include "VersaloonBench.h"

#include <FakeDeviceController.h>
#include <doctest/doctest.h>
#include <usb/Device.h>
#include <usb/MsOs20.h>

#include <array>
#include <cstdint>

namespace {

using art::usb::test::setupPacket;
using personality::Personality;
using personality::PersonalitySwitch;
using vsltest::switchesTo;
using vsltest::switchRequest;

} // namespace

TEST_CASE("The switch request has the code of SWITCH_PERSONALITY and not the MS OS 2.0 vendor code") {
   CHECK(usb::switchRequestCode == protocol::switchPersonality);
   CHECK(usb::switchRequestCode != usb::dap::msOs20VendorCode);
}

TEST_CASE("The switch request in the Versaloon personality") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };
   vsltest::Bench bench(nullptr, nullptr, &personalitySwitch);
   bench.enumerate();

   CHECK_FALSE(switchRequest(bench.controller(), personality::toCode(Personality::WchLink)).stalled);
   CHECK(switchesTo(personalitySwitch, Personality::WchLink));
}

TEST_CASE("The switch request to the active personality cancels a pending switch") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };
   vsltest::Bench bench(nullptr, nullptr, &personalitySwitch);
   bench.enumerate();

   CHECK_FALSE(switchRequest(bench.controller(), personality::toCode(Personality::CmsisDap)).stalled);
   CHECK_FALSE(switchRequest(bench.controller(), personality::toCode(Personality::Versaloon)).stalled);
   CHECK_FALSE(personalitySwitch.requested().has_value());
   CHECK_FALSE(personalitySwitch.isDue(PersonalitySwitch::replyDelayMs));
}

TEST_CASE("A bad switch request stalls and does not switch") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };
   vsltest::Bench bench(nullptr, nullptr, &personalitySwitch);
   bench.enumerate();
   auto &controller = bench.controller();

   SUBCASE("A code that is not a personality") {
      CHECK(switchRequest(controller, static_cast<uint16_t>(personality::maxPersonalities - 1)).stalled);
      CHECK(switchRequest(controller, 0xFF).stalled);
   }
   SUBCASE("A code above 8 bits") {
      CHECK(switchRequest(controller, 0x0100 | personality::toCode(Personality::CmsisDap)).stalled);
   }
   SUBCASE("wIndex is not 0") {
      CHECK(switchRequest(controller, personality::toCode(Personality::CmsisDap), 1).stalled);
   }
   SUBCASE("A data stage") {
      const std::array<uint8_t, 1> data{ personality::toCode(Personality::CmsisDap) };
      CHECK(controller
            .controlOut(setupPacket(usb::switchRequestType, usb::switchRequestCode, personality::toCode(Personality::CmsisDap), 0, 1),
                        art::span<const uint8_t>(data.data(), data.size()))
            .stalled);
   }
   SUBCASE("The direction device-to-host") {
      CHECK(
      controller.controlIn(setupPacket(0xC0, usb::switchRequestCode, personality::toCode(Personality::CmsisDap), 0, 1)).stalled);
   }
   CHECK_FALSE(personalitySwitch.requested().has_value());
   // The device still takes a good request.
   CHECK_FALSE(switchRequest(controller, personality::toCode(Personality::CmsisDap)).stalled);
   CHECK(personalitySwitch.requested() == Personality::CmsisDap);
}

TEST_CASE("The switch request stalls for a personality that is not in the image") {
   // An image with Versaloon and WCH-Link only
   static constexpr std::array<Personality, 2> image{ Personality::Versaloon, Personality::WchLink };
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };
   usb::SwitchRequestHandler handler(personalitySwitch, nullptr, art::span<const Personality>(image.data(), image.size()));
   art::usb::SetupPacket setup{ .bmRequestType = usb::switchRequestType,
                                .bRequest = usb::switchRequestCode,
                                .wValue = personality::toCode(Personality::CmsisDap) };

   CHECK_FALSE(handler.vendorRequest(setup, {}).has_value());
   CHECK_FALSE(personalitySwitch.requested().has_value());

   setup.wValue = personality::toCode(Personality::WchLink);
   CHECK(handler.vendorRequest(setup, {}).has_value());
   CHECK(personalitySwitch.requested() == Personality::WchLink);
}

TEST_CASE("Without a next handler, another vendor request stalls") {
   PersonalitySwitch personalitySwitch{ Personality::Versaloon };
   vsltest::Bench bench(nullptr, nullptr, &personalitySwitch);
   bench.enumerate();

   CHECK(bench.controller()
         .controlIn(setupPacket(0xC0, usb::dap::msOs20VendorCode, 0, art::usb::msos20::kDescriptorIndex, 16))
         .stalled);
}
