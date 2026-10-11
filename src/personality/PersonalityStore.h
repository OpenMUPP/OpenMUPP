#pragma once

// The stored personality of a dual image, in one flash page at the end of
// the application area. Hardware-independent: the host unit tests run it
// on a fake page. personality/FlashPage.h has the page of the target.
//
// The page is a log of 16-bit records. An erased slot is 0xFFFF. A record
// is recordTag | the personality code (personality/Personality.h). The
// first erased slot ends the log, and the record before it is the stored
// personality. store() writes the next slot, so the page is erased only
// when it is full: one erase for each slotCount switches.
//
// A slot that a power loss cut is not a valid record: load() then returns
// no value, and the image uses its default personality. A new store()
// writes after that slot. A page with data after the first erased slot is
// not a log (for example code of an older, larger image): store() erases
// it first.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utils/compat/span.h>

#include "personality/Personality.h"

namespace personality {

// One erasable flash page, programmed in 16-bit units, as on the STM32F1.
class FlashPage {
   public:
   // The content of the page
   [[nodiscard]] virtual art::span<const uint16_t> slots() const = 0;
   // Sets all slots to 0xFFFF. False on a flash error.
   virtual bool erase() = 0;
   // Writes an erased slot. False on a flash error.
   virtual bool program(std::size_t index, uint16_t value) = 0;

   protected:
   ~FlashPage() = default;
};

class PersonalityStore {
   public:
   static constexpr uint16_t erasedSlot = 0xFFFF;
   static constexpr uint16_t recordTag = 0x5A00;
   static constexpr uint16_t tagMask = 0xFF00;

   explicit PersonalityStore(FlashPage &page)
      : page_(page) {
   }

   // The stored personality. Empty when the page holds no valid record.
   [[nodiscard]] std::optional<Personality> load() const {
      const auto slots = page_.slots();
      const std::size_t end = firstErased(slots);
      if(end == 0) {
         return std::nullopt;
      }
      const uint16_t record = slots[end - 1];
      if((record & tagMask) != recordTag) {
         return std::nullopt;
      }
      return fromCode(static_cast<uint8_t>(record & ~tagMask));
   }

   // Stores the personality. True when load() then returns it.
   bool store(Personality personality) {
      if(load() == personality) {
         return true;
      }
      const auto slots = page_.slots();
      std::size_t next = firstErased(slots);
      if(next == slots.size() || !isErasedFrom(slots, next)) {
         if(!page_.erase()) {
            return false;
         }
         next = 0;
      }
      return page_.program(next, static_cast<uint16_t>(recordTag | toCode(personality))) && load() == personality;
   }

   private:
   static std::size_t firstErased(art::span<const uint16_t> slots) {
      std::size_t index = 0;
      while(index < slots.size() && slots[index] != erasedSlot) {
         ++index;
      }
      return index;
   }

   static bool isErasedFrom(art::span<const uint16_t> slots, std::size_t index) {
      for(; index < slots.size(); ++index) {
         if(slots[index] != erasedSlot) {
            return false;
         }
      }
      return true;
   }

   FlashPage &page_;
};

} // namespace personality
