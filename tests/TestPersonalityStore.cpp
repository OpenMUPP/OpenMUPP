#include "personality/PersonalityStore.h"

#include <array>
#include <doctest/doctest.h>

using personality::Personality;
using personality::PersonalityStore;

namespace {

// A flash page that behaves as the STM32F1 flash: a slot can be written
// only when it is erased.
class FakeFlashPage final : public personality::FlashPage {
   public:
   static constexpr std::size_t slotCount = 8;

   FakeFlashPage() {
      content.fill(PersonalityStore::erasedSlot);
   }

   [[nodiscard]] art::span<const uint16_t> slots() const override {
      return art::span<const uint16_t>(content.data(), content.size());
   }

   bool erase() override {
      ++erases;
      if(failErase) {
         return false;
      }
      content.fill(PersonalityStore::erasedSlot);
      return true;
   }

   bool program(std::size_t index, uint16_t value) override {
      ++programs;
      REQUIRE(index < content.size());
      if(failProgram || content[index] != PersonalityStore::erasedSlot) {
         return false;
      }
      content[index] = value;
      return true;
   }

   std::array<uint16_t, slotCount> content{};
   int erases = 0;
   int programs = 0;
   bool failErase = false;
   bool failProgram = false;
};

constexpr uint16_t record(Personality personality) {
   return PersonalityStore::recordOf(personality);
}

} // namespace

TEST_CASE("The personality codes") {
   CHECK(personality::toCode(Personality::Versaloon) == 0);
   CHECK(personality::toCode(Personality::CmsisDap) == 1);
   CHECK(personality::fromCode(0) == Personality::Versaloon);
   CHECK(personality::fromCode(1) == Personality::CmsisDap);
   CHECK_FALSE(personality::fromCode(2).has_value());
   CHECK_FALSE(personality::fromCode(0xFF).has_value());
   CHECK(personality::next(Personality::Versaloon) == Personality::CmsisDap);
   CHECK(personality::next(Personality::CmsisDap) == Personality::Versaloon);
}

TEST_CASE("The cycle visits three and four personalities in order") {
   constexpr auto a = Personality::Versaloon;
   constexpr auto b = Personality::CmsisDap;
   constexpr auto c = static_cast<Personality>(2);
   constexpr auto d = static_cast<Personality>(3);
   constexpr std::array<Personality, 3> three{ a, b, c };
   constexpr std::array<Personality, 4> four{ a, b, c, d };
   const auto next3 = [&](Personality p) {
      return personality::nextIn(art::span<const Personality>(three.data(), three.size()), p);
   };
   const auto next4 = [&](Personality p) {
      return personality::nextIn(art::span<const Personality>(four.data(), four.size()), p);
   };

   CHECK(next3(a) == b);
   CHECK(next3(b) == c);
   CHECK(next3(c) == a);
   CHECK(next4(c) == d);
   CHECK(next4(d) == a);
   CHECK(personality::nextIn(art::span<const Personality>(three.data(), 2), c) == a);
}

TEST_CASE("The code check rejects a repeated code and a code that is too large") {
   constexpr std::array<Personality, 2> repeated{ Personality::CmsisDap, Personality::CmsisDap };
   constexpr std::array<Personality, 2> tooLarge{ Personality::Versaloon, static_cast<Personality>(personality::maxPersonalities) };
   constexpr std::array<Personality, 3> good{ Personality::Versaloon, Personality::CmsisDap, static_cast<Personality>(3) };

   CHECK(personality::hasValidCodes(art::span<const Personality>(good.data(), good.size())));
   CHECK_FALSE(personality::hasValidCodes(art::span<const Personality>(repeated.data(), repeated.size())));
   CHECK_FALSE(personality::hasValidCodes(art::span<const Personality>(tooLarge.data(), tooLarge.size())));
}

TEST_CASE("The records of four personalities are distinct and none is a subset of another") {
   for(uint8_t x = 0; x < personality::maxPersonalities; ++x) {
      CHECK(__builtin_popcount(PersonalityStore::recordOfCode(x)) == 8);
      for(uint8_t y = 0; y < personality::maxPersonalities; ++y) {
         if(x != y) {
            const uint16_t rx = PersonalityStore::recordOfCode(x);
            const uint16_t ry = PersonalityStore::recordOfCode(y);
            CHECK(rx != ry);
            CHECK((rx & ry) != ry);
         }
      }
   }
}

TEST_CASE("An erased page has no stored personality") {
   FakeFlashPage page;
   const PersonalityStore store{ page };

   CHECK_FALSE(store.load().has_value());
}

TEST_CASE("A stored personality is loaded") {
   FakeFlashPage page;
   PersonalityStore store{ page };

   CHECK(store.store(Personality::CmsisDap));
   CHECK(store.load() == Personality::CmsisDap);
   CHECK(page.content[0] == record(Personality::CmsisDap));
   CHECK(page.erases == 0);

   CHECK(store.store(Personality::Versaloon));
   CHECK(store.load() == Personality::Versaloon);
   CHECK(page.content[1] == record(Personality::Versaloon));
   CHECK(page.erases == 0);
}

TEST_CASE("Storing the stored personality writes nothing") {
   FakeFlashPage page;
   PersonalityStore store{ page };

   REQUIRE(store.store(Personality::CmsisDap));
   CHECK(store.store(Personality::CmsisDap));
   CHECK(page.programs == 1);
   CHECK(page.content[1] == PersonalityStore::erasedSlot);
}

TEST_CASE("A full page is erased before the next record") {
   FakeFlashPage page;
   PersonalityStore store{ page };

   auto personality = Personality::CmsisDap;
   for(std::size_t i = 0; i < FakeFlashPage::slotCount; ++i) {
      REQUIRE(store.store(personality));
      personality = personality::next(personality);
   }
   CHECK(page.erases == 0);
   CHECK(store.load() == Personality::Versaloon);

   CHECK(store.store(Personality::CmsisDap));
   CHECK(page.erases == 1);
   CHECK(page.content[0] == record(Personality::CmsisDap));
   CHECK(page.content[1] == PersonalityStore::erasedSlot);
   CHECK(store.load() == Personality::CmsisDap);
}

TEST_CASE("A cut record is not a stored personality") {
   FakeFlashPage page;
   PersonalityStore store{ page };
   // A power loss during the write cleared only some bits
   page.content[0] = static_cast<uint16_t>(record(Personality::CmsisDap) | 0x0100);

   CHECK_FALSE(store.load().has_value());

   // The next record goes after the cut one
   CHECK(store.store(Personality::CmsisDap));
   CHECK(page.erases == 0);
   CHECK(page.content[1] == record(Personality::CmsisDap));
   CHECK(store.load() == Personality::CmsisDap);
}

TEST_CASE("Another value is not a stored personality") {
   FakeFlashPage page;
   const PersonalityStore store{ page };
   page.content[0] = 0x0001;

   CHECK_FALSE(store.load().has_value());
}

TEST_CASE("A cut record is never the record of the other personality") {
   // A cut write leaves a value with all the 0 bits of a subset of the
   // record: each value v with (v & record) == record.
   for(const auto target : { Personality::Versaloon, Personality::CmsisDap }) {
      const uint16_t full = record(target);
      for(uint32_t value = 0; value <= 0xFFFF; ++value) {
         if((value & full) != full || value == full || value == PersonalityStore::erasedSlot) {
            continue;
         }
         FakeFlashPage page;
         const PersonalityStore store{ page };
         page.content[0] = static_cast<uint16_t>(value);
         const auto loaded = store.load();
         CAPTURE(value);
         CHECK_FALSE(loaded.has_value());
      }
   }
}

TEST_CASE("A page with data after the first erased slot is erased first") {
   FakeFlashPage page;
   PersonalityStore store{ page };
   // Code of an older image: not a log
   page.content[0] = 0x4770;
   page.content[2] = 0xBF00;

   CHECK(store.store(Personality::Versaloon));
   CHECK(page.erases == 1);
   CHECK(page.content[0] == record(Personality::Versaloon));
   CHECK(page.content[2] == PersonalityStore::erasedSlot);
   CHECK(store.load() == Personality::Versaloon);
}

TEST_CASE("A flash error fails the store") {
   FakeFlashPage page;
   PersonalityStore store{ page };

   SUBCASE("program") {
      page.failProgram = true;
      CHECK_FALSE(store.store(Personality::CmsisDap));
      CHECK_FALSE(store.load().has_value());
   }
   SUBCASE("erase") {
      page.content.fill(record(Personality::Versaloon));
      page.failErase = true;
      CHECK_FALSE(store.store(Personality::CmsisDap));
      CHECK(page.programs == 0);
      CHECK(store.load() == Personality::Versaloon);
   }
}
