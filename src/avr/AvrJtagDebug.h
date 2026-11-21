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
//   OCD answers, attach() halts the core. The TAP reset and IDCODE give
//   TCK cycles in Run-Test/Idle: each moves the PC field of a stopped
//   core by 2 (docs/avr-jtag-ocd.md, "Instruction execute"). When reg D
//   shows a core that is already stopped, attach() removes these moves
//   from the context PC. Cycles before attach() are not known and stay.
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
//   PC - 11, as the reference debugger does on revision 4: the five
//   instructions after EIJMP add 10 to Z + 1. Revision 3 has the same
//   rule (hardware test on the ATmega32U4), not the PC - 5 of the
//   scripts. The same sequence (EIJMP, EIND)
//   goes to all devices, also to the devices without EIND (no test).
// - Memory: the data space (registers, I/O, SRAM), the flash (read only)
//   and the EEPROM, with injected instructions. They change r16, r30 and
//   r31 on the target. Thus reads and writes of r0 to r31 and EIND use
//   the context, never the target.
// - The flash read uses two IR 0xA scans for each word, as the reference
//   debugger. A field that is not 0 reads a flash word. Field 0 executes
//   the instruction, so word 0 goes as the flash size in words: the
//   address wraps at the flash size (hardware test, ATmega32U4). An
//   injected LPM does not work: on revision 3 it reads its own opcode.
//   A word address that does not fit the field (below revision 4: word
//   0x10000 and above, and word 0 of a flash of 0x10000 words or more)
//   gives OutOfRange, also for a flash page write that the read back
//   cannot verify.
// - The EEPROM write sets EEMPE, then EEPE with two adjacent injected
//   OUT instructions. That the 4-cycle limit of EEMPE holds for this is not
//   tested, so each byte is read back after the write.
// - The flash page write (writeFlashPage(), for the software breakpoints)
//   uses the SPM sequence of the reference debugger (docs/avr-jtag-ocd.md,
//   "Flash write in debug mode"): save SPMCSR and SREG (and RAMPZ above
//   64 KB), then for each SPM an EIJMP into the boot section, the SPMCSR
//   command with ST Y, and the SPM as a single step (BCR 0xA000, RUN).
//   Then restore SPMCSR (without its command bits), SREG, RAMPZ and EIND,
//   and read the page back. Differences from the reference debugger: the
//   order is one page buffer load for each word, page erase, page write,
//   RWW enable ("alternative 1" of the datasheet; the reference debugger
//   erases first), so that the flash changes only at the end (issue
//   #121). The probe polls SPMCSR after each SPM until SPMEN is clear, it writes RAMPZ only for a flash above 64 KB (bits 17..16
//   of the page address), and the boot section address is the start of
//   the last flash page. This page is in the boot section for all BOOTSZ
//   fuses; the boot start of the device context is not (avrdude sends the
//   start of the last 4 pages, which is below the smallest boot section
//   of the ATmega16). The read back finds a page that SPM did not write,
//   for example when the boot lock bits stop SPM. r0, r1, r28 and r29 also
//   change on the target: the write-back restores them. An SPM step that
//   does not stop gets a FORCE_BREAK, then the restore and Timeout. When
//   the core also does not stop then, the probe sends nothing more.
// - Hardware breakpoints (program addresses, word addresses): the host
//   numbers 1 to 3 use reg 1, reg 2 and reg 3 (mode 3), reg 0 (one-shot)
//   is the RUN_TO address. The probe keeps them and writes them before
//   each run and step: halt and reset clear the BCR enables (reference
//   sequences), and a cleared breakpoint must not stay enabled for a step
//   (a step over a breakpoint would stop at the same PC). A step drops a
//   RUN_TO address that was not hit. On revision 4, reg 4 + n gets the
//   word bits 23..16 of reg n (inference: only 0 and 0xFF were seen,
//   docs/avr-jtag-ocd.md).
// - Data breakpoints (data space addresses): numbers 2 and 3 only (reg 1
//   has no data mode), with the OCD mode read (2), write (1) or access (0).
//   A number keeps one breakpoint: a data breakpoint on a number in use,
//   or a program breakpoint on a number with a data breakpoint, gives
//   InUse. The core stops after the instruction that makes the access.
// - A data mask (an address range, AVaRICE "range BP"): on number 2 or 3,
//   for the data breakpoint on the other number. The OCD has the mask
//   only in reg 2 for reg 3 (BCR mask mode, docs/avr-jtag-ocd.md). Thus
//   the probe writes the address to reg 3 and the mask to reg 2, for each
//   order of the numbers. A 1 bit in the mask compares the address bit.
//   A mask without a data breakpoint does nothing. The pair has no
//   program breakpoint and no second mask: InUse. A clear of the data
//   breakpoint also clears the mask (AVaRICE does not clear it, #118).
// - After each stop the probe reads the BSR (the cause of the stop). The
//   reference debugger does not read it.
// - stepInstead() executes an instruction in place of the flash word at
//   the PC, as one step without a flash write (a software breakpoint,
//   issue #115). RJMP, RCALL, JMP, CALL, LDS, STS, the branches (BRBS,
//   BRBC), the skips (CPSE, SBRC, SBRS, SBIC, SBIS), RET and RETI: the
//   probe does them on the context and the data space. A call puts the
//   return address onto the stack (2 bytes, 3 above 128 KiB of flash, the
//   high byte at the lowest address), RET and RETI take it back, RETI also
//   sets SREG I. A skip skips 2 words over JMP, CALL, LDS and STS. Each
//   other instruction that does not use the PC or the flash: injected
//   after the write-back, then r0 to r31 and EIND are read again. Indirect
//   jumps and calls, LPM, ELPM, SPM, SLEEP and BREAK: canStepInstead() is
//   false. The data breakpoints do not see these steps. Only the injection
//   of LDI, MOV, IN, OUT, LD, ST and SPM had a hardware test before.
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
   // An EEPROM byte or a flash page read back different after the write
   VerifyFailed,
   // The breakpoint number holds another breakpoint.
   InUse,
};

// The OCD modes of a data breakpoint (BCR mode of reg 2 and reg 3,
// docs/avr-jtag-ocd.md). Access on a write is open question 8: the
// hardware test showed only the stop on a read.
enum class JtagDataMode : uint8_t {
   Access = 0,
   Write = 1,
   Read = 2,
};

// The device values for the debug mode (EDBG device context). The register
// addresses are I/O addresses. Defaults: ATmega32U4.
struct JtagDebugDevice {
   uint32_t flashBytes{ 32768 };
   uint16_t flashPageBytes{ 128 };
   // A data space address (I/O + 0x20)
   uint8_t spmcsr{ 0x57 };
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

// The PC write loads Z with the PC minus this offset (word addresses),
// on revision 3 and 4.
inline constexpr uint32_t pcOffset = 11;

// An EEPROM write of a stopped core took 34 ms on the ATmega32U4
// (hardware test), not the 3.4 ms of the datasheet.
inline constexpr uint32_t eepromTimeoutMs = 100;
// TCK cycles of one EEPE poll, at least: two instruction scans and two
// register reads
inline constexpr uint32_t eepromPollCycles = 2 * (3 + 32 + 2) + 2 * readScanCycles;

inline constexpr uint8_t ioEind = 0x3C;
inline constexpr uint8_t ioRampz = 0x3B;
inline constexpr uint8_t ioSreg = 0x3F;
inline constexpr uint16_t dataIo = 0x20;
inline constexpr uint16_t dataSram = 0x60;
// SPL (SPH at + 1)
inline constexpr uint16_t dataSp = 0x5D;
inline constexpr uint16_t dataSreg = 0x5F;
inline constexpr uint8_t sregI = 0x80;
inline constexpr uint8_t eecrRead = 0x01;
inline constexpr uint8_t eecrWrite = 0x02;
inline constexpr uint8_t eecrMasterWrite = 0x04;

// The SPMCSR commands (datasheet): SPMEN alone is a page buffer load.
inline constexpr uint8_t spmEnable = 0x01;
inline constexpr uint8_t spmPageErase = 0x03;
inline constexpr uint8_t spmPageWrite = 0x05;
inline constexpr uint8_t spmRwwEnable = 0x11;
inline constexpr uint8_t spmCommandBits = 0x1F;
// The single step of an SPM in the reference sequence
inline constexpr uint16_t spmStep = jtagOcd::bcr::bit15 | jtagOcd::bcr::step;
static_assert(spmStep == 0xA000);
// Longer than tWD_FLASH (4.5 ms) of the datasheets
inline constexpr uint32_t spmTimeoutMs = 100;

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
// ST Y, Rr
constexpr uint16_t stY(uint8_t r) {
   return static_cast<uint16_t>(0x8208u | ((r & 0x1Fu) << 4));
}
inline constexpr uint16_t eijmp = 0x9419;
inline constexpr uint16_t spm = 0x95E8;
inline constexpr uint16_t wdr = 0x95A8;
// The instruction bits of the flash read scans
inline constexpr uint16_t flashReadFill = 0xFFFF;

static_assert(ldi(31, 0x02) == 0xE0F2);
static_assert(out(0x31, 16) == 0xBF01);
static_assert(mov(5, 16) == 0x2E50);
static_assert(stY(16) == 0x8308);

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
      const uint32_t idleBefore = tap_.idleCycles();
      uint32_t idcode = 0;
      if(!tap_.reset() || !tap_.readIdcode(idcode) || !tap::isAtmelIdcode(idcode)) {
         return JtagDebugStatus::NoTap;
      }
      uint16_t control = 0;
      if(!ocd_.readRegister(jtagOcd::reg::control, control)) {
         return JtagDebugStatus::Ok;
      }
      ocdAnswers_ = true;
      // A core that stops during attach() (a breakpoint) shows here as
      // running: no correction for it.
      const uint32_t moves = (control & jtagOcd::control::stopped) != 0 ? tap_.idleCycles() - idleBefore : 0;
      const JtagDebugStatus status = halt();
      if(status == JtagDebugStatus::Ok) {
         pc_ = (pc_ - 2u * moves) & fieldMask();
      }
      return status;
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

   // True when the core is stopped and its context is valid: the PC and
   // the registers are the ones of the target.
   [[nodiscard]] bool hasContext() const {
      return stopped_ && contextValid_;
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
      if((breaks_[number] && breaks_[number]->mode != jtagOcd::bcr::modeProgram) ||
         (partner(number) != 0 && maskNumber() == partner(number))) {
         return JtagDebugStatus::InUse;
      }
      breaks_[number] = Break{ word, jtagOcd::bcr::modeProgram };
      return JtagDebugStatus::Ok;
   }

   // Sets data breakpoint number (2 or 3) at a data space address. It takes
   // effect at the next run.
   [[nodiscard]] JtagDebugStatus setDataBreakpoint(uint8_t number, uint16_t address, JtagDataMode mode) {
      if(number < 2 || number > hardwareBreaks) {
         return JtagDebugStatus::OutOfRange;
      }
      if(breaks_[number]) {
         return JtagDebugStatus::InUse;
      }
      breaks_[number] = Break{ address, static_cast<uint8_t>(mode) };
      return JtagDebugStatus::Ok;
   }

   // Sets a data mask on number 2 or 3 for the data breakpoint on the
   // other number. It takes effect at the next run.
   [[nodiscard]] JtagDebugStatus setDataMask(uint8_t number, uint16_t mask) {
      if(number < 2 || number > hardwareBreaks) {
         return JtagDebugStatus::OutOfRange;
      }
      const std::optional<Break> &other = breaks_[partner(number)];
      if(breaks_[number] || (other && (other->mode == jtagOcd::bcr::modeProgram || other->mode == maskEntry))) {
         return JtagDebugStatus::InUse;
      }
      breaks_[number] = Break{ mask, maskEntry };
      return JtagDebugStatus::Ok;
   }

   [[nodiscard]] JtagDebugStatus clearBreakpoint(uint8_t number) {
      if(number < 1 || number > hardwareBreaks) {
         return JtagDebugStatus::OutOfRange;
      }
      // AVaRICE clears only the data breakpoint of a range, not its mask
      // (issue #118): the mask goes with its data breakpoint.
      if(breaks_[number] && partner(number) != 0 && maskNumber() == partner(number)) {
         breaks_[partner(number)].reset();
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
      breaks_[0].reset();
      if(runTo) {
         breaks_[0] = Break{ *runTo, jtagOcd::bcr::modeProgram };
      }
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
      breaks_[0].reset();
      if(!writeBack() || !writeBreakpoints() || !writeBcr(static_cast<uint16_t>(keep(bcr_) | jtagOcd::bcr::step)) ||
         !ocd_.run()) {
         return JtagDebugStatus::NoOcd;
      }
      stopped_ = false;
      if(JtagDebugStatus status = waitStop(); status != JtagDebugStatus::Ok) {
         return status;
      }
      return afterStop();
   }

   // True when stepInstead() can execute instruction (see above)
   [[nodiscard]] static constexpr bool canStepInstead(uint16_t instruction) {
      return stepKind(instruction) != StepKind::None;
   }

   // Executes instruction as one step in place of the flash word at the PC
   // (see above). after: the word after it, when the flash does not hold it
   // (a software breakpoint), else it comes from the flash. As step(), but
   // with no break event cause and no stop of the core.
   [[nodiscard]] JtagDebugStatus stepInstead(uint16_t instruction, std::optional<uint16_t> after = std::nullopt) {
      if(JtagDebugStatus status = ready(); status != JtagDebugStatus::Ok) {
         return status;
      }
      const StepKind kind = stepKind(instruction);
      if(kind == StepKind::None) {
         return JtagDebugStatus::OutOfRange;
      }
      breaks_[0].reset();
      const uint32_t words = device_.flashBytes / 2u;
      uint32_t next = pc_ + 1u;
      uint16_t second = after.value_or(0);
      if(!after && (twoWords(kind) || kind == StepKind::Skip)) {
         if(JtagDebugStatus status = readFlashWord((pc_ + 1u) % words, second); status != JtagDebugStatus::Ok) {
            return status;
         }
      }
      if(twoWords(kind)) {
         next = pc_ + 2u;
      }
      // Rd of LDS, Rr of STS, SBRC and SBRS
      const auto r = static_cast<uint8_t>((instruction >> 4) & 0x1Fu);
      const auto bit = static_cast<uint8_t>(1u << (instruction & 7u));
      // The 12-bit offset of RJMP and RCALL, sign-extended
      const uint32_t relative = pc_ + 1u + (static_cast<uint32_t>(instruction & 0x0FFFu) ^ 0x800u) - 0x800u;
      const uint32_t absolute = ((instruction & 0x01F0u) << 13u | (instruction & 1u) << 16u) | second;
      JtagDebugStatus status = JtagDebugStatus::Ok;
      switch(kind) {
      case StepKind::Inject:
         status = inject(instruction);
         break;
      case StepKind::Rjmp:
         next = relative;
         break;
      case StepKind::Rcall:
         status = push(next);
         next = relative;
         break;
      case StepKind::Jmp:
         next = absolute;
         break;
      case StepKind::Call:
         status = push(next);
         next = absolute;
         break;
      case StepKind::Lds:
         status = readData(second, &regs_[r], 1);
         break;
      case StepKind::Sts:
         status = writeData(second, &regs_[r], 1);
         break;
      case StepKind::Branch: {
         // BRBS (bit 10 clear) or BRBC, 7-bit offset
         uint8_t sreg = 0;
         status = readData(jtagDebug::dataSreg, &sreg, 1);
         if(((sreg & bit) != 0) == ((instruction & 0x0400u) == 0)) {
            next = pc_ + 1u + (((instruction >> 3u) & 0x7Fu) ^ 0x40u) - 0x40u;
         }
         break;
      }
      case StepKind::Skip: {
         bool skip = false;
         if((instruction & 0xFC00u) == 0x1000u) { // CPSE
            skip = regs_[r] == regs_[((instruction >> 5u) & 0x10u) | (instruction & 0x0Fu)];
         } else if((instruction & 0xFC00u) == 0xFC00u) { // SBRC, SBRS
            skip = ((regs_[r] & bit) != 0) == ((instruction & 0x0200u) != 0);
         } else { // SBIC, SBIS
            uint8_t io = 0;
            status = readData(static_cast<uint16_t>(jtagDebug::dataIo + ((instruction >> 3u) & 0x1Fu)), &io, 1);
            skip = ((io & bit) != 0) == ((instruction & 0x0200u) != 0);
         }
         if(skip) {
            next = pc_ + (twoWords(stepKind(second)) ? 3u : 2u);
         }
         break;
      }
      case StepKind::Ret:
         status = pop(next, (instruction & 0x0010u) != 0);
         break;
      case StepKind::None:
         break;
      }
      if(status != JtagDebugStatus::Ok) {
         return status;
      }
      pc_ = next % words;
      breakStatus_ = 0;
      return JtagDebugStatus::Ok;
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

   // Writes one flash page at a page-aligned byte address with SPM (see
   // above), and reads it back. The page has device.flashPageBytes bytes.
   [[nodiscard]] JtagDebugStatus writeFlashPage(uint32_t address, const uint8_t *page) {
      JtagDebugStatus status = beginFlashPage(address, page);
      bool loaded = false;
      while(status == JtagDebugStatus::Ok && !loaded) {
         status = loadFlashPage(device_.flashPageBytes / 2u, loaded);
      }
      if(status == JtagDebugStatus::Ok) {
         status = endFlashPage();
      }
      return status == JtagDebugStatus::Ok ? verifyFlashPage(address, page, 0, device_.flashPageBytes) : status;
   }

   // writeFlashPage() in parts, so that a host that waits for an event gets
   // its answers between them (issue #121). beginFlashPage() saves the
   // state, each loadFlashPage() loads up to words words of the page
   // buffer, and endFlashPage() erases and writes the page and restores the
   // state. The flash changes only in endFlashPage(): the page buffer keeps
   // its data over the page erase (datasheet, "alternative 1"). page must
   // stay valid until the end. cancelFlashPage() clears the page buffer and
   // restores the state. A part that fails ends the write: it restores the
   // state. Between the parts only these calls may go to the core.
   // verifyFlashPage() reads the page back after the end.
   [[nodiscard]] JtagDebugStatus beginFlashPage(uint32_t address, const uint8_t *page) {
      if(JtagDebugStatus status = ready(); status != JtagDebugStatus::Ok) {
         return status;
      }
      const uint16_t pageBytes = device_.flashPageBytes;
      // SPMCSR goes through LD Z and ST Y: any data address above the
      // registers (0x68 on the ATmega64 and ATmega128). 0: not sent.
      if(pageBytes < 2 || address % pageBytes != 0 || !inRange(address, pageBytes, device_.flashBytes) ||
         device_.spmcsr < jtagDebug::dataIo || flashReadField(address / 2u) == 0 ||
         flashReadField((address + pageBytes) / 2u - 1u) == 0) {
         return JtagDebugStatus::OutOfRange;
      }
      write_ = { address, page, 0, 0, 0, 0, device_.flashBytes > 0x10000u };
      if(!loadZ(device_.spmcsr) || !execute(jtagDebug::ldZ(16))) {
         return JtagDebugStatus::NoOcd;
      }
      if(JtagDebugStatus status = readOcdr(16, write_.spmcsr); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(JtagDebugStatus status = readIo(jtagDebug::ioSreg, write_.sreg); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(write_.far) {
         if(JtagDebugStatus status = readIo(jtagDebug::ioRampz, write_.rampz); status != JtagDebugStatus::Ok) {
            return status;
         }
      }
      writing_ = true;
      if(write_.far && !writeIo(jtagDebug::ioRampz, static_cast<uint8_t>(address >> 16))) {
         return finishFlashPage(JtagDebugStatus::NoOcd);
      }
      return JtagDebugStatus::Ok;
   }

   // loaded: all words of the page are in the page buffer.
   [[nodiscard]] JtagDebugStatus loadFlashPage(uint16_t words, bool &loaded) {
      if(!writing_) {
         return JtagDebugStatus::NoContext;
      }
      const auto z = static_cast<uint16_t>(write_.address);
      JtagDebugStatus status = JtagDebugStatus::Ok;
      for(uint16_t i = 0; i < words && write_.next < device_.flashPageBytes && status == JtagDebugStatus::Ok; ++i) {
         status = spm(jtagDebug::spmEnable, static_cast<uint16_t>(z + write_.next), write_.page + write_.next);
         write_.next = static_cast<uint16_t>(write_.next + 2u);
      }
      loaded = write_.next >= device_.flashPageBytes;
      return status == JtagDebugStatus::Ok ? status : finishFlashPage(status);
   }

   // Page erase, page write, RWW enable, then the restore. Not the RWW
   // enable between the loads and the erase: RWWSRE clears the page buffer.
   [[nodiscard]] JtagDebugStatus endFlashPage() {
      if(!writing_) {
         return JtagDebugStatus::NoContext;
      }
      const auto z = static_cast<uint16_t>(write_.address);
      JtagDebugStatus status = write_.next >= device_.flashPageBytes ? spm(jtagDebug::spmPageErase, z) : JtagDebugStatus::NoContext;
      if(status == JtagDebugStatus::Ok) {
         status = spm(jtagDebug::spmPageWrite, z);
      }
      if(status == JtagDebugStatus::Ok) {
         status = spm(jtagDebug::spmRwwEnable, z);
      }
      return finishFlashPage(status);
   }

   // The flash is not changed. The RWW enable clears the page buffer, so
   // that no SPM of the program writes the old buffer data.
   [[nodiscard]] JtagDebugStatus cancelFlashPage() {
      if(!writing_) {
         return JtagDebugStatus::Ok;
      }
      return finishFlashPage(spm(jtagDebug::spmRwwEnable, static_cast<uint16_t>(write_.address)));
   }

   // Compares bytes at..at + length - 1 of the page at the byte address
   // with the flash. A different word gives VerifyFailed, for example when
   // the boot lock bits stop SPM.
   [[nodiscard]] JtagDebugStatus verifyFlashPage(uint32_t address, const uint8_t *page, uint16_t at, uint16_t length) {
      if(JtagDebugStatus status = ready(); status != JtagDebugStatus::Ok) {
         return status;
      }
      for(uint16_t i = at; i < at + length; i += 2) {
         uint16_t word = 0;
         if(JtagDebugStatus status = readFlashWord((address + i) / 2u, word); status != JtagDebugStatus::Ok) {
            return status;
         }
         if(word != static_cast<uint16_t>(page[i] | page[i + 1u] << 8u)) {
            return JtagDebugStatus::VerifyFailed;
         }
      }
      return JtagDebugStatus::Ok;
   }

   // True between beginFlashPage() and the end of the write
   [[nodiscard]] bool writingFlashPage() const {
      return writing_;
   }

   private:
   // How stepInstead() executes an instruction
   enum class StepKind : uint8_t { Inject, Rjmp, Rcall, Jmp, Call, Lds, Sts, Branch, Skip, Ret, None };

   [[nodiscard]] static constexpr bool twoWords(StepKind kind) {
      return kind == StepKind::Jmp || kind == StepKind::Call || kind == StepKind::Lds || kind == StepKind::Sts;
   }

   [[nodiscard]] static constexpr StepKind stepKind(uint16_t op) {
      if((op & 0xF000u) == 0xC000u) {
         return StepKind::Rjmp;
      }
      if((op & 0xF000u) == 0xD000u) {
         return StepKind::Rcall;
      }
      if((op & 0xFE0Eu) == 0x940Cu) {
         return StepKind::Jmp;
      }
      if((op & 0xFE0Eu) == 0x940Eu) {
         return StepKind::Call;
      }
      if((op & 0xFE0Fu) == 0x9000u) {
         return StepKind::Lds;
      }
      if((op & 0xFE0Fu) == 0x9200u) {
         return StepKind::Sts;
      }
      if((op & 0xF800u) == 0xF000u) { // BRBS, BRBC
         return StepKind::Branch;
      }
      if((op & 0xFC00u) == 0xFC00u || (op & 0xFC00u) == 0x1000u || (op & 0xFD00u) == 0x9900u) { // SBRC, SBRS, CPSE, SBIC, SBIS
         return StepKind::Skip;
      }
      if((op & 0xFFEFu) == 0x9508u) { // RET, RETI
         return StepKind::Ret;
      }
      // LPM and ELPM Rd, Z(+); IJMP, EIJMP, ICALL, EICALL; SLEEP, BREAK;
      // LPM, ELPM, SPM, SPM Z+
      const bool usesPcOrFlash =
      (op & 0xFE0Cu) == 0x9004u || (op & 0xFEEFu) == 0x9409u || (op & 0xFFEFu) == 0x9588u || (op & 0xFFCFu) == 0x95C8u;
      return usesPcOrFlash ? StepKind::None : StepKind::Inject;
   }

   // The core executes instruction with the registers of the context, then
   // the context gets the new registers. A failed read leaves no valid
   // context.
   [[nodiscard]] JtagDebugStatus inject(uint16_t instruction) {
      if(!writeBack() || !execute(instruction)) {
         return JtagDebugStatus::NoOcd;
      }
      contextValid_ = false;
      const JtagDebugStatus status = readRegisters(nullptr);
      contextValid_ = status == JtagDebugStatus::Ok;
      return status;
   }

   // The return address of a call goes onto the stack: 2 bytes, 3 above
   // 128 KiB of flash. SP is at the data address 0x5D (SPL) and 0x5E.
   [[nodiscard]] JtagDebugStatus push(uint32_t ret) {
      std::array<uint8_t, 2> sp{};
      if(JtagDebugStatus status = readData(jtagDebug::dataSp, sp.data(), 2); status != JtagDebugStatus::Ok) {
         return status;
      }
      const uint8_t bytes = returnBytes();
      std::array<uint8_t, 3> stack{};
      for(uint8_t i = 0; i < bytes; ++i) {
         // The low byte at SP, the high byte at the lowest address
         stack[bytes - 1u - i] = static_cast<uint8_t>(ret >> (8u * i));
      }
      const auto top = static_cast<uint16_t>(sp[0] | sp[1] << 8u);
      const auto newTop = static_cast<uint16_t>(top - bytes);
      if(JtagDebugStatus status = writeData(static_cast<uint16_t>(newTop + 1u), stack.data(), bytes); status != JtagDebugStatus::Ok) {
         return status;
      }
      sp = { static_cast<uint8_t>(newTop), static_cast<uint8_t>(newTop >> 8) };
      return writeData(jtagDebug::dataSp, sp.data(), 2);
   }

   // RET and RETI: ret = the return address from the stack (see push()).
   // reti: SREG I is set.
   [[nodiscard]] JtagDebugStatus pop(uint32_t &ret, bool reti) {
      std::array<uint8_t, 2> sp{};
      if(JtagDebugStatus status = readData(jtagDebug::dataSp, sp.data(), 2); status != JtagDebugStatus::Ok) {
         return status;
      }
      const uint8_t bytes = returnBytes();
      const auto top = static_cast<uint16_t>(sp[0] | sp[1] << 8u);
      std::array<uint8_t, 3> stack{};
      if(JtagDebugStatus status = readData(static_cast<uint16_t>(top + 1u), stack.data(), bytes); status != JtagDebugStatus::Ok) {
         return status;
      }
      ret = 0;
      for(uint8_t i = 0; i < bytes; ++i) {
         ret = ret << 8u | stack[i];
      }
      const auto newTop = static_cast<uint16_t>(top + bytes);
      sp = { static_cast<uint8_t>(newTop), static_cast<uint8_t>(newTop >> 8) };
      if(JtagDebugStatus status = writeData(jtagDebug::dataSp, sp.data(), 2); status != JtagDebugStatus::Ok || !reti) {
         return status;
      }
      uint8_t sreg = 0;
      if(JtagDebugStatus status = readData(jtagDebug::dataSreg, &sreg, 1); status != JtagDebugStatus::Ok) {
         return status;
      }
      sreg = static_cast<uint8_t>(sreg | jtagDebug::sregI);
      return writeData(jtagDebug::dataSreg, &sreg, 1);
   }

   // The bytes of a return address: 3 above 128 KiB of flash (22-bit PC)
   [[nodiscard]] uint8_t returnBytes() const {
      return device_.flashBytes > 0x20000u ? 3 : 2;
   }

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
      const JtagDebugStatus status = readRegisters(&field);
      pc_ = (field - 1u) & fieldMask();
      contextValid_ = status == JtagDebugStatus::Ok;
      return status;
   }

   // r0 to r31 and EIND from the target. field: the PC field of the first
   // scan.
   [[nodiscard]] JtagDebugStatus readRegisters(uint32_t *field) {
      for(uint8_t r = 0; r < regs_.size(); ++r) {
         if(JtagDebugStatus status = readOcdr(r, regs_[r], r == 0 ? field : nullptr); status != JtagDebugStatus::Ok) {
            return status;
         }
      }
      if(!execute(jtagDebug::in(16, jtagDebug::ioEind))) {
         return JtagDebugStatus::NoOcd;
      }
      return readOcdr(16, eind_);
   }

   [[nodiscard]] uint32_t fieldMask() const {
      return (uint32_t{ 1 } << jtagOcd::fieldBits(ocd_.revision())) - 1u;
   }

   // The BCR enable and mode bits of breakpoint n (reg 0 and reg 1 have
   // no mode field)
   [[nodiscard]] static uint16_t enableBits(uint8_t n, uint8_t mode) {
      namespace bcr = jtagOcd::bcr;
      switch(n) {
      case 0:
         return bcr::enable0;
      case 1:
         return bcr::enable1;
      case 2:
         return static_cast<uint16_t>(bcr::enable2 | (mode << bcr::mode2Shift));
      default:
         return static_cast<uint16_t>(bcr::enable3 | (mode << bcr::mode3Shift));
      }
   }

   // The number (2 or 3) that holds the data mask, else 0
   [[nodiscard]] uint8_t maskNumber() const {
      for(uint8_t n = 2; n <= hardwareBreaks; ++n) {
         if(breaks_[n] && breaks_[n]->mode == maskEntry) {
            return n;
         }
      }
      return 0;
   }

   // The other number of the pair 2 and 3 (else 0)
   [[nodiscard]] static uint8_t partner(uint8_t number) {
      return number == 2 || number == 3 ? static_cast<uint8_t>(5 - number) : 0;
   }

   // Reg n, and reg 4 + n (bits 23..16) on revision 4
   [[nodiscard]] bool writeAddress(uint8_t n, uint32_t value) {
      return ocd_.writeRegister(n, static_cast<uint16_t>(value)) &&
             (ocd_.revision() < 4 || ocd_.writeRegister(static_cast<uint8_t>(jtagOcd::reg::breakHigh0 + n),
                                                        static_cast<uint16_t>((value >> 16) & 0xFFu)));
   }

   // Reg n (and reg 4 + n on revision 4) for each breakpoint, then the BCR
   // enables. The BCR bits 11 to 3 come from the breakpoints only. A data
   // breakpoint with a mask: reg 3, the mask in reg 2 and the mask mode.
   [[nodiscard]] bool writeBreakpoints() {
      auto bcr = static_cast<uint16_t>(bcr_ & ~jtagDebug::breakBits);
      const uint8_t maskAt = maskNumber();
      for(uint8_t n = 0; n < breaks_.size(); ++n) {
         if(!breaks_[n] || (maskAt != 0 && n == maskAt)) {
            continue;
         }
         if(maskAt != 0 && n == partner(maskAt)) {
            if(!writeAddress(3, breaks_[n]->address) || !writeAddress(2, breaks_[maskAt]->address)) {
               return false;
            }
            bcr = static_cast<uint16_t>(bcr | jtagOcd::bcr::maskMode | enableBits(3, breaks_[n]->mode));
            continue;
         }
         if(!writeAddress(n, breaks_[n]->address)) {
            return false;
         }
         bcr = static_cast<uint16_t>(bcr | enableBits(n, breaks_[n]->mode));
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
      const uint32_t z = (pc_ - jtagDebug::pcOffset) & fieldMask();
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
      return write ? commit() : JtagDebugStatus::Ok;
   }

   // The core executes an injected instruction at the next scan
   // (avr/AvrJtagOcd.h). A reg D read executes the last one now.
   [[nodiscard]] JtagDebugStatus commit() {
      uint16_t control = 0;
      return ocd_.readRegister(jtagOcd::reg::control, control) ? JtagDebugStatus::Ok : JtagDebugStatus::NoOcd;
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

   // IN r16, io, then the OCDR
   [[nodiscard]] JtagDebugStatus readIo(uint8_t io, uint8_t &value) {
      if(!execute(jtagDebug::in(16, io))) {
         return JtagDebugStatus::NoOcd;
      }
      return readOcdr(16, value);
   }

   // The end of a page write: the restore of the state
   [[nodiscard]] JtagDebugStatus finishFlashPage(JtagDebugStatus status) {
      writing_ = false;
      if(!stopped_) {
         // An SPM step did not stop, also not after FORCE_BREAK: no
         // instruction can go to the core.
         return status;
      }
      // The restore also after a failure: the program must not run with a
      // changed SREG or RAMPZ.
      const bool restored = (!write_.far || writeIo(jtagDebug::ioRampz, write_.rampz)) &&
                            writeIo(jtagDebug::ioSreg, write_.sreg) && loadZ(device_.spmcsr) &&
                            execute(jtagDebug::ldi(16, static_cast<uint8_t>(write_.spmcsr & ~jtagDebug::spmCommandBits))) &&
                            execute(jtagDebug::stZ(16)) && writeIo(jtagDebug::ioEind, eind_) && commit() == JtagDebugStatus::Ok;
      if(status != JtagDebugStatus::Ok) {
         return status;
      }
      return restored ? JtagDebugStatus::Ok : JtagDebugStatus::NoOcd;
   }

   // One SPM with command at Z = z, r1:r0 = word (a page buffer load), in
   // the reference order: EIJMP into the boot section, Z, r0 and r1,
   // BCR 0xC000, the command to SPMCSR with ST Y, BCR 0xA000, SPM and RUN
   // (one step). Then the stop, BCR 0xC000, and SPMCSR until SPMEN is
   // clear.
   [[nodiscard]] JtagDebugStatus spm(uint8_t command, uint16_t z, const uint8_t *word = nullptr) {
      // Each injected instruction after the EIJMP moves the PC by about 2
      // words: the 10 instructions before SPM must stay in the last page
      // (64 words at the smallest JTAG page). To check in the hardware test.
      const uint32_t boot = (device_.flashBytes - device_.flashPageBytes) / 2u;
      if(!loadZ(static_cast<uint16_t>(boot)) || !writeIo(jtagDebug::ioEind, static_cast<uint8_t>(boot >> 16)) ||
         !execute(jtagDebug::eijmp) || !loadZ(z)) {
         return JtagDebugStatus::NoOcd;
      }
      if(word != nullptr && (!execute(jtagDebug::ldi(16, word[0])) || !execute(jtagDebug::mov(0, 16)) ||
                             !execute(jtagDebug::ldi(16, word[1])) || !execute(jtagDebug::mov(1, 16)))) {
         return JtagDebugStatus::NoOcd;
      }
      if(!writeBcr(jtagDebug::idle) || !execute(jtagDebug::ldi(16, command)) || !execute(jtagDebug::ldi(29, 0)) ||
         !execute(jtagDebug::ldi(28, device_.spmcsr)) || !execute(jtagDebug::stY(16)) ||
         !writeBcr(jtagDebug::spmStep) || !execute(jtagDebug::spm) || !ocd_.run()) {
         return JtagDebugStatus::NoOcd;
      }
      if(JtagDebugStatus status = waitStop(); status != JtagDebugStatus::Ok) {
         // The core can run in the boot section now: stop it, keep the
         // cached context (the write-back restores it), and give the
         // failure after the restore.
         if(!ocd_.forceBreak() || waitStop() != JtagDebugStatus::Ok || !writeBcr(jtagDebug::idle)) {
            stopped_ = false;
            return status;
         }
         return JtagDebugStatus::Timeout;
      }
      if(!writeBcr(jtagDebug::idle) || !loadZ(device_.spmcsr)) {
         return JtagDebugStatus::NoOcd;
      }
      // The same poll cost as the EEPROM: two instructions, two reads
      const uint32_t polls = jtagDebug::spmTimeoutMs * tap_.kHz() / jtagDebug::eepromPollCycles + 1u;
      for(uint32_t i = 0; i < polls; ++i) {
         uint8_t value = 0;
         if(!execute(jtagDebug::ldZ(16))) {
            return JtagDebugStatus::NoOcd;
         }
         if(JtagDebugStatus status = readOcdr(16, value); status != JtagDebugStatus::Ok) {
            return status;
         }
         if((value & jtagDebug::spmEnable) == 0) {
            return JtagDebugStatus::Ok;
         }
      }
      return JtagDebugStatus::Timeout;
   }

   // The field that reads word: word 0 as the flash size (see above). 0
   // when the field cannot hold it.
   [[nodiscard]] uint32_t flashReadField(uint32_t word) const {
      const uint32_t address = word == 0 ? device_.flashBytes / 2u : word;
      return address <= fieldMask() ? address : 0;
   }

   // Two IR 0xA scans: the word address, then the PC field. The second
   // scan returns the word.
   [[nodiscard]] JtagDebugStatus readFlashWord(uint32_t word, uint16_t &value) {
      const uint32_t address = flashReadField(word);
      if(address == 0) {
         return JtagDebugStatus::OutOfRange;
      }
      uint16_t instruction = jtagDebug::flashReadFill;
      uint32_t field = address;
      if(!ocd_.instructionScan(instruction, field)) {
         return JtagDebugStatus::NoOcd;
      }
      // field is the PC field now. Not 0: a field of 0 executes.
      if(field == 0) {
         field = address;
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
      // A page write between its parts holds the core state (SREG, RAMPZ).
      if(writing_) {
         return JtagDebugStatus::InUse;
      }
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
   // The page write of beginFlashPage() to endFlashPage()
   struct PageWrite {
      uint32_t address;
      const uint8_t *page;
      uint16_t next;
      uint8_t spmcsr;
      uint8_t sreg;
      uint8_t rampz;
      bool far;
   };
   PageWrite write_{};
   bool writing_{ false };
   uint16_t bcr_{ 0 };
   uint16_t breakStatus_{ 0 };
   // A program breakpoint (word address, mode 3), a data breakpoint (data
   // space address, a JtagDataMode) or a data mask (the mask, maskEntry)
   static constexpr uint8_t maskEntry = 4;
   struct Break {
      uint32_t address;
      uint8_t mode;
   };
   // Reg 0 (RUN_TO) to reg 3
   std::array<std::optional<Break>, 4> breaks_{};
   bool ocdAnswers_{ false };
   bool stopped_{ false };
   bool contextValid_{ false };
};

} // namespace avr
