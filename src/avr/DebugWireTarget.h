// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// debugWIRE (dW) target operations on the physical layer
// (avr/DebugWireEngine.h): stop, go, step, reset, registers, data space
// (SRAM and I/O), flash, EEPROM, signature and disable.
//
// Microchip does not document the dW commands. The command bytes and
// sequences here come from the public reverse-engineering work (RikusW,
// dwire-debug, dw-link). The hardware test of the AVR support checks them on
// real chips. The commands:
//
//   0x06        disable dW until a power cycle (ISP works again)
//   0x07        reset; the target answers with a break and 0x55
//   0x20        start the repeated operation of the 0xC2 mode
//   0x23        execute the instruction of 0xD2
//   0x30        go
//   0x31        single step; the target answers with a break and 0x55
//   0x33        execute the instruction of 0xD2 (SPM erase or write); the
//               target answers with a break and 0x55 when it is done
//   0x40, 0x41  context for go: 0x41 with the hardware breakpoint of 0xD1
//   0x60        context for single step (also 0x60 for go without
//               breakpoint, see below)
//   0x64        context for 0x23 and 0x33 (single instructions)
//   0x66        context for 0x20 (repeated operations)
//   0xC2 m      the mode of 0x20: 0x00 read data (ld Z+), 0x01 read
//               registers, 0x02 read flash (lpm Z+), 0x04 write data (st
//               Z+), 0x05 write registers
//   0xD0 h l    the PC register: the PC for go and step, the first index
//               of a repeated operation
//   0xD1 h l    the hardware breakpoint, the end index of a repeated
//               operation
//   0xD2 h l    the instruction for 0x23 and 0x33
//   0xF0        read the PC register (2 bytes, high first)
//   0xF3        read the signature (2 bytes, high first)
//
// Bit 5 (0x20) of a context byte stops the timers of the target while it
// is not running the program. The engine clears it when runTimers is set
// (0x40, 0x44, 0x46 instead of 0x60, 0x64, 0x66).
//
// Injected instructions: "in r, DWDR" takes the next byte from the probe,
// "out DWDR, r" sends r to the probe. Each executed instruction and each
// repeated operation changes the PC register, and the repeated data
// operations change Z (r30, r31) and Y (r28, r29). Thus each stop saves
// the context (PC, r0-r31, SREG) in the engine, and go and step restore it.
// The register functions, PC and SREG use the saved values, and the
// memory functions can change r0, r1, r28-r31 and SREG freely.
//
// After a stop, the PC register is one word after the stop address.
//
// Each operation needs an attached, stopped target (else TargetRunning),
// except stop(), poll() and attach().

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "avr/DebugWireEngine.h"

namespace avr {

// The device parameters, from the host (device context of the EDBG AVR8
// scope). The register addresses are I/O addresses (0x00-0x3F), for in and
// out. The data space address is the I/O address + 0x20.
struct DwDevice {
   uint16_t flashPageBytes{ 128 };
   uint32_t flashBytes{ 32768 };
   // A word address in the smallest boot section (megaAVR): SPM executes
   // only in the boot section, and the probe sets the PC register to this
   // address for each SPM. The smallest boot section (BOOTSZ = 11, the
   // last 256 words on an ATmega328P) is in the boot section for all
   // BOOTSZ values, so its first word, or the start of the last flash page,
   // works for all fuse settings. 0: no boot section (tinyAVR), SPM
   // executes from all addresses.
   uint16_t bootStartWords{ 0 };
   uint16_t eepromBytes{ 1024 };
   uint8_t dwdr{ 0x31 };
   uint8_t spmcsr{ 0x37 };
   // EEARH equal to EEARL: the EEPROM address register has one byte.
   uint8_t eearh{ 0x22 };
   uint8_t eearl{ 0x21 };
   uint8_t eecr{ 0x1F };
   uint8_t eedr{ 0x20 };
};

// SREG, the same on all AVR
inline constexpr uint8_t avrIoSreg = 0x3F;
// The data space address of I/O address 0
inline constexpr uint16_t avrIoData = 0x20;

// "in r, A" and "out A, r": 1011 0AAr rrrr AAAA, bit 11 set for out
constexpr uint16_t avrIn(uint8_t reg, uint8_t io) {
   return static_cast<uint16_t>(0xB000u | ((io & 0x30u) << 5) | ((reg & 0x1Fu) << 4) | (io & 0x0Fu));
}
constexpr uint16_t avrOut(uint8_t io, uint8_t reg) {
   return static_cast<uint16_t>(avrIn(reg, io) | 0x0800u);
}
inline constexpr uint16_t avrSpm = 0x95E8;
// adiw r30, 2 (Z += 2)
inline constexpr uint16_t avrAdiwZ2 = 0x9632;

// The time limits of the answers (break and 0x55) of the target
inline constexpr uint32_t dwStepTimeoutUs = 100'000;
// The start-up time of the target after a reset can be 65 ms and more.
inline constexpr uint32_t dwResetTimeoutUs = 300'000;
// Page erase and page write take about 4.5 ms.
inline constexpr uint32_t dwSpmTimeoutUs = 50'000;
// The attempts of an SPM page erase or write without an answer
inline constexpr uint32_t dwSpmAttempts = 3;
// The time limit of an EEPROM write (3.4 ms in the datasheets) and of a
// busy SPMCSR, and the time between two polls
inline constexpr uint32_t dwBusyTimeoutUs = 20'000;
inline constexpr uint32_t dwBusyPollUs = 100;
// The bytes of one repeated data or flash operation
inline constexpr uint16_t dwChunkBytes = 128;

// SPMCSR values (SPMEN with PGERS, PGWRT, RWWSRE)
inline constexpr uint8_t spmFill = 0x01;
inline constexpr uint8_t spmErase = 0x03;
inline constexpr uint8_t spmWrite = 0x05;
inline constexpr uint8_t spmRwwEnable = 0x11;
// EECR bits
inline constexpr uint8_t eecrRead = 0x01;
inline constexpr uint8_t eecrWrite = 0x02;
inline constexpr uint8_t eecrMasterWrite = 0x04;

template <typename Engine>
class DebugWireTarget {
   public:
   DebugWireTarget(Engine &engine, const DwDevice &device)
      : engine_(engine)
      , device_(device) {
   }

   void setDevice(const DwDevice &device) {
      device_ = device;
   }
   [[nodiscard]] const DwDevice &device() const {
      return device_;
   }

   // The timers of the target run while the probe accesses it.
   void setRunTimers(bool run) {
      mask_ = run ? 0xDFu : 0xFFu;
   }

   // Connects the engine and stops the target (break).
   [[nodiscard]] DwStatus attach() {
      engine_.connect();
      attached_ = true;
      stopped_ = false;
      return stop();
   }

   // Lets a stopped target run on (without breakpoint) and releases the
   // line.
   DwStatus detach() {
      DwStatus status = DwStatus::Ok;
      if(stopped_) {
         status = go(std::nullopt);
      }
      engine_.disconnect();
      attached_ = false;
      stopped_ = false;
      return status;
   }

   // Stops a running target with a break. A stopped target stays stopped.
   [[nodiscard]] DwStatus stop() {
      if(!attached_) {
         return DwStatus::TargetRunning;
      }
      if(stopped_) {
         return DwStatus::Ok;
      }
      const DwStatus status = engine_.sync();
      return status == DwStatus::Ok ? stopped() : status;
   }

   // For a running target: Ok when it stopped (breakpoint), Timeout when
   // it runs. Checks the receiver one time.
   [[nodiscard]] DwStatus poll() {
      if(!attached_) {
         return DwStatus::TargetRunning;
      }
      if(stopped_) {
         return DwStatus::Ok;
      }
      const DwStatus status = engine_.waitBreak(0);
      return status == DwStatus::Ok ? stopped() : status;
   }

   // Restores the context and starts the program at pc(). breakWord: the
   // hardware breakpoint (word address).
   [[nodiscard]] DwStatus go(std::optional<uint16_t> breakWord) {
      if(DwStatus status = restore(); status != DwStatus::Ok) {
         return status;
      }
      if(breakWord) {
         const std::array<uint8_t, 7> command{ { context(0x61), 0xD1, high(*breakWord), low(*breakWord), 0xD0, high(pc_), low(pc_) } };
         if(DwStatus status = send(command); status != DwStatus::Ok) {
            return status;
         }
      } else {
         const std::array<uint8_t, 4> command{ { context(0x60), 0xD0, high(pc_), low(pc_) } };
         if(DwStatus status = send(command); status != DwStatus::Ok) {
            return status;
         }
      }
      stopped_ = false;
      return sendByte(0x30);
   }

   // Executes one instruction at pc().
   [[nodiscard]] DwStatus step() {
      if(DwStatus status = restore(); status != DwStatus::Ok) {
         return status;
      }
      const std::array<uint8_t, 5> command{ { context(0x60), 0xD0, high(pc_), low(pc_), 0x31 } };
      stopped_ = false;
      if(DwStatus status = send(command); status != DwStatus::Ok) {
         return status;
      }
      const DwStatus status = engine_.waitBreak(dwStepTimeoutUs);
      return status == DwStatus::Ok ? stopped() : status;
   }

   // Resets the target. It stops at address 0.
   [[nodiscard]] DwStatus reset() {
      if(!stopped_) {
         return DwStatus::TargetRunning;
      }
      engine_.flush();
      stopped_ = false;
      if(DwStatus status = sendByte(0x07); status != DwStatus::Ok) {
         return status;
      }
      const DwStatus status = engine_.waitBreak(dwResetTimeoutUs);
      return status == DwStatus::Ok ? stopped() : status;
   }

   // Turns dW off until a power cycle and releases the line. ISP works
   // then. The DWEN fuse does not change.
   [[nodiscard]] DwStatus disable() {
      if(!stopped_) {
         return DwStatus::TargetRunning;
      }
      const DwStatus status = sendByte(0x06);
      engine_.disconnect();
      attached_ = false;
      stopped_ = false;
      return status;
   }

   // The signature of the dW module, for example 0x950F (ATmega328P)
   [[nodiscard]] DwStatus readSignature(uint16_t &signature) {
      if(!stopped_) {
         return DwStatus::TargetRunning;
      }
      std::array<uint8_t, 2> value{};
      const DwStatus status = query(std::array<uint8_t, 1>{ { 0xF3 } }, value.data(), value.size());
      signature = static_cast<uint16_t>((value[0] << 8) | value[1]);
      return status;
   }

   [[nodiscard]] bool attached() const {
      return attached_;
   }
   [[nodiscard]] bool isStopped() const {
      return stopped_;
   }

   // The saved context. Valid while the target is stopped. Changes go to
   // the target at go() and step().
   [[nodiscard]] uint16_t pc() const {
      return pc_;
   }
   void setPc(uint16_t word) {
      pc_ = word;
   }
   [[nodiscard]] uint8_t sreg() const {
      return sreg_;
   }
   void setSreg(uint8_t value) {
      sreg_ = value;
   }

   // Registers first to first + count - 1 of the saved context
   [[nodiscard]] DwStatus readRegisters(uint8_t first, uint8_t *out, uint8_t count) const {
      if(!stopped_) {
         return DwStatus::TargetRunning;
      }
      if(first + count > 32) {
         return DwStatus::InvalidArgument;
      }
      for(uint8_t i = 0; i < count; ++i) {
         out[i] = regs_[first + i];
      }
      return DwStatus::Ok;
   }
   [[nodiscard]] DwStatus writeRegisters(uint8_t first, const uint8_t *data, uint8_t count) {
      if(!stopped_) {
         return DwStatus::TargetRunning;
      }
      if(first + count > 32) {
         return DwStatus::InvalidArgument;
      }
      for(uint8_t i = 0; i < count; ++i) {
         regs_[first + i] = data[i];
      }
      return DwStatus::Ok;
   }

   // The data space: registers (0x00-0x1F, saved context), I/O, extended
   // I/O and SRAM. SREG (0x5F) is the saved context too. DWDR is not
   // accessible: a range with it gives InvalidArgument (the EDBG scope
   // masks it).
   [[nodiscard]] DwStatus readData(uint16_t address, uint8_t *out, uint16_t length) {
      return accessData(address, out, length, false);
   }
   [[nodiscard]] DwStatus writeData(uint16_t address, const uint8_t *data, uint16_t length) {
      return accessData(address, const_cast<uint8_t *>(data), length, true);
   }

   // Flash bytes (byte address). Software breakpoints are not filtered out.
   [[nodiscard]] DwStatus readFlash(uint32_t address, uint8_t *out, uint32_t length) {
      if(!stopped_) {
         return DwStatus::TargetRunning;
      }
      // Z has 16 bits: all dW devices have 32 KB of flash or less.
      if(!inRange(address, length, device_.flashBytes) || !inRange(address, length, 0x10000u)) {
         return DwStatus::InvalidArgument;
      }
      while(length > 0) {
         const auto chunk = static_cast<uint16_t>(length < dwChunkBytes ? length : dwChunkBytes);
         if(DwStatus status = setZ(static_cast<uint16_t>(address)); status != DwStatus::Ok) {
            return status;
         }
         if(DwStatus status = repeatQuery(0x02, 0, static_cast<uint16_t>(2u * chunk), out, chunk); status != DwStatus::Ok) {
            return status;
         }
         address += chunk;
         out += chunk;
         length -= chunk;
      }
      return DwStatus::Ok;
   }

   // Erases one flash page (byte address, page aligned).
   [[nodiscard]] DwStatus eraseFlashPage(uint32_t address) {
      if(!stopped_) {
         return DwStatus::TargetRunning;
      }
      if(!isPage(address)) {
         return DwStatus::InvalidArgument;
      }
      return spmPage(spmErase, static_cast<uint16_t>(address));
   }

   // Erases and writes one flash page (byte address, page aligned,
   // flashPageBytes of data). megaAVR: then the RWW section is enabled
   // again.
   [[nodiscard]] DwStatus writeFlashPage(uint32_t address, const uint8_t *data) {
      if(!stopped_) {
         return DwStatus::TargetRunning;
      }
      if(!isPage(address)) {
         return DwStatus::InvalidArgument;
      }
      const auto page = static_cast<uint16_t>(address);
      if(DwStatus status = spmPage(spmErase, page); status != DwStatus::Ok) {
         return status;
      }
      if(DwStatus status = fillPageBuffer(page, data); status != DwStatus::Ok) {
         return status;
      }
      if(DwStatus status = spmPage(spmWrite, page); status != DwStatus::Ok) {
         return status;
      }
      return enableRww();
   }

   [[nodiscard]] DwStatus readEeprom(uint16_t address, uint8_t *out, uint16_t length) {
      if(!stopped_) {
         return DwStatus::TargetRunning;
      }
      if(!inRange(address, length, device_.eepromBytes)) {
         return DwStatus::InvalidArgument;
      }
      for(uint16_t i = 0; i < length; ++i) {
         const auto at = static_cast<uint16_t>(address + i);
         // r28 EERE, r29 the data, r30 and r31 the address
         if(DwStatus status = writeRegs(28, std::array<uint8_t, 4>{ { eecrRead, 0, low(at), high(at) } }); status != DwStatus::Ok) {
            return status;
         }
         if(DwStatus status = setEepromAddress(); status != DwStatus::Ok) {
            return status;
         }
         const std::array<uint8_t, 12> command{ { context(0x64), 0xD2, high(avrOut(device_.eecr, 28)),
                                                  low(avrOut(device_.eecr, 28)), 0x23, 0xD2,
                                                  high(avrIn(29, device_.eedr)), low(avrIn(29, device_.eedr)), 0x23,
                                                  0xD2, high(avrOut(device_.dwdr, 29)), low(avrOut(device_.dwdr, 29)) } };
         if(DwStatus status = send(command); status != DwStatus::Ok) {
            return status;
         }
         if(DwStatus status = queryByte(0x23, out[i]); status != DwStatus::Ok) {
            return status;
         }
      }
      return DwStatus::Ok;
   }

   // Writes EEPROM bytes (erase and write of each byte). Waits for the end
   // of each write.
   [[nodiscard]] DwStatus writeEeprom(uint16_t address, const uint8_t *data, uint16_t length) {
      if(!stopped_) {
         return DwStatus::TargetRunning;
      }
      if(!inRange(address, length, device_.eepromBytes)) {
         return DwStatus::InvalidArgument;
      }
      for(uint16_t i = 0; i < length; ++i) {
         const auto at = static_cast<uint16_t>(address + i);
         // r28 EEMPE, r29 EEPE, r30 and r31 the address
         if(DwStatus status = writeRegs(28, std::array<uint8_t, 4>{ { eecrMasterWrite, eecrWrite, low(at), high(at) } });
            status != DwStatus::Ok) {
            return status;
         }
         if(DwStatus status = setEepromAddress(); status != DwStatus::Ok) {
            return status;
         }
         // in r30, DWDR (the data); out EEDR, r30; out EECR, r28 (EEMPE);
         // out EECR, r29 (EEPE). EEPE must follow EEMPE.
         const std::array<uint8_t, 18> command{
            { context(0x64), 0xD2, high(avrIn(30, device_.dwdr)), low(avrIn(30, device_.dwdr)), 0x23, data[i], 0xD2,
              high(avrOut(device_.eedr, 30)), low(avrOut(device_.eedr, 30)), 0x23, 0xD2, high(avrOut(device_.eecr, 28)),
              low(avrOut(device_.eecr, 28)), 0x23, 0xD2, high(avrOut(device_.eecr, 29)), low(avrOut(device_.eecr, 29)), 0x23 }
         };
         if(DwStatus status = send(command); status != DwStatus::Ok) {
            return status;
         }
         if(DwStatus status = waitIoClear(device_.eecr, eecrWrite); status != DwStatus::Ok) {
            return status;
         }
      }
      return DwStatus::Ok;
   }

   private:
   static constexpr uint8_t high(uint16_t value) {
      return static_cast<uint8_t>(value >> 8);
   }
   static constexpr uint8_t low(uint16_t value) {
      return static_cast<uint8_t>(value);
   }
   static constexpr bool inRange(uint32_t address, uint32_t length, uint32_t size) {
      return address <= size && length <= size - address;
   }

   [[nodiscard]] uint8_t context(uint8_t value) const {
      return static_cast<uint8_t>(value & mask_);
   }

   [[nodiscard]] bool isPage(uint32_t address) const {
      return device_.flashPageBytes != 0 && address % device_.flashPageBytes == 0 &&
             inRange(address, device_.flashPageBytes, device_.flashBytes) && address < 0x10000u;
   }

   template <std::size_t N>
   [[nodiscard]] DwStatus send(const std::array<uint8_t, N> &bytes) {
      return engine_.send(bytes.data(), static_cast<uint32_t>(N));
   }
   [[nodiscard]] DwStatus sendByte(uint8_t value) {
      return engine_.send(&value, 1);
   }

   // Sends a command and receives the answer. Old bytes are dropped
   // first.
   template <std::size_t N>
   [[nodiscard]] DwStatus query(const std::array<uint8_t, N> &command, uint8_t *out, uint32_t length) {
      engine_.flush();
      if(DwStatus status = send(command); status != DwStatus::Ok) {
         return status;
      }
      return engine_.receive(out, length);
   }
   [[nodiscard]] DwStatus queryByte(uint8_t command, uint8_t &out) {
      return query(std::array<uint8_t, 1>{ { command } }, &out, 1);
   }

   // A repeated operation from index first to end (0xD0, 0xD1, 0xC2, 0x20)
   [[nodiscard]] std::array<uint8_t, 10> repeat(uint8_t mode, uint16_t first, uint16_t end) const {
      return { { context(0x66), 0xD0, high(first), low(first), 0xD1, high(end), low(end), 0xC2, mode, 0x20 } };
   }

   template <std::size_t N>
   [[nodiscard]] DwStatus writeRegs(uint8_t first, const std::array<uint8_t, N> &values) {
      if(DwStatus status = send(repeat(0x05, first, static_cast<uint16_t>(first + N))); status != DwStatus::Ok) {
         return status;
      }
      return send(values);
   }

   [[nodiscard]] DwStatus repeatQuery(uint8_t mode, uint16_t first, uint16_t end, uint8_t *out, uint16_t length) {
      return query(repeat(mode, first, end), out, length);
   }

   [[nodiscard]] DwStatus setZ(uint16_t address) {
      return writeRegs(30, std::array<uint8_t, 2>{ { low(address), high(address) } });
   }

   [[nodiscard]] DwStatus readIo(uint8_t io, uint8_t &value) {
      // in r0, io; out DWDR, r0
      const std::array<uint8_t, 8> command{ { context(0x64), 0xD2, high(avrIn(0, io)), low(avrIn(0, io)), 0x23, 0xD2,
                                              high(avrOut(device_.dwdr, 0)), low(avrOut(device_.dwdr, 0)) } };
      engine_.flush();
      if(DwStatus status = send(command); status != DwStatus::Ok) {
         return status;
      }
      return queryByte(0x23, value);
   }

   [[nodiscard]] DwStatus writeIo(uint8_t io, uint8_t value) {
      // in r0, DWDR (value); out io, r0
      const std::array<uint8_t, 10> command{ { context(0x64), 0xD2, high(avrIn(0, device_.dwdr)), low(avrIn(0, device_.dwdr)),
                                               0x23, value, 0xD2, high(avrOut(io, 0)), low(avrOut(io, 0)), 0x23 } };
      return send(command);
   }

   // Waits until the bits of the I/O register are 0.
   [[nodiscard]] DwStatus waitIoClear(uint8_t io, uint8_t bits) {
      for(uint32_t waited = 0;; waited += dwBusyPollUs) {
         uint8_t value = 0;
         if(DwStatus status = readIo(io, value); status != DwStatus::Ok) {
            return status;
         }
         if((value & bits) == 0) {
            return DwStatus::Ok;
         }
         if(waited >= dwBusyTimeoutUs) {
            return DwStatus::Timeout;
         }
         engine_.waitUs(dwBusyPollUs);
      }
   }

   // out EEARH, r31 (if the device has it); out EEARL, r30
   [[nodiscard]] DwStatus setEepromAddress() {
      if(device_.eearh != device_.eearl) {
         const std::array<uint8_t, 5> command{ { context(0x64), 0xD2, high(avrOut(device_.eearh, 31)),
                                                 low(avrOut(device_.eearh, 31)), 0x23 } };
         if(DwStatus status = send(command); status != DwStatus::Ok) {
            return status;
         }
      }
      const std::array<uint8_t, 5> command{ { context(0x64), 0xD2, high(avrOut(device_.eearl, 30)),
                                              low(avrOut(device_.eearl, 30)), 0x23 } };
      return send(command);
   }

   // megaAVR: SPM executes only in the boot section.
   [[nodiscard]] DwStatus setSpmPc() {
      if(device_.bootStartWords == 0) {
         return DwStatus::Ok;
      }
      return send(std::array<uint8_t, 3>{ { 0xD0, high(device_.bootStartWords), low(device_.bootStartWords) } });
   }

   // out SPMCSR, r29; spm. The SPMCSR value is in r29, the address in Z.
   [[nodiscard]] std::array<uint8_t, 9> spmCommand(uint8_t execute) const {
      return { { context(0x64), 0xD2, high(avrOut(device_.spmcsr, 29)), low(avrOut(device_.spmcsr, 29)), 0x23, 0xD2,
                 high(avrSpm), low(avrSpm), execute } };
   }

   // Page erase or page write. 0x33 makes the target answer with a break
   // and 0x55 at the end. A missing answer gets new attempts.
   [[nodiscard]] DwStatus spmPage(uint8_t operation, uint16_t page) {
      DwStatus status = DwStatus::Timeout;
      for(uint32_t attempt = 0; attempt < dwSpmAttempts; ++attempt) {
         engine_.flush();
         status = writeRegs(29, std::array<uint8_t, 3>{ { operation, low(page), high(page) } });
         if(status == DwStatus::Ok) {
            status = setSpmPc();
         }
         if(status == DwStatus::Ok) {
            status = send(spmCommand(0x33));
         }
         if(status == DwStatus::Ok) {
            status = engine_.waitBreak(dwSpmTimeoutUs);
         }
         if(status == DwStatus::Ok) {
            break;
         }
      }
      if(status != DwStatus::Ok) {
         return status;
      }
      // megaAVR: the CPU runs during the page write. SPMEN and the
      // operation bits clear at the end.
      return device_.bootStartWords == 0 ? DwStatus::Ok : waitIoClear(device_.spmcsr, 0x1F);
   }

   // Loads the page buffer one word at a time: r1:r0 the word, Z the
   // address, then spm and Z += 2.
   [[nodiscard]] DwStatus fillPageBuffer(uint16_t page, const uint8_t *data) {
      if(DwStatus status = writeRegs(29, std::array<uint8_t, 3>{ { spmFill, low(page), high(page) } }); status != DwStatus::Ok) {
         return status;
      }
      const auto spm = spmCommand(0x23);
      for(uint16_t i = 0; i < device_.flashPageBytes; i += 2) {
         if(DwStatus status = writeRegs(0, std::array<uint8_t, 2>{ { data[i], data[i + 1] } }); status != DwStatus::Ok) {
            return status;
         }
         if(DwStatus status = setSpmPc(); status != DwStatus::Ok) {
            return status;
         }
         if(DwStatus status = send(spm); status != DwStatus::Ok) {
            return status;
         }
         const std::array<uint8_t, 4> adiw{ { 0xD2, high(avrAdiwZ2), low(avrAdiwZ2), 0x23 } };
         if(DwStatus status = send(adiw); status != DwStatus::Ok) {
            return status;
         }
      }
      return DwStatus::Ok;
   }

   // megaAVR: the program can read the RWW section again.
   [[nodiscard]] DwStatus enableRww() {
      if(device_.bootStartWords == 0) {
         return DwStatus::Ok;
      }
      if(DwStatus status = writeRegs(29, std::array<uint8_t, 1>{ { spmRwwEnable } }); status != DwStatus::Ok) {
         return status;
      }
      if(DwStatus status = setSpmPc(); status != DwStatus::Ok) {
         return status;
      }
      return send(spmCommand(0x23));
   }

   // The data space split into the saved context (registers, SREG) and
   // runs of target memory.
   [[nodiscard]] DwStatus accessData(uint16_t address, uint8_t *buffer, uint16_t length, bool write) {
      if(!stopped_) {
         return DwStatus::TargetRunning;
      }
      if(!inRange(address, length, 0x10000u)) {
         return DwStatus::InvalidArgument;
      }
      const uint32_t dwdrData = avrIoData + device_.dwdr;
      const uint32_t sregData = avrIoData + avrIoSreg;
      if(address <= dwdrData && dwdrData < uint32_t{ address } + length) {
         return DwStatus::InvalidArgument;
      }
      uint32_t at = address;
      const uint32_t end = uint32_t{ address } + length;
      while(at < end) {
         uint8_t *cached = at < 0x20 ? &regs_[at] : (at == sregData ? &sreg_ : nullptr);
         if(cached != nullptr) {
            uint8_t &value = buffer[at - address];
            if(write) {
               *cached = value;
            } else {
               value = *cached;
            }
            ++at;
            continue;
         }
         uint32_t runEnd = at < sregData && end > sregData ? sregData : end;
         runEnd = runEnd - at > dwChunkBytes ? at + dwChunkBytes : runEnd;
         const auto count = static_cast<uint16_t>(runEnd - at);
         if(DwStatus status = memoryRun(static_cast<uint16_t>(at), buffer + (at - address), count, write); status != DwStatus::Ok) {
            return status;
         }
         at = runEnd;
      }
      return DwStatus::Ok;
   }

   // ld Z+ (mode 0x00, indexes 0 to 2 * count) or st Z+ (mode 0x04,
   // indexes 1 to 1 + 2 * count)
   [[nodiscard]] DwStatus memoryRun(uint16_t address, uint8_t *buffer, uint16_t count, bool write) {
      if(DwStatus status = setZ(address); status != DwStatus::Ok) {
         return status;
      }
      if(!write) {
         return repeatQuery(0x00, 0, static_cast<uint16_t>(2u * count), buffer, count);
      }
      if(DwStatus status = send(repeat(0x04, 1, static_cast<uint16_t>(1u + 2u * count))); status != DwStatus::Ok) {
         return status;
      }
      return engine_.send(buffer, count);
   }

   // Reads the context after a stop. The PC first: the instructions of
   // the other reads change the PC register.
   [[nodiscard]] DwStatus stopped() {
      stopped_ = true;
      std::array<uint8_t, 2> pc{};
      if(DwStatus status = query(std::array<uint8_t, 1>{ { 0xF0 } }, pc.data(), pc.size()); status != DwStatus::Ok) {
         return status;
      }
      pc_ = static_cast<uint16_t>(((pc[0] << 8) | pc[1]) - 1u);
      if(DwStatus status = repeatQuery(0x01, 0, 32, regs_.data(), 32); status != DwStatus::Ok) {
         return status;
      }
      return readIo(avrIoSreg, sreg_);
   }

   // Writes the context back: SREG first (it uses r0), then the
   // registers. go() and step() set the PC register last.
   [[nodiscard]] DwStatus restore() {
      if(!stopped_) {
         return DwStatus::TargetRunning;
      }
      engine_.flush();
      if(DwStatus status = writeIo(avrIoSreg, sreg_); status != DwStatus::Ok) {
         return status;
      }
      return writeRegs(0, regs_);
   }

   Engine &engine_;
   DwDevice device_;
   uint8_t mask_{ 0xFF };
   bool attached_{ false };
   bool stopped_{ false };
   uint16_t pc_{ 0 };
   uint8_t sreg_{ 0 };
   std::array<uint8_t, 32> regs_{};
};

} // namespace avr
