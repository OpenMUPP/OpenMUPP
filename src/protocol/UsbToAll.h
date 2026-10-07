#pragma once

// The USB_TO_XXX dispatcher: runs the blocks of a USB_TO_ALL command
// (0x7F), the format that OpenOCD uses for all adapter operations.
// Hardware-independent: the host unit tests check it.
//
//   [0x7F][total length u16][block][block]...
//
// See protocol/UsbToXxx.h for the blocks. The dispatcher runs the blocks in
// order and joins their replies. A top-level command 0x20-0x7E is one
// block, as in the original firmware.
//
// The dispatcher runs two blocks itself:
//
//   INFO   [0x7E][3 u16]              -> [OK][12-byte capability bitmap]
//   DELAY  [0x61][5 u16][delay u16]   -> [OK]
//
// Bit n of the bitmap is set when the dispatcher runs block 0x20 + n. A
// delay with bit 15 set is in ms (bits 0-14), else in us.
//
// The other blocks are Block objects (GPIO, JTAG_RAW, SWD, POWER), added
// with add(). A block type without a Block gets one CMD_NOT_SUPPORT status
// byte.
//
// The dispatcher checks the framing of all blocks and sub-commands before
// it runs the first one. A malformed command (a length that does not fit)
// runs nothing and gets an empty reply. A reply that does not fit the reply
// buffer stops the command at the first sub-command that does not fit: the
// sub-commands before it have run, and the command gets an empty reply. The
// host makes no command whose reply is larger than the buffer size of
// GET_INFO, so this is a host error.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "protocol/CommandProcessor.h"
#include "protocol/UsbToXxx.h"

namespace protocol {

// Waits during a DELAY block.
class Delay {
   public:
   virtual void microseconds(uint16_t us) = 0;
   virtual void milliseconds(uint16_t ms) = 0;

   protected:
   ~Delay() = default;
};

class UsbToAll final : public UsbToXxxHandler {
   public:
   static constexpr std::size_t maxBlocks = 8;
   // Bytes of the capability bitmap: one bit for each of 0x20-0x7F
   static constexpr std::size_t capabilityBytes = 12;

   explicit UsbToAll(Delay &delay);

   // Adds a block type. Returns false when the table is full, or the type
   // is not 0x20-0x7D or is there already.
   bool add(Block &block);

   std::optional<std::size_t> process(art::span<const uint8_t> command, art::span<uint8_t> reply) override;

   // Calls Block::hostGone() of each block.
   void hostGone() override;

   // The capability bitmap of the INFO reply
   [[nodiscard]] std::array<uint8_t, capabilityBytes> capabilities() const;

   private:
   // Calls visit(type, payload) for each block of command. Returns false
   // when the framing is wrong, or when visit() returns false.
   template <typename Visit>
   static bool forEachBlock(art::span<const uint8_t> command, Visit visit);
   void runBlock(uint8_t type, art::span<const uint8_t> payload, Reply &reply);
   void runDelay(art::span<const uint8_t> payload, Reply &reply);
   [[nodiscard]] Block *find(uint8_t type) const;

   Delay &delay_;
   std::array<Block *, maxBlocks> blocks_{};
   std::size_t blockCount_{ 0 };
};

} // namespace protocol
