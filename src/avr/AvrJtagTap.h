// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The TAP layer of the AVR JTAG port (docs/avr-jtag-ocd.md, "JTAG
// instructions"): IR and DR scans, Test-Logic-Reset and IDCODE on a JTAG
// port (protocol/JtagPort.h). The JTAG programming and the OCD use it.
// Hardware-independent: the host unit tests run it on the JTAG bit-bang
// engine with mock pins.
//
// - One device in the chain only: no BYPASS bits before or after the AVR.
// - The IR is 4 bits. An IR scan returns the Capture-IR value (bits 1..0
//   are 01 on each IEEE 1149.1 TAP).
// - A DR scan has 1 to maxDrBits bits, LSB first. It sends the data and
//   returns the bits that the target shifted out.
// - Each scan starts and ends in Run-Test/Idle, with no extra idle
//   cycles, as in cmsisdap::JtagDp.
// - setIr() keeps the IR from one call to the next, as
//   cmsisdap::JtagDp::setIr(). scanIr() always scans: FORCE_BREAK and RUN
//   act at Update-IR, so the OCD sends them again with the same IR.
//   After reset() the IR is IDCODE (IEEE 1149.1). After a failed scan the
//   IR is not known, and the next setIr() scans.
//
// The caller owns the target pins (protocol/TargetPinOwner.h) from open()
// to close(): the EDBG transport takes them at ACTIVATE_PHYSICAL.
//
// The AVR has no RTCK. The TCK limit of the datasheet is 1/4 of the CPU
// clock: the host gives the frequency.

#include <array>
#include <cstddef>
#include <cstdint>

#include "protocol/JtagPort.h"

namespace avr {

namespace tap {

inline constexpr uint8_t irLength = 4;
// The longest DR: the instruction scan of OCD revision 4 has 38 bits.
inline constexpr uint8_t maxDrBits = 64;

// The instructions (datasheet, and docs/avr-jtag-ocd.md for 0x8 to 0xB)
namespace ir {
inline constexpr uint8_t idcode = 0x1;
inline constexpr uint8_t progEnable = 0x4;
inline constexpr uint8_t progCommands = 0x5;
inline constexpr uint8_t progPageLoad = 0x6;
inline constexpr uint8_t progPageRead = 0x7;
inline constexpr uint8_t forceBreak = 0x8;
inline constexpr uint8_t run = 0x9;
inline constexpr uint8_t execute = 0xA;
inline constexpr uint8_t ocdAccess = 0xB;
inline constexpr uint8_t avrReset = 0xC;
inline constexpr uint8_t bypass = 0xF;
} // namespace ir

// IDCODE: version in bits 31..28, part number in bits 27..12, manufacturer
// in bits 11..1 and bit 0 = 1. Atmel (JEP106 0x1F) gives 0x03F in bits
// 11..0.
inline constexpr uint32_t manufacturerMask = 0x00000FFFu;
inline constexpr uint32_t manufacturerAtmel = 0x0000003Fu;

constexpr bool isAtmelIdcode(uint32_t idcode) {
   return (idcode & manufacturerMask) == manufacturerAtmel;
}

constexpr uint16_t partNumber(uint32_t idcode) {
   return static_cast<uint16_t>(idcode >> 12);
}

} // namespace tap

class AvrJtagTap {
   public:
   explicit AvrJtagTap(protocol::JtagPort &port)
      : port_(port) {
   }

   // Drives the pins, sets TCK to kHz and resets the TAP. False when the
   // port cannot do kHz or the reset fails: the pins are then released.
   // kHz 0 (adaptive clocking) fails: the AVR has no RTCK.
   [[nodiscard]] bool open(uint32_t kHz) {
      // init() first: the fast path sets up its SPI in init().
      port_.init();
      if(setSpeed(kHz) && reset()) {
         return true;
      }
      close();
      return false;
   }

   // Releases the pins.
   void close() {
      ir_ = noIr;
      port_.fini();
   }

   [[nodiscard]] bool setSpeed(uint32_t kHz) {
      return kHz != 0 && port_.setSpeed(kHz);
   }

   // Test-Logic-Reset (5 TCK cycles with TMS high), then Run-Test/Idle.
   // From an unknown state, the way to Test-Logic-Reset can pass
   // Update-DR or Update-IR (IEEE 1149.1).
   [[nodiscard]] bool reset() {
      start();
      put(0, 5, true);
      put(0, 1, false);
      ir_ = run() ? tap::ir::idcode : noIr;
      return ir_ != noIr;
   }

   // Selects the instruction. No scan when the IR has it already.
   [[nodiscard]] bool setIr(uint8_t instruction) {
      return ir_ == instruction || scanIr(instruction);
   }

   // One IR scan. captured gets the Capture-IR value.
   [[nodiscard]] bool scanIr(uint8_t instruction, uint8_t *captured = nullptr) {
      start();
      // Select-DR-Scan, Select-IR-Scan, Capture-IR, Shift-IR
      put(0, 2, true);
      put(0, 2, false);
      const uint32_t first = shiftData(instruction, tap::irLength);
      if(!run()) {
         return false;
      }
      ir_ = instruction & ((1u << tap::irLength) - 1u);
      if(captured != nullptr) {
         *captured = static_cast<uint8_t>(tdoBits(first, tap::irLength));
      }
      return true;
   }

   // One DR scan of bitCount bits (1 to maxDrBits) in the current IR. data
   // goes out, and gets the bits that the target shifted out.
   [[nodiscard]] bool scanDr(uint64_t &data, uint8_t bitCount) {
      if(bitCount == 0 || bitCount > tap::maxDrBits) {
         return false;
      }
      start();
      // Select-DR-Scan, Capture-DR, Shift-DR
      put(0, 1, true);
      put(0, 2, false);
      const uint32_t first = shiftData(data, bitCount);
      if(!run()) {
         return false;
      }
      data = tdoBits(first, bitCount);
      return true;
   }

   // setIr() and scanDr()
   [[nodiscard]] bool scanDr(uint8_t instruction, uint64_t &data, uint8_t bitCount) {
      return setIr(instruction) && scanDr(data, bitCount);
   }

   [[nodiscard]] bool readIdcode(uint32_t &idcode) {
      uint64_t data = 0;
      if(!scanDr(tap::ir::idcode, data, 32)) {
         return false;
      }
      idcode = static_cast<uint32_t>(data);
      return true;
   }

   // The IR is not known: the next setIr() scans.
   void invalidate() {
      ir_ = noIr;
   }

   // The cached IR, or noIr
   [[nodiscard]] uint8_t ir() const {
      return ir_;
   }

   static constexpr uint8_t noIr = 0xFF;

   private:
   // Select-DR-Scan to Shift-xR, the data, Exit1, Update, Run-Test/Idle
   static constexpr uint32_t maxScanBits = 4 + tap::maxDrBits + 2;
   static constexpr std::size_t scanBytes = protocol::jtagRaw::byteCount(maxScanBits);

   void start() {
      bits_ = 0;
      tdi_.fill(0);
      tms_.fill(0);
   }

   // Adds count bits of value, LSB first.
   void put(uint64_t value, uint32_t count, bool tms) {
      for(uint32_t i = 0; i < count; ++i, ++bits_) {
         const auto mask = static_cast<uint8_t>(1u << (bits_ % 8));
         if(((value >> i) & 1u) != 0) {
            tdi_[bits_ / 8] = static_cast<uint8_t>(tdi_[bits_ / 8] | mask);
         }
         if(tms) {
            tms_[bits_ / 8] = static_cast<uint8_t>(tms_[bits_ / 8] | mask);
         }
      }
   }

   // From Shift-xR: the data (the last bit goes to Exit1), Update-xR and
   // Run-Test/Idle. Returns the index of the first data bit.
   uint32_t shiftData(uint64_t value, uint8_t count) {
      const uint32_t first = bits_;
      put(value, count - 1u, false);
      put(value >> (count - 1u), 1, true);
      put(0, 1, true);
      put(0, 1, false);
      return first;
   }

   [[nodiscard]] bool run() {
      const std::size_t bytes = protocol::jtagRaw::byteCount(bits_);
      tdo_.fill(0);
      if(port_.shift(bits_, { tdi_.data(), bytes }, { tms_.data(), bytes }, { tdo_.data(), bytes })) {
         return true;
      }
      ir_ = noIr;
      return false;
   }

   [[nodiscard]] uint64_t tdoBits(uint32_t first, uint32_t count) const {
      uint64_t value = 0;
      for(uint32_t i = 0; i < count; ++i) {
         const uint32_t bit = first + i;
         if(((tdo_[bit / 8] >> (bit % 8)) & 1u) != 0) {
            value |= uint64_t{ 1 } << i;
         }
      }
      return value;
   }

   protocol::JtagPort &port_;
   uint8_t ir_{ noIr };
   std::array<uint8_t, scanBytes> tdi_{};
   std::array<uint8_t, scanBytes> tms_{};
   std::array<uint8_t, scanBytes> tdo_{};
   uint32_t bits_{ 0 };
};

} // namespace avr
