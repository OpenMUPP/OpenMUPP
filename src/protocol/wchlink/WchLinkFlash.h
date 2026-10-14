#pragma once

// The flash operations of the WCH-Link processor (WchLinkProcessor.h), on
// the flash controller (FPEC) of the CH32V00x and the CH32V20x. The two
// controllers have the same registers for these operations (CH32V003 and
// CH32FV2x_V3x reference manuals, chapter "Flash Memory and User Option
// Bytes"). docs/wch-link-protocol.md, "Flash", has the sequences.
//
// Own algorithm, for the commands without a host stub: the mass erase
// (02 01) and the read protection (06 02, 06 03). The module writes the
// FPEC registers with DMI memory accesses.
//
// Each operation starts with a reset that halts the hart (resetHalt()):
// the flash controller is locked, the clock is the reset clock, and no
// code of the target runs. The WCH-LinkE sets a clock of its own at the
// connect of a CH32V20x. The reset makes that clock setup unnecessary: the
// reference manual asks for 120 MHz or less for flash operations, and the
// reset clock is the HSI.

#include <cstddef>
#include <cstdint>

#include "protocol/wchlink/WchLinkTarget.h"

namespace wchlink {

class Platform;

namespace fpec {
inline constexpr uint32_t keyr = 0x4002'2004;
inline constexpr uint32_t obkeyr = 0x4002'2008;
inline constexpr uint32_t statr = 0x4002'200c;
inline constexpr uint32_t ctlr = 0x4002'2010;
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
// CH32V00x has WRPR0 and WRPR1 only.
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

class Flash {
   public:
   // The polls of a flash operation: one read for each pollIntervalUs, up
   // to operationTimeoutUs. A mass erase of the CH32V208 took 138 ms on
   // the WCH-LinkE.
   static constexpr uint32_t pollIntervalUs = 1'000;
   static constexpr uint32_t operationTimeoutUs = 3'000'000;
   // The time that resetHalt() holds ndmreset
   static constexpr uint32_t resetPulseUs = 1'000;

   Flash(Target &target, Platform &platform);

   // ndmreset with haltreq: the hart halts at the reset vector. Then
   // havereset is acknowledged and CSR 0x7c0 gets the value of the
   // connect again.
   [[nodiscard]] bool resetHalt();

   // Erases the main flash (MER) with the own algorithm.
   [[nodiscard]] bool massErase();
   // Writes the option bytes with the RDPR value: unprotected (0xa5) or
   // protected. USER and DATA keep their values. Unprotect also clears the
   // write protection (WRPR 0xff), and the chip erases the main flash.
   // Unprotect ends with a reset, so that FLASH_OBR has the new state, as
   // on the WCH-LinkE. Protect takes effect at the next reset.
   // optionBytes: the option bytes of the chip family (half words).
   [[nodiscard]] bool setReadProtection(bool protect, std::size_t optionBytes);

   private:
   [[nodiscard]] bool read(uint32_t address, uint32_t &value);
   [[nodiscard]] bool write(uint32_t address, uint32_t value);
   [[nodiscard]] bool unlockController();
   [[nodiscard]] bool waitIdle();
   [[nodiscard]] bool lockController();

   Target &target_;
   Platform &platform_;
};

} // namespace wchlink
