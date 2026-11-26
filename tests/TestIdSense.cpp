// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/IdSense.h"

#include <doctest/doctest.h>

#include <string>

using board::IdKind;
using board::IdReading;
using board::idReadingOf;
using board::TargetProtocol;

namespace {

// The smallest raw value with 200 raw >= adcMaxRaw * edge200 (the edge in
// units of 0.005)
constexpr uint32_t firstRawAt(uint32_t edge200) {
   return (board::adcMaxRaw * edge200 + 199) / 200;
}

std::string vendor(IdReading adapter) {
   char text[32];
   board::formatAdapterVendor(text, "OpenMUPP", adapter);
   return text;
}

} // namespace

TEST_CASE("The ID windows: fault, slots 1 to 9, no adapter") {
   CHECK(idReadingOf(0) == IdReading{ IdKind::Fault, 0 });
   CHECK(idReadingOf(firstRawAt(10) - 1) == IdReading{ IdKind::Fault, 0 });
   CHECK(idReadingOf(firstRawAt(10)) == IdReading{ IdKind::Slot, 1 });
   for(uint8_t slot = 1; slot < board::idSlotCount; ++slot) {
      // The upper edge of the slot: 0.1 slot + 0.05
      const uint32_t edge = firstRawAt(10u + 20u * slot);
      const IdKind below = board::isFreeSlot(slot) ? IdKind::Unknown : IdKind::Slot;
      const auto next = static_cast<uint8_t>(slot + 1);
      const IdKind above = board::isFreeSlot(next) ? IdKind::Unknown : IdKind::Slot;
      CAPTURE(slot);
      CHECK(idReadingOf(edge - 1) == IdReading{ below, slot });
      CHECK(idReadingOf(edge) == IdReading{ above, next });
   }
   // x = 0.95 is still slot 9, above it no adapter
   const uint32_t top = board::adcMaxRaw * 190 / 200;
   CHECK(idReadingOf(top) == IdReading{ IdKind::Slot, 9 });
   CHECK(idReadingOf(top + 1) == IdReading{ IdKind::None, 0 });
   CHECK(idReadingOf(board::adcMaxRaw) == IdReading{ IdKind::None, 0 });
}

TEST_CASE("The ID resistors of the slot table read as their slot") {
   // R_ID with the 10 kOhm pull-up: x = R / (R + 10k), from the hardware README
   const uint32_t ohms[] = { 1100, 2400, 4300, 6800, 10000, 15000, 24000, 39000, 91000 };
   for(uint8_t slot = 1; slot <= board::idSlotCount; ++slot) {
      const uint32_t r = ohms[slot - 1];
      const uint32_t raw = board::adcMaxRaw * r / (r + 10000);
      CAPTURE(slot);
      CHECK(idReadingOf(raw).slot == slot);
   }
   // Board revision 1: R3 1 kOhm, R49 10 kOhm, x = 0.091
   CHECK(board::isKnownBoardRevision(idReadingOf(board::adcMaxRaw * 1000 / 11000)));
}

TEST_CASE("The free slots are unknown adapters") {
   CHECK(idReadingOf(board::adcMaxRaw * 6 / 10).kind == IdKind::Unknown);
   CHECK(idReadingOf(board::adcMaxRaw * 7 / 10).kind == IdKind::Unknown);
   CHECK(idReadingOf(board::adcMaxRaw * 8 / 10).kind == IdKind::Slot);
}

TEST_CASE("Only board ID slot 1 is a known revision") {
   CHECK(board::isKnownBoardRevision({ IdKind::Slot, 1 }));
   CHECK_FALSE(board::isKnownBoardRevision({ IdKind::Slot, 2 }));
   CHECK_FALSE(board::isKnownBoardRevision({ IdKind::Unknown, 6 }));
   CHECK_FALSE(board::isKnownBoardRevision({ IdKind::Fault, 0 }));
   CHECK_FALSE(board::isKnownBoardRevision({ IdKind::None, 0 }));
}

TEST_CASE("The adapter slots of the protocols") {
   const IdReading avr{ IdKind::Slot, 1 };
   const IdReading arm{ IdKind::Slot, 4 };
   const IdReading header{ IdKind::Slot, 5 };
   const IdReading pic{ IdKind::Slot, 9 };
   for(const auto protocol : { TargetProtocol::Swd, TargetProtocol::RvSwd }) {
      CHECK(board::adapterMatches(protocol, arm));
      CHECK(board::adapterMatches(protocol, header));
      CHECK_FALSE(board::adapterMatches(protocol, avr));
      CHECK_FALSE(board::adapterMatches(protocol, pic));
   }
   CHECK(board::adapterMatches(TargetProtocol::Jtag, avr));
   CHECK(board::adapterMatches(TargetProtocol::Jtag, arm));
   CHECK(board::adapterMatches(TargetProtocol::Jtag, header));
   CHECK(board::adapterMatches(TargetProtocol::Avr, avr));
   CHECK_FALSE(board::adapterMatches(TargetProtocol::Avr, arm));
   CHECK_FALSE(board::adapterMatches(TargetProtocol::Avr, header));
   // No match without a known adapter
   for(const IdReading other : { IdReading{ IdKind::Fault, 0 }, IdReading{ IdKind::None, 0 }, IdReading{ IdKind::Unknown, 6 } }) {
      CHECK_FALSE(board::adapterMatches(TargetProtocol::Jtag, other));
      // No protocol: never a mismatch
      CHECK(board::adapterMatches(TargetProtocol::None, other));
   }
}

TEST_CASE("VPP only on slots 8 and 9, and on no adapter with the setting") {
   for(uint8_t slot = 1; slot <= board::idSlotCount; ++slot) {
      const IdReading reading{ board::isFreeSlot(slot) ? IdKind::Unknown : IdKind::Slot, slot };
      CAPTURE(slot);
      const bool hv = slot == 8 || slot == 9;
      CHECK(board::isVppAllowed(reading, false) == hv);
      // The setting is for no adapter only
      CHECK(board::isVppAllowed(reading, true) == hv);
   }
   CHECK_FALSE(board::isVppAllowed({ IdKind::None, 0 }, false));
   CHECK(board::isVppAllowed({ IdKind::None, 0 }, true));
   CHECK_FALSE(board::isVppAllowed({ IdKind::Fault, 0 }, false));
   CHECK_FALSE(board::isVppAllowed({ IdKind::Fault, 0 }, true));
}

TEST_CASE("The DAP_Info vendor string shows the adapter") {
   CHECK(vendor({ IdKind::Slot, 4 }) == "OpenMUPP, adapter 4");
   CHECK(vendor({ IdKind::Unknown, 7 }) == "OpenMUPP, adapter 7 (unknown)");
   CHECK(vendor({ IdKind::None, 0 }) == "OpenMUPP, no adapter");
   CHECK(vendor({ IdKind::Fault, 0 }) == "OpenMUPP, adapter fault");
   // Cut to the buffer, with the NUL
   char shortText[12];
   board::formatAdapterVendor(shortText, "OpenMUPP", { IdKind::Slot, 4 });
   CHECK(std::string(shortText) == "OpenMUPP, a");
}

TEST_CASE("The target gate decision: board ID, ADC and adapter fault") {
   const IdReading arm{ IdKind::Slot, 4 };
   CHECK(board::isTargetAllowed(true, arm));
   CHECK(board::isTargetAllowed(true, IdReading{ IdKind::None, 0 }));
   CHECK(board::isTargetAllowed(true, IdReading{ IdKind::Unknown, 6 }));
   CHECK_FALSE(board::isTargetAllowed(true, IdReading{ IdKind::Fault, 0 }));
   // An unknown board revision, and an ADC that fails
   CHECK_FALSE(board::isTargetAllowed(false, arm));
   CHECK_FALSE(board::isTargetAllowed(true, std::nullopt));
}

TEST_CASE("VPP needs the gate decision and the VPP rule") {
   // As BoardTargetGate::isVppAllowed()
   const auto vpp = [](bool boardKnown, std::optional<IdReading> adapter, IdReading last, bool setting) {
      return board::isTargetAllowed(boardKnown, adapter) && board::isVppAllowed(last, setting);
   };
   const IdReading open{ IdKind::None, 0 };
   const IdReading pic{ IdKind::Slot, 9 };
   CHECK(vpp(true, open, open, true));
   CHECK(vpp(true, pic, pic, false));
   // Unknown board revision, pin 16 open, setting on
   CHECK_FALSE(vpp(false, open, open, true));
   CHECK_FALSE(vpp(false, pic, pic, false));
   // The ADC fails: the old reading (no adapter) does not allow VPP
   CHECK_FALSE(vpp(true, std::nullopt, open, true));
}

TEST_CASE("The kind codes of ID_DAP_Vendor4") {
   CHECK(board::idKindCode(IdKind::None) == 0);
   CHECK(board::idKindCode(IdKind::Slot) == 1);
   CHECK(board::idKindCode(IdKind::Fault) == 2);
   CHECK(board::idKindCode(IdKind::Unknown) == 3);
}
