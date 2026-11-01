#pragma once

// The host side of the personality switch request on EP0
// (usb/SwitchRequest.h), for the USB tests of each personality.

#include "personality/Personality.h"
#include "personality/PersonalitySwitch.h"
#include "usb/SwitchRequest.h"

#include <FakeDeviceController.h>
#include <doctest/doctest.h>

#include <cstdint>

namespace mupptest {

// The host sends the switch request with no data stage.
inline art::usb::test::FakeDeviceController::ControlResult
switchRequest(art::usb::test::FakeDeviceController &controller, uint16_t code, uint16_t index = 0) {
   bool ok = false;
   auto result =
   controller.controlOut(art::usb::test::setupPacket(usb::switchRequestType, usb::switchRequestCode, code, index, 0), {}, &ok);
   CHECK(ok);
   return result;
}

// True when the main loop resets for target after the reply delay
inline bool switchesTo(personality::PersonalitySwitch &personalitySwitch, personality::Personality target) {
   if(personalitySwitch.requested() != target) {
      return false;
   }
   return !personalitySwitch.isDue(0) && personalitySwitch.isDue(personality::PersonalitySwitch::replyDelayMs);
}

} // namespace mupptest
