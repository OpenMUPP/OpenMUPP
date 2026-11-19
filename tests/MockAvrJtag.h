// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A model of the AVR JTAG port (docs/avr-jtag-ocd.md) for the mock JTAG
// pins (MockJtagPins.h): one TAP with the IEEE 1149.1 state machine and a
// 4-bit IR.
//
// - Capture-IR loads 0001. Test-Logic-Reset selects IDCODE.
// - IDCODE (0x1): 32 bits, the idcode member.
// - The other instructions have the DR length of docs/avr-jtag-ocd.md
//   ("JTAG instructions") and the datasheet. Capture-DR loads the value in
//   capture for the instruction, else the model value: the command result
//   for PROG_COMMANDS, the OCD results below, or 0. Unknown instructions
//   select BYPASS (1 bit, Capture-DR loads 0).
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
// The OCD (docs/avr-jtag-ocd.md), when ocden is true:
//
// - OCD register access (0xB, 21 bits): Update-DR runs the read or write,
//   and the next Capture-DR loads its result: register << 16 | data for a
//   read, the command itself for a write. Reg 0 to 3, BCR (8) and reg A, B,
//   E, F keep the written value. Reg 4 to 7 keep the low byte on revision
//   4, and are 0 on revision 3. Writes to the BSR (9), the OCDR (C) and reg
//   D are ignored. A read of the OCDR clears reg D bit 4. A read of the
//   empty OCDR stays pending: it takes the next byte that the core writes,
//   and reg D bit 4 does not come (hardware test, ATmega32U4).
// - Reg D: 0x000C while the core is stopped, else bit 0 = BCR bit 14. Bit
//   4 while the OCDR holds a byte.
// - FORCE_BREAK (0x8) at Update-IR stops the running core: BSR bit 1.
// - RUN (0x9) at Update-IR starts the stopped core at pc. With BCR bit 13
//   it executes one step: pc + 1 (the mock does not execute the flash),
//   BSR bit 7, and bit 13 clear. After an EIJMP since the stop, the core
//   starts at the PC field, modulo the flash words: the PC write of the
//   reference debugger (Z = PC - 11, then five instructions) then starts
//   at the PC (hardware test on revision 3).
// - AVR_RESET (0xC): the core is in reset while the bit is 1, and starts
//   at resetVector at the release, and stops there at once when a program
//   breakpoint matches it (see reach()). With ocdenFromFuse, the OCD
//   answers after the release only when the high fuse has OCDEN.
// - Program breakpoints (mode 3 for reg 2 and reg 3): reach(word) moves
//   the running core to word and stops it before that instruction when a
//   breakpoint matches: reg 0 (BCR bit 11, one-shot: the hit clears bit
//   11), reg 1 (bit 10), reg 2 (bit 8, not in mask mode), reg 3 (bit 7,
//   with reg 2 as the mask in mask mode). The BSR gets the bits of all
//   hits. Reg 4 to 7 compare with the word bits 23..16 (always 0 on
//   revision 3). Data breakpoints are not modeled.
// - stopDelay: the reg D reads after a stop that still show 0x0000, as
//   after FORCE_BREAK in the captures (70 ms).
// - Instruction execute (0xA, 38 bits on revision 4, 32 on revision 3):
//   the instruction in the high 16 bits, the field below it. At Update-DR
//   of the stopped core, a field of 0 loads the instruction, and any
//   other field reads the flash word at that word address. The next
//   rising TCK edge in Run-Test/Idle executes the loaded instruction.
//   Each other rising edge in Run-Test/Idle of the stopped core adds 2 to
//   the PC field (hardware test on revision 3, the same model for
//   revision 4). Capture-DR loads the last flash word read above the PC
//   field.
// - The PC field (docs/avr-jtag-ocd.md, "Instruction execute"): pc + 1 at
//   a stop, + 2 for each executed instruction, Z + 1 after EIJMP. The
//   field wraps at its bit count. Only EIJMP changes pc: pc = EIND:Z.
// - The core: LDI, MOV, IN, OUT, LD and ST with X, Y or Z, LPM Rd, Z (on
//   revision 3 it loads a byte of its own opcode, hardware test),
//   EIJMP, SPM, BREAK, WDR and NOP. Other instructions are recorded in
//   unknownInstructions. The data space has r0 to r31 at 0x00, the I/O
//   registers at 0x20 and the SRAM. A write to the I/O address
//   ocdDataReg goes to the OCDR and sets reg D bit 4.
// - SPM uses SPMCSR at the data address spmcsr and the byte address
//   RAMPZ:Z modulo the flash size: page erase (0x03), page buffer load of
//   r1:r0 (0x01), page write (0x05, AND as without an erase) and RWW
//   enable (0x11). Other commands record SPM in unknownInstructions.
//   SPMEN clears at once, but after a page erase or write it shows in the
//   next spmBusyReads reads of SPMCSR. SPM does nothing at a pc below bootStartWords (the pc of the
//   last EIJMP or stop), or with spmLocked (the boot lock bits).
// - EEPROM: a write to EECR (I/O address eecrIo) with EERE loads EEDR from
//   eeprom at EEARH:EEARL. EEPE while EEMPE is set writes EEDR to the
//   EEPROM (eepromWrites), and EEPE stays set for eepeBusyReads reads of
//   EECR. EERE and EEPE do not stay set otherwise. eepeIgnored: EEPE does
//   nothing, as when EEMPE timed out.
// - ocdrStuck: a write to the OCDR does not set reg D bit 4.
//
// TDO is the bit 0 of the shift register in Shift-DR and Shift-IR. The
// mock records the TAP state at each rising edge of TCK in Run-Test/Idle,
// so the tests see where a scan ends.

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
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
   [[nodiscard]] uint8_t drLength(uint8_t ir) const {
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
      case 0xA: // instruction execute
         return ocdRevision >= 4 ? 38 : 32;
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
         idle();
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
         } else if(ir == 0x5) {
            shift_ = result_;
         } else if(ir == 0xA) {
            shift_ = (static_cast<uint64_t>(flashRead_) << fieldBits()) | (pcField & fieldMask());
         } else if(ir == 0xB) {
            shift_ = ocdResult_;
         } else {
            shift_ = 0;
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
         updateIr();
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
   // The signature row of avrdude (sigrow, prodsig): the signature byte n
   // at 2n, the calibration byte n at 2n + 1, the serial number (sernum)
   // at 0x0E to 0x17. The ATmega16 and ATmega32 have four calibration
   // bytes.
   std::array<uint8_t, 0x20> sigrow = makeSigrow();
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

   // The OCD
   bool ocden{ true };
   // OCDEN follows the high fuse (bit 7, 0 = programmed) at the end of
   // each reset (the end of the programming mode)
   bool ocdenFromFuse{ false };
   uint8_t ocdRevision{ 3 };
   // The I/O address OCD_DATAREG and the data address of SPMCSR
   uint8_t ocdDataReg{ 0x31 };
   uint16_t spmcsr{ 0x57 };
   std::array<uint16_t, 16> ocdRegs{};
   uint16_t bsr{ 0 };
   uint8_t ocdr{ 0 };
   bool ocdrFull{ false };
   // The core: stopped in debug mode, its PC and the PC field (word
   // addresses)
   bool stopped{ false };
   uint32_t pc{ 0 };
   uint32_t pcField{ 0 };
   uint32_t resetVector{ 0 };
   uint32_t stopDelay{ 0 };
   // The EEPROM registers (I/O addresses of the ATmega32U4)
   uint8_t eecrIo{ 0x1F };
   uint8_t eedrIo{ 0x20 };
   uint8_t eearlIo{ 0x21 };
   uint8_t eearhIo{ 0x22 };
   uint32_t eepeBusyReads{ 0 };
   uint32_t eepromWrites{ 0 };
   bool eepeIgnored{ false };
   bool ocdrStuck{ false };
   // The PC field at each RUN
   std::vector<uint32_t> runFields;
   // r0 to r31, the I/O registers and the SRAM of the ATmega32U4
   std::vector<uint8_t> data = std::vector<uint8_t>(0x0B00, 0);
   std::vector<uint16_t> executed;
   std::vector<uint16_t> unknownInstructions;
   uint32_t spmPageErases{ 0 };
   uint32_t spmPageWrites{ 0 };
   uint32_t spmBusyReads{ 0 };
   uint32_t bootStartWords{ 0 };
   bool spmLocked{ false };

   // The running core comes to word: it stops there when a program
   // breakpoint matches. True: it stopped.
   bool reach(uint32_t word) {
      if(stopped || inReset || !ocden) {
         return false;
      }
      pc = word;
      breakAt(word);
      return stopped;
   }

   // Stops the core at word address at, as a breakpoint does.
   void stop(uint32_t at, uint16_t cause) {
      stopped = true;
      pc = at;
      pcField = at + 1;
      bsr = cause;
      hiddenStopReads_ = stopDelay;
      jumped_ = false;
   }

   uint8_t &reg(uint8_t n) {
      return data.at(n);
   }

   [[nodiscard]] uint16_t z() const {
      return static_cast<uint16_t>(data.at(31) << 8 | data.at(30));
   }

   private:
   static constexpr uint8_t eind = 0x3C;
   static constexpr uint8_t rampz = 0x3B;

   [[nodiscard]] uint8_t fieldBits() const {
      return static_cast<uint8_t>(drLength(0xA) - 16u);
   }

   [[nodiscard]] uint64_t fieldMask() const {
      return (uint64_t{ 1 } << fieldBits()) - 1u;
   }

   [[nodiscard]] uint16_t flashWordAt(uint32_t word) const {
      return word * 2 + 1 < flash.size() ? static_cast<uint16_t>(flash[word * 2 + 1] << 8 | flash[word * 2]) : 0xFFFF;
   }

   void updateIr() {
      if(!ocden || inReset) {
         return;
      }
      if(ir == 0x8 && !stopped) {
         stop(pc, 1u << 1);
      } else if(ir == 0x9 && stopped) {
         runFields.push_back(pcField);
         if(jumped_) {
            const auto words = static_cast<uint32_t>(flash.size() / 2);
            pc = pcField % words;
         }
         if((ocdRegs[8] & (1u << 13)) != 0) {
            ocdRegs[8] = static_cast<uint16_t>(ocdRegs[8] & ~(1u << 13));
            stop(pc + 1, 1u << 7);
         } else {
            stopped = false;
         }
      }
   }

   // The BSR bits of the program breakpoints that match word
   [[nodiscard]] uint16_t programBreak(uint32_t word) const {
      const uint16_t bcr = ocdRegs[8];
      const bool maskMode = (bcr & (1u << 9)) != 0;
      const auto matches = [&](uint8_t n, uint16_t mask) {
         return ((word ^ ocdRegs[n]) & mask) == 0 && (ocdRegs[4u + n] & 0xFFu) == ((word >> 16) & 0xFFu);
      };
      uint16_t cause = 0;
      if((bcr & (1u << 11)) != 0 && matches(0, 0xFFFF)) {
         cause |= 1u << 5;
      }
      if((bcr & (1u << 10)) != 0 && matches(1, 0xFFFF)) {
         cause |= 1u << 4;
      }
      if((bcr & (1u << 8)) != 0 && !maskMode && ((bcr >> 5) & 3u) == 3u && matches(2, 0xFFFF)) {
         cause |= 1u << 2;
      }
      if((bcr & (1u << 7)) != 0 && ((bcr >> 3) & 3u) == 3u && matches(3, maskMode ? ocdRegs[2] : 0xFFFF)) {
         cause |= 1u << 3;
      }
      return cause;
   }

   void breakAt(uint32_t word) {
      if(const uint16_t cause = programBreak(word); cause != 0) {
         if((cause & (1u << 5)) != 0) {
            ocdRegs[8] = static_cast<uint16_t>(ocdRegs[8] & ~(1u << 11));
         }
         stop(word, cause);
      }
   }

   [[nodiscard]] uint16_t controlValue() const {
      if(stopped) {
         return static_cast<uint16_t>(0x000C | (ocdrFull ? 0x10 : 0));
      }
      return static_cast<uint16_t>(((ocdRegs[8] >> 14) & 1u) | (ocdrFull ? 0x10 : 0));
   }

   void ocdAccess() {
      if(!ocden) {
         ocdResult_ = 0;
         return;
      }
      const auto n = static_cast<uint8_t>((shift_ >> 16) & 0xFu);
      const auto value = static_cast<uint16_t>(shift_);
      if((shift_ & (1u << 20)) != 0) {
         ocdResult_ = shift_;
         if(n >= 4 && n <= 7) {
            ocdRegs[n] = ocdRevision >= 4 ? static_cast<uint16_t>(value & 0xFFu) : 0;
         } else if(n != 9 && n != 0xC && n != 0xD) {
            ocdRegs[n] = value;
         }
         return;
      }
      uint16_t result = ocdRegs[n];
      if(n == 9) {
         result = bsr;
      } else if(n == 0xC) {
         result = static_cast<uint16_t>(ocdr << 8);
         ocdrPending_ = !ocdrFull;
         ocdrFull = false;
      } else if(n == 0xD) {
         result = controlValue();
         if(stopped && hiddenStopReads_ > 0) {
            --hiddenStopReads_;
            result = 0;
         }
      }
      ocdResult_ = static_cast<uint64_t>(n) << 16 | result;
   }

   void instruction() {
      if(!ocden || !stopped || inReset) {
         return;
      }
      const auto field = static_cast<uint32_t>(shift_ & fieldMask());
      if(field != 0) {
         // The address wraps at the flash size (hardware test).
         flashRead_ = flashWordAt(field % static_cast<uint32_t>(flash.size() / 2));
         return;
      }
      loaded_ = static_cast<uint16_t>(shift_ >> fieldBits());
   }

   // A rising TCK edge in Run-Test/Idle
   void idle() {
      if(!ocden || !stopped || inReset) {
         loaded_.reset();
         return;
      }
      if(loaded_) {
         const uint16_t op = *loaded_;
         loaded_.reset();
         execute(op);
         return;
      }
      pcField = static_cast<uint32_t>((pcField + 2) & fieldMask());
   }

   [[nodiscard]] uint8_t load(uint16_t address) {
      if(address == 0x20u + eecrIo && eepeBusy_ > 0) {
         --eepeBusy_;
         return static_cast<uint8_t>(data[address] | 0x02u);
      }
      if(address == spmcsr && spmBusy_ > 0) {
         --spmBusy_;
         return static_cast<uint8_t>(data[address] | 0x01u);
      }
      return address < data.size() ? data[address] : 0;
   }

   void store(uint16_t address, uint8_t value) {
      if(address == 0x20u + ocdDataReg) {
         ocdr = value;
         ocdrFull = !ocdrStuck && !ocdrPending_;
         ocdrPending_ = false;
      } else if(address == 0x20u + eecrIo) {
         eecr(value);
      } else if(address < data.size()) {
         data[address] = value;
      }
   }

   void eecr(uint8_t value) {
      const uint8_t io = 0x20;
      const auto address =
      static_cast<uint16_t>(eearhIo == eearlIo ? data[io + eearlIo] : data[io + eearhIo] << 8 | data[io + eearlIo]);
      const bool armed = (data[io + eecrIo] & 0x04u) != 0;
      if((value & 0x01u) != 0 && address < eeprom.size()) {
         data[io + eedrIo] = eeprom[address];
      }
      if((value & 0x02u) != 0 && armed && !eepeIgnored && address < eeprom.size()) {
         eeprom[address] = data[io + eedrIo];
         ++eepromWrites;
         eepeBusy_ = eepeBusyReads;
      }
      data[io + eecrIo] = static_cast<uint8_t>(value & ~0x03u);
   }

   [[nodiscard]] uint16_t pointer(uint8_t low) const {
      return static_cast<uint16_t>(data.at(low + 1u) << 8 | data.at(low));
   }

   void execute(uint16_t op) {
      executed.push_back(op);
      const auto d = static_cast<uint8_t>((op >> 4) & 0x1Fu);
      const auto io = static_cast<uint8_t>(((op >> 5) & 0x30u) | (op & 0x0Fu));
      if((op & 0xF000u) == 0xE000u) { // LDI
         reg(static_cast<uint8_t>(16 + (d & 0x0Fu))) = static_cast<uint8_t>(((op >> 4) & 0xF0u) | (op & 0x0Fu));
      } else if((op & 0xFC00u) == 0x2C00u) { // MOV
         reg(d) = reg(static_cast<uint8_t>(((op >> 5) & 0x10u) | (op & 0x0Fu)));
      } else if((op & 0xF800u) == 0xB000u) { // IN
         reg(d) = load(static_cast<uint16_t>(0x20u + io));
      } else if((op & 0xF800u) == 0xB800u) { // OUT
         store(static_cast<uint16_t>(0x20u + io), reg(d));
      } else if(const uint16_t ldst = op & 0xFE0Fu; ldst == 0x8000u || ldst == 0x8008u || ldst == 0x900Cu) { // LD
         reg(d) = load(pointer(ldst == 0x8000u ? 30 : ldst == 0x8008u ? 28 : 26));
      } else if(ldst == 0x8200u || ldst == 0x8208u || ldst == 0x920Cu) { // ST
         store(pointer(ldst == 0x8200u ? 30 : ldst == 0x8208u ? 28 : 26), reg(d));
      } else if((op & 0xFE0Fu) == 0x9004u) { // LPM Rd, Z
         // Revision 3 reads the injected opcode (hardware test).
         reg(d) = ocdRevision < 4    ? static_cast<uint8_t>((z() & 1u) != 0 ? op >> 8 : op) :
                  z() < flash.size() ? flash[z()] :
                                       0xFF;
      } else if(op == 0x9419u) { // EIJMP
         pc = static_cast<uint32_t>(load(0x20u + eind)) << 16 | z();
         pcField = static_cast<uint32_t>((pc + 1) & fieldMask());
         jumped_ = true;
         return;
      } else if(op == 0x95E8u) { // SPM
         spm();
      } else if(op != 0x9598u && op != 0x95A8u && op != 0x0000u) { // BREAK, WDR, NOP
         unknownInstructions.push_back(op);
      }
      pcField = static_cast<uint32_t>((pcField + 2) & fieldMask());
   }

   void spm() {
      const uint8_t command = data[spmcsr] & 0x1Fu;
      data[spmcsr] = static_cast<uint8_t>(data[spmcsr] & 0xE0u);
      if(spmLocked || pc < bootStartWords) {
         return;
      }
      // The address bits above the flash size have no effect.
      const uint32_t address = (static_cast<uint32_t>(load(0x20u + rampz)) << 16 | z()) % static_cast<uint32_t>(flash.size());
      const uint32_t page = address / flashPageBytes * flashPageBytes;
      if(command == 0x03) {
         ++spmPageErases;
         spmBusy_ = spmBusyReads;
         if(page + flashPageBytes <= flash.size()) {
            std::fill_n(flash.begin() + page, flashPageBytes, 0xFF);
         }
      } else if(command == 0x01) {
         spmBuffer_[address / 2] = static_cast<uint16_t>(reg(1) << 8 | reg(0));
      } else if(command == 0x05) {
         ++spmPageWrites;
         spmBusy_ = spmBusyReads;
         for(const auto &[word, value] : spmBuffer_) {
            if(word * 2 / flashPageBytes * flashPageBytes == page && word * 2 + 1 < flash.size()) {
               flash[word * 2] &= static_cast<uint8_t>(value);
               flash[word * 2 + 1] &= static_cast<uint8_t>(value >> 8);
            }
         }
         spmBuffer_.clear();
      } else if(command != 0x11) {
         unknownInstructions.push_back(0x95E8u);
      }
   }

   static std::array<uint8_t, 0x20> makeSigrow() {
      std::array<uint8_t, 0x20> row{};
      for(std::size_t i = 0; i < row.size(); ++i) {
         row[i] = static_cast<uint8_t>(0x40 + i);
      }
      const std::array<uint8_t, 4> fixed{ 0x1E, 0x95, 0x87, 0xFF };
      const std::array<uint8_t, 4> calibration{ 0x5A, 0xA1, 0xA2, 0xA3 };
      for(std::size_t i = 0; i < 4; ++i) {
         if(i < 3) {
            row[2 * i] = fixed[i];
         }
         row[2 * i + 1] = calibration[i];
      }
      return row;
   }

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
      case 0xC: {
         const bool wasInReset = inReset;
         inReset = (shift_ & 1u) != 0;
         if(inReset) {
            stopped = false;
         } else {
            programming = false;
            if(wasInReset && ocdenFromFuse) {
               ocden = (fuses[1] & 0x80u) == 0;
            }
            if(wasInReset) {
               pc = resetVector;
               if(ocden) {
                  breakAt(resetVector);
               }
            }
         }
         break;
      }
      case 0xA:
         instruction();
         break;
      case 0xB:
         ocdAccess();
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
         if(high == 0x32 || high == 0x36) {
            const std::size_t at = 2u * addressLow_ + (high == 0x36 ? 1u : 0u);
            readResult(at < sigrow.size() ? sigrow[at] : 0xFF);
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
   uint64_t ocdResult_{ 0 };
   uint32_t hiddenStopReads_{ 0 };
   bool ocdrPending_{ false };
   // The instruction of the last instruction execute scan, for the next
   // rising edge in Run-Test/Idle
   std::optional<uint16_t> loaded_;
   uint32_t eepeBusy_{ 0 };
   uint32_t spmBusy_{ 0 };
   bool jumped_{ false };
   uint16_t flashRead_{ 0 };
   std::map<uint32_t, uint16_t> spmBuffer_;
};

} // namespace mupptest
