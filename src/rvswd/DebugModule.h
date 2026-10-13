#pragma once

// The RISC-V debug module (DM, RISC-V debug spec 0.13) of a CH32V target,
// on the DMI accesses of the wire engine (rvswd/RvSwdEngine.h): DMI read
// and write, DM reset, halt, resume, abstract register access and memory
// access. Hardware-independent: the host unit tests run it on the engine
// with mock pins and a mock DM (tests/MockDebugModule.h).
//
// The sequences come from the WCH-LinkE captures (docs/wch-link-protocol.md,
// "Probe actions" and "Debug module layer"):
//
// - DM reset: dmcontrol 0, then dmactive (wlink halt).
// - Halt: dmcontrol haltreq twice, then dmstatus until allhalted (the
//   connect command of the probe). haltreq stays set, as on the probe:
//   the host tools read dmcontrol 80000001 after a connect.
// - Resume: dmcontrol resumereq, then dmstatus until allresumeack or
//   allrunning (the first two accesses of the quit command of the probe).
//   The write also clears haltreq. A resumereq to a running hart clears
//   allresumeack and sets nothing (debug spec), so allrunning also ends
//   the poll. The quit of the probe also writes dmcontrol 40000000
//   (dmactive 0) after the poll. resume() does not do that write.
// - Abstract register access: command 0x0022xxxx (read) and 0x0023xxxx
//   (write), with the value in data0.
// - Memory access, two methods:
//   - Abstract: the abstract memory access of the CH32V20x and CH32V30x.
//     data1 holds the address, data0 the value. Command 0x02200000 reads
//     a word, 0x02210000 writes a word, 0x02000000 reads a byte.
//   - ProgramBuffer: a program in the program buffer, as on the CH32V00x.
//     data1 holds the address, data0 the value. The program reads data0
//     and data1 at their memory addresses 0xe00000f4 and 0xe00000f8, and
//     keeps a0 and a1 in dscratch0 and dscratch1. Command 0x00040000 runs
//     it. The read program is the program of the probe. The write program
//     has the same layout, with a store. No capture shows it. The module
//     loads a program once, until a DM reset or a DMI write of the host.
//     A load or store that faults (cmderr 3) stops the program before it
//     restores a0 and a1. Then the module runs restoreProgram, which
//     copies dscratch0 and dscratch1 back to a0 and a1.
//
// The module refuses a misaligned word or half word address with
// Status::Misaligned before any frame: the CH32V003 faults on it.
//
// After each abstract command, the module reads abstractcs until busy is
// 0 and checks cmderr. The probe does not do this read in its memory
// accesses: the check costs one frame for each access. A cmderr that is
// not 0 blocks the next commands. Thus the module clears cmderr after an
// error, and before its first command after a DM reset or a DMI write of
// the host. The CH32V003 shows cmderr 6 after the connect of the probe.
//
// Errors: the flags of the engine give Status::Wire. Two wires: a frame
// with the response and the tail bits 1111 (the pull-up, no target
// drives) gives Status::NoTarget. One wire: a frame without a target
// reads 0xffffffff. Thus each dmstatus read checks the version field
// (2: spec 0.13), else Status::NoTarget. Other accesses do not detect a
// missing target on one wire: dmiRead() gives 0xffffffff with Status::Ok,
// and an abstract command reads abstractcs ffffffff (busy) and ends with
// Status::Timeout. After a Timeout, readDmstatus() tells the two apart.
//
// The Engine type has the members of RvSwdEngine: read(), write(),
// head(), tail() and wire().

#include <array>
#include <cstddef>
#include <cstdint>

#include "rvswd/RvSwdEngine.h"

namespace rvswd {

// DM register addresses
namespace dm {
inline constexpr uint8_t data0 = 0x04;
inline constexpr uint8_t data1 = 0x05;
inline constexpr uint8_t dmcontrol = 0x10;
inline constexpr uint8_t dmstatus = 0x11;
inline constexpr uint8_t hartinfo = 0x12;
inline constexpr uint8_t abstractcs = 0x16;
inline constexpr uint8_t command = 0x17;
inline constexpr uint8_t abstractauto = 0x18;
inline constexpr uint8_t progbuf0 = 0x20;
inline constexpr uint8_t haltsum0 = 0x40;
// WCH: the chip ID
inline constexpr uint8_t chipId = 0x7f;

// dmcontrol
inline constexpr uint32_t haltreq = 1u << 31;
inline constexpr uint32_t resumereq = 1u << 30;
inline constexpr uint32_t ndmreset = 1u << 1;
inline constexpr uint32_t dmactive = 1u << 0;

// dmstatus
inline constexpr uint32_t allresumeack = 1u << 17;
inline constexpr uint32_t allrunning = 1u << 11;
inline constexpr uint32_t allhalted = 1u << 9;
inline constexpr uint32_t versionMask = 0xF;
inline constexpr uint32_t version013 = 2;

// abstractcs
inline constexpr uint32_t busy = 1u << 12;
inline constexpr uint32_t cmderrShift = 8;
inline constexpr uint32_t cmderrMask = 0x7;
// Writing 1s clears cmderr.
inline constexpr uint32_t cmderrClear = cmderrMask << cmderrShift;

// command: access register, 32 bits, with transfer
inline constexpr uint32_t readRegister = 0x0022'0000;
inline constexpr uint32_t writeRegister = 0x0023'0000;
// command: access register, without transfer, with postexec: runs the
// program buffer
inline constexpr uint32_t execute = 0x0004'0000;
// command: access memory, aamsize in bits 20-22
inline constexpr uint32_t readMemory = 0x0200'0000;
inline constexpr uint32_t writeMemory = 0x0201'0000;
inline constexpr uint32_t aamsizeShift = 20;

// Register numbers of the abstract register access
inline constexpr uint16_t dpc = 0x07b1;
inline constexpr uint16_t gpr0 = 0x1000;
} // namespace dm

// The program buffer has 8 words on the CH32V00x.
inline constexpr std::size_t programWords = 8;
using Program = std::array<uint32_t, programWords>;

// Reads the word at the address in data1 into data0. The program of the
// WCH-LinkE (v003-wlink-status-timing.csv):
//   csrw dscratch0, a0;  csrw dscratch1, a1;  lui a0, 0xe0000
//   lw a1, 0xf8(a0);  c.lw a1, 0(a1);  sw a1, 0xf4(a0)
//   csrr a0, dscratch0;  csrr a1, dscratch1;  c.ebreak
inline constexpr Program readWordProgram{
   0x7b251073, 0x7b359073, 0xe0000537, 0x0f852583, 0x2a23418c, 0x25730eb5, 0x25f37b20, 0x90027b30,
};

// Writes the word in data0 to the address in data1. Not captured:
//   csrw dscratch0, a0;  csrw dscratch1, a1;  lui a0, 0xe0000
//   lw a1, 0xf4(a0);  lw a0, 0xf8(a0);  c.sw a1, 0(a0)
//   csrr a0, dscratch0;  csrr a1, dscratch1;  c.ebreak
inline constexpr Program writeWordProgram{
   0x7b251073, 0x7b359073, 0xe0000537, 0x0f452583, 0x0f852503, 0x2573c10c, 0x25f37b20, 0x90027b30,
};

// Restores a0 and a1 after a fault in the read or the write program. Not
// captured:
//   csrr a0, dscratch0;  csrr a1, dscratch1;  c.ebreak
inline constexpr std::array<uint32_t, 3> restoreProgram{
   0x7b202573,
   0x7b3025f3,
   0x00009002,
};

// cmderr of an exception in the program buffer
inline constexpr uint8_t cmderrException = 3;

enum class Status : uint8_t {
   Ok,
   // The engine returned flags: parity error or line low.
   Wire,
   // No target answers (see above).
   NoTarget,
   // A poll ended after pollLimit reads.
   Timeout,
   // An abstract command failed. cmderr() gives the reason.
   Command,
   // The memory access method does not have this width.
   Unsupported,
   // The address is not aligned to the width.
   Misaligned,
};

enum class MemoryAccess : uint8_t {
   // Abstract memory access command (CH32V20x, CH32V30x)
   Abstract,
   // A program in the program buffer (CH32V00x)
   ProgramBuffer,
};

// Memory access width, the aamsize values
enum class Width : uint8_t {
   Byte = 0,
   Half = 1,
   Word = 2,
};

// The DMI reads of a poll (dmstatus, dmcontrol, abstractcs). A frame takes
// 50 to 120 us, so a poll ends after about 5 to 12 ms.
inline constexpr uint32_t pollLimit = 100;

template <typename Engine>
class DebugModule {
   public:
   explicit DebugModule(Engine &engine)
      : engine_(engine) {
   }

   // After Engine::init(): the memory access method of the wire (one
   // wire: ProgramBuffer, two wires: Abstract), and no loaded program.
   void init() {
      memoryAccess_ = engine_.wire() == Wire::One ? MemoryAccess::ProgramBuffer : MemoryAccess::Abstract;
      forget();
   }

   void setMemoryAccess(MemoryAccess access) {
      memoryAccess_ = access;
   }
   [[nodiscard]] MemoryAccess memoryAccess() const {
      return memoryAccess_;
   }

   // The cmderr field of the last failed abstract command
   [[nodiscard]] uint8_t cmderr() const {
      return cmderr_;
   }

   // DMI access for the host. A write can change the DM state, so the
   // module forgets the loaded program and the cmderr state.
   Status dmiRead(uint8_t address, uint32_t &data) {
      return read(address, data);
   }
   Status dmiWrite(uint8_t address, uint32_t data) {
      forget();
      return write(address, data);
   }

   // dmactive 0, then 1. Waits until dmactive reads 1.
   Status resetDm() {
      forget();
      Status status = write(dm::dmcontrol, 0);
      if(status == Status::Ok) {
         status = write(dm::dmcontrol, dm::dmactive);
      }
      uint32_t control = 0;
      for(uint32_t i = 0; status == Status::Ok && (control & dm::dmactive) == 0; ++i) {
         if(i >= pollLimit) {
            return Status::Timeout;
         }
         status = read(dm::dmcontrol, control);
      }
      uint32_t value = 0;
      return status == Status::Ok ? readDmstatus(value) : status;
   }

   // dmstatus, with the version check
   Status readDmstatus(uint32_t &value) {
      const Status status = read(dm::dmstatus, value);
      if(status != Status::Ok) {
         return status;
      }
      return (value & dm::versionMask) == dm::version013 ? Status::Ok : Status::NoTarget;
   }

   Status halted(bool &halted) {
      uint32_t value = 0;
      const Status status = readDmstatus(value);
      halted = (value & dm::allhalted) != 0;
      return status;
   }

   Status halt() {
      for(int i = 0; i < 2; ++i) {
         const Status status = write(dm::dmcontrol, dm::haltreq | dm::dmactive);
         if(status != Status::Ok) {
            return status;
         }
      }
      return pollDmstatus(dm::allhalted);
   }

   Status resume() {
      const Status status = write(dm::dmcontrol, dm::resumereq | dm::dmactive);
      if(status != Status::Ok) {
         return status;
      }
      return pollDmstatus(dm::allresumeack | dm::allrunning);
   }

   // Runs an abstract command and waits for its end. Status::Command
   // when cmderr is not 0, after it is cleared.
   Status execute(uint32_t command) {
      Status status = Status::Ok;
      if(!abstractClean_) {
         status = write(dm::abstractcs, dm::cmderrClear);
         if(status != Status::Ok) {
            return status;
         }
         abstractClean_ = true;
      }
      status = write(dm::command, command);
      uint32_t abstract = dm::busy;
      for(uint32_t i = 0; status == Status::Ok && (abstract & dm::busy) != 0; ++i) {
         if(i >= pollLimit) {
            status = Status::Timeout;
            break;
         }
         status = read(dm::abstractcs, abstract);
      }
      if(status != Status::Ok) {
         // The state of the command is not known.
         abstractClean_ = false;
         return status;
      }
      const uint32_t error = (abstract >> dm::cmderrShift) & dm::cmderrMask;
      if(error != 0) {
         cmderr_ = static_cast<uint8_t>(error);
         status = write(dm::abstractcs, dm::cmderrClear);
         abstractClean_ = status == Status::Ok;
         return status == Status::Ok ? Status::Command : status;
      }
      return Status::Ok;
   }

   // Abstract register access (32 bits). regno: CSR 0x0000-0x0fff, GPR
   // 0x1000-0x101f.
   Status readRegister(uint16_t regno, uint32_t &value) {
      const Status status = execute(dm::readRegister | regno);
      return status == Status::Ok ? read(dm::data0, value) : status;
   }

   Status writeRegister(uint16_t regno, uint32_t value) {
      const Status status = write(dm::data0, value);
      return status == Status::Ok ? execute(dm::writeRegister | regno) : status;
   }

   // Memory access with the method of memoryAccess(). ProgramBuffer has
   // words only. A read of a byte or a half word gives the value in the
   // low bits.
   Status readMemory(uint32_t address, uint32_t &value, Width width = Width::Word) {
      Status status = prepare(address, width, LoadedProgram::Read);
      if(status != Status::Ok) {
         return status;
      }
      status = write(dm::data1, address);
      if(status == Status::Ok) {
         status = run(dm::readMemory | static_cast<uint32_t>(width) << dm::aamsizeShift);
      }
      uint32_t data = 0;
      if(status == Status::Ok) {
         status = read(dm::data0, data);
      }
      if(status == Status::Ok) {
         value = data & widthMask(width);
      }
      return status;
   }

   Status writeMemory(uint32_t address, uint32_t value, Width width = Width::Word) {
      Status status = prepare(address, width, LoadedProgram::Write);
      if(status != Status::Ok) {
         return status;
      }
      status = write(dm::data1, address);
      if(status == Status::Ok) {
         status = write(dm::data0, value & widthMask(width));
      }
      return status == Status::Ok ? run(dm::writeMemory | static_cast<uint32_t>(width) << dm::aamsizeShift) : status;
   }

   private:
   enum class LoadedProgram : uint8_t {
      None,
      Read,
      Write,
   };

   static constexpr uint32_t widthMask(Width width) {
      return width == Width::Byte ? 0xFFu : width == Width::Half ? 0xFFFFu : 0xFFFF'FFFFu;
   }

   // The checks before a memory access, and the program for ProgramBuffer
   Status prepare(uint32_t address, Width width, LoadedProgram program) {
      if((address & ((1u << static_cast<uint32_t>(width)) - 1)) != 0) {
         return Status::Misaligned;
      }
      if(memoryAccess_ == MemoryAccess::Abstract) {
         return Status::Ok;
      }
      return width == Width::Word ? load(program) : Status::Unsupported;
   }

   // The abstract memory command, or the loaded program. After a fault in
   // the program, the restore program puts a0 and a1 back.
   Status run(uint32_t command) {
      if(memoryAccess_ == MemoryAccess::Abstract) {
         return execute(command);
      }
      const Status status = execute(dm::execute);
      if(status != Status::Command || cmderr_ != cmderrException) {
         return status;
      }
      loaded_ = LoadedProgram::None;
      Status restore = Status::Ok;
      for(std::size_t i = 0; i < restoreProgram.size() && restore == Status::Ok; ++i) {
         restore = write(static_cast<uint8_t>(dm::progbuf0 + i), restoreProgram[i]);
      }
      if(restore == Status::Ok) {
         restore = execute(dm::execute);
      }
      if(restore != Status::Ok) {
         return restore;
      }
      // cmderr of the access, not of the restore
      cmderr_ = cmderrException;
      return Status::Command;
   }

   void forget() {
      loaded_ = LoadedProgram::None;
      abstractClean_ = false;
   }

   Status read(uint8_t address, uint32_t &data) {
      return check(engine_.read(address, data));
   }

   Status write(uint8_t address, uint32_t data) {
      return check(engine_.write(address, data));
   }

   // Without a target, a two-wire read also has a parity error: the
   // check of the response and tail bits comes first.
   Status check(uint8_t flags) {
      if(engine_.wire() == Wire::Two && engine_.head() == 0b1111 && engine_.tail() == 0b1111) {
         return Status::NoTarget;
      }
      return flags != 0 ? Status::Wire : Status::Ok;
   }

   // Until one of the bits is 1
   Status pollDmstatus(uint32_t bits) {
      for(uint32_t i = 0; i < pollLimit; ++i) {
         uint32_t value = 0;
         const Status status = readDmstatus(value);
         if(status != Status::Ok) {
            return status;
         }
         if((value & bits) != 0) {
            return Status::Ok;
         }
      }
      return Status::Timeout;
   }

   // abstractauto 0 (no autoexec), then the program
   Status load(LoadedProgram program) {
      if(loaded_ == program) {
         return Status::Ok;
      }
      loaded_ = LoadedProgram::None;
      Status status = write(dm::abstractauto, 0);
      const Program &words = program == LoadedProgram::Read ? readWordProgram : writeWordProgram;
      for(std::size_t i = 0; i < words.size() && status == Status::Ok; ++i) {
         status = write(static_cast<uint8_t>(dm::progbuf0 + i), words[i]);
      }
      if(status == Status::Ok) {
         loaded_ = program;
      }
      return status;
   }

   Engine &engine_;
   MemoryAccess memoryAccess_{ MemoryAccess::Abstract };
   LoadedProgram loaded_{ LoadedProgram::None };
   // cmderr is 0 (cleared or checked by the module).
   bool abstractClean_{ false };
   uint8_t cmderr_{ 0 };
};

} // namespace rvswd
