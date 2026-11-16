// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The AVR8 generic scope of the EDBG protocol on debugWIRE ("EDBG-based
// Tools Protocols", DS50002630A, chapter 7): the commands of avrdude
// (jtagice3_dw) and Bloom. Hardware-independent: it runs on an
// avr::DebugWireTarget (avr/DebugWireTarget.h), and the host unit tests use
// the mock dW target. The EDBG transport gives one command payload and
// sends the response, and gives the AVR_EVT requests to event().
//
// A command is [id][version 0][fields]. Values are LSB first. The
// responses are OK [0x80][0], DATA [0x84][1][data][0], PC [0x83][0][pc
// u32] and FAILED [0xA0][0][code]. Addresses of flash, SRAM and EEPROM are
// byte addresses. The PC, RUN_TO and the break event use word addresses.
//
//   0x01 SET, 0x02 GET [context][address][bytes][data]: the parameters.
//        Context 0: variant, function. Context 1: physical interface.
//        Context 2: the device context (below). Context 3 address 0: run
//        the timers of the target while it is stopped.
//   0x10 ACTIVATE_PHYSICAL [reset]: needs variant 1 (debugWIRE) and
//        physical 5 (debugWIRE), connects and stops the target (break).
//        The reset flag is not used: the RESET pin is the dW line.
//   0x11 DEACTIVATE_PHYSICAL: a stopped target gets its software
//        breakpoints removed, a reset and runs; the line is then
//        released. OK also without an active physical (avrdude ISP).
//   0x12 GET_ID -> DATA [ID low][ID high][0x1E][0]: the dW ID (for
//        example 0x950F) as the signature 1E 95 0F, read backwards.
//   0x13 ATTACH [break]: break 1: the target stays stopped, and a break
//        event comes. Break 0: the target runs.
//   0x14 DETACH: the software breakpoints are removed, the target runs.
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
// The device context (context 2, as avrdude struct mega_device_desc):
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
// this wears the flash (some 10000 cycles per page). An injection of the
// original instruction (as dw-link does) would avoid it.
//
// Break events: event() gives one frame [0x0E][0][sequence u16][0x12]
// [0x40][pc u32][cause][0x01][0x00], with cause 1 at a breakpoint. While
// the target runs, each event() checks the line one time.
//
// All fields come from the host. The scope checks them before it touches
// the target: a command that is too short gives FAILED 0x35, an address
// outside the device 0x38, a flash write that is not page aligned 0x37, a
// device context with values out of range at ACTIVATE_PHYSICAL 0x33.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <utils/compat/span.h>

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
inline constexpr uint8_t notActivated = 0x31;
inline constexpr uint8_t illegalState = 0x32;
inline constexpr uint8_t invalidConfig = 0x33;
inline constexpr uint8_t memoryType = 0x34;
inline constexpr uint8_t length = 0x35;
inline constexpr uint8_t badAddress = 0x36;
inline constexpr uint8_t alignment = 0x37;
inline constexpr uint8_t range = 0x38;
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
inline constexpr uint8_t signature = 0xB4;
} // namespace memory

// Parameters
inline constexpr uint8_t variantDebugWire = 0x01;
inline constexpr uint8_t physicalDebugWire = 0x05;
inline constexpr uint8_t eventBreak = 0x40;
inline constexpr uint8_t handler = 0x12;
inline constexpr std::size_t eventFrameBytes = 13;
inline constexpr std::size_t deviceBytes = 0x20;
inline constexpr std::size_t maxSoftwareBreaks = 32;
// The largest flash page of an AVR with debugWIRE is 128 bytes.
inline constexpr std::size_t maxPageBytes = 128;
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

   protected:
   ~Avr8Handler() = default;
};

template <typename Target>
class Avr8Scope final : public Avr8Handler {
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
      if(request.empty()) {
         return failed(avr8::failure::unknown);
      }
      return execute(request[0]);
   }

   std::size_t event(art::span<uint8_t> frame) override {
      if(frame.size() < avr8::eventFrameBytes) {
         return 0;
      }
      if(active_ && running_ && !event_) {
         // The program stops at a breakpoint: the target sends a break.
         if(target_.poll() == DwStatus::Ok) {
            stopped(true);
         }
      }
      if(!event_) {
         return 0;
      }
      event_ = false;
      const uint32_t pc = target_.pc();
      const std::array<uint8_t, avr8::eventFrameBytes> bytes{
         { 0x0E, 0x00, static_cast<uint8_t>(eventSequence_), static_cast<uint8_t>(eventSequence_ >> 8), avr8::handler,
           avr8::eventBreak, static_cast<uint8_t>(pc), static_cast<uint8_t>(pc >> 8), 0x00, 0x00,
           static_cast<uint8_t>(eventAtBreak_ ? 1 : 0), 0x01, 0x00 }
      };
      ++eventSequence_;
      std::copy(bytes.begin(), bytes.end(), frame.begin());
      return bytes.size();
   }

   void release() override {
      if(!active_) {
         return;
      }
      if(running_ && target_.poll() != DwStatus::Ok) {
         static_cast<void>(target_.stop());
      }
      if(target_.isStopped()) {
         clearAllBreaks();
         static_cast<void>(commitBreaks());
      }
      static_cast<void>(target_.detach());
      end();
   }

   void forget() override {
      release();
      config_ = {};
      physical_ = {};
      device_ = {};
      options_ = {};
      breakCount_ = 0;
   }

   [[nodiscard]] bool active() const override {
      return active_;
   }

   private:
   enum class BreakState : uint8_t { Inserting, Active, Removing };
   struct SoftwareBreak {
      uint16_t word;
      uint16_t original;
      BreakState state;
   };

   static constexpr std::size_t headerBytes = 2;
   static constexpr std::size_t memoryHeaderBytes = 11;

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
      case cmd::erase:
      case cmd::crc:
      case cmd::pageErase:
         return failed(avr8::failure::notSupported);
      default:
         break;
      }
      if(!active_) {
         return failed(avr8::failure::notActivated);
      }
      switch(id) {
      case cmd::getId:
         return getId();
      case cmd::attach:
         return attach();
      case cmd::detach:
         return detach();
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
      switch(context) {
      case 0:
         table = config_;
         break;
      case 1:
         table = physical_;
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
      if(bytes == 0 || std::size_t{ address } + bytes > table.size() || (set && request_.size() - accessBytes < bytes)) {
         return failed(avr8::failure::notSupported);
      }
      if(set) {
         std::copy_n(request_.begin() + accessBytes, bytes, table.begin() + address);
         target_.setRunTimers(options_[0] != 0);
         return ok();
      }
      std::copy_n(table.begin() + address, bytes, response_.begin() + 2);
      return data(bytes);
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

   std::size_t activate() {
      if(active_) {
         return ok();
      }
      const auto dw = device();
      if(config_[0] != avr8::variantDebugWire || physical_[0] != avr8::physicalDebugWire || !dw) {
         return failed(avr8::failure::invalidConfig);
      }
      target_.setDevice(*dw);
      target_.setRunTimers(options_[0] != 0);
      const DwStatus status = target_.attach();
      if(status != DwStatus::Ok) {
         static_cast<void>(target_.detach());
         return failed(failureOf(status));
      }
      active_ = true;
      running_ = false;
      event_ = false;
      hardwareBreak_.reset();
      return ok();
   }

   std::size_t deactivate() {
      if(!active_) {
         return ok();
      }
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

   void end() {
      active_ = false;
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
      const uint16_t pc = target_.pc();
      response_[0] = avr8::response::pc;
      response_[1] = 0;
      response_[2] = static_cast<uint8_t>(pc);
      response_[3] = static_cast<uint8_t>(pc >> 8);
      response_[4] = 0;
      response_[5] = 0;
      return 6;
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
      if(request_[set ? 3 : 2] != 1) {
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
         if(address >= target_.device().flashBytes || address % 2 != 0) {
            return failed(avr8::failure::range);
         }
      }
      for(std::size_t at = headerBytes; at < request_.size(); at += 4) {
         const auto word = static_cast<uint16_t>(u32(request_, at) / 2u);
         if(!set) {
            clearBreak(word);
         } else if(!setBreak(word)) {
            return failed(avr8::failure::resources);
         }
      }
      return ok();
   }

   [[nodiscard]] SoftwareBreak *findBreak(uint16_t word, std::optional<BreakState> state = std::nullopt) {
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

   bool setBreak(uint16_t word) {
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

   void clearBreak(uint16_t word) {
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

   [[nodiscard]] uint32_t pageOf(uint16_t word) const {
      return (uint32_t{ word } * 2u) & ~(uint32_t{ target_.device().flashPageBytes } - 1u);
   }

   static void putWord(uint8_t *at, uint16_t value) {
      at[0] = static_cast<uint8_t>(value);
      at[1] = static_cast<uint8_t>(value >> 8);
   }

   // Writes the changes of the breakpoint table into flash, one page
   // write for each page with a change.
   DwStatus commitBreaks() {
      for(;;) {
         const SoftwareBreak *pending = nullptr;
         for(std::size_t i = 0; i < breakCount_ && pending == nullptr; ++i) {
            if(breaks_[i].state != BreakState::Active) {
               pending = &breaks_[i];
            }
         }
         if(pending == nullptr) {
            return DwStatus::Ok;
         }
         const uint32_t page = pageOf(pending->word);
         const uint16_t pageBytes = target_.device().flashPageBytes;
         if(DwStatus status = target_.readFlash(page, page_.data(), pageBytes); status != DwStatus::Ok) {
            return status;
         }
         for(std::size_t i = breakCount_; i > 0; --i) {
            SoftwareBreak &entry = breaks_[i - 1];
            if(pageOf(entry.word) != page || entry.state == BreakState::Active) {
               continue;
            }
            uint8_t *at = &page_[entry.word * 2u - page];
            if(entry.state == BreakState::Inserting) {
               entry.original = static_cast<uint16_t>(at[0] | (at[1] << 8));
               entry.state = BreakState::Active;
               putWord(at, avr8::breakOpcode);
            } else {
               putWord(at, entry.original);
               removeBreak(&entry);
            }
         }
         if(DwStatus status = target_.writeFlashPage(page, page_.data()); status != DwStatus::Ok) {
            return status;
         }
      }
   }

   // At an active software breakpoint: the original word for one step,
   // then BREAK again.
   DwStatus stepOverBreak() {
      const SoftwareBreak *entry = findBreak(target_.pc(), BreakState::Active);
      if(entry == nullptr) {
         return DwStatus::Ok;
      }
      const uint32_t page = pageOf(entry->word);
      uint8_t *at = &page_[entry->word * 2u - page];
      if(DwStatus status = target_.readFlash(page, page_.data(), target_.device().flashPageBytes); status != DwStatus::Ok) {
         return status;
      }
      putWord(at, entry->original);
      if(DwStatus status = target_.writeFlashPage(page, page_.data()); status != DwStatus::Ok) {
         return status;
      }
      const DwStatus stepped = target_.step();
      putWord(at, avr8::breakOpcode);
      const DwStatus written = target_.writeFlashPage(page, page_.data());
      return stepped != DwStatus::Ok ? stepped : written;
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

   static constexpr bool inRange(uint32_t address, uint32_t length, uint32_t size) {
      return address <= size && length <= size - address;
   }

   // The data space in runs without DWDR and without the masked bytes. A
   // read gives 0 for them, a write skips them.
   DwStatus accessData(uint16_t address, uint8_t *buffer, uint16_t length, const uint8_t *mask, bool write) {
      const uint32_t dwdr = avrIoData + target_.device().dwdr;
      const auto skip = [&](uint32_t i) {
         return address + i == dwdr || (mask != nullptr && ((mask[i / 8u] >> (i % 8u)) & 1u) == 0);
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
         const auto from = static_cast<uint16_t>(address + at);
         const auto count = static_cast<uint16_t>(end - at);
         const DwStatus status = write ? target_.writeData(from, buffer + at, count) : target_.readData(from, buffer + at, count);
         if(status != DwStatus::Ok) {
            return status;
         }
         at = end;
      }
      return DwStatus::Ok;
   }

   Target &target_;
   art::span<const uint8_t> request_;
   art::span<uint8_t> response_;
   std::array<uint8_t, 2> config_{};
   std::array<uint8_t, 1> physical_{};
   std::array<uint8_t, avr8::deviceBytes> device_{};
   std::array<uint8_t, 1> options_{};
   bool active_{ false };
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
   std::size_t breakCount_{ 0 };
   std::array<uint8_t, avr8::maxPageBytes> page_{};
};

} // namespace avr
