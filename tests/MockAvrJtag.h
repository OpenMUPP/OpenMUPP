// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A model of the AVR JTAG port (docs/avr-jtag-ocd.md) for the mock JTAG
// pins (MockJtagPins.h): one TAP with the IEEE 1149.1 state machine and a
// 4-bit IR.
//
// - Capture-IR loads 0001. Test-Logic-Reset selects IDCODE.
// - IDCODE (0x1): 32 bits, the idcode member.
// - The other instructions have the DR length of docs/avr-jtag-ocd.md
//   ("JTAG instructions") and the datasheet. Capture-DR loads the capture
//   value of the instruction (0 by default). Unknown instructions select
//   BYPASS (1 bit, Capture-DR loads 0).
// - Each Update-IR and Update-DR is recorded.
//
// TDO is the bit 0 of the shift register in Shift-DR and Shift-IR. The
// mock records the TAP state at each rising edge of TCK in Run-Test/Idle,
// so the tests see where a scan ends.

#include <cstdint>
#include <map>
#include <vector>

#include "MockJtagPins.h"

namespace mupptest {

class MockAvrJtag final : public JtagTarget {
   public:
   enum class State : uint8_t {
      Reset,
      Idle,
      SelectDr,
      CaptureDr,
      ShiftDr,
      Exit1Dr,
      PauseDr,
      Exit2Dr,
      UpdateDr,
      SelectIr,
      CaptureIr,
      ShiftIr,
      Exit1Ir,
      PauseIr,
      Exit2Ir,
      UpdateIr,
   };

   // A DR scan at Update-DR: the IR, the length and the shifted-in data
   struct DrScan {
      uint8_t ir;
      uint8_t bits;
      uint64_t data;

      bool operator==(const DrScan &) const = default;
   };

   static constexpr uint8_t irLength = 4;
   static constexpr uint8_t irCapture = 0x1;

   // The DR length of an instruction
   static uint8_t drLength(uint8_t ir) {
      switch(ir) {
      case 0x1: // IDCODE
         return 32;
      case 0x4: // PROG_ENABLE
         return 16;
      case 0x5: // PROG_COMMANDS
         return 15;
      case 0x6: // PROG_PAGELOAD
      case 0x7: // PROG_PAGEREAD
         return 8;
      case 0xA: // instruction execute, revision 4
         return 38;
      case 0xB: // OCD register access
         return 21;
      case 0xC: // AVR_RESET
         return 1;
      default: // BYPASS, FORCE_BREAK, RUN
         return 1;
      }
   }

   void rising(bool tms, bool tdi) override {
      switch(state) {
      case State::Reset:
         ++resetClocks;
         break;
      case State::Idle:
         ++idleClocks;
         break;
      case State::CaptureIr:
         shift_ = irCapture;
         length_ = irLength;
         break;
      case State::CaptureDr:
         length_ = drLength(ir);
         if(ir == 0x1) {
            shift_ = idcode;
         } else {
            const auto it = capture.find(ir);
            shift_ = it != capture.end() ? it->second : 0;
         }
         break;
      case State::ShiftIr:
      case State::ShiftDr:
         shift_ = (shift_ >> 1) | (static_cast<uint64_t>(tdi) << (length_ - 1));
         break;
      default:
         break;
      }
      state = next(state, tms);
      if(state == State::Reset) {
         ir = 0x1;
      }
   }

   bool falling() override {
      if(state == State::UpdateIr) {
         ir = static_cast<uint8_t>(shift_);
         irScans.push_back(ir);
      } else if(state == State::UpdateDr) {
         drScans.push_back({ ir, length_, shift_ });
      }
      if(state == State::ShiftIr || state == State::ShiftDr) {
         tdo_ = (shift_ & 1u) != 0;
      }
      return tdo_;
   }

   State state{ State::Reset };
   uint8_t ir{ 0x1 };
   // An ATmega32U4: part 0x9587, the version is an example
   uint32_t idcode{ 0x4958703F };
   // The Capture-DR value of each instruction (not IDCODE)
   std::map<uint8_t, uint64_t> capture;
   std::vector<uint8_t> irScans;
   std::vector<DrScan> drScans;
   // Rising edges of TCK in Test-Logic-Reset and in Run-Test/Idle
   uint32_t resetClocks{ 0 };
   uint32_t idleClocks{ 0 };

   private:
   static State next(State state, bool tms) {
      switch(state) {
      case State::Reset:
         return tms ? State::Reset : State::Idle;
      case State::Idle:
         return tms ? State::SelectDr : State::Idle;
      case State::SelectDr:
         return tms ? State::SelectIr : State::CaptureDr;
      case State::CaptureDr:
      case State::ShiftDr:
         return tms ? State::Exit1Dr : State::ShiftDr;
      case State::Exit1Dr:
         return tms ? State::UpdateDr : State::PauseDr;
      case State::PauseDr:
         return tms ? State::Exit2Dr : State::PauseDr;
      case State::Exit2Dr:
         return tms ? State::UpdateDr : State::ShiftDr;
      case State::UpdateDr:
      case State::UpdateIr:
         return tms ? State::SelectDr : State::Idle;
      case State::SelectIr:
         return tms ? State::Reset : State::CaptureIr;
      case State::CaptureIr:
      case State::ShiftIr:
         return tms ? State::Exit1Ir : State::ShiftIr;
      case State::Exit1Ir:
         return tms ? State::UpdateIr : State::PauseIr;
      case State::PauseIr:
         return tms ? State::Exit2Ir : State::PauseIr;
      case State::Exit2Ir:
         return tms ? State::UpdateIr : State::ShiftIr;
      }
      return State::Reset;
   }

   uint64_t shift_{ 0 };
   uint8_t length_{ 1 };
   bool tdo_{ false };
};

} // namespace mupptest
