#include "personality/FlashWordSlot.h"

#include <array>
#include <doctest/doctest.h>

using personality::corruptSlot;
using personality::FlashWord;
using personality::flashWordOf;
using personality::Personality;
using personality::PersonalityStore;
using personality::slotOf;

namespace {

constexpr FlashWord erasedWord{ 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU };

// A settings sector as on the STM32H5: 128-bit flash words, each written
// one time. A second write to a flash word gives a double ECC error, as a
// write that a power loss cut can.
class FakeFlashSector final : public personality::FlashPage {
   public:
   static constexpr std::size_t wordCount = 8;

   FakeFlashSector() {
      words.fill(erasedWord);
      refresh();
   }

   [[nodiscard]] art::span<const uint16_t> slots() const override {
      return art::span<const uint16_t>(view.data(), view.size());
   }

   bool erase() override {
      words.fill(erasedWord);
      eccError.fill(false);
      refresh();
      return true;
   }

   bool program(std::size_t index, uint16_t value) override {
      REQUIRE(index < words.size());
      if(words[index] != erasedWord) {
         eccError[index] = true;
      }
      const FlashWord word = flashWordOf(value);
      for(std::size_t i = 0; i < word.size(); ++i) {
         words[index][i] &= word[i];
      }
      refresh();
      return !eccError[index];
   }

   void refresh() {
      for(std::size_t i = 0; i < words.size(); ++i) {
         view[i] = slotOf(words[i], eccError[i]);
      }
   }

   std::array<FlashWord, wordCount> words{};
   std::array<bool, wordCount> eccError{};
   std::array<uint16_t, wordCount> view{};
};

} // namespace

TEST_CASE("A flash word holds the record in its low half-word, all other bits 1") {
   const uint16_t record = PersonalityStore::recordOf(Personality::CmsisDap);
   const FlashWord word = flashWordOf(record);

   CHECK(word[0] == (0xFFFF0000U | record));
   CHECK(word[1] == 0xFFFFFFFFU);
   CHECK(word[2] == 0xFFFFFFFFU);
   CHECK(word[3] == 0xFFFFFFFFU);
   CHECK(slotOf(word, false) == record);
   CHECK(slotOf(erasedWord, false) == PersonalityStore::erasedSlot);
}

TEST_CASE("A flash word with an ECC error is a corrupt slot") {
   // One written bit: the ECC corrects the word to erased and reports ECCC
   CHECK(slotOf(erasedWord, true) == corruptSlot);
   CHECK(slotOf(flashWordOf(PersonalityStore::recordOf(Personality::Versaloon)), true) == corruptSlot);
}

TEST_CASE("A partly written flash word that the ECC does not correct is never erased and never a record") {
   const uint16_t record = PersonalityStore::recordOf(Personality::Versaloon);
   // The record half-word is erased, but other bits of the word are written.
   // On the target the ECC corrects one wrong bit (ECCC); these words model
   // a cut write with more wrong bits that still has a valid ECC.
   for(std::size_t i = 0; i < 4; ++i) {
      FlashWord word = erasedWord;
      word[i] &= (i == 0) ? 0xFFF0FFFFU : 0xFFFFFFF0U;
      CHECK(slotOf(word, false) == corruptSlot);
   }
   // The record is complete, but one more bit is written
   FlashWord word = flashWordOf(record);
   word[3] = 0x7FFFFFFFU;
   CHECK(slotOf(word, false) == corruptSlot);
   // Only some bits of the record are written
   word = erasedWord;
   word[0] = 0xFFFF0000U | (record | 0x0001U);
   CHECK(slotOf(word, false) != record);
   CHECK(slotOf(word, false) != PersonalityStore::erasedSlot);
}

TEST_CASE("The store writes each flash word of the sector only one time") {
   FakeFlashSector sector;
   PersonalityStore store{ sector };

   constexpr std::array<Personality, 3> sequence{ Personality::CmsisDap, Personality::Versaloon, Personality::WchLink };
   for(std::size_t round = 0; round < 3 * FakeFlashSector::wordCount; ++round) {
      const Personality target = sequence[round % sequence.size()];
      REQUIRE(store.store(target));
      CHECK(store.load() == target);
   }
   for(const bool error : sector.eccError) {
      CHECK_FALSE(error);
   }
}

TEST_CASE("A cut write leaves the default personality, and the next store writes after it") {
   FakeFlashSector sector;
   PersonalityStore store{ sector };
   REQUIRE(store.store(Personality::CmsisDap));

   // A power loss cut the write of slot 1 with a bad ECC
   sector.eccError[1] = true;
   sector.words[1][0] = 0xFFFF5AFFU;
   sector.refresh();
   CHECK_FALSE(store.load().has_value());

   REQUIRE(store.store(Personality::Versaloon));
   CHECK(store.load() == Personality::Versaloon);
   CHECK(sector.words[2] == flashWordOf(PersonalityStore::recordOf(Personality::Versaloon)));
}

TEST_CASE("A cut write that left the record half-word erased is not written again") {
   FakeFlashSector sector;
   PersonalityStore store{ sector };
   REQUIRE(store.store(Personality::CmsisDap));

   // Slot 1: only some bits of the second 32-bit word were written, and the
   // ECC reports no error
   sector.words[1][1] = 0xFFFFF0F0U;
   sector.refresh();

   REQUIRE(store.store(Personality::Versaloon));
   CHECK(store.load() == Personality::Versaloon);
   CHECK_FALSE(sector.eccError[1]);
   CHECK(sector.words[1][1] == 0xFFFFF0F0U);
   CHECK(sector.words[2] == flashWordOf(PersonalityStore::recordOf(Personality::Versaloon)));
}
