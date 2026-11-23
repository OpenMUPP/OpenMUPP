// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The adapter ID and the board ID (board::IdSense): a divider with a 10 kOhm
// pull-up to 3.3 V and an ID resistor to GND. The ADC reads the ratio
// x = R_ID / (R_ID + 10 kOhm), the reading / adcMaxRaw: the ADC reference is
// the same 3.3 V. The slot table and the accept windows are in the README of
// the openmupp-v1 repository. Hardware-independent: the host unit tests
// check it. board/BoardIdSense.h reads the pins of board::current.

#include <cstddef>
#include <cstdint>

#include "board/VrefScale.h"

namespace board {

// Fault: x below 0.05 (pin shorted to GND). None: x above 0.95 (pin open,
// no adapter). Unknown: the window of a free slot (6, 7).
enum class IdKind : uint8_t { Fault, Slot, Unknown, None };

struct IdReading {
   IdKind kind;
   // 1 to 9 for Slot and Unknown, else 0
   uint8_t slot;

   constexpr bool operator==(const IdReading &) const = default;
};

inline constexpr uint8_t idSlotCount = 9;

// The slots with no adapter yet
constexpr bool isFreeSlot(uint8_t slot) {
   return slot == 6 || slot == 7;
}

// The reading of a mean ADC value. The window of slot n is 0.1 n - 0.05 to
// 0.1 n + 0.05. A value on the edge between two slots is in the higher slot.
// x = 0.05 is slot 1, x = 0.95 is slot 9.
constexpr IdReading idReadingOf(uint32_t raw) {
   // 200 x, compared with adcMaxRaw times the edge in units of 0.005
   const uint32_t scaled = raw * 200;
   if(scaled < adcMaxRaw * 10) {
      return { IdKind::Fault, 0 };
   }
   if(scaled > adcMaxRaw * 190) {
      return { IdKind::None, 0 };
   }
   uint8_t slot = 1;
   while(slot < idSlotCount && scaled >= adcMaxRaw * (10u + 20u * slot)) {
      ++slot;
   }
   return { isFreeSlot(slot) ? IdKind::Unknown : IdKind::Slot, slot };
}

// The board ID of the revisions that this firmware knows: revision 1 is
// slot 1 (R3 1 kOhm, x = 0.091).
constexpr bool isKnownBoardRevision(IdReading reading) {
   return reading == IdReading{ IdKind::Slot, 1 };
}

// The protocols of the engines, for the adapter check
enum class TargetProtocol : uint8_t { Swd, Jtag, RvSwd, AvrIsp, DebugWire };

constexpr const char *nameOf(TargetProtocol protocol) {
   switch(protocol) {
   case TargetProtocol::Swd:
      return "SWD";
   case TargetProtocol::Jtag:
      return "JTAG";
   case TargetProtocol::RvSwd:
      return "RISC-V wire";
   case TargetProtocol::AvrIsp:
      return "AVR ISP";
   case TargetProtocol::DebugWire:
      return "debugWIRE";
   }
   return "";
}

// True when the adapter has a connector for the protocol: slot 1 (AVR JTAG
// and ISP) has JTAG, ISP and debugWIRE. Slot 4 (Arm) and slot 5 (pin header)
// have SWD, JTAG and the RISC-V wire.
constexpr bool adapterMatches(TargetProtocol protocol, IdReading adapter) {
   if(adapter.kind != IdKind::Slot) {
      return false;
   }
   switch(protocol) {
   case TargetProtocol::Swd:
   case TargetProtocol::RvSwd:
      return adapter.slot == 4 || adapter.slot == 5;
   case TargetProtocol::Jtag:
      return adapter.slot == 1 || adapter.slot == 4 || adapter.slot == 5;
   case TargetProtocol::AvrIsp:
   case TargetProtocol::DebugWire:
      return adapter.slot == 1;
   }
   return false;
}

// The VPP rule (decision D2): VPP only on slot 8 (AVR high voltage) and
// slot 9 (PIC high voltage), and with no adapter (pin open) when the setting
// "VPP without an adapter" is on. Never on a fault or an unknown slot.
constexpr bool isVppAllowed(IdReading adapter, bool vppWithoutAdapter) {
   if(adapter.kind == IdKind::None) {
      return vppWithoutAdapter;
   }
   return adapter.kind == IdKind::Slot && (adapter.slot == 8 || adapter.slot == 9);
}

// The vendor string of DAP_Info with the adapter: "OpenMUPP, adapter 4",
// "OpenMUPP, adapter 6 (unknown)", "OpenMUPP, no adapter" or
// "OpenMUPP, adapter fault". The text is cut to the buffer and ends with a
// NUL. No printf: the firmware links no newlib stdio.
template <std::size_t N>
void formatAdapterVendor(char (&text)[N], const char *vendor, IdReading adapter) {
   static_assert(N > 0);
   std::size_t length = 0;
   const auto append = [&](const char *part) {
      for(; *part != '\0' && length + 1 < N; ++part) {
         text[length++] = *part;
      }
   };
   const char slot[] = { static_cast<char>('0' + adapter.slot), '\0' };
   append(vendor);
   switch(adapter.kind) {
   case IdKind::Fault:
      append(", adapter fault");
      break;
   case IdKind::Slot:
   case IdKind::Unknown:
      append(", adapter ");
      append(slot);
      if(adapter.kind == IdKind::Unknown) {
         append(" (unknown)");
      }
      break;
   case IdKind::None:
      append(", no adapter");
      break;
   }
   text[length] = '\0';
}

} // namespace board
