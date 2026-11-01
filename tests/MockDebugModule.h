// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A mock RISC-V debug module (debug spec 0.13) of one hart, behind the
// registers of a MockRvSwdTarget. It models the parts that the debug
// module layer (rvswd/DebugModule.h) uses, with the register values of
// the WCH-LinkE captures (docs/wch-link-protocol.md):
//
// - dmcontrol: dmactive, haltreq, resumereq. dmactive 0 resets the DM,
//   and the DM ignores writes to the registers below 0x40.
// - dmstatus: version 2, authenticated, halted or running, resumeack.
// - abstractcs: progbufsize 8, datacount 2, busy, cmderr (write 1 to
//   clear). A command while cmderr is not 0 does nothing, as the spec says.
// - command: access register (32 bits, transfer, postexec) and access
//   memory (8, 16, 32 bits). Both need a halted hart, else cmderr 4.
// - The program buffer runs on a small RV32EC interpreter. It has the
//   instructions of the programs of the module only: csrrw, csrrs, lui,
//   lw, sw, sh, c.lw, c.sw, c.ebreak. data0 and data1 are in memory at
//   0xe00000f4 and 0xe00000f8, as on the CH32V003. Another instruction,
//   or a program without ebreak, gives cmderr 3 (exception).
// - A memory access at a misaligned address or at an address of
//   faultAddresses faults, and a load at an address of faultLoadAddresses. In the program buffer, the program stops at
//   the faulting instruction with cmderr 3, and the registers keep the
//   values of the instructions before it, as on a real hart. The abstract
//   memory command gives cmderr 5 (bus error).
// - resumereq clears resumeack, and resumes a halted hart only (debug
//   spec 0.13). A halted hart then sets resumeack, and onResume runs: it
//   can model code that the hart runs, and halt the hart again.
// - ndmreset 1 calls onSystemReset. With haltreq, the hart halts.
// - onLoad and onStore model memory-mapped registers (see MockWchFlash.h).

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "MockRvSwdTarget.h"
#include "rvswd/DebugModule.h"

namespace mupptest {

class MockDebugModule {
   public:
   static constexpr uint32_t data0Address = 0xe000'00f4;
   static constexpr uint32_t data1Address = 0xe000'00f8;

   explicit MockDebugModule(MockRvSwdTarget &target)
      : target_(target) {
      target_.onRead = [this](uint8_t address) { readHook(address); };
      target_.onWrite = [this](uint8_t address) { writeHook(address); };
      reset();
      target_.registers[rvswd::dm::chipId] = 0x0030'0500;
      target_.registers[rvswd::dm::hartinfo] = 0x0021'20f4;
   }

   // The hart

   bool halted{ false };
   bool resumeAck{ false };
   // The hart does not halt (no ebreak to debug mode, a lock).
   bool neverHalts{ false };
   // The reads of abstractcs with busy 1 after each command
   uint32_t busyReads{ 0 };
   // The next command fails with this cmderr (0: none).
   uint32_t failNext{ 0 };
   // x0-x15 (RV32E)
   std::array<uint32_t, 16> gprs{};
   std::map<uint16_t, uint32_t> csrs;
   // Little-endian bytes. A missing byte reads 0.
   std::map<uint32_t, uint8_t> memory;
   // An access that touches one of these addresses faults.
   std::set<uint32_t> faultAddresses;
   // A load that touches one of these addresses faults. Stores work.
   std::set<uint32_t> faultLoadAddresses;
   // The commands that started
   std::vector<uint32_t> commands;
   int programRuns{ 0 };
   int resumes{ 0 };
   int systemResets{ 0 };

   // A register value for a load, or no value for memory
   std::function<std::optional<uint32_t>(uint32_t address, uint32_t bytes)> onLoad;
   // Returns true when it took the store, false for memory.
   std::function<bool(uint32_t address, uint32_t value, uint32_t bytes)> onStore;
   std::function<void()> onResume;
   std::function<void()> onSystemReset;

   uint32_t load(uint32_t address, uint32_t bytes) {
      if(address == data0Address && bytes == 4) {
         return target_.registers[rvswd::dm::data0];
      }
      if(address == data1Address && bytes == 4) {
         return target_.registers[rvswd::dm::data1];
      }
      if(onLoad) {
         if(const auto value = onLoad(address, bytes)) {
            return *value;
         }
      }
      uint32_t value = 0;
      for(uint32_t i = bytes; i > 0; --i) {
         const auto byte = memory.find(address + i - 1);
         value = value << 8 | (byte == memory.end() ? 0u : byte->second);
      }
      return value;
   }

   void store(uint32_t address, uint32_t value, uint32_t bytes) {
      if(address == data0Address && bytes == 4) {
         target_.registers[rvswd::dm::data0] = value;
         return;
      }
      if(address == data1Address && bytes == 4) {
         target_.registers[rvswd::dm::data1] = value;
         return;
      }
      if(onStore && onStore(address, value, bytes)) {
         return;
      }
      for(uint32_t i = 0; i < bytes; ++i) {
         memory[address + i] = static_cast<uint8_t>(value >> (8 * i));
      }
   }

   // The running abstract command stays busy for this many abstractcs reads
   // (a store that starts a long flash operation, for example).
   void holdBusy(uint32_t reads) {
      busy_ = reads;
      update();
   }

   [[nodiscard]] bool active() const {
      return active_;
   }
   [[nodiscard]] uint32_t cmderr() const {
      return cmderr_;
   }
   void setCmderr(uint32_t value) {
      cmderr_ = value;
      update();
   }

   private:
   // dmactive 0: the DM state is reset, the hart is not.
   void reset() {
      active_ = false;
      cmderr_ = 0;
      busy_ = 0;
      for(uint32_t i = 0; i < 16; ++i) {
         target_.registers[rvswd::dm::progbuf0 + i] = 0;
      }
      target_.registers[rvswd::dm::abstractauto] = 0;
      target_.registers[rvswd::dm::data0] = 0;
      target_.registers[rvswd::dm::data1] = 0;
      target_.registers[rvswd::dm::dmcontrol] = 0;
      update();
   }

   // The read values of dmstatus and abstractcs
   void update() {
      uint32_t status = rvswd::dm::version013 | 1u << 7;
      status |= halted ? 0x3u << 8 : 0x3u << 10;
      status |= resumeAck ? 0x3u << 16 : 0;
      target_.registers[rvswd::dm::dmstatus] = status;
      target_.registers[rvswd::dm::abstractcs] =
      8u << 24 | (busy_ > 0 ? rvswd::dm::busy : 0) | cmderr_ << rvswd::dm::cmderrShift | 2u;
   }

   void readHook(uint8_t address) {
      if(address == rvswd::dm::abstractcs) {
         update();
         if(busy_ > 0) {
            --busy_;
         }
      }
   }

   void writeHook(uint8_t address) {
      uint32_t &value = target_.registers[address];
      if(address == rvswd::dm::dmcontrol) {
         control(value);
      } else if(!active_ && address < 0x40) {
         // The DM ignores its other registers while dmactive is 0. The
         // WCH registers (0x7c-0x7f) take writes.
         value = 0;
      } else if(address == rvswd::dm::abstractcs) {
         cmderr_ &= ~(value >> rvswd::dm::cmderrShift & rvswd::dm::cmderrMask);
      } else if(address == rvswd::dm::command) {
         command(value);
      }
      update();
   }

   void control(uint32_t &value) {
      if((value & rvswd::dm::dmactive) == 0) {
         reset();
         return;
      }
      active_ = true;
      if((value & rvswd::dm::ndmreset) != 0) {
         ++systemResets;
         if(onSystemReset) {
            onSystemReset();
         }
      }
      if((value & rvswd::dm::haltreq) != 0) {
         resumeAck = false;
         if(!neverHalts) {
            halted = true;
         }
      } else if((value & rvswd::dm::resumereq) != 0) {
         const bool wasHalted = halted;
         resumeAck = halted;
         halted = false;
         if(wasHalted) {
            ++resumes;
            if(onResume) {
               onResume();
            }
         }
      }
      // resumereq reads 0.
      value &= ~rvswd::dm::resumereq;
   }

   void command(uint32_t value) {
      if(busy_ > 0) {
         cmderr_ = 1;
         return;
      }
      if(cmderr_ != 0) {
         return;
      }
      commands.push_back(value);
      busy_ = busyReads;
      if(failNext != 0) {
         cmderr_ = failNext;
         failNext = 0;
         return;
      }
      if(!halted) {
         cmderr_ = 4;
         return;
      }
      const uint32_t type = value >> 24;
      if(type == 0) {
         accessRegister(value);
      } else if(type == 2) {
         accessMemory(value);
      } else {
         cmderr_ = 2;
      }
   }

   void accessRegister(uint32_t value) {
      const uint32_t size = value >> 20 & 0x7;
      const bool postexec = (value & 1u << 18) != 0;
      const bool transfer = (value & 1u << 17) != 0;
      const bool write = (value & 1u << 16) != 0;
      const auto regno = static_cast<uint16_t>(value & 0xFFFF);
      if(transfer) {
         if(size != 2) {
            cmderr_ = 2;
            return;
         }
         uint32_t &data = target_.registers[rvswd::dm::data0];
         if(regno >= 0x1000 && regno < 0x1010) {
            if(write) {
               gprs[regno - 0x1000] = data;
            } else {
               data = gprs[regno - 0x1000];
            }
         } else if(regno < 0x1000) {
            if(write) {
               csrs[regno] = data;
            } else {
               data = csrs[regno];
            }
         } else {
            cmderr_ = 3;
            return;
         }
      }
      if(postexec) {
         run();
      }
   }

   void accessMemory(uint32_t value) {
      const uint32_t size = value >> 20 & 0x7;
      if(size > 2 || (value & (1u << 23 | 1u << 19)) != 0) {
         // Wider than 32 bits, or postincrement or a virtual address
         cmderr_ = 2;
         return;
      }
      const uint32_t bytes = 1u << size;
      const uint32_t address = target_.registers[rvswd::dm::data1];
      uint32_t &data = target_.registers[rvswd::dm::data0];
      if(faults(address, bytes, (value & 1u << 16) != 0)) {
         cmderr_ = 5;
         return;
      }
      if((value & 1u << 16) != 0) {
         store(address, data, bytes);
      } else {
         data = load(address, bytes);
      }
   }

   [[nodiscard]] bool faults(uint32_t address, uint32_t bytes, bool isStore) const {
      if(address % bytes != 0) {
         return true;
      }
      for(uint32_t i = 0; i < bytes; ++i) {
         if(faultAddresses.count(address + i) != 0 || (!isStore && faultLoadAddresses.count(address + i) != 0)) {
            return true;
         }
      }
      return false;
   }

   // Returns false after an exception.
   bool loadOp(uint32_t rd, uint32_t address) {
      if(faults(address, 4, false)) {
         exception();
         return false;
      }
      gpr(rd) = load(address, 4);
      return true;
   }
   bool storeOp(uint32_t address, uint32_t value, uint32_t bytes = 4) {
      if(faults(address, bytes, true)) {
         exception();
         return false;
      }
      store(address, value & (bytes == 4 ? 0xFFFF'FFFFu : 0xFFFFu), bytes);
      return true;
   }

   // The program buffer, as bytes
   [[nodiscard]] uint16_t half(uint32_t pc) const {
      const uint32_t word = target_.registers[rvswd::dm::progbuf0 + pc / 4];
      return static_cast<uint16_t>(pc % 4 == 0 ? word : word >> 16);
   }

   uint32_t &gpr(uint32_t index) {
      // x0 reads 0: a write to it goes to a scratch value.
      scratch_ = 0;
      return index == 0 ? scratch_ : gprs[index];
   }

   void exception() {
      cmderr_ = 3;
   }

   void run() {
      ++programRuns;
      const uint32_t end = 4 * rvswd::programWords;
      for(uint32_t pc = 0; pc + 2 <= end;) {
         const uint16_t low = half(pc);
         if((low & 0x3) != 0x3) {
            if(!compressed(low)) {
               return;
            }
            if(low == 0x9002) {
               return;
            }
            pc += 2;
            continue;
         }
         if(pc + 4 > end) {
            break;
         }
         const uint32_t insn = low | static_cast<uint32_t>(half(pc + 2)) << 16;
         if(insn == 0x0010'0073) {
            // ebreak
            return;
         }
         if(!full(insn)) {
            return;
         }
         pc += 4;
      }
      // No ebreak
      exception();
   }

   // Returns false after an exception.
   bool compressed(uint16_t insn) {
      if(insn == 0x9002) {
         return true;
      }
      const uint32_t funct3 = insn >> 13;
      const uint32_t rd = 8 + (insn >> 2 & 0x7);
      const uint32_t rs1 = 8 + (insn >> 7 & 0x7);
      const uint32_t offset = (insn >> 7 & 0x38) | (insn >> 4 & 0x4) | (insn << 1 & 0x40);
      if((insn & 0x3) != 0 || rd > 15 || rs1 > 15) {
         exception();
         return false;
      }
      if(funct3 == 0b010) {
         return loadOp(rd, gprs[rs1] + offset);
      }
      if(funct3 == 0b110) {
         return storeOp(gprs[rs1] + offset, gprs[rd]);
      }
      exception();
      return false;
   }

   // Returns false after an exception.
   bool full(uint32_t insn) {
      const uint32_t opcode = insn & 0x7F;
      const uint32_t rd = insn >> 7 & 0x1F;
      const uint32_t funct3 = insn >> 12 & 0x7;
      const uint32_t rs1 = insn >> 15 & 0x1F;
      const uint32_t rs2 = insn >> 20 & 0x1F;
      // A store has rs2 and no rd.
      const bool isStore = opcode == 0x23;
      if(rs1 > 15 || (isStore ? rs2 > 15 : rd > 15)) {
         exception();
         return false;
      }
      const auto immI = static_cast<uint32_t>(static_cast<int32_t>(insn) >> 20);
      if(opcode == 0x37) {
         gpr(rd) = insn & 0xFFFF'F000u;
         return true;
      }
      if(opcode == 0x03 && funct3 == 0b010) {
         return loadOp(rd, gprs[rs1] + immI);
      }
      if(opcode == 0x23 && (funct3 == 0b010 || funct3 == 0b001)) {
         const auto immS = static_cast<uint32_t>(static_cast<int32_t>(insn & 0xFE00'0000u) >> 20) | (insn >> 7 & 0x1F);
         return storeOp(gprs[rs1] + immS, gprs[rs2], funct3 == 0b010 ? 4 : 2);
      }
      if(opcode == 0x73 && (funct3 == 0b001 || funct3 == 0b010)) {
         const auto csr = static_cast<uint16_t>(insn >> 20);
         const uint32_t old = csrs[csr];
         const uint32_t source = gprs[rs1];
         if(funct3 == 0b001) {
            csrs[csr] = source;
         } else if(rs1 != 0) {
            csrs[csr] = old | source;
         }
         gpr(rd) = old;
         return true;
      }
      exception();
      return false;
   }

   MockRvSwdTarget &target_;
   bool active_{ false };
   uint32_t cmderr_{ 0 };
   uint32_t busy_{ 0 };
   uint32_t scratch_{ 0 };
};

} // namespace mupptest
