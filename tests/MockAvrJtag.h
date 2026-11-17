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
//   value of the instruction (by default 0, or the command result for
//   PROG_COMMANDS). Unknown instructions select BYPASS (1 bit, Capture-DR
//   loads 0).
// - Each Update-IR and Update-DR is recorded.
//
// The JTAG programming interface (datasheet, "Programming via the JTAG
// Interface"):
//
// - AVR_RESET (0xC) holds the core in reset while its bit is 1.
// - PROG_ENABLE (0x4) with 0xA370 at Update-DR enters the programming
//   mode (only in reset), any other value leaves it.
// - PROG_COMMANDS (0x5): Update-DR runs the command of the datasheet
//   table in the programming mode, and the next Capture-DR loads its
//   result: a read byte in bits 7..0, and the ready bit 9. A write strobe
//   (bits 14..8 = 0x31, 0x35, 0x39) makes the next busyPolls results not
//   ready. A flash page write programs the page buffer into the flash
//   (AND, as without an erase). Unknown commands are recorded in
//   unknownCommands.
// - PROG_PAGELOAD (0x6) with fullPageBitstream: each 8 bits in Shift-DR go
//   into the page buffer, low byte then high byte of each word, from the
//   loaded address. Without it the DR is 8 bits and does nothing.
//
// TDO is the bit 0 of the shift register in Shift-DR and Shift-IR. The
// mock records the TAP state at each rising edge of TCK in Run-Test/Idle,
// so the tests see where a scan ends.

#include <algorithm>
#include <array>
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
         } else if(const auto it = capture.find(ir); it != capture.end()) {
            shift_ = it->second;
         } else {
            shift_ = ir == 0x5 ? result_ : 0;
         }
         pageLoadBits_ = 0;
         break;
      case State::ShiftIr:
      case State::ShiftDr:
         shift_ = (shift_ >> 1) | (static_cast<uint64_t>(tdi) << (length_ - 1));
         if(state == State::ShiftDr && ir == 0x6 && fullPageBitstream) {
            pageLoadBit(tdi);
         }
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
         updateDr();
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

   // The programming interface
   bool inReset{ false };
   bool programming{ false };
   bool fullPageBitstream{ false };
   uint16_t flashPageBytes{ 128 };
   std::vector<uint8_t> flash = std::vector<uint8_t>(32768, 0xFF);
   std::vector<uint8_t> eeprom = std::vector<uint8_t>(1024, 0xFF);
   // Low, high, extended. EESAVE (high fuse bit 3, 0 = programmed) keeps
   // the EEPROM at a chip erase.
   std::array<uint8_t, 3> fuses{ 0x5E, 0x99, 0xF3 };
   uint8_t lockBits{ 0xFF };
   std::array<uint8_t, 3> signature{ 0x1E, 0x95, 0x87 };
   // The ATmega16 and ATmega32 have four calibration bytes.
   std::array<uint8_t, 4> calibration{ 0x5A, 0xA1, 0xA2, 0xA3 };
   // Results that are not ready after each write strobe
   uint32_t busyPolls{ 3 };
   // The ready bit never comes.
   bool stuckBusy{ false };
   // The commands at Update-DR in the programming mode
   std::vector<uint16_t> commands;
   std::vector<uint16_t> unknownCommands;
   uint32_t chipErases{ 0 };
   uint32_t flashPageWrites{ 0 };
   uint32_t eepromPageWrites{ 0 };

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

   // The operations of the "enter" commands (0x23xx)
   enum class Mode : uint8_t {
      None,
      ChipErase,
      FlashWrite,
      FlashRead,
      EepromWrite,
      EepromRead,
      FuseWrite,
      LockWrite,
      FuseLockRead,
      SignatureRead
   };

   [[nodiscard]] uint32_t flashWord() const {
      return (static_cast<uint32_t>(addressExtended_) << 16) | (static_cast<uint32_t>(addressHigh_) << 8) | addressLow_;
   }

   void updateDr() {
      switch(ir) {
      case 0xC:
         inReset = (shift_ & 1u) != 0;
         if(!inReset) {
            programming = false;
         }
         break;
      case 0x4:
         programming = inReset && shift_ == 0xA370;
         break;
      case 0x5:
         if(programming) {
            command(static_cast<uint16_t>(shift_));
         }
         break;
      default:
         break;
      }
   }

   void pageLoadBit(bool tdi) {
      byte_ = static_cast<uint8_t>((byte_ >> 1) | (tdi ? 0x80u : 0u));
      if(++pageLoadBits_ % 8 != 0) {
         return;
      }
      const uint32_t index = pageLoadBits_ / 8 - 1;
      const uint32_t word = flashWord() + index / 2;
      uint16_t &slot = pageBuffer_.try_emplace(word, 0xFFFF).first->second;
      slot = index % 2 == 0 ? static_cast<uint16_t>((slot & 0xFF00u) | byte_) :
                              static_cast<uint16_t>((slot & 0x00FFu) | (byte_ << 8));
   }

   void readResult(uint8_t value) {
      result_ = static_cast<uint16_t>(0x0200u | value);
   }

   void strobe() {
      pending_ = busyPolls;
   }

   void command(uint16_t value) {
      commands.push_back(value);
      const auto high = static_cast<uint8_t>(value >> 8);
      const auto low = static_cast<uint8_t>(value);
      // The result of a command that reads nothing: the ready bit
      const bool busy = stuckBusy || pending_ != 0;
      if(pending_ != 0) {
         --pending_;
      }
      result_ = busy ? 0 : 0x0200u;
      switch(high) {
      case 0x23:
         enter(low);
         return;
      case 0x0B:
         addressExtended_ = low;
         return;
      case 0x07:
         addressHigh_ = low;
         return;
      case 0x03:
         addressLow_ = low;
         return;
      case 0x13:
         dataLow_ = low;
         return;
      case 0x17:
         dataHigh_ = low;
         return;
      case 0x77:
         latch();
         return;
      case 0x31:
      case 0x35:
      case 0x39:
         write(high);
         return;
      case 0x32:
      case 0x36:
      case 0x3A:
      case 0x3E:
         read(high);
         return;
      case 0x33:
         if(mode_ == Mode::EepromRead) {
            // 5d: the address low byte again
            return;
         }
         [[fallthrough]];
      case 0x37:
      case 0x3B:
      case 0x3F:
         if(low != 0 && !(mode_ == Mode::ChipErase && value == 0x3380)) {
            unknownCommands.push_back(value);
         }
         return;
      default:
         unknownCommands.push_back(value);
         return;
      }
   }

   void enter(uint8_t low) {
      switch(low) {
      case 0x80:
         mode_ = Mode::ChipErase;
         break;
      case 0x10:
         mode_ = Mode::FlashWrite;
         break;
      case 0x02:
         mode_ = Mode::FlashRead;
         break;
      case 0x11:
         mode_ = Mode::EepromWrite;
         break;
      case 0x03:
         mode_ = Mode::EepromRead;
         break;
      case 0x40:
         mode_ = Mode::FuseWrite;
         break;
      case 0x20:
         mode_ = Mode::LockWrite;
         break;
      case 0x04:
         mode_ = Mode::FuseLockRead;
         break;
      case 0x08:
         mode_ = Mode::SignatureRead;
         break;
      case 0x00:
         mode_ = Mode::None;
         break;
      default:
         unknownCommands.push_back(static_cast<uint16_t>(0x2300u | low));
         break;
      }
   }

   void latch() {
      if(mode_ == Mode::FlashWrite) {
         pageBuffer_[flashWord()] = static_cast<uint16_t>((dataHigh_ << 8) | dataLow_);
      } else if(mode_ == Mode::EepromWrite) {
         eepromBuffer_[static_cast<uint16_t>((addressHigh_ << 8) | addressLow_)] = dataLow_;
      } else {
         unknownCommands.push_back(0x7700);
      }
   }

   void write(uint8_t high) {
      strobe();
      if(mode_ == Mode::ChipErase && high == 0x31) {
         ++chipErases;
         std::fill(flash.begin(), flash.end(), 0xFF);
         if((fuses[1] & 0x08u) != 0) {
            std::fill(eeprom.begin(), eeprom.end(), 0xFF);
         }
         lockBits = 0xFF;
      } else if(mode_ == Mode::FlashWrite && high == 0x35) {
         ++flashPageWrites;
         const uint32_t pageWords = flashPageBytes / 2u;
         const uint32_t first = flashWord() / pageWords * pageWords;
         for(const auto &[word, value] : pageBuffer_) {
            if(word / pageWords * pageWords == first && word * 2 + 1 < flash.size()) {
               flash[word * 2] &= static_cast<uint8_t>(value);
               flash[word * 2 + 1] &= static_cast<uint8_t>(value >> 8);
            }
         }
         pageBuffer_.clear();
      } else if(mode_ == Mode::EepromWrite && high == 0x31) {
         ++eepromPageWrites;
         for(const auto &[address, value] : eepromBuffer_) {
            eeprom.at(address) = value;
         }
         eepromBuffer_.clear();
      } else if(mode_ == Mode::FuseWrite) {
         fuses[high == 0x31 ? 0 : high == 0x35 ? 1 : 2] = dataLow_;
      } else if(mode_ == Mode::LockWrite && high == 0x31) {
         lockBits &= dataLow_;
      } else {
         unknownCommands.push_back(static_cast<uint16_t>(high << 8));
      }
   }

   void read(uint8_t high) {
      const uint32_t word = flashWord();
      switch(mode_) {
      case Mode::FlashRead:
         if(high == 0x32 || high == 0x36) {
            readResult(flash.at(word * 2 + (high == 0x36 ? 1 : 0)));
            return;
         }
         break;
      case Mode::EepromRead:
         if(high == 0x32) {
            readResult(eeprom.at(static_cast<uint16_t>((addressHigh_ << 8) | addressLow_)));
            return;
         }
         break;
      case Mode::FuseLockRead:
         readResult(high == 0x32 ? fuses[0] : high == 0x3E ? fuses[1] : high == 0x3A ? fuses[2] : lockBits);
         return;
      case Mode::SignatureRead:
         if(high == 0x32) {
            readResult(addressLow_ < 3 ? signature[addressLow_] : 0xFF);
            return;
         }
         if(high == 0x36) {
            readResult(addressLow_ < calibration.size() ? calibration[addressLow_] : 0xFF);
            return;
         }
         break;
      default:
         break;
      }
      unknownCommands.push_back(static_cast<uint16_t>(high << 8));
   }

   uint64_t shift_{ 0 };
   uint8_t length_{ 1 };
   bool tdo_{ false };
   Mode mode_{ Mode::None };
   uint8_t addressExtended_{ 0 };
   uint8_t addressHigh_{ 0 };
   uint8_t addressLow_{ 0 };
   uint8_t dataLow_{ 0xFF };
   uint8_t dataHigh_{ 0xFF };
   uint16_t result_{ 0 };
   uint32_t pending_{ 0 };
   std::map<uint32_t, uint16_t> pageBuffer_;
   std::map<uint16_t, uint8_t> eepromBuffer_;
   uint32_t pageLoadBits_{ 0 };
   uint8_t byte_{ 0 };
};

} // namespace mupptest
