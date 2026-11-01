// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The layout of the settings sector on the STM32H5 (board/SettingsFlashPage.cpp).
// Hardware-independent: the host unit tests check it.
//
// The STM32H5 writes the flash in 128-bit flash words with 9 ECC bits
// (RM0481 7.3.5). A second write to a flash word gives a bad ECC, so each
// slot of the PersonalityStore log is one flash word, written one time.
// The record is the low half-word of the first 32-bit word. All other bits
// stay 1.
//
// A power loss during a write can leave any mix of bits, and a bad ECC.
// slotOf() maps each flash word that is not erased and not a full record
// to corruptSlot. The log treats it as a cut record: load() returns no
// value, and the next store() writes the next slot. The ECC corrects a
// word with one wrong bit, for example back to an erased word, and detects
// a word with two wrong bits. The driver reports both as an ECC error, and
// slotOf() gives corruptSlot. Thus a cut write is not seen as erased, and the
// store does not write that word again. Limit: a word with three or more
// wrong bits can pass the ECC; slotOf() then sees the data as it is.

#include <array>
#include <cstddef>
#include <cstdint>

#include "personality/PersonalityStore.h"

namespace personality {

using FlashWord = std::array<uint32_t, 4>;

constexpr std::size_t flashWordBytes = sizeof(FlashWord);

// Not erased and not the record of a personality (all records have 8 bits set)
constexpr uint16_t corruptSlot = 0x0000;

// The flash word that stores a record
constexpr FlashWord flashWordOf(uint16_t record) {
   return { 0xFFFF0000U | record, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU };
}

// The log slot of a flash word. eccError: the read gave an ECC correction
// or a double ECC error.
constexpr uint16_t slotOf(const FlashWord &word, bool eccError) {
   if(eccError) {
      return corruptSlot;
   }
   const auto record = static_cast<uint16_t>(word[0] & 0xFFFFU);
   if(word == flashWordOf(record)) {
      return record; // erasedSlot for an erased word
   }
   return corruptSlot;
}

static_assert(slotOf(flashWordOf(PersonalityStore::erasedSlot), false) == PersonalityStore::erasedSlot);
static_assert(corruptSlot != PersonalityStore::erasedSlot);

} // namespace personality
