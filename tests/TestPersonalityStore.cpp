#include "personality/PersonalityStore.h"

#include <array>
#include <doctest/doctest.h>
#include <vector>

using personality::Personality;
using personality::PersonalityStore;
using personality::Settings;

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
   CHECK(personality::toCode(Personality::WchLink) == 2);
   CHECK(personality::fromCode(0) == Personality::Versaloon);
   CHECK(personality::fromCode(1) == Personality::CmsisDap);
   CHECK(personality::fromCode(2) == Personality::WchLink);
   CHECK_FALSE(personality::fromCode(3).has_value());
   CHECK_FALSE(personality::fromCode(0xFF).has_value());
}

TEST_CASE("The host build has all personalities in code order") {
   // The test build defines each MUPP_WITH_<P> (tests/CMakeLists.txt)
   static_assert(personality::all.size() == 3);
   CHECK(personality::all[0] == Personality::Versaloon);
   CHECK(personality::all[1] == Personality::CmsisDap);
   CHECK(personality::all[2] == Personality::WchLink);
   CHECK(personality::has(Personality::Versaloon));
   CHECK(personality::has(Personality::CmsisDap));
   CHECK(personality::has(Personality::WchLink));
   // No MUPP_DEFAULT_PERSONALITY: the first one of the image
   CHECK(personality::defaultPersonality == Personality::Versaloon);
}

TEST_CASE("The key cycle visits the three personalities in code order") {
   CHECK(personality::next(Personality::Versaloon) == Personality::CmsisDap);
   CHECK(personality::next(Personality::CmsisDap) == Personality::WchLink);
   CHECK(personality::next(Personality::WchLink) == Personality::Versaloon);
}

TEST_CASE("A code that is not in the image is not a personality") {
   // An image with CMSIS-DAP and WCH-Link only
   constexpr std::array<Personality, 2> image{ Personality::CmsisDap, Personality::WchLink };
   const art::span<const Personality> list(image.data(), image.size());

   CHECK_FALSE(personality::fromCodeIn(list, 0).has_value());
   CHECK(personality::fromCodeIn(list, 1) == Personality::CmsisDap);
   CHECK(personality::fromCodeIn(list, 2) == Personality::WchLink);
   CHECK_FALSE(personality::fromCodeIn(list, 3).has_value());
   CHECK_FALSE(personality::contains(list, Personality::Versaloon));
   CHECK(personality::contains(list, Personality::WchLink));
   // The cycle skips Versaloon
   CHECK(personality::nextIn(list, Personality::CmsisDap) == Personality::WchLink);
   CHECK(personality::nextIn(list, Personality::WchLink) == Personality::CmsisDap);
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

TEST_CASE("The records of all settings are distinct and none is a subset of another") {
   std::vector<uint16_t> records;
   for(uint8_t code = 0; code < personality::maxPersonalities; ++code) {
      for(uint8_t mode = 0; mode < personality::bridgeModes; ++mode) {
         records.push_back(PersonalityStore::recordOfCode(code, mode));
      }
   }
   for(std::size_t x = 0; x < records.size(); ++x) {
      CHECK(__builtin_popcount(records[x]) == 8);
      CHECK(records[x] != PersonalityStore::erasedSlot);
      for(std::size_t y = 0; y < records.size(); ++y) {
         if(x != y) {
            CHECK(records[x] != records[y]);
            CHECK((records[x] & records[y]) != records[y]);
         }
      }
   }
}

TEST_CASE("The records of the older images are the records of the bridge mode auto") {
   CHECK(PersonalityStore::recordOfCode(0) == 0xA55A);
   CHECK(PersonalityStore::recordOfCode(1) == 0x5AA5);
   CHECK(PersonalityStore::recordOfCode(2) == 0x3CC3);
   CHECK(PersonalityStore::recordOfCode(3) == 0xC33C);
   CHECK(PersonalityStore::recordOfCode(1, personality::bridgeModeAuto) == 0x5AA5);

   FakeFlashPage page;
   page.content[0] = 0x5AA5;
   const PersonalityStore store{ page };
   CHECK(store.loadSettings() == Settings{ Personality::CmsisDap, personality::bridgeModeAuto });
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

TEST_CASE("The WCH-Link personality (code 2) is stored and loaded") {
   FakeFlashPage page;
   PersonalityStore store{ page };

   CHECK(store.store(Personality::WchLink));
   CHECK(store.load() == Personality::WchLink);
   CHECK(page.content[0] == record(Personality::WchLink));

   CHECK(store.store(Personality::CmsisDap));
   CHECK(store.load() == Personality::CmsisDap);
   CHECK(store.store(Personality::WchLink));
   CHECK(store.load() == Personality::WchLink);
   CHECK(page.content[2] == record(Personality::WchLink));
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
   auto last = personality;
   for(std::size_t i = 0; i < FakeFlashPage::slotCount; ++i) {
      REQUIRE(store.store(personality));
      last = personality;
      personality = personality::next(personality);
   }
   CHECK(page.erases == 0);
   CHECK(store.load() == last);
   REQUIRE(last != Personality::CmsisDap);

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

TEST_CASE("The bridge mode is stored with the personality") {
   FakeFlashPage page;
   PersonalityStore store{ page };
   constexpr uint8_t off = 0;
   constexpr uint8_t on = 1;

   // An empty page: the personality of the image
   CHECK(store.storeBridgeMode(on, Personality::CmsisDap));
   CHECK(store.loadSettings() == Settings{ Personality::CmsisDap, on });
   CHECK(page.content[0] == PersonalityStore::recordOf(Personality::CmsisDap, on));

   // A switch keeps the bridge mode
   CHECK(store.store(Personality::Versaloon));
   CHECK(store.loadSettings() == Settings{ Personality::Versaloon, on });

   // A bridge mode keeps the stored personality, not the active one
   CHECK(store.storeBridgeMode(off, Personality::CmsisDap));
   CHECK(store.loadSettings() == Settings{ Personality::Versaloon, off });
   CHECK(store.load() == Personality::Versaloon);

   // The same mode again writes nothing
   const int programs = page.programs;
   CHECK(store.storeBridgeMode(off, Personality::CmsisDap));
   CHECK(page.programs == programs);
}

TEST_CASE("The bridge mode of a personality that is not in the image is loaded and kept") {
   FakeFlashPage page;
   PersonalityStore store{ page };
   // Code 3 is in no image: as a personality of another image
   constexpr uint8_t other = 3;
   REQUIRE_FALSE(personality::fromCode(other).has_value());
   page.content[0] = PersonalityStore::recordOfCode(other, 0);

   REQUIRE(store.loadSettings().has_value());
   CHECK(store.loadSettings()->bridgeMode == 0);
   CHECK_FALSE(store.load().has_value());

   // The store of a mode keeps the personality of the other image
   CHECK(store.storeBridgeMode(1, Personality::CmsisDap));
   CHECK(page.content[1] == PersonalityStore::recordOfCode(other, 1));
}

TEST_CASE("A cut record of a bridge mode is no stored settings") {
   FakeFlashPage page;
   PersonalityStore store{ page };
   REQUIRE(store.store(Personality::CmsisDap));
   page.content[1] = static_cast<uint16_t>(PersonalityStore::recordOf(Personality::CmsisDap, 0) | 0x0100);

   CHECK_FALSE(store.loadSettings().has_value());
   // The next store writes after the cut record, with the default personality
   CHECK(store.storeBridgeMode(0, Personality::CmsisDap));
   CHECK(page.content[2] == PersonalityStore::recordOf(Personality::CmsisDap, 0));
   CHECK(page.erases == 0);
}
