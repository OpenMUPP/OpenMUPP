#pragma once

// The stored personality of a dual image, in one flash page at the end of
// the application area. Hardware-independent: the host unit tests run it
// on a fake page. board/SettingsFlashPage.h has the page of the target.
//
// The page is a log of 16-bit records. An erased slot is 0xFFFF. Each
// personality has one record value (recordOf()). The first erased slot
// ends the log, and the record before it is the stored personality.
// store() writes the next slot, so the page is erased only when it is
// full: one erase for each slot of the page.
//
// A write clears bits from 1 to 0, so a write that a power loss cut keeps
// some 1 bits of the erased slot. The record values have 8 bits set each,
// so no record has all the 1 bits of another one: a cut record is never
// the record of the other personality. load() then returns no value, and
// the image uses its default personality. A new store() writes after
// that slot. A page with data after the first erased slot is
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

   static constexpr uint16_t recordOf(Personality personality) {
      return personality == Personality::CmsisDap ? 0x5AA5 : 0xA55A;
   }

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
      for(const auto personality : { Personality::Versaloon, Personality::CmsisDap }) {
         if(record == recordOf(personality)) {
            return personality;
         }
      }
      return std::nullopt;
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
      return page_.program(next, recordOf(personality)) && load() == personality;
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
