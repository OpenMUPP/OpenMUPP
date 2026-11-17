// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// JTAG programming of the classic AVR chips (datasheet, "Programming via
// the JTAG Interface", for example ATmega16U4/32U4 chapter 28.9): the
// programming mode, chip erase, flash, EEPROM, fuses, lock bits, signature
// and calibration byte, on the TAP layer (avr/AvrJtagTap.h).
// Hardware-independent: the host unit tests run it on mock pins with a
// model of the programming interface.
//
// The commands are the 15-bit PROG_COMMANDS values of the datasheet table
// "JTAG Programming Instruction Set" (bits 14..8 select the operation,
// bits 7..0 are the address or data byte). Each scan shifts out the result
// of the command before it: a read sends its command, and the next scan
// returns the byte in bits 7..0.
//
// - enter(): AVR_RESET 1, then PROG_ENABLE with the signature 0xA370. The
//   core stays in reset until leave(). There is no answer: the host reads
//   the signature to know that the mode works.
// - leave(): the no-operation command, PROG_ENABLE 0, AVR_RESET 0.
// - Writes (chip erase, flash and EEPROM page, fuses, lock bits) poll the
//   ready bit (bit 9) with the last command of the write, as the table
//   does. For the extended fuse, the ATmega32U4 and ATmega2560 tables give
//   0x3700 for the poll, the ATmega16 pattern gives 0x3B00: the poll does
//   not strobe, so the probe uses 0x3B00, the same as the write. A poll
//   that does not see ready within writeTimeoutMs fails. Each poll is one
//   scan, so the JTAG engine services the Background (protocol/Background.h)
//   during a long write.
// - Flash pages: byte addresses, one page for each write, the page buffer
//   is loaded word by word with PROG_COMMANDS. When the device allows it
//   (the ATmega16 and ATmega32: allowfullpagebitstream of avrdude, offset
//   0x15 of the EDBG device context), the page goes in one PROG_PAGELOAD
//   scan of the full page. The extended address byte is sent only for a
//   flash of more than 128 KiB: the ATmega16 has no such command.
// - A flash or EEPROM write needs a chip erase first (datasheet). The
//   caller does it.
// - Flash and EEPROM reads use PROG_COMMANDS. Any byte address and length.
//
// Each operation needs the programming mode (enter()), else it gives false
// without a scan. A failed scan gives false.

#include <cstddef>
#include <cstdint>
#include <utils/compat/span.h>

#include "avr/AvrJtagTap.h"

namespace avr {

// The device parameters for JTAG programming, from the host (device
// context of the EDBG AVR8 scope)
struct JtagProgDevice {
   uint16_t flashPageBytes{ 128 };
   uint32_t flashBytes{ 32768 };
   uint8_t eepromPageBytes{ 4 };
   // The full page goes in one PROG_PAGELOAD scan.
   bool fullPageBitstream{ false };
};

// The fuse bytes, as the EDBG FUSES memory addresses
enum class Fuse : uint8_t { Low = 0, High = 1, Extended = 2 };

namespace jtagProg {

inline constexpr uint16_t enableSignature = 0xA370;
inline constexpr uint8_t commandBits = 15;
inline constexpr uint8_t enableBits = 16;
// The ready bit of the poll commands (bit 9 of the result)
inline constexpr uint16_t ready = 1u << 9;
// Longer than tWLRH_CE (9 ms) and tWLRH (4.5 ms) of the datasheets
inline constexpr uint32_t writeTimeoutMs = 100;
// TCK cycles of one PROG_COMMANDS scan (AvrJtagTap: 3 + 15 + 2)
inline constexpr uint32_t commandScanCycles = 3 + commandBits + 2;

// The PROG_COMMANDS values (datasheet table, instruction number)
namespace cmd {
// 1a, 1b chip erase
inline constexpr uint16_t chipErase1 = 0x2380;
inline constexpr uint16_t chipErase2 = 0x3180;
inline constexpr uint16_t chipErase3 = 0x3380;
// 2a, 3a, 4a, 5a, 6a, 7a, 8a, 9a, 10a, 11a: the operation
inline constexpr uint16_t enterFlashWrite = 0x2310;
inline constexpr uint16_t enterFlashRead = 0x2302;
inline constexpr uint16_t enterEepromWrite = 0x2311;
inline constexpr uint16_t enterEepromRead = 0x2303;
inline constexpr uint16_t enterFuseWrite = 0x2340;
inline constexpr uint16_t enterLockWrite = 0x2320;
inline constexpr uint16_t enterFuseLockRead = 0x2304;
inline constexpr uint16_t enterSignatureRead = 0x2308;
inline constexpr uint16_t noOperation = 0x2300;
// Address and data bytes: the byte goes into bits 7..0.
inline constexpr uint16_t loadAddressExtended = 0x0B00;
inline constexpr uint16_t loadAddressHigh = 0x0700;
inline constexpr uint16_t loadAddressLow = 0x0300;
inline constexpr uint16_t loadDataLow = 0x1300;
inline constexpr uint16_t loadDataHigh = 0x1700;
// 2g, 4e latch data
inline constexpr uint16_t latch = 0x7700;
// The idle and the strobe value of each write: x3, x1 (low fuse, EEPROM,
// lock bits), x7, x5 (flash, high fuse), xB, x9 (extended fuse)
inline constexpr uint16_t idle3 = 0x3300;
inline constexpr uint16_t strobe3 = 0x3100;
inline constexpr uint16_t idle7 = 0x3700;
inline constexpr uint16_t strobe7 = 0x3500;
inline constexpr uint16_t idleB = 0x3B00;
inline constexpr uint16_t strobeB = 0x3900;
// The reads: the first command selects the byte, the next scan returns it.
inline constexpr uint16_t readLow = 0x3200;  // flash low byte, EEPROM, low fuse, signature
inline constexpr uint16_t readHigh = 0x3600; // flash high byte, lock bits, calibration byte
inline constexpr uint16_t readFuseHigh = 0x3E00;
inline constexpr uint16_t idleF = 0x3F00;
inline constexpr uint16_t readFuseExtended = 0x3A00;
} // namespace cmd

} // namespace jtagProg

class AvrJtagProg {
   public:
   explicit AvrJtagProg(AvrJtagTap &tap)
      : tap_(tap) {
   }

   [[nodiscard]] bool programming() const {
      return programming_;
   }

   [[nodiscard]] bool enter() {
      uint64_t reset = 1;
      uint64_t enable = jtagProg::enableSignature;
      programming_ = tap_.scanDr(tap::ir::avrReset, reset, 1) && tap_.scanDr(tap::ir::progEnable, enable, jtagProg::enableBits);
      return programming_;
   }

   // Ends the programming mode and releases the core from reset. The mode
   // ends also when a scan fails.
   bool leave() {
      programming_ = false;
      uint64_t disable = 0;
      uint64_t reset = 0;
      return command(jtagProg::cmd::noOperation) && command(jtagProg::cmd::idle3) &&
             tap_.scanDr(tap::ir::progEnable, disable, jtagProg::enableBits) && tap_.scanDr(tap::ir::avrReset, reset, 1);
   }

   // Erases the flash, the lock bits and the EEPROM (unless EESAVE).
   [[nodiscard]] bool chipErase() {
      using namespace jtagProg::cmd;
      return programming_ && command(chipErase1) && command(chipErase2) && command(chipErase3) && command(chipErase3) &&
             poll(chipErase3);
   }

   // Writes one flash page. byteAddress is the start of a page, and page
   // has flashPageBytes bytes.
   [[nodiscard]] bool writeFlashPage(const JtagProgDevice &device, uint32_t byteAddress, art::span<const uint8_t> page) {
      using namespace jtagProg::cmd;
      if(!programming_ || device.flashPageBytes == 0 || device.flashPageBytes % 2 != 0 || page.size() != device.flashPageBytes ||
         byteAddress % device.flashPageBytes != 0 || !inFlash(device, byteAddress, page.size())) {
         return false;
      }
      const uint32_t word = byteAddress / 2;
      if(!command(enterFlashWrite) || !loadAddress(device, word)) {
         return false;
      }
      if(device.fullPageBitstream) {
         if(!command(loadAddressLow | (word & 0xFFu)) || !tap_.setIr(tap::ir::progPageLoad) || !tap_.shiftDr(page)) {
            return false;
         }
      } else {
         for(std::size_t i = 0; i < page.size(); i += 2) {
            const uint32_t at = word + static_cast<uint32_t>(i / 2);
            // A page of more than 256 words gets a new high byte.
            if(i != 0 && (at & 0xFFu) == 0 && !loadAddress(device, at)) {
               return false;
            }
            if(!command(loadAddressLow | (at & 0xFFu)) || !command(loadDataLow | page[i]) ||
               !command(loadDataHigh | page[i + 1]) || !latchData()) {
               return false;
            }
         }
      }
      return strobe(idle7, strobe7);
   }

   // Reads the flash from byteAddress into data.
   [[nodiscard]] bool readFlash(const JtagProgDevice &device, uint32_t byteAddress, art::span<uint8_t> data) {
      using namespace jtagProg::cmd;
      if(!programming_ || !inFlash(device, byteAddress, data.size()) || !command(enterFlashRead)) {
         return false;
      }
      uint32_t loaded = noAddress;
      for(std::size_t i = 0; i < data.size();) {
         const uint32_t byte = byteAddress + i;
         const uint32_t word = byte / 2;
         if((word >> 8) != loaded && !loadAddress(device, word)) {
            return false;
         }
         loaded = word >> 8;
         uint8_t low = 0;
         uint8_t high = 0;
         if(!command(loadAddressLow | (word & 0xFFu)) || !command(readLow) || !command(readHigh, &low) || !command(idle7, &high)) {
            return false;
         }
         if(byte % 2 == 0) {
            data[i++] = low;
         }
         if(i < data.size()) {
            data[i++] = high;
         }
      }
      return true;
   }

   // Writes data into the EEPROM, one page write for each page that it
   // touches. Bytes of a page outside data are not loaded.
   [[nodiscard]] bool writeEeprom(const JtagProgDevice &device, uint16_t address, art::span<const uint8_t> data) {
      using namespace jtagProg::cmd;
      if(!programming_ || device.eepromPageBytes == 0 || !inEeprom(address, data.size()) || !command(enterEepromWrite)) {
         return false;
      }
      for(std::size_t i = 0; i < data.size(); ++i) {
         const auto at = static_cast<uint16_t>(address + i);
         if(!command(loadAddressHigh | (at >> 8u)) || !command(loadAddressLow | (at & 0xFFu)) ||
            !command(loadDataLow | data[i]) || !latchData()) {
            return false;
         }
         const bool pageEnd = (at + 1u) % device.eepromPageBytes == 0;
         if((pageEnd || i + 1 == data.size()) && !strobe(idle3, strobe3)) {
            return false;
         }
      }
      return true;
   }

   [[nodiscard]] bool readEeprom(uint16_t address, art::span<uint8_t> data) {
      using namespace jtagProg::cmd;
      if(!programming_ || !inEeprom(address, data.size()) || !command(enterEepromRead)) {
         return false;
      }
      for(std::size_t i = 0; i < data.size(); ++i) {
         const auto at = static_cast<uint16_t>(address + i);
         const uint16_t low = at & 0xFFu;
         if(!command(loadAddressHigh | (at >> 8u)) || !command(loadAddressLow | low) || !command(idle3 | low) ||
            !command(readLow) || !command(idle3, &data[i])) {
            return false;
         }
      }
      return true;
   }

   [[nodiscard]] bool writeFuse(Fuse fuse, uint8_t value) {
      using namespace jtagProg::cmd;
      if(!programming_ || !command(enterFuseWrite) || !command(loadDataLow | value)) {
         return false;
      }
      switch(fuse) {
      case Fuse::Low:
         return strobe(idle3, strobe3);
      case Fuse::High:
         return strobe(idle7, strobe7);
      case Fuse::Extended:
         return strobe(idleB, strobeB);
      }
      return false;
   }

   [[nodiscard]] bool readFuse(Fuse fuse, uint8_t &value) {
      using namespace jtagProg::cmd;
      switch(fuse) {
      case Fuse::Low:
         return read(enterFuseLockRead, readLow, idle3, value);
      case Fuse::High:
         return read(enterFuseLockRead, readFuseHigh, idleF, value);
      case Fuse::Extended:
         return read(enterFuseLockRead, readFuseExtended, idleB, value);
      }
      return false;
   }

   // Bits 7..6 are not lock bits: the command keeps them 1.
   [[nodiscard]] bool writeLockBits(uint8_t value) {
      using namespace jtagProg::cmd;
      return programming_ && command(enterLockWrite) && command(loadDataLow | 0xC0u | value) && strobe(idle3, strobe3);
   }

   [[nodiscard]] bool readLockBits(uint8_t &value) {
      using namespace jtagProg::cmd;
      return read(enterFuseLockRead, readHigh, idle7, value);
   }

   // Signature byte 0, 1 or 2
   [[nodiscard]] bool readSignature(uint8_t index, uint8_t &value) {
      using namespace jtagProg::cmd;
      return programming_ && command(enterSignatureRead) && command(loadAddressLow | index) && command(readLow) &&
             command(idle3, &value);
   }

   // Calibration byte 0, or 0 to 3 on the ATmega16 and ATmega32 (1, 2, 4
   // and 8 MHz)
   [[nodiscard]] bool readCalibration(uint8_t index, uint8_t &value) {
      using namespace jtagProg::cmd;
      return programming_ && command(enterSignatureRead) && command(loadAddressLow | index) && command(readHigh) &&
             command(idle7, &value);
   }

   private:
   static constexpr uint32_t noAddress = 0xFFFFFFFFu;

   // The 16-bit EEPROM address does not wrap. The caller checks the
   // EEPROM size of the device.
   static bool inEeprom(uint16_t address, std::size_t count) {
      return count <= 0x10000u - address;
   }

   // The host gives the addresses: no overflow.
   static bool inFlash(const JtagProgDevice &device, uint32_t byteAddress, std::size_t count) {
      return byteAddress <= device.flashBytes && count <= device.flashBytes - byteAddress;
   }

   // One PROG_COMMANDS scan. result gets bits 7..0 of the result of the
   // command before it.
   bool command(uint32_t value, uint8_t *result = nullptr) {
      uint64_t data = value;
      if(!tap_.scanDr(tap::ir::progCommands, data, jtagProg::commandBits)) {
         return false;
      }
      if(result != nullptr) {
         *result = static_cast<uint8_t>(data);
      }
      last_ = static_cast<uint16_t>(data);
      return true;
   }

   // The extended (more than 128 KiB) and the high address byte of a word
   bool loadAddress(const JtagProgDevice &device, uint32_t word) {
      using namespace jtagProg::cmd;
      return (device.flashBytes <= 0x20000u || command(loadAddressExtended | ((word >> 16) & 0xFFu))) &&
             command(loadAddressHigh | ((word >> 8) & 0xFFu));
   }

   bool latchData() {
      using namespace jtagProg::cmd;
      return command(idle7) && command(latch) && command(idle7);
   }

   // A write: idle, strobe, idle, idle, then poll with idle.
   bool strobe(uint16_t idle, uint16_t strobe) {
      return command(idle) && command(strobe) && command(idle) && command(idle) && poll(idle);
   }

   // Sends value until the result has the ready bit, at most
   // writeTimeoutMs at the TCK frequency of the TAP.
   bool poll(uint16_t value) {
      const uint32_t polls = jtagProg::writeTimeoutMs * tap_.kHz() / jtagProg::commandScanCycles + 1u;
      for(uint32_t i = 0; i < polls; ++i) {
         if(!command(value)) {
            return false;
         }
         if((last_ & jtagProg::ready) != 0) {
            return true;
         }
      }
      return false;
   }

   // A fuse or lock bit read: the operation, the select command, then the
   // next command returns the byte.
   bool read(uint16_t enter, uint16_t select, uint16_t next, uint8_t &value) {
      return programming_ && command(enter) && command(select) && command(next, &value);
   }

   AvrJtagTap &tap_;
   bool programming_{ false };
   // The result of the last scan
   uint16_t last_{ 0 };
};

} // namespace avr
