// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A mock AVR with a debugWIRE module, on the mock dW line
// (MockDebugWireTarget.h), for the dW target operations
// (avr/DebugWireTarget.h).
//
// The model has the registers, the data space (I/O and SRAM), flash with a
// page buffer, EEPROM, the CPU state (running or stopped, PC) and the dW
// registers: the PC register (0xD0), the breakpoint register (0xD1), the
// instruction register (0xD2), the mode (0xC2) and the context byte. It
// executes the dW commands of the header of avr/DebugWireTarget.h, and of
// the instructions only in, out, spm and adiw Z, 2 (others count in
// badInstructions).
//
// The model checks the rules that the real target has, so the tests see
// a wrong sequence:
// - 0x23 and 0x33 need a 0x64 context, 0x20 needs 0x66, go and step 0x60
//   or 0x61 (with or without bit 5).
// - Each executed instruction adds 1 to the PC register. A repeated
//   operation sets it to its end index.
// - A repeated data operation changes Y (r28, r29) and Z.
// - adiw changes SREG.
// - On a device with a boot section, SPM executes only when the PC register
//   is in the boot section.
// - EEPE writes the EEPROM only directly after EEMPE.
// - After a stop, the PC register is the stop address + 1.
//
// The running program: go starts it at the PC register. The program runs
// straight up from there: it stops at the first BREAK instruction (0x9598)
// in flash at or after the PC, or at the breakpoint of the breakpoint
// context (0x61), the first of the two, after runBits bit times, with a
// break and 0x55. Without them, it runs until a probe break and stops at
// runStopPc. A step on a BREAK instruction stops at the same address (the
// probe must write the original instruction first).

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "MockDebugWireTarget.h"

namespace mupptest {

class MockAvrDw {
   public:
   struct Device {
      uint16_t signature;
      uint32_t flashBytes;
      uint16_t pageBytes;
      // 0: no boot section
      uint16_t bootStartWords;
      uint16_t eepromBytes;
      // The data space size (registers, I/O, SRAM)
      uint32_t dataBytes;
      uint8_t dwdr;
      uint8_t spmcsr;
      uint8_t eearh;
      uint8_t eearl;
      uint8_t eecr;
      uint8_t eedr;
   };

   // ATmega328P: 32 KB flash, 128-byte pages, the smallest boot section
   // at 0x3F00 (words), 1 KB EEPROM, 2 KB SRAM
   static constexpr Device atmega328p{ 0x950F, 32768, 128, 0x3F00, 1024, 0x900, 0x31, 0x37, 0x22, 0x21, 0x1F, 0x20 };
   // ATtiny85: 8 KB flash, 64-byte pages, no boot section, 512 bytes
   // EEPROM, 512 bytes SRAM
   static constexpr Device attiny85{ 0x930B, 8192, 64, 0, 512, 0x260, 0x22, 0x37, 0x1F, 0x1E, 0x1C, 0x1D };

   MockAvrDw(MockDwTarget &line, const Device &device)
      : device(device)
      , flash(device.flashBytes, 0xFF)
      , eeprom(device.eepromBytes, 0xFF)
      , data(device.dataBytes, 0)
      , pageBuffer(device.pageBytes, 0xFF)
      , line_(line) {
      line_.onByte = [this](uint8_t value, uint64_t stopEnd) { receive(value, stopEnd); };
      line_.onBreak = [this](uint64_t end) { probeBreak(end); };
   }

   // The I/O register (I/O address)
   uint8_t &io(uint8_t address) {
      return data[0x20u + address];
   }

   Device device;
   std::array<uint8_t, 32> regs{};
   std::vector<uint8_t> flash;
   std::vector<uint8_t> eeprom;
   std::vector<uint8_t> data;
   std::vector<uint8_t> pageBuffer;
   bool running{ true };
   bool disabled{ false };
   // The CPU PC (word address)
   uint16_t pc{ 0x0100 };
   // Where a probe break stops the running program
   uint16_t runStopPc{ 0x0234 };
   // The time until the breakpoint stop (bit times)
   uint32_t runBits{ 200 };
   // The time until the answer of a 0x33 SPM (bit times)
   uint32_t spmBits{ 100 };
   // SPMCSR reads busy this count of times after a page operation (megaAVR)
   int spmBusyReads{ 2 };
   // EECR reads EEPE this count of times after an EEPROM write
   int eepromBusyReads{ 2 };
   // A 0x33 SPM gets no answer this count of times.
   int spmLostAnswers{ 0 };
   uint8_t context{ 0 };
   uint8_t mode{ 0 };
   uint16_t dwPc{ 0 };
   uint16_t dwBp{ 0 };
   uint16_t instruction{ 0 };
   bool timersRun{ false };

   // Statistics for the tests
   int badInstructions{ 0 };
   int badContexts{ 0 };
   int unknownCommands{ 0 };
   int spmOutsideBoot{ 0 };
   int pageErases{ 0 };
   int pageWrites{ 0 };
   int rwwEnables{ 0 };
   int resets{ 0 };
   int steps{ 0 };
   int gos{ 0 };
   int disables{ 0 };
   // Steps that executed a BREAK instruction
   int breakSteps{ 0 };
   // The PC and breakpoint of the last go, and if it had the breakpoint
   uint16_t goPc{ 0 };
   uint16_t goBreak{ 0 };
   bool goWithBreak{ false };
   // The registers and SREG at the last go or step
   std::array<uint8_t, 32> goRegs{};
   uint8_t goSreg{ 0 };

   private:
   enum class Sink : uint8_t { None, Args, Mode, InRegister, WriteRegisters, WriteData };

   [[nodiscard]] uint16_t z() const {
      return static_cast<uint16_t>(regs[30] | (regs[31] << 8));
   }
   void setZ(uint16_t value) {
      regs[30] = static_cast<uint8_t>(value);
      regs[31] = static_cast<uint8_t>(value >> 8);
   }

   // The data space: 0x00-0x1F are the registers.
   uint8_t readData(uint16_t address) {
      if(address < 0x20) {
         return regs[address];
      }
      return address < data.size() ? data[address] : 0;
   }
   void writeData(uint16_t address, uint8_t value) {
      if(address < 0x20) {
         regs[address] = value;
      } else if(address < data.size()) {
         data[address] = value;
      }
   }

   uint8_t readIo(uint8_t address) {
      if(address == device.spmcsr && spmBusy_ > 0) {
         --spmBusy_;
         return static_cast<uint8_t>(io(address) | 0x01u);
      }
      if(address == device.eecr && eepromBusy_ > 0) {
         --eepromBusy_;
         return static_cast<uint8_t>(io(address) | 0x02u);
      }
      return io(address);
   }

   [[nodiscard]] uint16_t eepromAddress() {
      const uint16_t high = device.eearh == device.eearl ? 0 : io(device.eearh);
      return static_cast<uint16_t>(((high << 8) | io(device.eearl)) % device.eepromBytes);
   }

   void writeIo(uint8_t address, uint8_t value) {
      const bool masterWrite = eepromMasterWrite_;
      eepromMasterWrite_ = false;
      if(address == device.eecr) {
         if((value & 0x01u) != 0) {
            io(device.eedr) = eeprom[eepromAddress()];
         }
         if((value & 0x04u) != 0) {
            eepromMasterWrite_ = true;
         }
         if((value & 0x02u) != 0 && masterWrite) {
            eeprom[eepromAddress()] = io(device.eedr);
            eepromBusy_ = eepromBusyReads;
            ++eepromWrites;
         }
         return;
      }
      io(address) = value;
   }

   public:
   int eepromWrites{ 0 };

   private:
   void send(uint8_t value) {
      line_.send(value, at_);
   }

   void stopAt(uint16_t stopPc, uint64_t time, bool withBreak) {
      running = false;
      pc = stopPc;
      dwPc = static_cast<uint16_t>(stopPc + 1u);
      if(withBreak) {
         line_.sendBreak(time);
      }
   }

   void probeBreak(uint64_t end) {
      if(disabled) {
         return;
      }
      if(running) {
         stopAt(runStopPc, end, false);
      }
      sink_ = Sink::None;
      line_.send(0x55, end + 2u * line_.bitCycles);
   }

   [[nodiscard]] bool isBreak(uint32_t word) const {
      return 2u * word + 1u < flash.size() && flash[2u * word] == 0x98 && flash[2u * word + 1u] == 0x95;
   }

   // The first BREAK at or after word, or none
   [[nodiscard]] std::optional<uint16_t> nextBreak(uint16_t word) const {
      for(uint32_t at = word; 2u * at < flash.size(); ++at) {
         if(isBreak(at)) {
            return static_cast<uint16_t>(at);
         }
      }
      return std::nullopt;
   }

   [[nodiscard]] bool contextIs(uint8_t value) const {
      return (context | 0x20u) == value;
   }

   void execute(bool spmAnswer) {
      ++dwPc;
      const uint16_t op = instruction;
      if((op & 0xF000u) == 0xB000u) {
         const auto reg = static_cast<uint8_t>((op >> 4) & 0x1Fu);
         const auto address = static_cast<uint8_t>(((op >> 5) & 0x30u) | (op & 0x0Fu));
         const bool out = (op & 0x0800u) != 0;
         if(address == device.dwdr) {
            if(out) {
               send(regs[reg]);
            } else {
               sink_ = Sink::InRegister;
               sinkIndex_ = reg;
            }
         } else if(out) {
            writeIo(address, regs[reg]);
         } else {
            regs[reg] = readIo(address);
         }
      } else if(op == 0x95E8u) {
         spm(spmAnswer);
      } else if(op == 0x9632u) {
         setZ(static_cast<uint16_t>(z() + 2u));
         io(0x3F) = 0x55;
      } else {
         ++badInstructions;
      }
   }

   void spm(bool answer) {
      // dwPc was incremented: the instruction executes at dwPc - 1.
      if(device.bootStartWords != 0 && static_cast<uint16_t>(dwPc - 1u) < device.bootStartWords) {
         ++spmOutsideBoot;
         return;
      }
      const uint8_t operation = io(device.spmcsr) & 0x1Fu;
      const uint16_t page = static_cast<uint16_t>(z() & ~(device.pageBytes - 1u));
      io(device.spmcsr) = 0;
      switch(operation) {
      case 0x01: {
         const uint16_t offset = static_cast<uint16_t>(z() & (device.pageBytes - 1u) & ~1u);
         pageBuffer[offset] = regs[0];
         pageBuffer[offset + 1u] = regs[1];
         break;
      }
      case 0x03:
         if(page < flash.size()) {
            std::fill(flash.begin() + page, flash.begin() + page + device.pageBytes, 0xFF);
         }
         ++pageErases;
         spmBusy_ = device.bootStartWords != 0 ? spmBusyReads : 0;
         break;
      case 0x05:
         if(page < flash.size()) {
            for(uint16_t i = 0; i < device.pageBytes; ++i) {
               flash[page + i] &= pageBuffer[i];
            }
         }
         std::fill(pageBuffer.begin(), pageBuffer.end(), 0xFF);
         ++pageWrites;
         spmBusy_ = device.bootStartWords != 0 ? spmBusyReads : 0;
         break;
      case 0x11:
         ++rwwEnables;
         break;
      default:
         ++badInstructions;
         break;
      }
      if(answer) {
         if(spmLostAnswers > 0) {
            --spmLostAnswers;
            return;
         }
         line_.sendBreak(at_ + spmBits * line_.bitCycles);
      }
   }

   void repeat() {
      const uint32_t count = dwBp > dwPc ? dwBp - dwPc : 0;
      switch(mode) {
      case 0x01:
         for(uint16_t i = dwPc; i < dwBp && i < 32; ++i) {
            send(regs[i]);
         }
         break;
      case 0x05:
         sink_ = Sink::WriteRegisters;
         sinkIndex_ = dwPc;
         sinkEnd_ = dwBp;
         break;
      case 0x00:
      case 0x02:
         for(uint32_t i = 0; i < count / 2u; ++i) {
            const uint16_t address = z();
            send(mode == 0x00 ? readData(address) : (address < flash.size() ? flash[address] : 0xFF));
            setZ(static_cast<uint16_t>(address + 1u));
         }
         if(mode == 0x00) {
            regs[28] = 0xEE;
            regs[29] = 0xEE;
         }
         break;
      case 0x04:
         sink_ = Sink::WriteData;
         sinkEnd_ = static_cast<uint16_t>(count / 2u);
         sinkIndex_ = 0;
         regs[28] = 0xEE;
         regs[29] = 0xEE;
         break;
      default:
         ++unknownCommands;
         break;
      }
      dwPc = dwBp;
   }

   void receive(uint8_t value, uint64_t stopEnd) {
      if(disabled) {
         return;
      }
      at_ = stopEnd + line_.bitCycles;
      switch(sink_) {
      case Sink::Args:
         args_[argCount_++] = value;
         if(argCount_ == 2) {
            sink_ = Sink::None;
            const auto word = static_cast<uint16_t>((args_[0] << 8) | args_[1]);
            if(argCommand_ == 0xD0) {
               dwPc = word;
            } else if(argCommand_ == 0xD1) {
               dwBp = word;
            } else {
               instruction = word;
            }
         }
         return;
      case Sink::Mode:
         mode = value;
         sink_ = Sink::None;
         return;
      case Sink::InRegister:
         regs[sinkIndex_] = value;
         sink_ = Sink::None;
         return;
      case Sink::WriteRegisters:
         if(sinkIndex_ < 32) {
            regs[sinkIndex_] = value;
         }
         if(++sinkIndex_ >= sinkEnd_) {
            sink_ = Sink::None;
         }
         return;
      case Sink::WriteData:
         writeData(z(), value);
         setZ(static_cast<uint16_t>(z() + 1u));
         if(++sinkIndex_ >= sinkEnd_) {
            sink_ = Sink::None;
         }
         return;
      case Sink::None:
         break;
      }
      if(running) {
         // A running target listens only for a break.
         ++unknownCommands;
         return;
      }
      command(value);
   }

   void command(uint8_t value) {
      switch(value) {
      case 0xD0:
      case 0xD1:
      case 0xD2:
         sink_ = Sink::Args;
         argCommand_ = value;
         argCount_ = 0;
         return;
      case 0xC2:
         sink_ = Sink::Mode;
         return;
      case 0x40:
      case 0x41:
      case 0x44:
      case 0x46:
      case 0x60:
      case 0x61:
      case 0x64:
      case 0x66:
         context = value;
         timersRun = (value & 0x20u) == 0;
         return;
      case 0x20:
         if(!contextIs(0x66)) {
            ++badContexts;
         }
         repeat();
         return;
      case 0x23:
      case 0x33:
         if(!contextIs(0x64)) {
            ++badContexts;
         }
         execute(value == 0x33);
         return;
      case 0x30:
         if(!contextIs(0x60) && !contextIs(0x61)) {
            ++badContexts;
         }
         ++gos;
         goPc = dwPc;
         goBreak = dwBp;
         goWithBreak = contextIs(0x61);
         goRegs = regs;
         goSreg = io(0x3F);
         pc = dwPc;
         running = true;
         if(const auto stop = nextBreak(dwPc); stop || goWithBreak) {
            const uint16_t where = stop && (!goWithBreak || *stop < dwBp || dwBp < dwPc) ? *stop : dwBp;
            stopAt(where, at_ + runBits * line_.bitCycles, true);
         }
         return;
      case 0x31:
         if(!contextIs(0x60)) {
            ++badContexts;
         }
         ++steps;
         goRegs = regs;
         goSreg = io(0x3F);
         if(isBreak(dwPc)) {
            ++breakSteps;
            stopAt(dwPc, at_, true);
            return;
         }
         stopAt(static_cast<uint16_t>(dwPc + 1u), at_, true);
         return;
      case 0x07:
         ++resets;
         regs.fill(0);
         std::fill(data.begin(), data.end(), 0);
         stopAt(0, at_ + 20u * line_.bitCycles, true);
         return;
      case 0x06:
         ++disables;
         disabled = true;
         return;
      case 0xF0:
         send(static_cast<uint8_t>(dwPc >> 8));
         send(static_cast<uint8_t>(dwPc));
         return;
      case 0xF3:
         send(static_cast<uint8_t>(device.signature >> 8));
         send(static_cast<uint8_t>(device.signature));
         return;
      default:
         ++unknownCommands;
         return;
      }
   }

   MockDwTarget &line_;
   uint64_t at_{ 0 };
   Sink sink_{ Sink::None };
   uint8_t argCommand_{ 0 };
   uint8_t argCount_{ 0 };
   std::array<uint8_t, 2> args_{};
   uint16_t sinkIndex_{ 0 };
   uint16_t sinkEnd_{ 0 };
   int spmBusy_{ 0 };
   int eepromBusy_{ 0 };
   bool eepromMasterWrite_{ false };
};

} // namespace mupptest
