// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The flash operations of the WCH-Link processor (WchLinkProcessor.h), on
// the flash controller (FPEC) of the CH32V00x and the CH32V20x. The two
// controllers have the same registers for these operations (CH32V003 and
// CH32FV2x_V3x reference manuals, chapter "Flash Memory and User Option
// Bytes"). docs/wch-link.md, "Flash", has the sequences.
//
// Two kinds of operations:
//
// - Own algorithm, for the commands without a host stub: the mass erase
//   (02 01) and the read protection (06 02, 06 03). The module writes the
//   FPEC registers with DMI memory accesses.
// - Host stub, for the program sequence (02 05, 02 07, 02 02, 02 08): the
//   host sends a stub of 512 bytes. The module writes it to the target RAM
//   and calls it like a function. The calling convention comes from a
//   disassembly of the captured stubs of wlink and minichlink:
//     a0: operation bits (stubUnlock, stubMassErase, stubErasePages,
//         stubProgram, stubVerify), a1: flash address, a2: length.
//     The stub reads the data from a RAM buffer at a fixed address (one
//     block), and the verify reads a checksum after the buffer.
//     It ends with ebreak, with the result in a0 (0: success).
//
// Each operation starts with a reset that halts the hart (resetHalt()):
// the flash controller is locked, the clock is the reset clock, and no
// code of the target runs. The WCH-LinkE sets a clock of its own at the
// connect of a CH32V20x. The reset makes that clock setup unnecessary: the
// reference manual asks for 120 MHz or less for flash operations, and the
// reset clock is the HSI.
//
// The module keeps dpc, dcsr and mstatus of the halted hart after the
// reset, and writes them back at the end of the program sequence. A quit
// then resumes the hart at the reset vector.

#include <cstddef>
#include <cstdint>
#include <utils/compat/span.h>

#include "protocol/wchlink/WchLinkTarget.h"

namespace wchlink {

class Platform;

namespace fpec {
inline constexpr uint32_t keyr = 0x4002'2004;
inline constexpr uint32_t obkeyr = 0x4002'2008;
inline constexpr uint32_t statr = 0x4002'200c;
inline constexpr uint32_t ctlr = 0x4002'2010;
inline constexpr uint32_t addr = 0x4002'2014;
inline constexpr uint32_t obr = 0x4002'201c;
inline constexpr uint32_t wpr = 0x4002'2020;
inline constexpr uint32_t modekeyr = 0x4002'2024;
inline constexpr uint32_t key1 = 0x4567'0123;
inline constexpr uint32_t key2 = 0xcdef'89ab;

// statr
inline constexpr uint32_t bsy = 1u << 0;
inline constexpr uint32_t wrprterr = 1u << 4;
inline constexpr uint32_t eop = 1u << 5;

// ctlr
inline constexpr uint32_t mer = 1u << 2;
inline constexpr uint32_t obpg = 1u << 4;
inline constexpr uint32_t ober = 1u << 5;
inline constexpr uint32_t strt = 1u << 6;
inline constexpr uint32_t lock = 1u << 7;
inline constexpr uint32_t obwre = 1u << 9;
inline constexpr uint32_t flock = 1u << 15;

// The user option bytes: half words with the value in the low byte and its
// inverse in the high byte. RDPR, USER, DATA0, DATA1, WRPR0-WRPR3. The
// CH32V003 has WRPR0 and WRPR1 only.
inline constexpr uint32_t optionBytes = 0x1fff'f800;
inline constexpr uint8_t rdprUnprotected = 0xa5;
// Any other value protects. Not 0xcc: that is the permanent level on
// other chips.
inline constexpr uint8_t rdprProtected = 0x00;
inline constexpr std::size_t optionRdpr = 0;
inline constexpr std::size_t optionWrpr0 = 4;
inline constexpr std::size_t optionBytesV00x = 6;
inline constexpr std::size_t optionBytesV20x = 8;
} // namespace fpec

// The chip series of the flash operations
enum class Series : uint8_t {
   // CH32V003, CH641: 6 option half words, programmed with OBPG
   V00x,
   // CH32V002/4/5/6/7, CH32M007: 8 option half words, programmed with OBPG half words, each with STRT and its inverse
   V006,
   // CH32V20x, CH32V30x, CH32X035, CH32L103, CH643, CH645: 8 option half
   // words, programmed with OBPG
   V20x,
};

constexpr std::size_t optionBytesOf(Series series) {
   return series == Series::V00x ? fpec::optionBytesV00x : fpec::optionBytesV20x;
}

// The option bytes (RDPR first) from FLASH_OBR and FLASH_WPR. They hold
// the loaded values also on a protected chip, where the option byte area
// may not be readable.
// - CH32V003 and CH32V002/4/5/6/7: USER is OBR bits 9-2, DATA0 bits 17-10,
//   DATA1 bits 25-18 (CH32V003 RM 16.3.7, CH32V00X RM 18.3.7).
// - CH32V20x: USER bits 2-0 are OBR bits 4-2, USER bits 7-5
//   (SRAM_CODE_MODE) are OBR bits 10-8, USER bits 4-3 are reserved (1).
//   The manual does not agree with itself on the place of SRAM_CODE_MODE
//   (32.4.6 and 32.6). OBR has no DATA bytes: they get 0xff.
// - WRPR0-WRPR3 are the bytes of WPR, low byte first.
// RDPR is not set.
void optionsFromRegisters(Series series, uint32_t obr, uint32_t wpr, uint8_t (&values)[fpec::optionBytesV20x]);

// The operation bits of the stub (register a0)
inline constexpr uint32_t stubUnlock = 1u << 0;
inline constexpr uint32_t stubMassErase = 1u << 1;
inline constexpr uint32_t stubErasePages = 1u << 2;
inline constexpr uint32_t stubProgram = 1u << 3;
inline constexpr uint32_t stubVerify = 1u << 4;

inline constexpr uint32_t stubSize = 512;

// The RAM layout of a chip family for the stub. The stub has the buffer
// and the checksum address in its code. The values come from the captured
// stubs.
struct StubLayout {
   uint32_t stubAddress;
   uint32_t bufferAddress;
   // The bytes of one data block (one acknowledge)
   uint32_t blockSize;
   // The fast erase and program page
   uint32_t pageSize;
   // Under the end of the smallest RAM of the family
   uint32_t stackTop;

   [[nodiscard]] constexpr uint32_t checksumAddress() const {
      return bufferAddress + blockSize + 0x10;
   }
};

// CH32V003, CH641: 2 KB RAM, 64-byte pages. The buffer follows the stub.
inline constexpr StubLayout layoutV00x{ 0x2000'0000, 0x2000'0200, 1024, 64, 0x2000'0800 };
// CH32V002/4/5/6/7, CH32M007: 4 KB RAM or more, 256-byte pages. Checked on
// a CH32V006: the wlink stub has the buffer at 0x20000200, 1024-byte blocks
// and 256-byte pages.
inline constexpr StubLayout layoutV006{ 0x2000'0000, 0x2000'0200, 1024, 256, 0x2000'0800 };
// CH32V20x, CH32V30x, CH32X035, CH32L103, CH643, CH645: 10 KB RAM or more
inline constexpr StubLayout layoutV20x{ 0x2000'0000, 0x2000'1000, 4096, 256, 0x2000'2800 };

class Flash {
   public:
   // The polls of a flash operation: one read for each pollIntervalUs, up
   // to operationTimeoutUs. A mass erase of the CH32V208 took 138 ms on
   // the WCH-LinkE, a block of 4 KB about 350 ms.
   static constexpr uint32_t pollIntervalUs = 1'000;
   static constexpr uint32_t operationTimeoutUs = 3'000'000;
   // The time that resetHalt() holds ndmreset
   static constexpr uint32_t resetPulseUs = 1'000;
   // The time that resetHalt() waits for the halt. The CH32V307 halts
   // about 9 ms after the reset, longer than one poll of halt().
   static constexpr uint32_t resetHaltTimeoutUs = 100'000;

   Flash(Target &target, Platform &platform);

   // ndmreset with haltreq: the hart halts at the reset vector. Then
   // havereset is acknowledged and CSR 0x7c0 gets the value of the
   // connect again.
   [[nodiscard]] bool resetHalt();

   // Erases the main flash (MER) with the own algorithm.
   [[nodiscard]] bool massErase();
   // Writes the option bytes with the RDPR value: unprotected (0xa5) or
   // protected. USER and DATA keep their values: from the option byte
   // area where a byte has its inverse, else from FLASH_OBR and FLASH_WPR
   // (optionsFromRegisters()). A failed read of the area is no error: a
   // protected chip can hide it. Unprotect also clears the write
   // protection (WRPR 0xff), and the chip erases the main flash.
   // Unprotect ends with a reset, so that FLASH_OBR has the new state, as
   // on the WCH-LinkE. Protect takes effect at the next reset.
   [[nodiscard]] bool setReadProtection(bool protect, Series series);

   // The program sequence with the host stub. beginStub() resets the
   // target and keeps the hart state. stubData() writes the next bytes of
   // the stub. The stub is complete after stubSize bytes.
   [[nodiscard]] bool beginStub(const StubLayout &layout);
   [[nodiscard]] bool stubData(art::span<const uint8_t> data);
   [[nodiscard]] bool stubComplete() const {
      return stubReceived_ >= stubSize;
   }
   // Runs the stub to unlock the flash controller, once for each
   // beginStub(). Needs the complete stub.
   [[nodiscard]] bool unlock();

   // The data phase: the area of command 0x01. Needs unlock(). The
   // address must be a multiple of the page size.
   [[nodiscard]] bool beginData(uint32_t address, uint32_t length);
   // Takes the next bytes of the image. A block is programmed when it is
   // full, or at the end of the area. blockDone() is then true until the
   // next call. Bytes after the end of the area are ignored.
   [[nodiscard]] bool data(art::span<const uint8_t> bytes);
   [[nodiscard]] bool blockDone() const {
      return blockDone_;
   }
   [[nodiscard]] bool dataComplete() const {
      return dataOffset_ >= dataLength_;
   }
   // Locks the flash controller and writes the hart state back.
   [[nodiscard]] bool end();
   // The program sequence is between beginStub() and end().
   [[nodiscard]] bool active() const {
      return active_;
   }
   // Forgets the program sequence without target accesses.
   void drop() {
      active_ = false;
   }

   private:
   [[nodiscard]] bool read(uint32_t address, uint32_t &value);
   [[nodiscard]] bool write(uint32_t address, uint32_t value);
   [[nodiscard]] bool unlockController();
   [[nodiscard]] bool waitIdle();
   // Polls halt() until the hart halts, up to resetHaltTimeoutUs.
   [[nodiscard]] bool haltAfterReset();
   // Waits until the abstract command ends, after the module gave up on it.
   [[nodiscard]] bool waitCommand();
   [[nodiscard]] bool lockController();
   // Writes the next word of the image to the buffer.
   [[nodiscard]] bool bufferWord(uint32_t word);
   [[nodiscard]] bool flushBlock();
   [[nodiscard]] bool runStub(uint32_t operation, uint32_t address, uint32_t length, uint32_t &result);

   Target &target_;
   Platform &platform_;
   const StubLayout *layout_{ &layoutV00x };

   bool active_{ false };
   bool unlocked_{ false };
   uint32_t savedDpc_{ 0 };
   uint32_t savedDcsr_{ 0 };
   uint32_t savedMstatus_{ 0 };
   uint32_t stubReceived_{ 0 };

   uint32_t dataAddress_{ 0 };
   uint32_t dataLength_{ 0 };
   // Bytes of the area taken by data()
   uint32_t dataOffset_{ 0 };
   // Bytes in the buffer of the current block
   uint32_t blockBytes_{ 0 };
   uint32_t checksum_{ 0 };
   // The bytes of a word that is not complete, little-endian
   uint32_t partWord_{ 0 };
   uint32_t partBytes_{ 0 };
   bool blockDone_{ false };
};

} // namespace wchlink
