// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The AVR8 generic scope of the EDBG protocol ("EDBG-based Tools
// Protocols", DS50002630A, chapter 7) on debugWIRE and on JTAG: the
// commands of avrdude (jtagice3_dw, jtagice3 JTAG) and Bloom.
// Hardware-independent: it runs on an avr::DebugWireTarget
// (avr/DebugWireTarget.h) or an avr::AvrJtagTarget (avr/AvrJtagTarget.h),
// and the host unit tests use the mock targets. The target gives the traits
// of its physical interface: avr8Variant and avr8Physical (the values that
// ACTIVATE_PHYSICAL needs), hardwareBreaks (the host numbers 1 to
// hardwareBreaks) and hasProgmode. Avr8Select (below) puts the scope of
// each target on one EDBG transport. The EDBG transport gives one command
// payload and sends the response, and gives the AVR_EVT requests to
// event().
//
// The commands below are for debugWIRE. JTAG (hasProgmode): see "JTAG"
// below.
//
// A command is [id][version 0][fields]. Values are LSB first. The
// responses are OK [0x80][0], DATA [0x84][1][data][0], PC [0x83][0][pc
// u32] and FAILED [0xA0][0][code]. Addresses of flash, SRAM and EEPROM are
// byte addresses. The PC, RUN_TO and the break event use word addresses.
//
//   0x01 SET, 0x02 GET [context][address][bytes][data]: the parameters.
//        Context 0: variant, function. Context 1: address 0 the physical
//        interface, 0x01 the JTAG daisy chain (4 bytes: SET of a value
//        that is not 0 gives FAILED 0x39, one device only), 0x20 and 0x21
//        the JTAG programming and debug clocks (kHz u16, 200 by default,
//        0 gives FAILED 0x3B). Context 2: the device context (below).
//        Context 3 address 0: run the timers of the target while it is
//        stopped. The host can set them in any order (Bloom sets the
//        clock and the daisy chain before the variant).
//   0x10 ACTIVATE_PHYSICAL [reset]: needs the variant and the physical of
//        the target (debugWIRE: 1 and 5), connects and stops the target
//        (break).
//        The reset flag is not used: the RESET pin is the dW line.
//        Without a device context (all 0, as avrdude sends it before
//        DISABLE_DEBUGWIRE): the attach does not save SREG (DWDR is not
//        known), and only DISABLE_DEBUGWIRE, DETACH and
//        DEACTIVATE_PHYSICAL work. The other commands give FAILED 0x33.
//   0x11 DEACTIVATE_PHYSICAL: a stopped target gets its software
//        breakpoints removed, a reset and runs; the line is then
//        released. OK also without an active physical (avrdude ISP).
//   0x12 GET_ID -> DATA [ID low][ID high][0x1E][0]: the dW ID (for
//        example 0x950F) as the signature 1E 95 0F, read backwards.
//   0x13 ATTACH [break]: break 1: the target stays stopped, and a break
//        event comes. Break 0: the target runs.
//   0x14 DETACH: the software breakpoints are removed, the target runs.
//        OK also without an active physical (Bloom after a disable).
//   0x15 ENTER_PROGMODE, 0x16 LEAVE_PROGMODE: OK. debugWIRE has no
//        programming mode: the memory commands work in the debug mode.
//   0x17 DISABLE_DEBUGWIRE: dW off until a power cycle (ISP works then).
//   0x21 READ_MEMORY [type][address u32][length u32]
//   0x22 READ_MEMORY_MASKED [type][address u32][length u32][mask]: SRAM
//        only. Mask bit i (byte i / 8, bit i % 8) 0: the byte is not read
//        and is 0.
//   0x23 WRITE_MEMORY [type][address u32][length u32][async][data]
//   0x30 RESET [mode]: reset, the target stops at 0 with a break event.
//   0x31 STOP [mode]: a running target stops with a break event.
//   0x32 RUN, 0x33 RUN_TO [word address u32], 0x34 STEP [level][mode]:
//        the step gives a break event.
//   0x35 PC_READ -> PC [pc u32], 0x36 PC_WRITE [pc u32]
//   0x40 HW_BREAK_SET [type][number][address u32][mode], 0x41
//        HW_BREAK_CLEAR [number]: number 1 only (dW has one hardware
//        breakpoint). RUN_TO uses it for one run.
//   0x43 SW_BREAK_SET, 0x44 SW_BREAK_CLEAR [address u32]...,
//        0x45 SW_BREAK_CLEAR_ALL: up to avr8::maxSoftwareBreaks.
//   others: FAILED 0x90 (ERASE, CRC, PAGE_ERASE) or 0x91.
//
// The memory types: SRAM 0x20 (the data space: registers, I/O, SRAM;
// DWDR reads as 0, a write skips it), EEPROM 0x22 and EEPROM_PAGE 0xB1
// (byte by byte), SPM 0xA0 and FLASH_PAGE 0xB0 (a write takes whole,
// aligned pages: the probe erases and writes each page), SIGN_JTAG 0xB4
// (3 bytes: 0x1E and the dW ID). One read gives up to the response size
// minus 3 bytes.
//
// The device context (context 2, as avrdude struct mega_device_desc), for
// one debugWIRE session: its end clears it. avrdude and Bloom send it before
// each ACTIVATE_PHYSICAL.
//   0x00 flash page bytes u16, 0x02 flash bytes u32, 0x0A boot start
//   (words) u32, 0x10 EEPROM bytes u16, 0x18 DWDR (OCDR), 0x19 EEARH,
//   0x1A EEARL, 0x1B EECR, 0x1C EEDR, 0x1D SPMCSR. avrdude and Bloom send
//   DWDR and the EEPROM registers as I/O addresses, and SPMCSR as a data
//   space address (I/O + 0x20). The scope converts SPMCSR to I/O. The boot
//   start is not a reliable boot section flag: Bloom sends 0 without a
//   boot section, but avrdude sends a value for all parts (also tinyAVR
//   and the ATmega48). The scope takes a boot section when the boot start
//   is not 0 and the flash is larger than 4 KB (no dW part with 4 KB flash
//   has a boot section). On a tinyAVR above 4 KB, the RWW enable (SPMCSR
//   0x11) is CTPB and has no effect after a page write. For SPM, the scope
//   uses the start of the last flash page: it is in the boot section for
//   all BOOTSZ fuses (avrdude sends the start of the last 4 pages, which
//   is not in the smallest boot section of an ATmega168).
//
// Software breakpoints: the probe writes BREAK (0x9598) into flash at the
// next RUN, RUN_TO or STEP, and the original word back after a clear.
// DETACH removes all of them. A read of flash gives the flash as it is (with BREAK), as the
// EDBG tools do. A run or step from a software breakpoint writes the
// original word, steps, and writes BREAK again: two page writes. Limit:
// this wears the flash (some 10000 cycles per page). On JTAG the OCD
// executes most original instructions without a flash write
// (AvrJtagDebug::stepInstead()). On debugWIRE an injection of the original
// instruction (as dw-link does) would avoid it.
//
// Break events: event() gives one frame [0x0E][0][sequence u16][0x12]
// [0x40][pc u32][cause][0x01][0x00], with cause 1 at a breakpoint. While
// the target runs, each event() checks the line one time.
//
// JTAG (avrdude: variant 2, function 1, physical 4; Bloom: function 2):
// the programming mode of avr/AvrJtagProg.h and the debug mode of
// avr/AvrJtagDebug.h. The device context: 0x00 flash page bytes u16 (2
// to 256), 0x02 flash bytes u32 (up to 256 KiB), 0x10 EEPROM bytes u16,
// 0x12 EEPROM page bytes, 0x13 OCD revision (0 to 4), 0x15 full-page bit
// stream (not 0: the flash page goes in one PROG_PAGELOAD scan), 0x18
// OCDR, 0x19 EEARH, 0x1A EEARL, 0x1B EECR, 0x1C EEDR (I/O addresses, up to
// 0x3F, for the debug mode), 0x1D SPMCSR (a data space address, for the
// flash page write of the software breakpoints).
//   ACTIVATE_PHYSICAL [reset]: drives the pins at the programming clock
//        and reads the IDCODE. -> DATA [IDCODE u32]. No Atmel IDCODE (bits
//        11..0 not 0x03F): FAILED 0x20. Without a device context (all 0:
//        AVaRICE sends it after the IDCODE), GET_ID, DETACH and
//        DEACTIVATE_PHYSICAL work. Each other command takes the device
//        context first, and gives FAILED 0x33 when it is not valid. The
//        reset flag is not used: the probe does not drive RESET, the
//        programming mode resets the core with AVR_RESET.
//   GET_ID -> DATA [IDCODE u32].
//   ENTER_PROGMODE, LEAVE_PROGMODE: the JTAG programming mode. The core
//        is in reset in the programming mode.
//   ERASE [mode 0][address u32]: chip erase. Other modes: FAILED 0x39.
//   READ_MEMORY, WRITE_MEMORY, programming mode only (else FAILED 0x32):
//        FLASH_PAGE 0xB0 (a write takes whole, aligned pages, after a chip
//        erase), EEPROM_PAGE 0xB1, FUSES 0xB2 (0 low, 1 high, 2
//        extended), LOCK_BITS 0xB3 (address 0), and read only SIGN_JTAG
//        0xB4 and OSCCAL 0xB5. These two take the 8-bit index of the
//        signature row (0 to 0xFF): SIGN_JTAG index n is row byte 2n,
//        OSCCAL index n is row byte 2n + 1. avrdude reads the signature
//        (SIGN_JTAG 0 to 2) and its sigrow, prodsig and sernum memories in
//        this way. A read can span pages.
//   DEACTIVATE_PHYSICAL: DETACH, leaves the programming mode and releases
//        the pins. The device context stays for the next
//        ACTIVATE_PHYSICAL (Bloom sends it only one time).
//   ATTACH [break]: the debug mode at the debug clock. Not in the
//        programming mode (FAILED 0x32). When the OCD answers, the core
//        stops and a break event comes, also with break 0 (Bloom waits for
//        it). Without an OCD answer (OCDEN not programmed) ATTACH is OK,
//        and each command that needs the OCD gives FAILED 0x50 (Bloom:
//        "check OCDEN fuse bit"). A second ATTACH is a STOP.
//   DETACH: the hardware and software breakpoints are removed, a stopped
//        core runs.
//   STOP, RESET, RUN, RUN_TO [word address u32], STEP, PC_READ, PC_WRITE
//        (word addresses), in the debug mode only (else FAILED 0x32): as on
//        debugWIRE. The PC can be above 16 bits (ATmega2560). Without
//        ATTACH (AVaRICE), STOP, RESET, RUN, RUN_TO and STEP start the debug
//        mode first, as ATTACH does, but without its break event. In the
//        programming mode they give FAILED 0x32.
//   HW_BREAK_SET [type][number][address u32][mode], HW_BREAK_CLEAR
//        [number]: numbers 1 to 3 (OCD reg 1 to 3). Type 1 (program): a
//        byte address (Bloom), the OCD takes word addresses. Type 2
//        (data): numbers 2 and 3 only (1: FAILED 0x81), a data space
//        address below 0x10000, mode 0 read, 1 write, 2 access (other
//        modes FAILED 0x39). DS50002630A gives only type 1 and mode 3; the
//        data values are those of AVaRICE (jtag3bp.cc) and of the JTAGICE
//        mkII CMND_SET_BREAK. A number that holds a data breakpoint, or a
//        data breakpoint on a number in use, gives FAILED 0x81. Type 3
//        (data mask, AVaRICE range BP): numbers 2 and 3 only (1: FAILED
//        0x81), the address field is the mask (1 bits compare, bits 15..0
//        used), the mode is ignored. It applies to the data breakpoint on
//        the other number. A mask on a number in use, or next to a program
//        breakpoint or a mask, gives FAILED 0x81. HW_BREAK_CLEAR of the
//        data breakpoint also clears its mask. Other types: FAILED 0x90.
//        They take effect at the next RUN or STEP. RUN_TO uses OCD reg 0.
//   READ_MEMORY, READ_MEMORY_MASKED, WRITE_MEMORY in the debug mode (Bloom),
//        with a stopped core: SRAM 0x20 (the data space; the OCDR reads as
//        0, a write skips it), EEPROM 0x22 and read only SPM 0xA0 (the
//        flash). FUSES 0xB2 (Bloom manage_ocden_fuse_bit), also with a
//        running core: the probe enters the programming mode, reads or
//        writes the fuses, leaves it and attaches again. The core is reset
//        then, and no break event comes.
//   ENTER_PROGMODE in the debug mode: the hardware and software
//        breakpoints are removed and the debug mode ends (Bloom loads a
//        program in this way and then sends DEACTIVATE_PHYSICAL,
//        ACTIVATE_PHYSICAL, ATTACH). A chip erase empties the software
//        breakpoint table: no BREAK is left in flash.
//   SW_BREAK_SET, SW_BREAK_CLEAR: in the debug mode only (else FAILED
//        0x32), as on debugWIRE. The page write uses SPM in the debug mode
//        (AvrJtagDebug::writeFlashPage()). RUN, RUN_TO and STEP with a
//        change for flash give OK at once; the AVR_EVT polls write the
//        pages in slices, then the core starts (startJob(), issue #121).
//        DETACH and DEACTIVATE_PHYSICAL write them before the response.
//        A run or step from a software breakpoint executes the original
//        instruction with
//        AvrJtagDebug::stepInstead() when it can (no page write, issue
//        #115), else as on debugWIRE. Bloom sends them only when no
//        hardware breakpoint is free. SW_BREAK_CLEAR_ALL: OK in each mode.
//   DISABLE_DEBUGWIRE: FAILED 0x90.
//   Break events: as on debugWIRE, with cause 1 when the BSR shows a
//        hit of reg 0 to 3 (program or data) or a BREAK instruction. While the core runs,
//        each event() reads OCD reg D one time.
//   A failed JTAG scan, a write that does not end, a core that does not
//        stop in time or a context that the probe could not read gives
//        FAILED 0x13.
//
// All fields come from the host. The scope checks them before it touches
// the target: a command that is too short gives FAILED 0x35, an address
// outside the device 0x38, a flash write that is not page aligned 0x37, a
// device context with values out of range at ACTIVATE_PHYSICAL 0x33 (JTAG:
// also at the command that takes it).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

#include <utils/compat/span.h>

#include "avr/AvrJtagTarget.h"
#include "avr/DebugWireTarget.h"

namespace avr {

namespace avr8 {

namespace command {
inline constexpr uint8_t set = 0x01;
inline constexpr uint8_t get = 0x02;
inline constexpr uint8_t activatePhysical = 0x10;
inline constexpr uint8_t deactivatePhysical = 0x11;
inline constexpr uint8_t getId = 0x12;
inline constexpr uint8_t attach = 0x13;
inline constexpr uint8_t detach = 0x14;
inline constexpr uint8_t enterProgmode = 0x15;
inline constexpr uint8_t leaveProgmode = 0x16;
inline constexpr uint8_t disableDebugWire = 0x17;
inline constexpr uint8_t erase = 0x20;
inline constexpr uint8_t readMemory = 0x21;
inline constexpr uint8_t readMemoryMasked = 0x22;
inline constexpr uint8_t writeMemory = 0x23;
inline constexpr uint8_t crc = 0x24;
inline constexpr uint8_t reset = 0x30;
inline constexpr uint8_t stop = 0x31;
inline constexpr uint8_t run = 0x32;
inline constexpr uint8_t runTo = 0x33;
inline constexpr uint8_t step = 0x34;
inline constexpr uint8_t pcRead = 0x35;
inline constexpr uint8_t pcWrite = 0x36;
inline constexpr uint8_t hwBreakSet = 0x40;
inline constexpr uint8_t hwBreakClear = 0x41;
inline constexpr uint8_t swBreakSet = 0x43;
inline constexpr uint8_t swBreakClear = 0x44;
inline constexpr uint8_t swBreakClearAll = 0x45;
inline constexpr uint8_t pageErase = 0x50;
} // namespace command

namespace response {
inline constexpr uint8_t ok = 0x80;
inline constexpr uint8_t pc = 0x83;
inline constexpr uint8_t data = 0x84;
inline constexpr uint8_t failed = 0xA0;
} // namespace response

// The FAILED codes
namespace failure {
inline constexpr uint8_t debugWire = 0x10;
inline constexpr uint8_t jtag = 0x13;
inline constexpr uint8_t noDevice = 0x20;
inline constexpr uint8_t notActivated = 0x31;
inline constexpr uint8_t illegalState = 0x32;
inline constexpr uint8_t invalidConfig = 0x33;
inline constexpr uint8_t memoryType = 0x34;
inline constexpr uint8_t length = 0x35;
inline constexpr uint8_t badAddress = 0x36;
inline constexpr uint8_t alignment = 0x37;
inline constexpr uint8_t range = 0x38;
inline constexpr uint8_t illegalValue = 0x39;
inline constexpr uint8_t clockSpeed = 0x3B;
// "Device not under control" (Bloom adds "check OCDEN fuse bit")
inline constexpr uint8_t notUnderControl = 0x50;
inline constexpr uint8_t resources = 0x81;
inline constexpr uint8_t notSupported = 0x90;
inline constexpr uint8_t unknown = 0x91;
} // namespace failure

namespace memory {
inline constexpr uint8_t sram = 0x20;
inline constexpr uint8_t eeprom = 0x22;
inline constexpr uint8_t spm = 0xA0;
inline constexpr uint8_t flashPage = 0xB0;
inline constexpr uint8_t eepromPage = 0xB1;
inline constexpr uint8_t fuses = 0xB2;
inline constexpr uint8_t lockBits = 0xB3;
inline constexpr uint8_t signature = 0xB4;
inline constexpr uint8_t osccal = 0xB5;
} // namespace memory

// The JTAG addresses of the physical context (context 1)
namespace physical {
inline constexpr uint8_t daisyChain = 0x01;
inline constexpr uint8_t clockProg = 0x20;
inline constexpr uint8_t clockDebug = 0x21;
inline constexpr uint16_t defaultClockKHz = 200;
} // namespace physical

// Parameters
inline constexpr uint8_t eventBreak = 0x40;
inline constexpr uint8_t handler = 0x12;
inline constexpr std::size_t eventFrameBytes = 13;
inline constexpr std::size_t deviceBytes = 0x20;
inline constexpr std::size_t maxSoftwareBreaks = 32;
// The largest flash page of an AVR with debugWIRE is 128 bytes.
inline constexpr std::size_t maxPageBytes = 128;
// The classic AVR chips with JTAG: up to 256-byte pages and 256 KiB flash
// (ATmega2560), OCD revision 4
inline constexpr std::size_t maxJtagPageBytes = 256;
inline constexpr uint32_t maxJtagFlashBytes = 0x40000;
inline constexpr uint8_t maxOcdRevision = 4;
inline constexpr uint16_t breakOpcode = 0x9598;

} // namespace avr8

// The AVR8 scope as the EDBG transport sees it
class Avr8Handler {
   public:
   // Runs one command [id][version][fields]. Writes the response and
   // returns its length. response must hold 4 bytes or more; a read gets
   // up to response.size() - 3 bytes. An empty request gets FAILED.
   virtual std::size_t process(art::span<const uint8_t> request, art::span<uint8_t> response) = 0;
   // The next event frame, 0 for none. frame must hold
   // avr8::eventFrameBytes.
   virtual std::size_t event(art::span<uint8_t> frame) = 0;
   // The host or the pins are gone: removes the software breakpoints, lets
   // the target run and releases the line. No effect without an active
   // physical.
   virtual void release() = 0;
   // release(), and the parameters and breakpoints of the host are
   // dropped.
   virtual void forget() = 0;
   // True between a successful ACTIVATE_PHYSICAL and its end
   [[nodiscard]] virtual bool active() const = 0;
   // True when the variant and the physical interface of the host are the
   // ones of this scope
   [[nodiscard]] virtual bool selected() const = 0;

   protected:
   ~Avr8Handler() = default;
};

template <typename Target>
class Avr8Scope final : public Avr8Handler {
   // debugWIRE: one stored hardware breakpoint. The JTAG debug mode keeps
   // its own.
   static_assert(Target::hasProgmode || Target::hardwareBreaks <= 1, "the scope stores one hardware breakpoint");

   public:
   explicit Avr8Scope(Target &target)
      : target_(target) {
   }

   std::size_t process(art::span<const uint8_t> request, art::span<uint8_t> response) override {
      if(response.size() < 4) {
         return 0;
      }
      request_ = request;
      response_ = response;
      if constexpr(Target::hasProgmode) {
         completeJob();
      }
      if(request.empty()) {
         return failed(avr8::failure::unknown);
      }
      return execute(request[0]);
   }

   std::size_t event(art::span<uint8_t> frame) override {
      if constexpr(Target::hasProgmode) {
         return jtagEvent(frame);
      } else {
         return debugWireEvent(frame);
      }
   }

   void release() override {
      if(!active_) {
         return;
      }
      if constexpr(Target::hasProgmode) {
         completeJob();
         static_cast<void>(freeCore());
         static_cast<void>(target_.close());
      } else {
         releaseDebugWire();
      }
      end();
   }

   void forget() override {
      release();
      config_ = {};
      physical_ = {};
      daisyChain_ = {};
      clockProg_ = defaultClock;
      clockDebug_ = defaultClock;
      device_ = {};
      options_ = {};
      breakCount_ = 0;
   }

   [[nodiscard]] bool active() const override {
      return active_;
   }

   [[nodiscard]] bool selected() const override {
      return config_[0] == Target::avr8Variant && physical_[0] == Target::avr8Physical;
   }

   // True while the AVR_EVT polls write the flash pages of a RUN, RUN_TO or
   // STEP (startJob())
   [[nodiscard]] bool busy() const {
      return job_ != Job::None;
   }

   private:
   enum class BreakState : uint8_t { Inserting, Active, Removing };
   // The command of a job (startJob())
   enum class Job : uint8_t { None, Run, Step };
   struct SoftwareBreak {
      uint32_t word;
      uint16_t original;
      BreakState state;
   };

   static constexpr std::size_t headerBytes = 2;
   static constexpr std::size_t memoryHeaderBytes = 11;
   static constexpr std::array<uint8_t, 2> defaultClock{ { static_cast<uint8_t>(avr8::physical::defaultClockKHz),
                                                           static_cast<uint8_t>(avr8::physical::defaultClockKHz >> 8) } };

   std::size_t debugWireEvent(art::span<uint8_t> frame) {
      if(frame.size() < avr8::eventFrameBytes) {
         return 0;
      }
      if(active_ && running_ && !event_) {
         // The program stops at a breakpoint: the target sends a break.
         if(target_.poll() == DwStatus::Ok) {
            stopped(true);
         }
      }
      return breakFrame(frame, target_.pc());
   }

   // The waiting break event at pc (word address), 0 for none
   std::size_t breakFrame(art::span<uint8_t> frame, uint32_t pc) {
      if(!event_) {
         return 0;
      }
      event_ = false;
      const std::array<uint8_t, avr8::eventFrameBytes> bytes{
         { 0x0E, 0x00, static_cast<uint8_t>(eventSequence_), static_cast<uint8_t>(eventSequence_ >> 8), avr8::handler,
           avr8::eventBreak, static_cast<uint8_t>(pc), static_cast<uint8_t>(pc >> 8), static_cast<uint8_t>(pc >> 16),
           static_cast<uint8_t>(pc >> 24), static_cast<uint8_t>(eventAtBreak_ ? 1 : 0), 0x01, 0x00 }
      };
      ++eventSequence_;
      std::copy(bytes.begin(), bytes.end(), frame.begin());
      return bytes.size();
   }

   template <typename T = Target>
   std::size_t jtagEvent(art::span<uint8_t> frame) {
      if(frame.size() < avr8::eventFrameBytes) {
         return 0;
      }
      if(job_ != Job::None) {
         // The event of the command comes after the job, at a later poll.
         advanceJob();
         return 0;
      }
      if(attached_ && running_ && !event_) {
         // A breakpoint stopped the core. A failed context read after the
         // stop is still a stop: the next command gives the error.
         bool stopped = false;
         static_cast<void>(target_.debug().status(stopped));
         if(stopped) {
            jtagStopped();
         }
      }
      return breakFrame(frame, target_.debug().pc());
   }

   void releaseDebugWire() {
      if(running_ && target_.poll() != DwStatus::Ok) {
         static_cast<void>(target_.stop());
      }
      if(target_.isStopped()) {
         clearAllBreaks();
         static_cast<void>(commitBreaks());
      }
      static_cast<void>(target_.detach());
   }

   [[nodiscard]] static uint32_t u32(art::span<const uint8_t> bytes, std::size_t at) {
      return bytes[at] | (uint32_t{ bytes[at + 1] } << 8) | (uint32_t{ bytes[at + 2] } << 16) | (uint32_t{ bytes[at + 3] } << 24);
   }
   [[nodiscard]] static uint16_t u16(art::span<const uint8_t> bytes, std::size_t at) {
      return static_cast<uint16_t>(bytes[at] | (bytes[at + 1] << 8));
   }

   [[nodiscard]] static uint8_t failureOf(DwStatus status) {
      switch(status) {
      case DwStatus::TargetRunning:
         return avr8::failure::illegalState;
      case DwStatus::InvalidArgument:
         return avr8::failure::badAddress;
      default:
         return avr8::failure::debugWire;
      }
   }

   [[nodiscard]] static uint8_t failureOf(JtagDebugStatus status) {
      switch(status) {
      case JtagDebugStatus::NoTap:
         return avr8::failure::noDevice;
      case JtagDebugStatus::NoOcd:
         return avr8::failure::notUnderControl;
      case JtagDebugStatus::TargetRunning:
         return avr8::failure::illegalState;
      case JtagDebugStatus::OutOfRange:
         return avr8::failure::range;
      case JtagDebugStatus::InUse:
         return avr8::failure::resources;
      default:
         return avr8::failure::jtag;
      }
   }

   std::size_t ok() {
      response_[0] = avr8::response::ok;
      response_[1] = 0;
      return 2;
   }
   std::size_t failed(uint8_t code) {
      response_[0] = avr8::response::failed;
      response_[1] = 0;
      response_[2] = code;
      return 3;
   }
   std::size_t result(DwStatus status) {
      return status == DwStatus::Ok ? ok() : failed(failureOf(status));
   }
   std::size_t result(JtagDebugStatus status) {
      return status == JtagDebugStatus::Ok ? ok() : failed(failureOf(status));
   }
   // DATA with count bytes that the caller wrote at response_[2]
   std::size_t data(std::size_t count) {
      response_[0] = avr8::response::data;
      response_[1] = 1;
      response_[2 + count] = 0;
      return count + 3;
   }

   std::size_t execute(uint8_t id) {
      namespace cmd = avr8::command;
      switch(id) {
      case cmd::set:
      case cmd::get:
         return parameter(id == cmd::set);
      case cmd::activatePhysical:
         return activate();
      case cmd::deactivatePhysical:
         return deactivate();
      case cmd::detach:
         return active_ ? detach() : ok();
      case cmd::erase:
         if(!Target::hasProgmode) {
            return failed(avr8::failure::notSupported);
         }
         break;
      case cmd::crc:
      case cmd::pageErase:
         return failed(avr8::failure::notSupported);
      default:
         break;
      }
      if(!active_) {
         return failed(avr8::failure::notActivated);
      }
      if constexpr(Target::hasProgmode) {
         return executeJtag(id);
      } else {
         return executeDebugWire(id);
      }
   }

   std::size_t executeDebugWire(uint8_t id) {
      namespace cmd = avr8::command;
      if(bare_ && id != cmd::disableDebugWire) {
         return failed(avr8::failure::invalidConfig);
      }
      switch(id) {
      case cmd::getId:
         return getId();
      case cmd::attach:
         return attach();
      case cmd::enterProgmode:
      case cmd::leaveProgmode:
         return ok();
      case cmd::disableDebugWire:
         return disable();
      case cmd::readMemory:
      case cmd::readMemoryMasked:
         return read(id == cmd::readMemoryMasked);
      case cmd::writeMemory:
         return write();
      case cmd::reset:
         return reset();
      case cmd::stop:
         return stop();
      case cmd::run:
         return run(std::nullopt);
      case cmd::runTo:
         if(request_.size() < headerBytes + 4) {
            return failed(avr8::failure::length);
         }
         return run(static_cast<uint16_t>(u32(request_, headerBytes)));
      case cmd::step:
         return step();
      case cmd::pcRead:
         return pcRead();
      case cmd::pcWrite:
         return pcWrite();
      case cmd::hwBreakSet:
      case cmd::hwBreakClear:
         return hardwareBreak(id == cmd::hwBreakSet);
      case cmd::swBreakSet:
      case cmd::swBreakClear:
         return softwareBreaks(id == cmd::swBreakSet);
      case cmd::swBreakClearAll:
         clearAllBreaks();
         return ok();
      default:
         return failed(avr8::failure::unknown);
      }
   }

   // The JTAG-only members are templates: the explicit instantiation of the
   // debugWIRE scope (board/BoardDebugWireV30x.cpp) does not instantiate
   // them.
   template <typename T = Target>
   std::size_t executeJtag(uint8_t id) {
      namespace cmd = avr8::command;
      if(bare_ && id != cmd::getId && !takeJtagDevice()) {
         return failed(avr8::failure::invalidConfig);
      }
      switch(id) {
      case cmd::getId: {
         uint32_t idcode = 0;
         return target_.readIdcode(idcode) ? idcodeData(idcode) : failed(avr8::failure::jtag);
      }
      case cmd::enterProgmode:
      case cmd::leaveProgmode:
         return progmode(id == cmd::enterProgmode);
      case cmd::erase:
         return chipErase();
      case cmd::readMemory:
      case cmd::readMemoryMasked:
         if(id == cmd::readMemory && target_.prog().programming()) {
            return readJtag();
         }
         return attached_ ? readDebug(id == cmd::readMemoryMasked) : failed(avr8::failure::illegalState);
      case cmd::writeMemory:
         if(target_.prog().programming()) {
            return writeJtag();
         }
         return attached_ ? writeDebug() : failed(avr8::failure::illegalState);
      case cmd::attach:
         return attachJtag();
      case cmd::disableDebugWire:
         return failed(avr8::failure::notSupported);
      case cmd::swBreakClearAll:
         clearAllBreaks();
         return ok();
      default:
         break;
      }
      if(!attached_) {
         switch(id) {
         case cmd::reset:
         case cmd::stop:
         case cmd::run:
         case cmd::runTo:
         case cmd::step:
            if(const auto failure = implicitAttach()) {
               return failed(*failure);
            }
            break;
         case cmd::pcRead:
         case cmd::pcWrite:
         case cmd::hwBreakSet:
         case cmd::hwBreakClear:
         case cmd::swBreakSet:
         case cmd::swBreakClear:
            return failed(avr8::failure::illegalState);
         default:
            return failed(avr8::failure::unknown);
         }
      }
      AvrJtagDebug &debug = target_.debug();
      switch(id) {
      case cmd::reset:
         return afterCommand(debug.reset());
      case cmd::stop:
         return stopJtag();
      case cmd::run:
         return runJtag(std::nullopt);
      case cmd::runTo:
         if(request_.size() < headerBytes + 4) {
            return failed(avr8::failure::length);
         }
         return runJtag(u32(request_, headerBytes));
      case cmd::step:
         return stepCommandJtag();
      case cmd::pcRead:
         return debug.hasContext() ? pcData(debug.pc()) : failed(failureOf(contextFailure()));
      case cmd::pcWrite:
         return pcWriteJtag();
      case cmd::hwBreakSet:
      case cmd::hwBreakClear:
         return hardwareBreakJtag(id == cmd::hwBreakSet);
      case cmd::swBreakSet:
      case cmd::swBreakClear:
         return softwareBreaks(id == cmd::swBreakSet);
      default:
         return failed(avr8::failure::unknown);
      }
   }

   // STEP: the breakpoint changes go into flash first. At an active
   // software breakpoint the step executes the original word.
   template <typename T = Target>
   std::size_t stepCommandJtag() {
      if(pendingBreak() != nullptr) {
         return startJob(Job::Step, std::nullopt);
      }
      return afterCommand(stepJtag());
   }

   template <typename T = Target>
   JtagDebugStatus stepJtag() {
      JtagDebugStatus status = commitBreaks();
      if(status != JtagDebugStatus::Ok) {
         return status;
      }
      return findBreak(corePc(), BreakState::Active) != nullptr ? stepOverBreak() : target_.debug().step();
   }

   // The status of a stopped core without a valid context, or of a
   // running core
   template <typename T = Target>
   [[nodiscard]] JtagDebugStatus contextFailure() {
      return target_.debug().isStopped() ? JtagDebugStatus::NoContext : JtagDebugStatus::TargetRunning;
   }

   // The core stopped while it ran: one break event, with the cause from
   // the BSR.
   template <typename T = Target>
   void jtagStopped() {
      namespace bsr = jtagOcd::bsr;
      constexpr uint16_t breakpointHit = bsr::breakInstruction | bsr::break0 | bsr::break1 | bsr::break2 | bsr::break3;
      running_ = false;
      event_ = true;
      eventAtBreak_ = (target_.debug().breakStatus() & breakpointHit) != 0;
   }

   // A RESET, STEP or STOP that stopped the core: one break event with
   // cause 0
   std::size_t afterCommand(JtagDebugStatus status) {
      if(status != JtagDebugStatus::Ok) {
         return failed(failureOf(status));
      }
      running_ = false;
      event_ = true;
      eventAtBreak_ = false;
      return ok();
   }

   // The clock of the current mode
   [[nodiscard]] uint16_t modeClock() const {
      return u16(attached_ ? clockDebug_ : clockProg_, 0);
   }

   template <typename T = Target>
   std::size_t attachJtag() {
      if(request_.size() < headerBytes + 1) {
         return failed(avr8::failure::length);
      }
      if(target_.prog().programming()) {
         return failed(avr8::failure::illegalState);
      }
      if(attached_) {
         return stopJtag();
      }
      if(const auto failure = startDebugJtag()) {
         return failed(*failure);
      }
      if(target_.debug().isStopped()) {
         // Bloom waits for this event, also after ATTACH with break 0.
         event_ = true;
         eventAtBreak_ = false;
      }
      return ok();
   }

   // The debug mode at the debug clock, without a break event. A value:
   // the FAILED code.
   template <typename T = Target>
   std::optional<uint8_t> startDebugJtag() {
      clearHardwareBreaks();
      attached_ = true;
      running_ = false;
      event_ = false;
      if(!target_.setSpeed(modeClock())) {
         attached_ = false;
         return avr8::failure::clockSpeed;
      }
      if(const JtagDebugStatus status = target_.debug().attach(); status != JtagDebugStatus::Ok) {
         attached_ = false;
         static_cast<void>(target_.setSpeed(modeClock()));
         return failureOf(status);
      }
      return std::nullopt;
   }

   // AVaRICE sends no ATTACH (issue #108): the first RESET, STOP, RUN,
   // RUN_TO or STEP starts the debug mode. The command gives the break
   // event. A value: the FAILED code.
   template <typename T = Target>
   std::optional<uint8_t> implicitAttach() {
      if(target_.prog().programming()) {
         return avr8::failure::illegalState;
      }
      return startDebugJtag();
   }

   template <typename T = Target>
   void clearHardwareBreaks() {
      for(uint8_t number = 1; number <= Target::hardwareBreaks; ++number) {
         static_cast<void>(target_.debug().clearBreakpoint(number));
      }
   }

   // Ends the debug mode: no hardware breakpoints, and a core that the OCD
   // holds runs again. Ok without an OCD answer: the core runs.
   template <typename T = Target>
   JtagDebugStatus freeCore() {
      if(!attached_) {
         return JtagDebugStatus::Ok;
      }
      AvrJtagDebug &debug = target_.debug();
      clearHardwareBreaks();
      // The program runs without the debugger: no BREAK may stay in flash.
      clearAllBreaks();
      attached_ = false;
      running_ = false;
      event_ = false;
      // The clock values were checked at SET.
      static_cast<void>(target_.setSpeed(modeClock()));
      if(!debug.ocdAnswers()) {
         return JtagDebugStatus::Ok;
      }
      // A running core may stop later at an old BCR enable: stop it, and
      // let the run write the BCR without breakpoints. halt() of a stopped
      // core sends no scan.
      JtagDebugStatus status = debug.halt();
      if(status == JtagDebugStatus::Ok) {
         status = commitBreaks();
      }
      if(status == JtagDebugStatus::Ok) {
         status = debug.run();
      }
      return status;
   }

   template <typename T = Target>
   std::size_t stopJtag() {
      AvrJtagDebug &debug = target_.debug();
      if(running_) {
         // The core can be at a breakpoint already: the event gets its
         // cause.
         bool stopped = false;
         if(debug.status(stopped) == JtagDebugStatus::Ok && stopped) {
            jtagStopped();
            return ok();
         }
      }
      return afterCommand(debug.halt());
   }

   template <typename T = Target>
   std::size_t runJtag(std::optional<uint32_t> word) {
      if(pendingBreak() != nullptr) {
         return startJob(Job::Run, word);
      }
      JtagDebugStatus status = stepOverBreak();
      if(status == JtagDebugStatus::Ok) {
         status = target_.debug().run(word);
      }
      if(status != JtagDebugStatus::Ok) {
         return failed(failureOf(status));
      }
      running_ = true;
      event_ = false;
      return ok();
   }

   // A RUN, RUN_TO or STEP with breakpoint changes for flash: the response
   // comes at once, and the AVR_EVT polls write the pages in slices. A
   // page write takes some 1 s at 200 kHz (256 bytes); AVaRICE waits
   // 500 ms for a response and 200 ms for an event (issue #121). The core
   // starts after the last page. Each other command, and release(), first
   // completes the job. A failed job gives a break event at the PC: the
   // core did not run.
   template <typename T = Target>
   std::size_t startJob(Job kind, std::optional<uint32_t> word) {
      if(!target_.debug().hasContext()) {
         return failed(failureOf(contextFailure()));
      }
      job_ = kind;
      jobTo_ = word;
      jobRead_ = 0;
      jobLoaded_ = false;
      running_ = true;
      event_ = false;
      return ok();
   }

   // One slice of the job
   template <typename T = Target>
   void advanceJob() {
      bool done = false;
      JtagDebugStatus status = commitSlice(done);
      if(status == JtagDebugStatus::Ok && !done) {
         return;
      }
      const Job kind = job_;
      job_ = Job::None;
      jobRead_ = 0;
      jobLoaded_ = false;
      if(status == JtagDebugStatus::Ok) {
         if(kind == Job::Run) {
            status = stepOverBreak();
            if(status == JtagDebugStatus::Ok) {
               status = target_.debug().run(jobTo_);
            }
            if(status == JtagDebugStatus::Ok) {
               return;
            }
         } else {
            status = findBreak(corePc(), BreakState::Active) != nullptr ? stepOverBreak() : target_.debug().step();
         }
      }
      // The step is done, or the core did not start: a stop at the PC.
      running_ = false;
      event_ = true;
      eventAtBreak_ = false;
   }

   template <typename T = Target>
   void completeJob() {
      while(job_ != Job::None) {
         advanceJob();
      }
   }

   // The number of words of one slice: some sliceMs at the debug clock
   [[nodiscard]] uint16_t sliceWords(uint32_t wordCycles) const {
      const uint32_t words = sliceMs * modeClock() / wordCycles;
      return static_cast<uint16_t>(std::clamp<uint32_t>(words, 1, 0xFFFF));
   }

   // The next part of the commit: a part of the page read, the page erase,
   // or a part of the page buffer loads, then the page write. done: all
   // changes are in flash.
   template <typename T = Target>
   JtagDebugStatus commitSlice(bool &done) {
      AvrJtagDebug &debug = target_.debug();
      done = false;
      if(debug.writingFlashPage()) {
         // The page write and the read back are one slice of their own.
         if(jobLoaded_) {
            jobLoaded_ = false;
            const JtagDebugStatus status = debug.endFlashPage();
            if(status == JtagDebugStatus::Ok) {
               markPage(jobPage_);
            }
            return status;
         }
         return debug.loadFlashPage(sliceWords(spmLoadCycles), jobLoaded_);
      }
      if(jobRead_ == 0) {
         const SoftwareBreak *pending = pendingBreak();
         if(pending == nullptr) {
            done = true;
            return JtagDebugStatus::Ok;
         }
         jobPage_ = pageOf(pending->word);
      }
      const uint16_t pageBytes = flashPageBytes();
      if(jobRead_ < pageBytes) {
         const auto bytes = static_cast<uint16_t>(std::min<uint32_t>(pageBytes - jobRead_, 2u * sliceWords(flashReadCycles)));
         if(JtagDebugStatus status = debug.readFlash(jobPage_ + jobRead_, page_.data() + jobRead_, bytes);
            status != JtagDebugStatus::Ok) {
            return status;
         }
         jobRead_ = static_cast<uint16_t>(jobRead_ + bytes);
         return JtagDebugStatus::Ok;
      }
      jobRead_ = 0;
      preparePage(jobPage_);
      return debug.beginFlashPage(jobPage_, page_.data());
   }

   std::size_t pcData(uint32_t pc) {
      response_[0] = avr8::response::pc;
      response_[1] = 0;
      for(std::size_t i = 0; i < 4; ++i) {
         response_[2 + i] = static_cast<uint8_t>(pc >> (8 * i));
      }
      return 6;
   }

   template <typename T = Target>
   std::size_t pcWriteJtag() {
      if(request_.size() < headerBytes + 4) {
         return failed(avr8::failure::length);
      }
      AvrJtagDebug &debug = target_.debug();
      if(!debug.hasContext()) {
         return failed(failureOf(contextFailure()));
      }
      const uint32_t word = u32(request_, headerBytes);
      if(word >= target_.device().prog.flashBytes / 2u) {
         return failed(avr8::failure::range);
      }
      debug.setPc(word);
      return ok();
   }

   // HW_BREAK_SET [type][number][byte address u32][mode], HW_BREAK_CLEAR
   // [number]
   template <typename T = Target>
   std::size_t hardwareBreakJtag(bool set) {
      constexpr uint8_t programType = 1;
      constexpr uint8_t dataType = 2;
      constexpr uint8_t maskType = 3;
      const std::size_t bytes = set ? headerBytes + 7 : headerBytes + 1;
      if(request_.size() < bytes) {
         return failed(avr8::failure::length);
      }
      const uint8_t number = request_[set ? 3 : 2];
      if(number == 0 || number > Target::hardwareBreaks) {
         return failed(avr8::failure::resources);
      }
      AvrJtagDebug &debug = target_.debug();
      if(!set) {
         return result(debug.clearBreakpoint(number));
      }
      const uint32_t address = u32(request_, 4);
      if(request_[2] == dataType) {
         return dataBreakJtag(number, address, request_[8]);
      }
      if(request_[2] == maskType) {
         // Reg 1 has no data mode. The data space is below 0x10000: the
         // mask bits 31..16 do not matter.
         return number == 1 ? failed(avr8::failure::resources) :
                              result(debug.setDataMask(number, static_cast<uint16_t>(address)));
      }
      if(request_[2] != programType) {
         return failed(avr8::failure::notSupported);
      }
      if(address % 2 != 0) {
         return failed(avr8::failure::range);
      }
      return result(debug.setBreakpoint(number, address / 2u));
   }

   // A data breakpoint: a data space address and the EDBG mode (read 0,
   // write 1, access 2, as AVaRICE sends them, see the comment at the top)
   template <typename T = Target>
   std::size_t dataBreakJtag(uint8_t number, uint32_t address, uint8_t mode) {
      static constexpr std::array<JtagDataMode, 3> ocdMode{ JtagDataMode::Read, JtagDataMode::Write, JtagDataMode::Access };
      if(number == 1) {
         // Reg 1 has no data mode.
         return failed(avr8::failure::resources);
      }
      if(mode >= ocdMode.size()) {
         return failed(avr8::failure::illegalValue);
      }
      if(address > 0xFFFFu) {
         return failed(avr8::failure::range);
      }
      return result(target_.debug().setDataBreakpoint(number, static_cast<uint16_t>(address), ocdMode[mode]));
   }

   // [type][address u32][length u32] (masked: [mask]), in the debug mode
   template <typename T = Target>
   std::size_t readDebug(bool masked) {
      if(request_.size() < memoryHeaderBytes) {
         return failed(avr8::failure::length);
      }
      const uint8_t type = request_[2];
      const uint32_t address = u32(request_, 3);
      const uint32_t length = u32(request_, 7);
      if(length == 0 || length > response_.size() - 3) {
         return failed(avr8::failure::length);
      }
      const uint8_t *mask = nullptr;
      if(masked) {
         if(type != avr8::memory::sram) {
            return failed(avr8::failure::memoryType);
         }
         if(request_.size() < memoryHeaderBytes + (length + 7u) / 8u) {
            return failed(avr8::failure::length);
         }
         mask = request_.data() + memoryHeaderBytes;
      }
      AvrJtagDebug &debug = target_.debug();
      uint8_t *out = response_.data() + 2;
      JtagDebugStatus status = JtagDebugStatus::Ok;
      switch(type) {
      case avr8::memory::sram:
         if(!inRange(address, length, 0x10000u)) {
            return failed(avr8::failure::range);
         }
         status = accessDebugData(static_cast<uint16_t>(address), out, static_cast<uint16_t>(length), mask, false);
         break;
      case avr8::memory::spm:
         status = debug.readFlash(address, out, length);
         break;
      case avr8::memory::eeprom:
         if(!inRange(address, length, target_.device().eepromBytes)) {
            return failed(avr8::failure::range);
         }
         status = debug.readEeprom(static_cast<uint16_t>(address), out, static_cast<uint16_t>(length));
         break;
      case avr8::memory::fuses: {
         const auto failure = fusesInDebug(address, response_.subspan(2, length), false);
         return failure ? failed(*failure) : data(length);
      }
      default:
         return failed(avr8::failure::memoryType);
      }
      return status == JtagDebugStatus::Ok ? data(length) : failed(failureOf(status));
   }

   // [type][address u32][length u32][async][data], in the debug mode
   template <typename T = Target>
   std::size_t writeDebug() {
      constexpr std::size_t writeHeaderBytes = memoryHeaderBytes + 1;
      if(request_.size() < writeHeaderBytes) {
         return failed(avr8::failure::length);
      }
      const uint8_t type = request_[2];
      const uint32_t address = u32(request_, 3);
      const uint32_t length = u32(request_, 7);
      if(length == 0 || request_.size() - writeHeaderBytes < length) {
         return failed(avr8::failure::length);
      }
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): a write does not change the buffer.
      uint8_t *in = const_cast<uint8_t *>(request_.data() + writeHeaderBytes);
      switch(type) {
      case avr8::memory::sram:
         if(!inRange(address, length, 0x10000u)) {
            return failed(avr8::failure::range);
         }
         return result(accessDebugData(static_cast<uint16_t>(address), in, static_cast<uint16_t>(length), nullptr, true));
      case avr8::memory::eeprom:
         if(!inRange(address, length, target_.device().eepromBytes)) {
            return failed(avr8::failure::range);
         }
         return result(target_.debug().writeEeprom(static_cast<uint16_t>(address), in, static_cast<uint16_t>(length)));
      case avr8::memory::fuses: {
         const auto failure = fusesInDebug(address, art::span<uint8_t>(in, length), true);
         return failure ? failed(*failure) : ok();
      }
      default:
         // No host tool writes the flash with SPM in the debug mode (Bloom
         // loads a program in the programming mode). The SPM page write is
         // for the software breakpoints only.
         return failed(avr8::failure::memoryType);
      }
   }

   // FUSES in the debug mode (Bloom manage_ocden_fuse_bit): the programming
   // mode for the access, then the debug mode again. A value: the FAILED
   // code.
   template <typename T = Target>
   std::optional<uint8_t> fusesInDebug(uint32_t address, art::span<uint8_t> bytes, bool write) {
      if(!inRange(address, static_cast<uint32_t>(bytes.size()), 3)) {
         return avr8::failure::range;
      }
      AvrJtagProg &prog = target_.prog();
      bool done = prog.enter();
      for(std::size_t i = 0; i < bytes.size() && done; ++i) {
         const auto fuse = static_cast<Fuse>(address + i);
         done = write ? prog.writeFuse(fuse, bytes[i]) : prog.readFuse(fuse, bytes[i]);
      }
      done = prog.leave() && done;
      // The programming mode reset the core: a new attach (no break event,
      // the host does not wait for one).
      running_ = false;
      event_ = false;
      if(const JtagDebugStatus status = target_.debug().attach(); status != JtagDebugStatus::Ok) {
         return failureOf(status);
      }
      return done ? std::nullopt : std::optional<uint8_t>{ avr8::failure::jtag };
   }

   // The JTAG table of the physical context (context 1) that holds
   // address, and the address of its first byte. Address 0 is the physical
   // interface.
   art::span<uint8_t> physicalTable(uint8_t address, uint8_t &first) {
      namespace phys = avr8::physical;
      first = address;
      switch(address) {
      case 0:
         return physical_;
      case phys::clockProg:
         return clockProg_;
      case phys::clockDebug:
         return clockDebug_;
      default:
         break;
      }
      if(address >= phys::daisyChain && address < phys::daisyChain + daisyChain_.size()) {
         first = phys::daisyChain;
         return daisyChain_;
      }
      return {};
   }

   // A SET of a JTAG parameter of the physical context: FAILED when the
   // value is not valid. table and first from physicalTable().
   std::optional<uint8_t>
   checkPhysical(art::span<const uint8_t> table, uint8_t first, uint8_t address, art::span<const uint8_t> value) {
      if(first == avr8::physical::daisyChain) {
         // One device in the chain only
         if(std::any_of(value.begin(), value.end(), [](uint8_t byte) { return byte != 0; })) {
            return avr8::failure::illegalValue;
         }
         return std::nullopt;
      }
      if(first != avr8::physical::clockProg && first != avr8::physical::clockDebug) {
         return std::nullopt;
      }
      std::array<uint8_t, 2> clock{ { table[0], table[1] } };
      std::copy(value.begin(), value.end(), clock.begin() + (address - first));
      const uint16_t kHz = u16(clock, 0);
      if(kHz == 0) {
         return avr8::failure::clockSpeed;
      }
      if constexpr(Target::hasProgmode) {
         // Only the clock of the current mode goes to the TAP.
         const uint8_t current = attached_ ? avr8::physical::clockDebug : avr8::physical::clockProg;
         if(active_ && first == current && !target_.setSpeed(kHz)) {
            return avr8::failure::clockSpeed;
         }
      }
      return std::nullopt;
   }

   // [cmd][version][context][address][bytes][data]
   std::size_t parameter(bool set) {
      constexpr std::size_t accessBytes = 5;
      if(request_.size() < accessBytes) {
         return failed(avr8::failure::notSupported);
      }
      const uint8_t context = request_[2];
      const uint8_t address = request_[3];
      const uint8_t bytes = request_[4];
      art::span<uint8_t> table;
      // The address of table[0]
      uint8_t first = 0;
      switch(context) {
      case 0:
         table = config_;
         break;
      case 1:
         table = physicalTable(address, first);
         break;
      case 2:
         table = device_;
         break;
      case 3:
         table = options_;
         break;
      default:
         break;
      }
      const std::size_t offset = address - first;
      if(bytes == 0 || offset + bytes > table.size() || (set && request_.size() - accessBytes < bytes)) {
         return failed(avr8::failure::notSupported);
      }
      if(set) {
         const art::span<const uint8_t> value = request_.subspan(accessBytes, bytes);
         if(context == 1) {
            if(const auto failure = checkPhysical(table, first, address, value)) {
               return failed(*failure);
            }
         }
         std::copy(value.begin(), value.end(), table.begin() + offset);
         target_.setRunTimers(options_[0] != 0);
         return ok();
      }
      std::copy_n(table.begin() + offset, bytes, response_.begin() + 2);
      return data(bytes);
   }

   [[nodiscard]] bool noDevice() const {
      return std::all_of(device_.begin(), device_.end(), [](uint8_t value) { return value == 0; });
   }

   // The device context in DwDevice form. No value: a value out of range.
   [[nodiscard]] std::optional<DwDevice> device() const {
      const art::span<const uint8_t> d(device_.data(), device_.size());
      const uint16_t page = u16(d, 0x00);
      const uint32_t flash = u32(d, 0x02);
      const uint8_t spmcr = d[0x1D];
      const bool pageOk = page >= 2 && page <= avr8::maxPageBytes && (page & (page - 1u)) == 0;
      if(!pageOk || flash < page || flash > 0x10000u || flash % page != 0 || spmcr < avrIoData || spmcr >= avrIoData + 0x40u) {
         return std::nullopt;
      }
      for(std::size_t at = 0x18; at <= 0x1C; ++at) {
         if(d[at] > avrIoSreg) {
            return std::nullopt;
         }
      }
      return DwDevice{ .flashPageBytes = page,
                       .flashBytes = flash,
                       // avrdude sends a boot start for all parts. The 4 KB parts
                       // (ATmega48) have no boot section.
                       .bootStartWords =
                       u32(d, 0x0A) != 0 && flash > 0x1000u ? static_cast<uint16_t>((flash - page) / 2u) : uint16_t{ 0 },
                       .eepromBytes = u16(d, 0x10),
                       .dwdr = d[0x18],
                       .spmcsr = static_cast<uint8_t>(spmcr - avrIoData),
                       .eearh = d[0x19],
                       .eearl = d[0x1A],
                       .eecr = d[0x1B],
                       .eedr = d[0x1C] };
   }

   // The JTAG device context. No value: a value out of range.
   [[nodiscard]] std::optional<AvrJtagDevice> jtagDevice() const {
      const art::span<const uint8_t> d(device_.data(), device_.size());
      const uint16_t page = u16(d, 0x00);
      const uint32_t flash = u32(d, 0x02);
      const uint16_t eeprom = u16(d, 0x10);
      const uint8_t eepromPage = d[0x12];
      const auto powerOf2 = [](uint32_t value) { return value != 0 && (value & (value - 1u)) == 0; };
      if(!powerOf2(page) || page < 2 || page > avr8::maxJtagPageBytes || flash < page || flash > avr8::maxJtagFlashBytes ||
         flash % page != 0 || !powerOf2(eepromPage) || eeprom < eepromPage || eeprom % eepromPage != 0 || d[0x13] > avr8::maxOcdRevision ||
         std::any_of(d.begin() + 0x18, d.begin() + 0x1D, [](uint8_t io) { return io > avrIoSreg; })) {
         return std::nullopt;
      }
      return AvrJtagDevice{
         .prog = { .flashPageBytes = page, .flashBytes = flash, .eepromPageBytes = eepromPage, .fullPageBitstream = d[0x15] != 0 },
         .eepromBytes = eeprom,
         .ocdRevision = d[0x13],
         .ocdr = d[0x18],
         .eearh = d[0x19],
         .eearl = d[0x1A],
         .eecr = d[0x1B],
         .eedr = d[0x1C],
         // Checked at the flash write: avrdude needs no SPMCSR.
         .spmcsr = d[0x1D]
      };
   }

   std::size_t activate() {
      if(active_) {
         return ok();
      }
      if constexpr(Target::hasProgmode) {
         return activateJtag();
      } else {
         return activateDebugWire();
      }
   }

   template <typename T = Target>
   std::size_t activateJtag() {
      const auto device = jtagDevice();
      // AVaRICE sends the device context after ACTIVATE_PHYSICAL (issue
      // #102): without one, the next command takes it (takeJtagDevice()).
      const bool bare = noDevice();
      if(!selected() || (!device && !bare)) {
         return failed(avr8::failure::invalidConfig);
      }
      if(device) {
         target_.setDevice(*device);
      }
      // The debug clock comes with the debug mode.
      if(!target_.open(u16(clockProg_, 0))) {
         return failed(avr8::failure::jtag);
      }
      uint32_t idcode = 0;
      if(!target_.readIdcode(idcode) || !tap::isAtmelIdcode(idcode)) {
         static_cast<void>(target_.close());
         return failed(avr8::failure::noDevice);
      }
      active_ = true;
      bare_ = bare;
      running_ = false;
      event_ = false;
      return idcodeData(idcode);
   }

   // A JTAG session without a device context: the device context that
   // the host sent after ACTIVATE_PHYSICAL. False: no valid one.
   template <typename T = Target>
   bool takeJtagDevice() {
      const auto device = jtagDevice();
      if(!device) {
         return false;
      }
      target_.setDevice(*device);
      bare_ = false;
      return true;
   }

   std::size_t activateDebugWire() {
      const auto dw = device();
      // Without a device, the breakpoint table cannot go to flash.
      const bool bare = noDevice() && breakCount_ == 0;
      if(config_[0] != Target::avr8Variant || physical_[0] != Target::avr8Physical || (!dw && !bare)) {
         return failed(avr8::failure::invalidConfig);
      }
      if(dw) {
         target_.setDevice(*dw);
      }
      target_.setRunTimers(options_[0] != 0);
      const DwStatus status = target_.attach(!bare);
      if(status != DwStatus::Ok) {
         static_cast<void>(target_.detach());
         return failed(failureOf(status));
      }
      active_ = true;
      bare_ = bare;
      running_ = false;
      event_ = false;
      hardwareBreak_.reset();
      return ok();
   }

   std::size_t deactivate() {
      if(!active_) {
         return ok();
      }
      if constexpr(Target::hasProgmode) {
         const JtagDebugStatus freed = freeCore();
         const bool closed = target_.close();
         end();
         if(freed != JtagDebugStatus::Ok) {
            return failed(failureOf(freed));
         }
         return closed ? ok() : failed(avr8::failure::jtag);
      } else {
         DwStatus status = DwStatus::Ok;
         if(target_.isStopped()) {
            clearAllBreaks();
            status = commitBreaks();
            if(status == DwStatus::Ok) {
               status = target_.reset();
            }
         }
         const DwStatus detached = target_.detach();
         end();
         return result(status != DwStatus::Ok ? status : detached);
      }
   }

   void end() {
      active_ = false;
      bare_ = false;
      attached_ = false;
      // The next session can be another host on another chip (issue #69).
      // JTAG keeps it: Bloom loads a program with DEACTIVATE_PHYSICAL and
      // ACTIVATE_PHYSICAL and sends no new device context (forget() clears
      // it).
      if constexpr(!Target::hasProgmode) {
         device_ = {};
      }
      running_ = false;
      event_ = false;
      hardwareBreak_.reset();
   }

   std::size_t getId() {
      uint16_t id = 0;
      if(DwStatus status = target_.readSignature(id); status != DwStatus::Ok) {
         return failed(failureOf(status));
      }
      response_[2] = static_cast<uint8_t>(id);
      response_[3] = static_cast<uint8_t>(id >> 8);
      response_[4] = 0x1E;
      response_[5] = 0x00;
      return data(4);
   }

   std::size_t attach() {
      if(request_.size() < headerBytes + 1) {
         return failed(avr8::failure::length);
      }
      if(request_[2] == 0) {
         return target_.isStopped() ? run(std::nullopt) : ok();
      }
      return stop();
   }

   std::size_t detach() {
      if constexpr(Target::hasProgmode) {
         return result(freeCore());
      } else {
         return detachDebugWire();
      }
   }

   std::size_t detachDebugWire() {
      if(!target_.isStopped()) {
         return ok();
      }
      // The program runs without the debugger: no BREAK may stay in flash.
      clearAllBreaks();
      DwStatus status = commitBreaks();
      if(status == DwStatus::Ok) {
         status = target_.go(std::nullopt);
      }
      running_ = false;
      return result(status);
   }

   std::size_t disable() {
      DwStatus status = DwStatus::Ok;
      if(!target_.isStopped()) {
         status = stopTarget();
      }
      if(status == DwStatus::Ok) {
         clearAllBreaks();
         status = commitBreaks();
      }
      if(status == DwStatus::Ok) {
         status = target_.disable();
      } else {
         static_cast<void>(target_.detach());
      }
      end();
      return result(status);
   }

   // Ok when the target is stopped now: by a break that came (event with
   // cause 1), or by a probe break.
   DwStatus stopTarget() {
      if(running_ && target_.poll() == DwStatus::Ok) {
         stopped(true);
         return DwStatus::Ok;
      }
      const DwStatus status = target_.stop();
      if(status == DwStatus::Ok) {
         stopped(false);
      }
      return status;
   }

   // The target stopped: one break event.
   void stopped(bool maybeAtBreak) {
      running_ = false;
      event_ = true;
      const uint16_t pc = target_.pc();
      eventAtBreak_ = maybeAtBreak && (pc == runBreak_ || findBreak(pc, BreakState::Active) != nullptr);
   }

   std::size_t stop() {
      if(target_.isStopped()) {
         // ATTACH and STOP of a stopped target: the host waits for an event.
         event_ = true;
         eventAtBreak_ = false;
         return ok();
      }
      return result(stopTarget());
   }

   std::size_t reset() {
      DwStatus status = target_.isStopped() ? DwStatus::Ok : stopTarget();
      if(status == DwStatus::Ok) {
         status = target_.reset();
      }
      if(status != DwStatus::Ok) {
         return failed(failureOf(status));
      }
      stopped(false);
      return ok();
   }

   std::size_t run(std::optional<uint16_t> to) {
      if(!target_.isStopped()) {
         return failed(avr8::failure::illegalState);
      }
      DwStatus status = commitBreaks();
      if(status == DwStatus::Ok) {
         status = stepOverBreak();
      }
      const std::optional<uint16_t> breakWord = to ? to : hardwareBreak_;
      if(status == DwStatus::Ok) {
         runBreak_ = breakWord.value_or(0xFFFF);
         event_ = false;
         status = target_.go(breakWord);
      }
      if(status != DwStatus::Ok) {
         return failed(failureOf(status));
      }
      running_ = true;
      return ok();
   }

   std::size_t step() {
      if(!target_.isStopped()) {
         return failed(avr8::failure::illegalState);
      }
      DwStatus status = commitBreaks();
      if(status == DwStatus::Ok) {
         status = findBreak(target_.pc(), BreakState::Active) != nullptr ? stepOverBreak() : target_.step();
      }
      if(status != DwStatus::Ok) {
         return failed(failureOf(status));
      }
      stopped(false);
      return ok();
   }

   std::size_t pcRead() {
      if(!target_.isStopped()) {
         return failed(avr8::failure::illegalState);
      }
      return pcData(target_.pc());
   }

   std::size_t pcWrite() {
      if(request_.size() < headerBytes + 4) {
         return failed(avr8::failure::length);
      }
      if(!target_.isStopped()) {
         return failed(avr8::failure::illegalState);
      }
      const uint32_t word = u32(request_, headerBytes);
      if(word >= target_.device().flashBytes / 2u) {
         return failed(avr8::failure::range);
      }
      target_.setPc(static_cast<uint16_t>(word));
      return ok();
   }

   // HW_BREAK_SET [type][number][address u32][mode], HW_BREAK_CLEAR
   // [number]
   std::size_t hardwareBreak(bool set) {
      const std::size_t bytes = set ? headerBytes + 7 : headerBytes + 1;
      if(request_.size() < bytes) {
         return failed(avr8::failure::length);
      }
      const uint8_t number = request_[set ? 3 : 2];
      if(number == 0 || number > Target::hardwareBreaks) {
         return failed(avr8::failure::resources);
      }
      if(!set) {
         hardwareBreak_.reset();
         return ok();
      }
      const uint32_t address = u32(request_, 4);
      if(address >= target_.device().flashBytes || address % 2 != 0) {
         return failed(avr8::failure::range);
      }
      hardwareBreak_ = static_cast<uint16_t>(address / 2u);
      return ok();
   }

   // [address u32]... (byte addresses)
   std::size_t softwareBreaks(bool set) {
      if(request_.size() < headerBytes || (request_.size() - headerBytes) % 4 != 0) {
         return failed(avr8::failure::length);
      }
      for(std::size_t at = headerBytes; at < request_.size(); at += 4) {
         const uint32_t address = u32(request_, at);
         if(address >= flashBytes() || address % 2 != 0) {
            return failed(avr8::failure::range);
         }
      }
      for(std::size_t at = headerBytes; at < request_.size(); at += 4) {
         const uint32_t word = u32(request_, at) / 2u;
         if(!set) {
            clearBreak(word);
         } else if(!setBreak(word)) {
            return failed(avr8::failure::resources);
         }
      }
      return ok();
   }

   [[nodiscard]] SoftwareBreak *findBreak(uint32_t word, std::optional<BreakState> state = std::nullopt) {
      for(std::size_t i = 0; i < breakCount_; ++i) {
         if(breaks_[i].word == word && (!state || breaks_[i].state == *state)) {
            return &breaks_[i];
         }
      }
      return nullptr;
   }

   void removeBreak(const SoftwareBreak *entry) {
      const auto index = static_cast<std::size_t>(entry - breaks_.data());
      breaks_[index] = breaks_[--breakCount_];
   }

   bool setBreak(uint32_t word) {
      if(SoftwareBreak *entry = findBreak(word); entry != nullptr) {
         if(entry->state == BreakState::Removing) {
            entry->state = BreakState::Active;
         }
         return true;
      }
      if(breakCount_ == breaks_.size()) {
         return false;
      }
      breaks_[breakCount_++] = { word, 0, BreakState::Inserting };
      return true;
   }

   void clearBreak(uint32_t word) {
      SoftwareBreak *entry = findBreak(word);
      if(entry == nullptr) {
         return;
      }
      if(entry->state == BreakState::Inserting) {
         removeBreak(entry);
      } else {
         entry->state = BreakState::Removing;
      }
   }

   void clearAllBreaks() {
      for(std::size_t i = breakCount_; i > 0; --i) {
         clearBreak(breaks_[i - 1].word);
      }
   }

   [[nodiscard]] uint32_t pageOf(uint32_t word) const {
      return (word * 2u) & ~(uint32_t{ flashPageBytes() } - 1u);
   }

   // The flash and step operations of the software breakpoints, on the
   // target of the scope. The JTAG debug mode needs a stopped core with a
   // context.
   using Status = std::conditional_t<Target::hasProgmode, JtagDebugStatus, DwStatus>;

   [[nodiscard]] uint16_t flashPageBytes() const {
      if constexpr(Target::hasProgmode) {
         return target_.device().prog.flashPageBytes;
      } else {
         return target_.device().flashPageBytes;
      }
   }

   [[nodiscard]] uint32_t flashBytes() const {
      if constexpr(Target::hasProgmode) {
         return target_.device().prog.flashBytes;
      } else {
         return target_.device().flashBytes;
      }
   }

   [[nodiscard]] uint32_t corePc() const {
      if constexpr(Target::hasProgmode) {
         return target_.debug().pc();
      } else {
         return target_.pc();
      }
   }

   // page_ = the flash page at the byte address page
   [[nodiscard]] Status readPage(uint32_t page) {
      if constexpr(Target::hasProgmode) {
         return target_.debug().readFlash(page, page_.data(), flashPageBytes());
      } else {
         return target_.readFlash(page, page_.data(), flashPageBytes());
      }
   }

   // The flash page at the byte address page = page_
   [[nodiscard]] Status writePageBuffer(uint32_t page) {
      if constexpr(Target::hasProgmode) {
         return target_.debug().writeFlashPage(page, page_.data());
      } else {
         return target_.writeFlashPage(page, page_.data());
      }
   }

   [[nodiscard]] Status stepCore() {
      if constexpr(Target::hasProgmode) {
         return target_.debug().step();
      } else {
         return target_.step();
      }
   }

   static void putWord(uint8_t *at, uint16_t value) {
      at[0] = static_cast<uint8_t>(value);
      at[1] = static_cast<uint8_t>(value >> 8);
   }

   // An entry with a change that is not in flash yet, or nullptr
   [[nodiscard]] const SoftwareBreak *pendingBreak() const {
      for(std::size_t i = 0; i < breakCount_; ++i) {
         if(breaks_[i].state != BreakState::Active) {
            return &breaks_[i];
         }
      }
      return nullptr;
   }

   // page_ (the flash page at the byte address page) gets the changes of
   // the table: BREAK at an inserted entry, the original at a removed one.
   void preparePage(uint32_t page) {
      for(std::size_t i = 0; i < breakCount_; ++i) {
         SoftwareBreak &entry = breaks_[i];
         if(pageOf(entry.word) != page || entry.state == BreakState::Active) {
            continue;
         }
         uint8_t *at = &page_[entry.word * 2u - page];
         if(entry.state == BreakState::Inserting) {
            entry.original = static_cast<uint16_t>(at[0] | (at[1] << 8));
            putWord(at, avr8::breakOpcode);
         } else {
            putWord(at, entry.original);
         }
      }
   }

   // The changes of page are in flash now.
   void markPage(uint32_t page) {
      for(std::size_t i = breakCount_; i > 0; --i) {
         SoftwareBreak &entry = breaks_[i - 1];
         if(pageOf(entry.word) != page || entry.state == BreakState::Active) {
            continue;
         }
         if(entry.state == BreakState::Inserting) {
            entry.state = BreakState::Active;
         } else {
            removeBreak(&entry);
         }
      }
   }

   // Writes the changes of the breakpoint table into flash, one page
   // write for each page with a change.
   Status commitBreaks() {
      for(;;) {
         const SoftwareBreak *pending = pendingBreak();
         if(pending == nullptr) {
            return Status::Ok;
         }
         const uint32_t page = pageOf(pending->word);
         if(Status status = readPage(page); status != Status::Ok) {
            return status;
         }
         // The table changes only after the write: after a failed write
         // the next commit tries again, and no BREAK stays in flash without
         // an entry.
         preparePage(page);
         if(Status status = writePageBuffer(page); status != Status::Ok) {
            return status;
         }
         markPage(page);
      }
   }

   // At an active software breakpoint: one step of the original word. On
   // JTAG the OCD executes it without a flash write when it can: AVaRICE
   // waits 500 ms for the response, two page writes take some 1 s at
   // 200 kHz (issue #115). Else the original word for one step, then
   // BREAK again.
   Status stepOverBreak() {
      const SoftwareBreak *entry = findBreak(corePc(), BreakState::Active);
      if(entry == nullptr) {
         return Status::Ok;
      }
      if constexpr(Target::hasProgmode) {
         // Limit: IJMP, ICALL, LPM, ELPM, SPM and SLEEP still take the two
         // page writes, which are too slow for AVaRICE.
         if(AvrJtagDebug::canStepInstead(entry->original)) {
            // The flash holds BREAK at a breakpoint on the next word.
            const SoftwareBreak *after = findBreak(entry->word + 1u, BreakState::Active);
            return target_.debug().stepInstead(entry->original,
                                               after != nullptr ? std::optional<uint16_t>(after->original) : std::nullopt);
         }
      }
      const uint32_t page = pageOf(entry->word);
      uint8_t *at = &page_[entry->word * 2u - page];
      if(Status status = readPage(page); status != Status::Ok) {
         return status;
      }
      putWord(at, entry->original);
      if(Status status = writePageBuffer(page); status != Status::Ok) {
         return status;
      }
      const Status stepped = stepCore();
      putWord(at, avr8::breakOpcode);
      const Status written = writePageBuffer(page);
      return stepped != Status::Ok ? stepped : written;
   }

   // [type][address u32][length u32]
   std::size_t read(bool masked) {
      if(request_.size() < memoryHeaderBytes) {
         return failed(avr8::failure::length);
      }
      const uint8_t type = request_[2];
      const uint32_t address = u32(request_, 3);
      const uint32_t length = u32(request_, 7);
      if(length == 0 || length > response_.size() - 3) {
         return failed(avr8::failure::length);
      }
      const uint8_t *mask = nullptr;
      if(masked) {
         if(type != avr8::memory::sram) {
            return failed(avr8::failure::memoryType);
         }
         if(request_.size() < memoryHeaderBytes + (length + 7u) / 8u) {
            return failed(avr8::failure::length);
         }
         mask = request_.data() + memoryHeaderBytes;
      }
      if(!target_.isStopped()) {
         return failed(avr8::failure::illegalState);
      }
      uint8_t *out = response_.data() + 2;
      DwStatus status = DwStatus::Ok;
      switch(type) {
      case avr8::memory::sram:
         if(!inRange(address, length, 0x10000u)) {
            return failed(avr8::failure::range);
         }
         status = accessData(static_cast<uint16_t>(address), out, static_cast<uint16_t>(length), mask, false);
         break;
      case avr8::memory::spm:
      case avr8::memory::flashPage:
         if(!inRange(address, length, target_.device().flashBytes)) {
            return failed(avr8::failure::range);
         }
         status = target_.readFlash(address, out, length);
         break;
      case avr8::memory::eeprom:
      case avr8::memory::eepromPage:
         if(!inRange(address, length, target_.device().eepromBytes)) {
            return failed(avr8::failure::range);
         }
         status = target_.readEeprom(static_cast<uint16_t>(address), out, static_cast<uint16_t>(length));
         break;
      case avr8::memory::signature: {
         if(!inRange(address, length, 3)) {
            return failed(avr8::failure::range);
         }
         uint16_t id = 0;
         status = target_.readSignature(id);
         const std::array<uint8_t, 3> signature{ { 0x1E, static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id) } };
         std::copy_n(signature.begin() + address, length, out);
         break;
      }
      default:
         return failed(avr8::failure::memoryType);
      }
      if(status != DwStatus::Ok) {
         return failed(failureOf(status));
      }
      return data(length);
   }

   // [type][address u32][length u32][async][data]
   std::size_t write() {
      constexpr std::size_t writeHeaderBytes = memoryHeaderBytes + 1;
      if(request_.size() < writeHeaderBytes) {
         return failed(avr8::failure::length);
      }
      const uint8_t type = request_[2];
      const uint32_t address = u32(request_, 3);
      const uint32_t length = u32(request_, 7);
      if(length == 0 || request_.size() - writeHeaderBytes < length) {
         return failed(avr8::failure::length);
      }
      if(!target_.isStopped()) {
         return failed(avr8::failure::illegalState);
      }
      const uint8_t *in = request_.data() + writeHeaderBytes;
      DwStatus status = DwStatus::Ok;
      switch(type) {
      case avr8::memory::sram:
         if(!inRange(address, length, 0x10000u)) {
            return failed(avr8::failure::range);
         }
         status =
         accessData(static_cast<uint16_t>(address), const_cast<uint8_t *>(in), static_cast<uint16_t>(length), nullptr, true);
         break;
      case avr8::memory::spm:
      case avr8::memory::flashPage: {
         const uint16_t pageBytes = target_.device().flashPageBytes;
         if(!inRange(address, length, target_.device().flashBytes)) {
            return failed(avr8::failure::range);
         }
         if(address % pageBytes != 0 || length % pageBytes != 0) {
            return failed(avr8::failure::alignment);
         }
         for(uint32_t done = 0; done < length && status == DwStatus::Ok; done += pageBytes) {
            status = writePage(address + done, in + done);
         }
         break;
      }
      case avr8::memory::eeprom:
      case avr8::memory::eepromPage:
         if(!inRange(address, length, target_.device().eepromBytes)) {
            return failed(avr8::failure::range);
         }
         status = target_.writeEeprom(static_cast<uint16_t>(address), in, static_cast<uint16_t>(length));
         break;
      default:
         return failed(avr8::failure::memoryType);
      }
      return result(status);
   }

   // One flash page from the host. The breakpoints in the page stay: their
   // original words come from the new data.
   DwStatus writePage(uint32_t page, const uint8_t *in) {
      const uint16_t pageBytes = target_.device().flashPageBytes;
      std::copy_n(in, pageBytes, page_.begin());
      for(std::size_t i = breakCount_; i > 0; --i) {
         SoftwareBreak &entry = breaks_[i - 1];
         if(pageOf(entry.word) != page) {
            continue;
         }
         uint8_t *at = &page_[entry.word * 2u - page];
         if(entry.state == BreakState::Active) {
            entry.original = static_cast<uint16_t>(at[0] | (at[1] << 8));
            putWord(at, avr8::breakOpcode);
         } else if(entry.state == BreakState::Removing) {
            removeBreak(&entry);
         }
      }
      return target_.writeFlashPage(page, page_.data());
   }

   // DATA [IDCODE u32]
   std::size_t idcodeData(uint32_t idcode) {
      for(std::size_t i = 0; i < 4; ++i) {
         response_[2 + i] = static_cast<uint8_t>(idcode >> (8 * i));
      }
      return data(4);
   }

   template <typename T = Target>
   std::size_t progmode(bool enter) {
      AvrJtagProg &prog = target_.prog();
      if(prog.programming() == enter) {
         return ok();
      }
      if(enter) {
         // Bloom loads a program: no breakpoint enable may stay in the OCD
         // for the new program, and the host attaches again after it.
         static_cast<void>(freeCore());
      }
      return (enter ? prog.enter() : prog.leave()) ? ok() : failed(avr8::failure::jtag);
   }

   // [mode][address u32]: mode 0 is the chip erase.
   template <typename T = Target>
   std::size_t chipErase() {
      if(request_.size() < headerBytes + 5) {
         return failed(avr8::failure::length);
      }
      if(request_[2] != 0) {
         return failed(avr8::failure::illegalValue);
      }
      AvrJtagProg &prog = target_.prog();
      if(!prog.programming()) {
         return failed(avr8::failure::illegalState);
      }
      if(!prog.chipErase()) {
         return failed(avr8::failure::jtag);
      }
      // No BREAK is left in flash.
      breakCount_ = 0;
      return ok();
   }

   // [type][address u32][length u32], in the programming mode
   template <typename T = Target>
   std::size_t readJtag() {
      if(request_.size() < memoryHeaderBytes) {
         return failed(avr8::failure::length);
      }
      const uint8_t type = request_[2];
      const uint32_t address = u32(request_, 3);
      const uint32_t length = u32(request_, 7);
      if(length == 0 || length > response_.size() - 3) {
         return failed(avr8::failure::length);
      }
      AvrJtagProg &prog = target_.prog();
      if(!prog.programming()) {
         return failed(avr8::failure::illegalState);
      }
      const AvrJtagDevice &device = target_.device();
      const art::span<uint8_t> out = response_.subspan(2, length);
      // The size of each memory type
      uint32_t size = 0;
      switch(type) {
      case avr8::memory::flashPage:
         size = device.prog.flashBytes;
         break;
      case avr8::memory::eepromPage:
         size = device.eepromBytes;
         break;
      case avr8::memory::fuses:
         size = 3;
         break;
      case avr8::memory::lockBits:
         size = 1;
         break;
      case avr8::memory::signature:
      case avr8::memory::osccal:
         // The 8-bit index of the signature row (avrdude reads sigrow,
         // prodsig and sernum through it)
         size = 0x100;
         break;
      default:
         return failed(avr8::failure::memoryType);
      }
      if(!inRange(address, length, size)) {
         return failed(avr8::failure::range);
      }
      bool done = true;
      switch(type) {
      case avr8::memory::flashPage:
         done = prog.readFlash(device.prog, address, out);
         break;
      case avr8::memory::eepromPage:
         done = prog.readEeprom(static_cast<uint16_t>(address), out);
         break;
      case avr8::memory::lockBits:
         done = prog.readLockBits(out[0]);
         break;
      default:
         for(uint32_t i = 0; i < length && done; ++i) {
            const auto at = static_cast<uint8_t>(address + i);
            done = type == avr8::memory::fuses     ? prog.readFuse(static_cast<Fuse>(at), out[i]) :
                   type == avr8::memory::signature ? prog.readSignature(at, out[i]) :
                                                     prog.readCalibration(at, out[i]);
         }
         break;
      }
      return done ? data(length) : failed(avr8::failure::jtag);
   }

   // [type][address u32][length u32][async][data], in the programming mode
   template <typename T = Target>
   std::size_t writeJtag() {
      constexpr std::size_t writeHeaderBytes = memoryHeaderBytes + 1;
      if(request_.size() < writeHeaderBytes) {
         return failed(avr8::failure::length);
      }
      const uint8_t type = request_[2];
      const uint32_t address = u32(request_, 3);
      const uint32_t length = u32(request_, 7);
      if(length == 0 || request_.size() - writeHeaderBytes < length) {
         return failed(avr8::failure::length);
      }
      AvrJtagProg &prog = target_.prog();
      if(!prog.programming()) {
         return failed(avr8::failure::illegalState);
      }
      const AvrJtagDevice &device = target_.device();
      const art::span<const uint8_t> in = request_.subspan(writeHeaderBytes, length);
      bool done = true;
      switch(type) {
      case avr8::memory::flashPage: {
         const uint16_t pageBytes = device.prog.flashPageBytes;
         if(!inRange(address, length, device.prog.flashBytes)) {
            return failed(avr8::failure::range);
         }
         if(address % pageBytes != 0 || length % pageBytes != 0) {
            return failed(avr8::failure::alignment);
         }
         for(uint32_t at = 0; at < length && done; at += pageBytes) {
            done = prog.writeFlashPage(device.prog, address + at, in.subspan(at, pageBytes));
         }
         break;
      }
      case avr8::memory::eepromPage:
         if(!inRange(address, length, device.eepromBytes)) {
            return failed(avr8::failure::range);
         }
         done = prog.writeEeprom(device.prog, static_cast<uint16_t>(address), in);
         break;
      case avr8::memory::fuses:
         if(!inRange(address, length, 3)) {
            return failed(avr8::failure::range);
         }
         for(uint32_t i = 0; i < length && done; ++i) {
            done = prog.writeFuse(static_cast<Fuse>(address + i), in[i]);
         }
         break;
      case avr8::memory::lockBits:
         if(!inRange(address, length, 1)) {
            return failed(avr8::failure::range);
         }
         done = prog.writeLockBits(in[0]);
         break;
      default:
         return failed(avr8::failure::memoryType);
      }
      return done ? ok() : failed(avr8::failure::jtag);
   }

   static constexpr bool inRange(uint32_t address, uint32_t length, uint32_t size) {
      return address <= size && length <= size - address;
   }

   DwStatus accessData(uint16_t address, uint8_t *buffer, uint16_t length, const uint8_t *mask, bool write) {
      return dataRuns<DwStatus>(address, buffer, length, mask, write, target_.device().dwdr,
                                [&](uint16_t from, uint8_t *at, uint16_t count) {
                                   return write ? target_.writeData(from, at, count) : target_.readData(from, at, count);
                                });
   }

   template <typename T = Target>
   JtagDebugStatus accessDebugData(uint16_t address, uint8_t *buffer, uint16_t length, const uint8_t *mask, bool write) {
      AvrJtagDebug &debug = target_.debug();
      if(!debug.hasContext()) {
         return contextFailure();
      }
      return dataRuns<JtagDebugStatus>(address, buffer, length, mask, write, target_.device().ocdr,
                                       [&](uint16_t from, uint8_t *at, uint16_t count) {
                                          return write ? debug.writeData(from, at, count) : debug.readData(from, at, count);
                                       });
   }

   // The data space in runs without the OCD data register (DWDR, OCDR: I/O
   // address ocdrIo) and without the masked bytes. A read gives 0 for them,
   // a write skips them. access(address, buffer, count) does one run.
   template <typename Status, typename Access>
   static Status
   dataRuns(uint16_t address, uint8_t *buffer, uint16_t length, const uint8_t *mask, bool write, uint8_t ocdrIo, Access access) {
      const uint32_t ocdr = avrIoData + ocdrIo;
      const auto skip = [&](uint32_t i) {
         return address + i == ocdr || (mask != nullptr && ((mask[i / 8u] >> (i % 8u)) & 1u) == 0);
      };
      uint32_t at = 0;
      while(at < length) {
         if(skip(at)) {
            if(!write) {
               buffer[at] = 0;
            }
            ++at;
            continue;
         }
         uint32_t end = at;
         while(end < length && !skip(end)) {
            ++end;
         }
         const Status status = access(static_cast<uint16_t>(address + at), buffer + at, static_cast<uint16_t>(end - at));
         if(status != Status::Ok) {
            return status;
         }
         at = end;
      }
      return Status::Ok;
   }

   Target &target_;
   art::span<const uint8_t> request_;
   art::span<uint8_t> response_;
   std::array<uint8_t, 2> config_{};
   std::array<uint8_t, 1> physical_{};
   std::array<uint8_t, 4> daisyChain_{};
   std::array<uint8_t, 2> clockProg_{ defaultClock };
   std::array<uint8_t, 2> clockDebug_{ defaultClock };
   std::array<uint8_t, avr8::deviceBytes> device_{};
   std::array<uint8_t, 1> options_{};
   bool active_{ false };
   // JTAG: in the debug mode (ATTACH)
   bool attached_{ false };
   // Active without a device context (see ACTIVATE_PHYSICAL)
   bool bare_{ false };
   // The probe started the program (RUN, RUN_TO, ATTACH without break).
   bool running_{ false };
   // A break event waits for the host.
   bool event_{ false };
   bool eventAtBreak_{ false };
   uint16_t eventSequence_{ 0 };
   std::optional<uint16_t> hardwareBreak_;
   // The breakpoint of the last run (word), 0xFFFF for none
   uint16_t runBreak_{ 0xFFFF };
   std::array<SoftwareBreak, avr8::maxSoftwareBreaks> breaks_{};
   // The job of a RUN, RUN_TO or STEP (startJob()). jobRead_: the bytes of
   // the page at jobPage_ in page_ (0: no page read yet).
   Job job_{ Job::None };
   std::optional<uint32_t> jobTo_;
   uint32_t jobPage_{ 0 };
   uint16_t jobRead_{ 0 };
   bool jobLoaded_{ false };
   // One slice takes some 50 ms, below the 200 ms of an AVaRICE event poll.
   // The TCK cycles of one flash word read and of one page buffer load
   // (host model, tests/TestEdbgJtag.cpp).
   static constexpr uint32_t sliceMs = 50;
   static constexpr uint32_t flashReadCycles = 84;
   static constexpr uint32_t spmLoadCycles = 1200;
   std::size_t breakCount_{ 0 };
   std::array<uint8_t, Target::hasProgmode ? avr8::maxJtagPageBytes : avr8::maxPageBytes> page_{};
};

// Two scopes on one EDBG transport, as the debugWIRE scope and the JTAG
// scope. SET goes to both: the host can set the parameters before the
// variant and the physical interface. The active scope gets the other
// commands; without one, the scope that the host selected (else first)
// gets them. Thus ACTIVATE_PHYSICAL goes to the scope of the variant and
// the physical interface of the host.
class Avr8Select final : public Avr8Handler {
   public:
   Avr8Select(Avr8Handler &first, Avr8Handler &second)
      : first_(first)
      , second_(second) {
   }

   std::size_t process(art::span<const uint8_t> request, art::span<uint8_t> response) override {
      Avr8Handler &target = current();
      if(!request.empty() && request[0] == avr8::command::set) {
         // The response of the other scope is the same, except for a clock
         // that only the active JTAG scope applies.
         std::array<uint8_t, 4> ignored{};
         static_cast<void>((&target == &first_ ? second_ : first_).process(request, ignored));
      }
      return target.process(request, response);
   }

   std::size_t event(art::span<uint8_t> frame) override {
      return current().event(frame);
   }

   void release() override {
      first_.release();
      second_.release();
   }

   void forget() override {
      first_.forget();
      second_.forget();
   }

   [[nodiscard]] bool active() const override {
      return first_.active() || second_.active();
   }

   [[nodiscard]] bool selected() const override {
      return first_.selected() || second_.selected();
   }

   private:
   Avr8Handler &current() {
      if(first_.active()) {
         return first_;
      }
      if(second_.active() || second_.selected()) {
         return second_;
      }
      return first_;
   }

   Avr8Handler &first_;
   Avr8Handler &second_;
};

} // namespace avr
