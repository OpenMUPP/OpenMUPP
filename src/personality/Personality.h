#pragma once

// The USB personalities of the firmware (docs/usb-device.md).
// Hardware-independent: the host unit tests check it.
//
// A dual image (VSL_PROTOCOL=dual) links both personalities, and one is
// active after each start. The switch commands and the settings page
// (personality/PersonalityStore.h) use the code of a personality.

#include <cstdint>
#include <optional>

namespace personality {

enum class Personality : uint8_t {
   Versaloon = 0,
   CmsisDap = 1,
};

// The code of a personality in the switch commands and in the settings page
constexpr uint8_t toCode(Personality personality) {
   return static_cast<uint8_t>(personality);
}

// Empty for a code that is not a personality
constexpr std::optional<Personality> fromCode(uint8_t code) {
   switch(code) {
   case toCode(Personality::Versaloon):
      return Personality::Versaloon;
   case toCode(Personality::CmsisDap):
      return Personality::CmsisDap;
   default:
      return std::nullopt;
   }
}

constexpr Personality other(Personality personality) {
   return personality == Personality::Versaloon ? Personality::CmsisDap : Personality::Versaloon;
}

} // namespace personality
