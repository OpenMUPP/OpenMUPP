#pragma once

// The USB personalities of the firmware (docs/usb-device.md).
// Hardware-independent: the host unit tests check it.
//
// The build puts one or more personalities into the image: one
// VSL_WITH_<P> define for each. An image with more than one
// (VSL_MULTI_PERSONALITY) has one active personality after each start.
// The switch commands and the settings page (personality/PersonalityStore.h)
// use the code of a personality. The host test build defines all of them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utils/compat/span.h>

namespace personality {

enum class Personality : uint8_t {
   Versaloon = 0,
   CmsisDap = 1,
   WchLink = 2,
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
   case Personality::WchLink:
      return "WCH-Link";
   }
   return "unknown";
}

// True when the image has the personality. For `if constexpr` in code that
// compiles without the personality.
constexpr bool has(Personality personality) {
   switch(personality) {
   case Personality::Versaloon:
#if defined(VSL_WITH_VERSALOON)
      return true;
#else
      return false;
#endif
   case Personality::CmsisDap:
#if defined(VSL_WITH_CMSIS_DAP)
      return true;
#else
      return false;
#endif
   case Personality::WchLink:
#if defined(VSL_WITH_WCH_LINK)
      return true;
#else
      return false;
#endif
   }
   return false;
}

// Each personality, in code order. A new personality gets the next code,
// one entry here and one VSL_WITH_<P> define in has().
inline constexpr std::array<Personality, 3> everyPersonality{ Personality::Versaloon, Personality::CmsisDap, Personality::WchLink };

// The number of personalities in the image
constexpr std::size_t compiledInCount() {
   std::size_t count = 0;
   for(const auto personality : everyPersonality) {
      if(has(personality)) {
         ++count;
      }
   }
   return count;
}

// The personalities in the image, in code order
constexpr std::array<Personality, compiledInCount()> compiledIn() {
   std::array<Personality, compiledInCount()> list{};
   std::size_t count = 0;
   for(const auto personality : everyPersonality) {
      if(has(personality)) {
         list[count++] = personality;
      }
   }
   return list;
}

// The personalities of the image, in the order of the cycle (code order).
// The cycle, fromCode() and the settings page work on this list only.
inline constexpr auto all = compiledIn();
static_assert(!all.empty(), "the image needs at least one VSL_WITH_<P> define");
#if defined(VSL_MULTI_PERSONALITY)
static_assert(all.size() > 1, "VSL_MULTI_PERSONALITY needs more than one personality");
#else
static_assert(all.size() == 1, "an image with more than one personality needs VSL_MULTI_PERSONALITY");
#endif

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
static_assert(hasValidCodes(art::span<const Personality>(everyPersonality.data(), everyPersonality.size())),
              "personality codes must be unique and below maxPersonalities");

// Empty for a code that is not a personality of the list
constexpr std::optional<Personality> fromCodeIn(art::span<const Personality> list, uint8_t code) {
   for(const auto personality : list) {
      if(toCode(personality) == code) {
         return personality;
      }
   }
   return std::nullopt;
}

// Empty for a code that is not a personality of the image. A switch command
// to such a code is refused.
constexpr std::optional<Personality> fromCode(uint8_t code) {
   return fromCodeIn(art::span<const Personality>(all.data(), all.size()), code);
}

// True when the list has the personality
constexpr bool contains(art::span<const Personality> list, Personality personality) {
   return fromCodeIn(list, toCode(personality)).has_value();
}

// The personality when the settings page has none: VSL_DEFAULT_PERSONALITY
// (a code), else the first one of the image
#if defined(VSL_DEFAULT_PERSONALITY)
inline constexpr Personality defaultPersonality = static_cast<Personality>(VSL_DEFAULT_PERSONALITY);
#else
inline constexpr Personality defaultPersonality = all[0];
#endif
static_assert(contains(art::span<const Personality>(all.data(), all.size()), defaultPersonality),
              "VSL_DEFAULT_PERSONALITY must be a personality of the image");

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
