// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The low level of the AVR JTAG on-chip debug (OCD) on the TAP layer
// (avr/AvrJtagTap.h): OCD register access, FORCE_BREAK, RUN, instruction
// execute and AVR_RESET. The protocol is in docs/avr-jtag-ocd.md. The
// halt, run, step and reset sequences go on top of this class.
// Hardware-independent: the host unit tests run it on mock pins with a
// model of the OCD (tests/MockAvrJtag.h).
//
// - OCD register access (IR 0xB, 21 bits): bit 20 = write, bits 19..16 =
//   register, bits 15..0 = data. A write is one scan. A read is two scans
//   with the same read command: each scan returns the result of the scan
//   before it, register << 16 | data. A read fails when the result has a
//   different register number or the write bit.
// - FORCE_BREAK (IR 0x8) and RUN (IR 0x9) act at Update-IR: each call
//   scans the IR, also when the IR has the instruction already.
// - Instruction execute (IR 0xA): the instruction goes in the high 16 bits.
//   The low bits (the "field") are 22 bits on OCD revision 4 (38-bit DR).
//   The TDO field is the PC field (docs/avr-jtag-ocd.md). A TDI field that
//   is not 0 reads a flash word, and the core does not execute the
//   instruction. execute() sends field 0.
// - OCD revision 3 and lower: a 32-bit DR with the instruction in bits
//   31..16 (debugger scripts, no test). That the low 16 bits are the
//   field, as bits 21..0 on revision 4, is an assumption: step 2.6 of the
//   AVR JTAG plan tests it on the ATmega32U4. Revisions below 3 are not
//   known.
// - AVR_RESET (IR 0xC, 1 bit) holds the core in reset while the bit is 1.
//
// A failed scan gives false.

#include <cstdint>

#include "avr/AvrJtagTap.h"

namespace avr {

namespace jtagOcd {

inline constexpr uint8_t registerBits = 21;
inline constexpr uint32_t registerWrite = 1u << 20;
inline constexpr uint8_t registerCount = 16;

// The OCD registers (docs/avr-jtag-ocd.md, "OCD registers")
namespace reg {
// Program breakpoints. Reg 0 is one-shot. Reg 2 and 3 can also be data
// breakpoints, or reg 2 a mask for reg 3.
inline constexpr uint8_t break0 = 0x0;
inline constexpr uint8_t break1 = 0x1;
inline constexpr uint8_t break2 = 0x2;
inline constexpr uint8_t break3 = 0x3;
// Probably address bits 23..16 of reg 0 to reg 3 (revision 4 only)
inline constexpr uint8_t breakHigh0 = 0x4;
inline constexpr uint8_t breakHigh2 = 0x6;
inline constexpr uint8_t breakHigh3 = 0x7;
inline constexpr uint8_t bcr = 0x8;
inline constexpr uint8_t bsr = 0x9;
// The last byte that the core wrote to OCD_DATAREG, in bits 15..8
inline constexpr uint8_t ocdr = 0xC;
inline constexpr uint8_t control = 0xD;
} // namespace reg

// BCR (reg 8) bits
namespace bcr {
inline constexpr uint16_t bit15 = 1u << 15;
inline constexpr uint16_t bit14 = 1u << 14;
inline constexpr uint16_t step = 1u << 13;
inline constexpr uint16_t enable0 = 1u << 11;
inline constexpr uint16_t enable1 = 1u << 10;
inline constexpr uint16_t maskMode = 1u << 9;
inline constexpr uint16_t enable2 = 1u << 8;
inline constexpr uint16_t enable3 = 1u << 7;
inline constexpr uint8_t mode2Shift = 5;
inline constexpr uint8_t mode3Shift = 3;
inline constexpr uint8_t modeProgram = 3;
} // namespace bcr

// BSR (reg 9) bits: the cause of the last stop
namespace bsr {
inline constexpr uint16_t breakInstruction = 1u << 0;
inline constexpr uint16_t forceBreak = 1u << 1;
inline constexpr uint16_t break2 = 1u << 2;
inline constexpr uint16_t break3 = 1u << 3;
inline constexpr uint16_t break1 = 1u << 4;
inline constexpr uint16_t break0 = 1u << 5;
inline constexpr uint16_t step = 1u << 7;
} // namespace bsr

// Reg D bits
namespace control {
inline constexpr uint16_t ocdrFull = 1u << 4;
inline constexpr uint16_t stoppedAlso = 1u << 3;
inline constexpr uint16_t stopped = 1u << 2;
inline constexpr uint16_t running = 1u << 0;
} // namespace control

// The DR length of the instruction execute scan
constexpr uint8_t executeBits(uint8_t revision) {
   return revision >= 4 ? 38 : 32;
}

// The bits of the field below the instruction
constexpr uint8_t fieldBits(uint8_t revision) {
   return static_cast<uint8_t>(executeBits(revision) - 16u);
}

constexpr uint32_t readCommand(uint8_t reg) {
   return static_cast<uint32_t>(reg & 0x0Fu) << 16;
}

constexpr uint32_t writeCommand(uint8_t reg, uint16_t value) {
   return registerWrite | readCommand(reg) | value;
}

} // namespace jtagOcd

class AvrJtagOcd {
   public:
   explicit AvrJtagOcd(AvrJtagTap &tap)
      : tap_(tap) {
   }

   // The OCD revision of the device (EDBG device context). Default 3.
   void setRevision(uint8_t revision) {
      revision_ = revision;
   }

   [[nodiscard]] uint8_t revision() const {
      return revision_;
   }

   [[nodiscard]] bool writeRegister(uint8_t reg, uint16_t value) {
      uint64_t data = jtagOcd::writeCommand(reg, value);
      return reg < jtagOcd::registerCount && tap_.scanDr(tap::ir::ocdAccess, data, jtagOcd::registerBits);
   }

   // Two scans, as the reference debugger: the read, then the same read
   // again, which returns the result of the first.
   [[nodiscard]] bool readRegister(uint8_t reg, uint16_t &value) {
      if(reg >= jtagOcd::registerCount) {
         return false;
      }
      uint64_t data = jtagOcd::readCommand(reg);
      if(!tap_.scanDr(tap::ir::ocdAccess, data, jtagOcd::registerBits)) {
         return false;
      }
      data = jtagOcd::readCommand(reg);
      if(!tap_.scanDr(data, jtagOcd::registerBits) || (data >> 16) != reg) {
         return false;
      }
      value = static_cast<uint16_t>(data);
      return true;
   }

   // Stops the core.
   [[nodiscard]] bool forceBreak() {
      return tap_.scanIr(tap::ir::forceBreak);
   }

   // Starts the core, or executes one step when BCR bit 13 is set.
   [[nodiscard]] bool run() {
      return tap_.scanIr(tap::ir::run);
   }

   // AVR_RESET: true holds the core in reset, false releases it.
   [[nodiscard]] bool reset(bool hold) {
      uint64_t data = hold ? 1u : 0u;
      return tap_.scanDr(tap::ir::avrReset, data, 1);
   }

   // One instruction execute scan. instruction and field go out, and get
   // the instruction and field bits that the target shifted out. The field
   // is cut to fieldBits(revision()).
   [[nodiscard]] bool instructionScan(uint16_t &instruction, uint32_t &field) {
      const uint8_t bits = jtagOcd::fieldBits(revision_);
      const uint64_t fieldMask = (uint64_t{ 1 } << bits) - 1u;
      uint64_t data = (static_cast<uint64_t>(instruction) << bits) | (field & fieldMask);
      if(!tap_.scanDr(tap::ir::execute, data, jtagOcd::executeBits(revision_))) {
         return false;
      }
      instruction = static_cast<uint16_t>(data >> bits);
      field = static_cast<uint32_t>(data & fieldMask);
      return true;
   }

   // The stopped core executes one AVR instruction (one word). pcField
   // gets the PC field that the scan shifted out.
   [[nodiscard]] bool execute(uint16_t instruction, uint32_t *pcField = nullptr) {
      uint32_t field = 0;
      if(!instructionScan(instruction, field)) {
         return false;
      }
      if(pcField != nullptr) {
         *pcField = field;
      }
      return true;
   }

   private:
   AvrJtagTap &tap_;
   uint8_t revision_{ 3 };
};

} // namespace avr
