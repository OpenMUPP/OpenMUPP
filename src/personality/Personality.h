#pragma once

// The USB personalities of the firmware (docs/usb-device.md).
// Hardware-independent: the host unit tests check it.
//
// A dual image (VSL_PROTOCOL=dual) links both personalities, and one is
// active after each start. The switch commands and the settings page
// (personality/PersonalityStore.h) use the code of a personality.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utils/compat/span.h>

namespace personality {

enum class Personality : uint8_t {
   Versaloon = 0,
   CmsisDap = 1,
};

// The code of a personality in the switch commands and in the settings page
constexpr uint8_t toCode(Personality personality) {
   return static_cast<uint8_t>(personality);
}

// The name of a personality, for the log
constexpr const char *name(Personality personality) {
   switch(personality) {
   case Personality::Versaloon:
      return "Versaloon";
   case Personality::CmsisDap:
      return "CMSIS-DAP";
   }
   return "unknown";
}

// The personalities that this firmware knows, in the order of the cycle.
// A new personality gets the next code and one entry here.
inline constexpr std::array<Personality, 2> all{ Personality::Versaloon, Personality::CmsisDap };

// The largest number of personalities that the settings page can store
// (see recordOfCode() in personality/PersonalityStore.h).
inline constexpr std::size_t maxPersonalities = 4;

// True when each code of the list is below maxPersonalities and no code repeats
constexpr bool hasValidCodes(art::span<const Personality> list) {
   for(std::size_t index = 0; index < list.size(); ++index) {
      if(toCode(list[index]) >= maxPersonalities) {
         return false;
      }
      for(std::size_t other = 0; other < index; ++other) {
         if(toCode(list[other]) == toCode(list[index])) {
            return false;
         }
      }
   }
   return true;
}
static_assert(hasValidCodes(art::span<const Personality>(all.data(), all.size())),
              "personality codes must be unique and below maxPersonalities");

// Empty for a code that is not a personality
constexpr std::optional<Personality> fromCode(uint8_t code) {
   for(const auto personality : all) {
      if(toCode(personality) == code) {
         return personality;
      }
   }
   return std::nullopt;
}

// The personality after the given one in the list, or the first one at the end
constexpr Personality nextIn(art::span<const Personality> list, Personality personality) {
   for(std::size_t index = 0; index < list.size(); ++index) {
      if(list[index] == personality) {
         return list[(index + 1) % list.size()];
      }
   }
   return list[0];
}

// The next personality of the cycle. The key gesture uses it.
constexpr Personality next(Personality personality) {
   return nextIn(art::span<const Personality>(all.data(), all.size()), personality);
}

} // namespace personality
