// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// AVR JTAG debug mode: attach, halt, run, step, status and reset on the
// OCD low level (avr/AvrJtagOcd.h), with the sequences of the reference
// debugger (docs/avr-jtag-ocd.md, "Debug operations"). Hardware-
// independent: the host unit tests run it on mock pins with the OCD model
// of tests/MockAvrJtag.h.
//
// - attach(): TAP reset, IDCODE, then reg D. When the OCD does not answer
//   (OCDEN not programmed), attach() still succeeds: Bloom
//   (manage_ocden_fuse_bit) writes the fuse only after ATTACH. When the
//   OCD answers, attach() halts the core.
// - Each OCD operation reads reg D first when the OCD did not answer
//   before. No answer: NoOcd, which the scope sends as FAILED 0x50
//   ("Device not under control - check OCDEN fuse bit" in Bloom).
// - The probe keeps a copy of the BCR value it wrote last. The sequences
//   compute the next BCR from it, as the reference debugger does.
// - A wait for a stop reads reg D until bit 2 is set, at most
//   jtagDebug::stopTimeoutMs at the TCK frequency of the TAP.
// - The context (PC, r0 to r31, EIND) is read after each stop and written
//   back before each run and step, in the order of the reference debugger
//   (docs/avr-jtag-ocd.md, "Read and write the CPU state"): r0 to r31, the
//   PC, then r31, r30, EIND and r16 again. The PC write loads Z with the
//   PC - 11 (revision 4) or PC - 5 (revision 3, from the scripts, no
//   test), as the reference debugger does. The same sequence (EIJMP, EIND)
//   goes to all devices, also to the devices without EIND (no test).
// - Memory: the data space (registers, I/O, SRAM), the flash (read only)
//   and the EEPROM, with injected instructions. They change r16, r30 and
//   r31 on the target. Thus reads and writes of r0 to r31 and EIND use
//   the context, never the target.
// - The flash read uses two IR 0xA scans for each word, as the reference
//   debugger. That a field that is not 0 reads a flash word is an
//   inference (docs/avr-jtag-ocd.md). Field 0 executes the instruction, so
//   word 0 is read with LPM.
// - The EEPROM write sets EEMPE, then EEPE with two adjacent injected
//   OUT instructions. That the 4-cycle limit of EEMPE holds for this is not
//   tested, so each byte is read back after the write.
// - Hardware breakpoints (program addresses, word addresses): the host
//   numbers 1 to 3 use reg 1, reg 2 and reg 3 (mode 3), reg 0 (one-shot)
//   is the RUN_TO address. The probe keeps them and writes them before
//   each run: halt and reset clear the BCR enables (reference sequences).
//   On revision 4, reg 4 + n gets the word bits 23..16 of reg n (inference:
//   only 0 and 0xFF were seen, docs/avr-jtag-ocd.md). A step does not
//   write them: it keeps the BCR enables, as the reference step formula.
// - After each stop the probe reads the BSR (the cause of the stop). The
//   reference debugger does not read it.
// - A failed context read (NoOcd, Timeout) leaves no valid context: run,
//   step, halt and the memory access then give NoContext until a reset.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "avr/AvrJtagOcd.h"
#include "avr/AvrJtagTap.h"

namespace avr {

enum class JtagDebugStatus : uint8_t {
   Ok,
   // No TAP, or the IDCODE is not an Atmel IDCODE
   NoTap,
   // The OCD does not answer (OCDEN, or a failed scan)
   NoOcd,
   // The core did not stop, or the OCDR or EEPROM did not answer, in the
   // time limit
   Timeout,
   // The operation needs a stopped core.
   TargetRunning,
   // The address range is out of the memory.
   OutOfRange,
   // The context read after the last stop failed: run, step and memory
   // access would write wrong registers to the target. A reset reads a new
   // context.
   NoContext,
   // An EEPROM byte read back different after the write
   VerifyFailed,
};

// The device values for the debug mode (EDBG device context). The register
// addresses are I/O addresses. Defaults: ATmega32U4.
struct JtagDebugDevice {
   uint32_t flashBytes{ 32768 };
   uint16_t eepromBytes{ 1024 };
   uint8_t ocdr{ 0x31 };
   // EEARH equal to EEARL: the EEPROM address register has one byte.
   uint8_t eearh{ 0x22 };
   uint8_t eearl{ 0x21 };
   uint8_t eecr{ 0x1F };
   uint8_t eedr{ 0x20 };
};

namespace jtagDebug {

// Longer than the 70 ms of a FORCE_BREAK in the captures and than the
// start-up time of a reset (65 ms and more)
inline constexpr uint32_t stopTimeoutMs = 300;
// TCK cycles of one OCD register read: two scans of 3 + 21 + 2
inline constexpr uint32_t readScanCycles = 2 * (3 + jtagOcd::registerBits + 2);

// The BCR values of docs/avr-jtag-ocd.md, "Debug operations"
inline constexpr uint16_t keepMask = 0x9FFC;
inline constexpr uint16_t haltClear = 0x03E0;
inline constexpr uint16_t idle = jtagOcd::bcr::bit15 | jtagOcd::bcr::bit14;
// Reset: reg 3 with mode 3, masked by reg 2 = 0, matches all addresses.
inline constexpr uint16_t resetBreak =
idle | jtagOcd::bcr::maskMode | jtagOcd::bcr::enable3 | (jtagOcd::bcr::modeProgram << jtagOcd::bcr::mode3Shift);
static_assert(resetBreak == 0xC298);
// The BCR bits of the breakpoints: enables, mask mode and modes (11 to 3)
inline constexpr uint16_t breakBits = 0x0FF8;

// The PC write loads Z with the PC minus this offset (word addresses).
constexpr uint32_t pcOffset(uint8_t revision) {
   return revision >= 4 ? 11u : 5u;
}

// Longer than the 3.4 ms of an EEPROM write in the datasheets
inline constexpr uint32_t eepromTimeoutMs = 20;
// TCK cycles of one EEPE poll, at least: two instruction scans and two
// register reads
inline constexpr uint32_t eepromPollCycles = 2 * (3 + 32 + 2) + 2 * readScanCycles;

inline constexpr uint8_t ioEind = 0x3C;
inline constexpr uint16_t dataIo = 0x20;
inline constexpr uint16_t dataSram = 0x60;
inline constexpr uint8_t eecrRead = 0x01;
inline constexpr uint8_t eecrWrite = 0x02;
inline constexpr uint8_t eecrMasterWrite = 0x04;

// The injected AVR instructions
constexpr uint16_t ldi(uint8_t d, uint8_t k) {
   return static_cast<uint16_t>(0xE000u | ((k & 0xF0u) << 4) | (((d - 16u) & 0x0Fu) << 4) | (k & 0x0Fu));
}
constexpr uint16_t mov(uint8_t d, uint8_t r) {
   return static_cast<uint16_t>(0x2C00u | ((r & 0x10u) << 5) | ((d & 0x1Fu) << 4) | (r & 0x0Fu));
}
constexpr uint16_t in(uint8_t d, uint8_t a) {
   return static_cast<uint16_t>(0xB000u | ((a & 0x30u) << 5) | ((d & 0x1Fu) << 4) | (a & 0x0Fu));
}
constexpr uint16_t out(uint8_t a, uint8_t r) {
   return static_cast<uint16_t>(in(r, a) | 0x0800u);
}
// LD Rd, Z and ST Z, Rr
constexpr uint16_t ldZ(uint8_t d) {
   return static_cast<uint16_t>(0x8000u | ((d & 0x1Fu) << 4));
}
constexpr uint16_t stZ(uint8_t r) {
   return static_cast<uint16_t>(0x8200u | ((r & 0x1Fu) << 4));
}
// LPM Rd, Z
constexpr uint16_t lpmZ(uint8_t d) {
   return static_cast<uint16_t>(0x9004u | ((d & 0x1Fu) << 4));
}
inline constexpr uint16_t eijmp = 0x9419;
inline constexpr uint16_t wdr = 0x95A8;
// The instruction bits of the flash read scans
inline constexpr uint16_t flashReadFill = 0xFFFF;

static_assert(ldi(31, 0x02) == 0xE0F2);
static_assert(out(0x31, 16) == 0xBF01);
static_assert(mov(5, 16) == 0x2E50);

} // namespace jtagDebug

class AvrJtagDebug {
   public:
   AvrJtagDebug(AvrJtagTap &tap, AvrJtagOcd &ocd)
      : tap_(tap)
      , ocd_(ocd) {
   }

   void setDevice(const JtagDebugDevice &device) {
      device_ = device;
   }

   // Enters debug mode. Ok also without an OCD answer (see above).
   [[nodiscard]] JtagDebugStatus attach() {
      ocdAnswers_ = false;
      stopped_ = false;
      uint32_t idcode = 0;
      if(!tap_.reset() || !tap_.readIdcode(idcode) || !tap::isAtmelIdcode(idcode)) {
         return JtagDebugStatus::NoTap;
      }
      uint16_t control = 0;
      if(!ocd_.readRegister(jtagOcd::reg::control, control)) {
         return JtagDebugStatus::Ok;
      }
      ocdAnswers_ = true;
      return halt();
   }

   // True when the OCD answered since attach()
   [[nodiscard]] bool ocdAnswers() const {
      return ocdAnswers_;
   }

   // True after a stop that the probe saw: halt(), step(), reset() or
   // status()
   [[nodiscard]] bool isStopped() const {
      return stopped_;
   }

   // Stops the core. A stopped core stays stopped, with no scan: a new
   // context read would lose the context changes.
   [[nodiscard]] JtagDebugStatus halt() {
      if(JtagDebugStatus status = requireOcd(); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(stopped_) {
         return contextValid_ ? JtagDebugStatus::Ok : JtagDebugStatus::NoContext;
      }
      uint16_t value = 0;
      if(!ocd_.readRegister(jtagOcd::reg::bcr, value) || !writeBcr(static_cast<uint16_t>(keep(value) | jtagOcd::bcr::bit14)) ||
         !ocd_.readRegister(jtagOcd::reg::control, value) || !ocd_.forceBreak()) {
         return JtagDebugStatus::NoOcd;
      }
      if(JtagDebugStatus status = waitStop(); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(!writeBcr(static_cast<uint16_t>(bcr_ & ~jtagDebug::haltClear)) || !writeBcr(jtagDebug::idle)) {
         return JtagDebugStatus::NoOcd;
      }
      return stopped();
   }

   // The hardware breakpoints for the host: numbers 1 to 3
   static constexpr std::size_t hardwareBreaks = 3;

   // Sets breakpoint number (1 to 3) at a word address. It takes effect at
   // the next run.
   [[nodiscard]] JtagDebugStatus setBreakpoint(uint8_t number, uint32_t word) {
      if(number < 1 || number > hardwareBreaks || word >= device_.flashBytes / 2u) {
         return JtagDebugStatus::OutOfRange;
      }
      breaks_[number] = word;
      return JtagDebugStatus::Ok;
   }

   [[nodiscard]] JtagDebugStatus clearBreakpoint(uint8_t number) {
      if(number < 1 || number > hardwareBreaks) {
         return JtagDebugStatus::OutOfRange;
      }
      breaks_[number].reset();
      return JtagDebugStatus::Ok;
   }

   // The BSR read at the last stop (jtagOcd::bsr bits)
   [[nodiscard]] uint16_t breakStatus() const {
      return breakStatus_;
   }

   // Writes back the context, writes the breakpoints and starts the core.
   // runTo: a one-shot breakpoint (reg 0) at this word address.
   [[nodiscard]] JtagDebugStatus run(std::optional<uint32_t> runTo = std::nullopt) {
      if(JtagDebugStatus status = ready(); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(runTo && *runTo >= device_.flashBytes / 2u) {
         return JtagDebugStatus::OutOfRange;
      }
      breaks_[0] = runTo;
      if(!writeBack() || !writeBreakpoints() || !ocd_.run()) {
         return JtagDebugStatus::NoOcd;
      }
      stopped_ = false;
      return JtagDebugStatus::Ok;
   }

   // Writes back the context, executes one instruction and waits for the
   // stop.
   [[nodiscard]] JtagDebugStatus step() {
      if(JtagDebugStatus status = ready(); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(!writeBack() || !writeBcr(static_cast<uint16_t>(keep(bcr_) | jtagOcd::bcr::step)) || !ocd_.run()) {
         return JtagDebugStatus::NoOcd;
      }
      stopped_ = false;
      if(JtagDebugStatus status = waitStop(); status != JtagDebugStatus::Ok) {
         return status;
      }
      return afterStop();
   }

   // Reads reg D one time. stopped gets bit 2. A new stop (a breakpoint)
   // gets the "after each stop" sequence.
   [[nodiscard]] JtagDebugStatus status(bool &stopped) {
      if(JtagDebugStatus status = requireOcd(); status != JtagDebugStatus::Ok) {
         return status;
      }
      uint16_t control = 0;
      if(!ocd_.readRegister(jtagOcd::reg::control, control)) {
         return JtagDebugStatus::NoOcd;
      }
      stopped = (control & jtagOcd::control::stopped) != 0;
      if(stopped && !stopped_) {
         return afterStop();
      }
      return JtagDebugStatus::Ok;
   }

   // Resets the core. It stops at the first instruction (the reset vector).
   // The probe does not send the reg 9 write of the reference debugger
   // (docs/avr-jtag-ocd.md).
   [[nodiscard]] JtagDebugStatus reset() {
      if(JtagDebugStatus status = requireOcd(); status != JtagDebugStatus::Ok) {
         return status;
      }
      stopped_ = false;
      if(!writeBcr(jtagDebug::idle) || !ocd_.reset(true) || !writeBcr(jtagDebug::resetBreak) ||
         !ocd_.writeRegister(jtagOcd::reg::break2, 0) || !ocd_.writeRegister(jtagOcd::reg::break3, 0)) {
         return JtagDebugStatus::NoOcd;
      }
      // Reg 6 and 7: address bits 23..16 of reg 2 and reg 3, revision 4 only
      if(ocd_.revision() >= 4 &&
         (!ocd_.writeRegister(jtagOcd::reg::breakHigh2, 0) || !ocd_.writeRegister(jtagOcd::reg::breakHigh3, 0))) {
         return JtagDebugStatus::NoOcd;
      }
      if(!ocd_.reset(false)) {
         return JtagDebugStatus::NoOcd;
      }
      if(JtagDebugStatus status = waitStop(); status != JtagDebugStatus::Ok) {
         // Remove the match-all breakpoint, else each later run stops at
         // once. The status of the reset is the result.
         static_cast<void>(writeBcr(jtagDebug::idle));
         return status;
      }
      if(!writeBcr(jtagDebug::idle)) {
         return JtagDebugStatus::NoOcd;
      }
      return stopped();
   }

   // The context. Valid while the core is stopped. Changes go to the
   // target at run() and step().
   [[nodiscard]] uint32_t pc() const {
      return pc_;
   }
   void setPc(uint32_t word) {
      pc_ = word;
   }
   [[nodiscard]] uint8_t eind() const {
      return eind_;
   }

   // The data space: r0 to r31 (the context), the I/O registers and the
   // SRAM. EIND is in the context also.
   [[nodiscard]] JtagDebugStatus readData(uint16_t address, uint8_t *buffer, uint16_t length) {
      return accessData(address, buffer, length, false);
   }
   [[nodiscard]] JtagDebugStatus writeData(uint16_t address, const uint8_t *buffer, uint16_t length) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): a write does not change the buffer.
      return accessData(address, const_cast<uint8_t *>(buffer), length, true);
   }

   // Flash bytes at a byte address
   [[nodiscard]] JtagDebugStatus readFlash(uint32_t address, uint8_t *buffer, uint32_t length) {
      if(JtagDebugStatus status = ready(); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(!inRange(address, length, device_.flashBytes)) {
         return JtagDebugStatus::OutOfRange;
      }
      for(uint32_t i = 0; i < length;) {
         const uint32_t at = address + i;
         uint16_t word = 0;
         if(JtagDebugStatus status = readFlashWord(at / 2u, word); status != JtagDebugStatus::Ok) {
            return status;
         }
         for(uint32_t byte = at % 2u; byte < 2u && i < length; ++byte, ++i) {
            buffer[i] = static_cast<uint8_t>(word >> (8u * byte));
         }
      }
      return JtagDebugStatus::Ok;
   }

   [[nodiscard]] JtagDebugStatus readEeprom(uint16_t address, uint8_t *buffer, uint16_t length) {
      if(JtagDebugStatus status = ready(); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(!inRange(address, length, device_.eepromBytes)) {
         return JtagDebugStatus::OutOfRange;
      }
      for(uint16_t i = 0; i < length; ++i) {
         if(!eepromAddress(static_cast<uint16_t>(address + i)) || !writeIo(device_.eecr, jtagDebug::eecrRead) ||
            !execute(jtagDebug::in(16, device_.eedr))) {
            return JtagDebugStatus::NoOcd;
         }
         if(JtagDebugStatus status = readOcdr(16, buffer[i]); status != JtagDebugStatus::Ok) {
            return status;
         }
      }
      return JtagDebugStatus::Ok;
   }

   [[nodiscard]] JtagDebugStatus writeEeprom(uint16_t address, const uint8_t *buffer, uint16_t length) {
      if(JtagDebugStatus status = ready(); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(!inRange(address, length, device_.eepromBytes)) {
         return JtagDebugStatus::OutOfRange;
      }
      for(uint16_t i = 0; i < length; ++i) {
         // Both values in registers first, so that the two OUT to EECR are
         // adjacent, as in the datasheet code (EEPE within 4 cycles after
         // EEMPE). r31 comes back at the write-back.
         if(!eepromAddress(static_cast<uint16_t>(address + i)) || !writeIo(device_.eedr, buffer[i]) ||
            !execute(jtagDebug::ldi(16, jtagDebug::eecrMasterWrite)) ||
            !execute(jtagDebug::ldi(31, static_cast<uint8_t>(jtagDebug::eecrMasterWrite | jtagDebug::eecrWrite))) ||
            !execute(jtagDebug::out(device_.eecr, 16)) || !execute(jtagDebug::out(device_.eecr, 31))) {
            return JtagDebugStatus::NoOcd;
         }
         if(JtagDebugStatus status = waitEeprom(); status != JtagDebugStatus::Ok) {
            return status;
         }
         // Read back: a write that the hardware ignored leaves EEPE clear.
         uint8_t value = 0;
         if(!writeIo(device_.eecr, jtagDebug::eecrRead) || !execute(jtagDebug::in(16, device_.eedr))) {
            return JtagDebugStatus::NoOcd;
         }
         if(JtagDebugStatus status = readOcdr(16, value); status != JtagDebugStatus::Ok) {
            return status;
         }
         if(value != buffer[i]) {
            return JtagDebugStatus::VerifyFailed;
         }
      }
      return JtagDebugStatus::Ok;
   }

   private:
   static constexpr bool inRange(uint32_t address, uint32_t length, uint32_t size) {
      return address <= size && length <= size - address;
   }

   [[nodiscard]] JtagDebugStatus ready() {
      if(JtagDebugStatus status = requireOcd(); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(!stopped_) {
         return JtagDebugStatus::TargetRunning;
      }
      return contextValid_ ? JtagDebugStatus::Ok : JtagDebugStatus::NoContext;
   }

   [[nodiscard]] bool execute(uint16_t instruction, uint32_t *pcField = nullptr) {
      return ocd_.execute(instruction, pcField);
   }

   // I/O register = value through r16
   [[nodiscard]] bool writeIo(uint8_t io, uint8_t value) {
      return execute(jtagDebug::ldi(16, value)) && execute(jtagDebug::out(io, 16));
   }

   [[nodiscard]] bool loadZ(uint16_t value) {
      // r31 first, as the PC write of the reference debugger
      return execute(jtagDebug::ldi(31, static_cast<uint8_t>(value >> 8))) &&
             execute(jtagDebug::ldi(30, static_cast<uint8_t>(value)));
   }

   // OUT OCDR, r, then reg D until bit 4 is set, then reg C.
   [[nodiscard]] JtagDebugStatus readOcdr(uint8_t r, uint8_t &value, uint32_t *pcField = nullptr) {
      if(!execute(jtagDebug::out(device_.ocdr, r), pcField)) {
         return JtagDebugStatus::NoOcd;
      }
      const uint32_t polls = jtagDebug::stopTimeoutMs * tap_.kHz() / jtagDebug::readScanCycles + 1u;
      for(uint32_t i = 0; i < polls; ++i) {
         uint16_t control = 0;
         if(!ocd_.readRegister(jtagOcd::reg::control, control)) {
            return JtagDebugStatus::NoOcd;
         }
         if((control & jtagOcd::control::ocdrFull) != 0) {
            uint16_t data = 0;
            if(!ocd_.readRegister(jtagOcd::reg::ocdr, data)) {
               return JtagDebugStatus::NoOcd;
            }
            value = static_cast<uint8_t>(data >> 8);
            return JtagDebugStatus::Ok;
         }
      }
      return JtagDebugStatus::Timeout;
   }

   // The core stopped: read the context.
   // No retry on a failure: after IN r16, EIND a new read of r16 gets EIND.
   [[nodiscard]] JtagDebugStatus stopped() {
      stopped_ = true;
      contextValid_ = false;
      if(!ocd_.readRegister(jtagOcd::reg::bsr, breakStatus_)) {
         return JtagDebugStatus::NoOcd;
      }
      // The PC field of the first scan after the stop is PC + 1.
      uint32_t field = 0;
      for(uint8_t r = 0; r < regs_.size(); ++r) {
         if(JtagDebugStatus status = readOcdr(r, regs_[r], r == 0 ? &field : nullptr); status != JtagDebugStatus::Ok) {
            return status;
         }
      }
      pc_ = (field - 1u) & fieldMask();
      if(!execute(jtagDebug::in(16, jtagDebug::ioEind))) {
         return JtagDebugStatus::NoOcd;
      }
      const JtagDebugStatus status = readOcdr(16, eind_);
      contextValid_ = status == JtagDebugStatus::Ok;
      return status;
   }

   [[nodiscard]] uint32_t fieldMask() const {
      return (uint32_t{ 1 } << jtagOcd::fieldBits(ocd_.revision())) - 1u;
   }

   // Reg n (and reg 4 + n on revision 4) for each breakpoint, then the BCR
   // enables. The BCR bits 11 to 3 come from the breakpoints only.
   [[nodiscard]] bool writeBreakpoints() {
      static constexpr std::array<uint16_t, 4> enable{
         jtagOcd::bcr::enable0, jtagOcd::bcr::enable1,
         static_cast<uint16_t>(jtagOcd::bcr::enable2 | (jtagOcd::bcr::modeProgram << jtagOcd::bcr::mode2Shift)),
         static_cast<uint16_t>(jtagOcd::bcr::enable3 | (jtagOcd::bcr::modeProgram << jtagOcd::bcr::mode3Shift))
      };
      auto bcr = static_cast<uint16_t>(bcr_ & ~jtagDebug::breakBits);
      for(uint8_t n = 0; n < breaks_.size(); ++n) {
         if(!breaks_[n]) {
            continue;
         }
         const uint32_t word = *breaks_[n];
         if(!ocd_.writeRegister(n, static_cast<uint16_t>(word)) ||
            (ocd_.revision() >= 4 && !ocd_.writeRegister(static_cast<uint8_t>(jtagOcd::reg::breakHigh0 + n),
                                                         static_cast<uint16_t>((word >> 16) & 0xFFu)))) {
            return false;
         }
         bcr = static_cast<uint16_t>(bcr | enable[n]);
      }
      // Without a change no BCR write: the reference run sequence
      return bcr == bcr_ || writeBcr(bcr);
   }

   // The write-back before a run or step: r0 to r31, the PC, then r31,
   // r30, EIND and r16.
   [[nodiscard]] bool writeBack() {
      for(uint8_t r = 0; r < regs_.size(); ++r) {
         if(r < 16 ? !execute(jtagDebug::ldi(16, regs_[r])) || !execute(jtagDebug::mov(r, 16)) :
                     !execute(jtagDebug::ldi(r, regs_[r]))) {
            return false;
         }
      }
      const uint32_t z = (pc_ - jtagDebug::pcOffset(ocd_.revision())) & fieldMask();
      return loadZ(static_cast<uint16_t>(z)) && execute(jtagDebug::wdr) &&
             writeIo(jtagDebug::ioEind, static_cast<uint8_t>(z >> 16)) && execute(jtagDebug::eijmp) &&
             execute(jtagDebug::ldi(31, regs_[31])) && execute(jtagDebug::ldi(30, regs_[30])) &&
             writeIo(jtagDebug::ioEind, eind_) && execute(jtagDebug::ldi(16, regs_[16]));
   }

   [[nodiscard]] JtagDebugStatus accessData(uint16_t address, uint8_t *buffer, uint16_t length, bool write) {
      if(JtagDebugStatus status = ready(); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(!inRange(address, length, 0x10000u)) {
         return JtagDebugStatus::OutOfRange;
      }
      for(uint16_t i = 0; i < length; ++i) {
         const auto at = static_cast<uint16_t>(address + i);
         if(JtagDebugStatus status = accessByte(at, buffer[i], write); status != JtagDebugStatus::Ok) {
            return status;
         }
      }
      return JtagDebugStatus::Ok;
   }

   [[nodiscard]] JtagDebugStatus accessByte(uint16_t address, uint8_t &value, bool write) {
      uint8_t *cached = address < regs_.size()                           ? &regs_[address] :
                        address == jtagDebug::dataIo + jtagDebug::ioEind ? &eind_ :
                                                                           nullptr;
      if(cached != nullptr) {
         if(write) {
            *cached = value;
         } else {
            value = *cached;
         }
         return JtagDebugStatus::Ok;
      }
      bool sent = true;
      if(address < jtagDebug::dataSram) {
         const auto io = static_cast<uint8_t>(address - jtagDebug::dataIo);
         sent = write ? writeIo(io, value) : execute(jtagDebug::in(16, io));
      } else {
         sent = loadZ(address) &&
                (write ? execute(jtagDebug::ldi(16, value)) && execute(jtagDebug::stZ(16)) : execute(jtagDebug::ldZ(16)));
      }
      if(!sent) {
         return JtagDebugStatus::NoOcd;
      }
      return write ? JtagDebugStatus::Ok : readOcdr(16, value);
   }

   // Two IR 0xA scans: the word address, then the PC field. The second
   // scan returns the word. Word 0 with LPM (see above).
   [[nodiscard]] JtagDebugStatus readFlashWord(uint32_t word, uint16_t &value) {
      if(word == 0) {
         uint8_t low = 0;
         uint8_t high = 0;
         if(!loadZ(0) || !execute(jtagDebug::lpmZ(16))) {
            return JtagDebugStatus::NoOcd;
         }
         if(JtagDebugStatus status = readOcdr(16, low); status != JtagDebugStatus::Ok) {
            return status;
         }
         if(!loadZ(1) || !execute(jtagDebug::lpmZ(16))) {
            return JtagDebugStatus::NoOcd;
         }
         if(JtagDebugStatus status = readOcdr(16, high); status != JtagDebugStatus::Ok) {
            return status;
         }
         value = static_cast<uint16_t>(high << 8 | low);
         return JtagDebugStatus::Ok;
      }
      uint16_t instruction = jtagDebug::flashReadFill;
      uint32_t field = word;
      if(!ocd_.instructionScan(instruction, field)) {
         return JtagDebugStatus::NoOcd;
      }
      // field is the PC field now. Not 0: a field of 0 executes.
      if(field == 0) {
         field = word;
      }
      instruction = jtagDebug::flashReadFill;
      if(!ocd_.instructionScan(instruction, field)) {
         return JtagDebugStatus::NoOcd;
      }
      value = instruction;
      return JtagDebugStatus::Ok;
   }

   // EEARH:EEARL = address through r16
   [[nodiscard]] bool eepromAddress(uint16_t address) {
      return (device_.eearh == device_.eearl || writeIo(device_.eearh, static_cast<uint8_t>(address >> 8))) &&
             writeIo(device_.eearl, static_cast<uint8_t>(address));
   }

   // Reads EECR until EEPE is clear.
   [[nodiscard]] JtagDebugStatus waitEeprom() {
      const uint32_t polls = jtagDebug::eepromTimeoutMs * tap_.kHz() / jtagDebug::eepromPollCycles + 1u;
      for(uint32_t i = 0; i < polls; ++i) {
         uint8_t eecr = 0;
         if(!execute(jtagDebug::in(16, device_.eecr))) {
            return JtagDebugStatus::NoOcd;
         }
         if(JtagDebugStatus status = readOcdr(16, eecr); status != JtagDebugStatus::Ok) {
            return status;
         }
         if((eecr & jtagDebug::eecrWrite) == 0) {
            return JtagDebugStatus::Ok;
         }
      }
      return JtagDebugStatus::Timeout;
   }

   static constexpr uint16_t keep(uint16_t value) {
      return static_cast<uint16_t>(value & jtagDebug::keepMask);
   }

   // Reads reg D when the OCD did not answer yet: OCDEN can change after
   // attach().
   [[nodiscard]] JtagDebugStatus requireOcd() {
      uint16_t control = 0;
      if(!ocdAnswers_ && ocd_.readRegister(jtagOcd::reg::control, control)) {
         ocdAnswers_ = true;
      }
      return ocdAnswers_ ? JtagDebugStatus::Ok : JtagDebugStatus::NoOcd;
   }

   [[nodiscard]] bool writeBcr(uint16_t value) {
      if(!ocd_.writeRegister(jtagOcd::reg::bcr, value)) {
         return false;
      }
      bcr_ = value;
      return true;
   }

   [[nodiscard]] JtagDebugStatus waitStop() {
      const uint32_t polls = jtagDebug::stopTimeoutMs * tap_.kHz() / jtagDebug::readScanCycles + 1u;
      for(uint32_t i = 0; i < polls; ++i) {
         uint16_t control = 0;
         if(!ocd_.readRegister(jtagOcd::reg::control, control)) {
            return JtagDebugStatus::NoOcd;
         }
         if((control & jtagOcd::control::stopped) != 0) {
            return JtagDebugStatus::Ok;
         }
      }
      return JtagDebugStatus::Timeout;
   }

   // "After each stop": reg D, BCR = (BCR & 0x9FFC) | 0x4000, then BCR
   // again from the target with bit 2 clear.
   [[nodiscard]] JtagDebugStatus afterStop() {
      uint16_t value = 0;
      if(!ocd_.readRegister(jtagOcd::reg::control, value) || !writeBcr(static_cast<uint16_t>(keep(bcr_) | jtagOcd::bcr::bit14)) ||
         !ocd_.readRegister(jtagOcd::reg::bcr, value) || !writeBcr(static_cast<uint16_t>(value & ~(1u << 2)))) {
         return JtagDebugStatus::NoOcd;
      }
      return stopped();
   }

   AvrJtagTap &tap_;
   AvrJtagOcd &ocd_;
   JtagDebugDevice device_{};
   std::array<uint8_t, 32> regs_{};
   uint32_t pc_{ 0 };
   uint8_t eind_{ 0 };
   uint16_t bcr_{ 0 };
   uint16_t breakStatus_{ 0 };
   // Reg 0 (RUN_TO) to reg 3
   std::array<std::optional<uint32_t>, 4> breaks_{};
   bool ocdAnswers_{ false };
   bool stopped_{ false };
   bool contextValid_{ false };
};

} // namespace avr
