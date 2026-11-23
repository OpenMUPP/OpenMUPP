// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The settings of an image: the stored personality of an image with more
// than one personality (MUPP_MULTI_PERSONALITY), the stored mode of the
// RISC-V bridge (MUPP_CMSIS_DAP_RV_BRIDGE), and the setting "VPP without an
// adapter" of a board with an adapter ID (board/IdSense.h, ID_DAP_Vendor3).
// They are in one flash page at
// the end of the application area. Hardware-independent: the host unit
// tests run it on a fake page. board/SettingsFlashPage.h has the page of
// the target.
//
// The page is a log of 16-bit records. An erased slot is 0xFFFF. Each set
// of a personality, a bridge mode and the VPP setting has one record value
// (recordOf()). The
// first erased slot ends the log, and the record before it holds the
// stored settings. Each store writes the next slot, so the page is erased
// only when it is full: one erase for each slot of the page. A store of
// one setting writes the other settings of the last record again.
//
// The records of the bridge mode auto are the records of the older images,
// which stored only the personality. Such a page thus gives auto. The
// records with the VPP setting off are the records of the images before
// the VPP setting. An older image does not know a record with the VPP
// setting on: it then starts with its default settings.
//
// A write clears bits from 1 to 0, so a write that a power loss cut keeps
// some 1 bits of the erased slot. The record values have 8 bits set each,
// so no record has all the 1 bits of another one: a cut record is never
// the record of other settings. load() then returns no value, and the
// image uses its default settings. A new store writes after that slot. A
// page with data after the first erased slot is not a log (for example
// code of an older, larger image): a store erases it first. Limit: in such
// a page, the word before the first erased slot can be a record by chance
// (24 of 65536 values). The image then starts with those settings until
// the next store. When a write to a slot that reads as erased fails, the
// store erases the page and writes slot 0.

#include <array>
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

// The bridge modes of the settings page: the values of cmsisdap::BridgeMode
// (protocol/cmsisdap/DapProcessor.h)
inline constexpr uint8_t bridgeModes = 3;
inline constexpr uint8_t bridgeModeAuto = 2;

struct Settings {
   Personality personality;
   uint8_t bridgeMode{ bridgeModeAuto };
   // VPP is also allowed when no adapter is connected (decision D2)
   bool vppWithoutAdapter{ false };

   constexpr bool operator==(const Settings &) const = default;
};

class PersonalityStore {
   public:
   static constexpr uint16_t erasedSlot = 0xFFFF;

   // The record of a personality code (below maxPersonalities), a bridge
   // mode (below bridgeModes) and the VPP setting. Each value has 8 bits set
   // and the values differ, so none has all the bits of another.
   static constexpr uint16_t recordOfCode(uint8_t code, uint8_t bridgeMode = bridgeModeAuto, bool vppWithoutAdapter = false) {
      // Rows: off, on, auto. The auto row has the records of the older images.
      constexpr std::array<std::array<uint16_t, maxPersonalities>, bridgeModes> records{ {
      { 0x0FF0, 0xF00F, 0x33CC, 0xCC33 },
      { 0x6699, 0x9966, 0x55AA, 0xAA55 },
      { 0xA55A, 0x5AA5, 0x3CC3, 0xC33C },
      } };
      // The same rows with the VPP setting on
      constexpr std::array<std::array<uint16_t, maxPersonalities>, bridgeModes> vppRecords{ {
      { 0x0F0F, 0xF0F0, 0x00FF, 0xFF00 },
      { 0x3333, 0xCCCC, 0x5555, 0xAAAA },
      { 0x6666, 0x9999, 0x3C3C, 0xC3C3 },
      } };
      return (vppWithoutAdapter ? vppRecords : records)[bridgeMode][code];
   }

   static constexpr uint16_t recordOf(Personality personality, uint8_t bridgeMode = bridgeModeAuto, bool vppWithoutAdapter = false) {
      return recordOfCode(toCode(personality), bridgeMode, vppWithoutAdapter);
   }

   static constexpr uint16_t recordOf(const Settings &settings) {
      return recordOf(settings.personality, settings.bridgeMode, settings.vppWithoutAdapter);
   }

   explicit PersonalityStore(FlashPage &page)
      : page_(page) {
   }

   // The stored settings. Empty when the page holds no valid record.
   [[nodiscard]] std::optional<Settings> loadSettings() const {
      const auto slots = page_.slots();
      const std::size_t end = firstErased(slots);
      if(end == 0) {
         return std::nullopt;
      }
      const uint16_t record = slots[end - 1];
      // Each code, also one that is not in the image: a store of the bridge
      // mode then keeps the personality of another image.
      for(uint8_t code = 0; code < maxPersonalities; ++code) {
         for(uint8_t mode = 0; mode < bridgeModes; ++mode) {
            for(const bool vpp : { false, true }) {
               if(record == recordOfCode(code, mode, vpp)) {
                  return Settings{ static_cast<Personality>(code), mode, vpp };
               }
            }
         }
      }
      return std::nullopt;
   }

   // The stored personality. Empty when the page holds no valid record, or
   // a personality that is not in the image.
   [[nodiscard]] std::optional<Personality> load() const {
      const auto settings = loadSettings();
      return settings ? fromCode(toCode(settings->personality)) : std::nullopt;
   }

   // Stores the settings. True when loadSettings() then returns them.
   bool store(const Settings &settings) {
      if(loadSettings() == settings) {
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
      if(page_.program(next, recordOf(settings)) && loadSettings() == settings) {
         return true;
      }
      // A slot that reads as erased but does not take the write: on the
      // CH32V30x, a cut write can read as erased. Without this, each later
      // store fails at that slot until a mass erase.
      if(next == 0 || !page_.erase()) {
         return false;
      }
      return page_.program(0, recordOf(settings)) && loadSettings() == settings;
   }

   // Stores the personality with the other stored settings, the defaults
   // when the page holds no valid record. True when load() then returns it.
   bool store(Personality personality) {
      Settings settings = loadSettings().value_or(Settings{ personality });
      settings.personality = personality;
      return store(settings);
   }

   // Stores the bridge mode with the other stored settings, also a
   // personality that is not in the image. active: the personality when
   // the page holds no valid record. True when loadSettings() then has the
   // mode.
   bool storeBridgeMode(uint8_t bridgeMode, Personality active) {
      Settings settings = loadSettings().value_or(Settings{ active });
      settings.bridgeMode = bridgeMode;
      return store(settings);
   }

   // Stores the VPP setting with the other stored settings, as
   // storeBridgeMode(). True when loadSettings() then has the setting.
   bool storeVppWithoutAdapter(bool allowed, Personality active) {
      Settings settings = loadSettings().value_or(Settings{ active });
      settings.vppWithoutAdapter = allowed;
      return store(settings);
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
