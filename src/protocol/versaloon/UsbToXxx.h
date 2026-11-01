// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The blocks of a USB_TO_ALL command (protocol/versaloon/UsbToAll.h), as OpenOCD
// (usbtoxxx.c and usbtoxxx_internal.h) builds them. Hardware-independent.
//
// All values are little-endian. A block is
//
//   [type][length u16][payload]
//
// length is the size of the block, the 3-byte header included. The payload
// of most blocks (GPIO, JTAG_RAW, SWD, POWER) is a list of sub-commands:
//
//   [command | index][length u16][payload]
//
// Here length is the size of the payload only. The command is in the top 5
// bits, the port index in the low 3 bits. The reply of a block is the
// reply of each sub-command in order: [status][data]. The host knows the
// data size of each sub-command and rejects the whole reply when its size
// is wrong. Thus a sub-command writes its full data size also when it fails.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utils/compat/span.h>

namespace protocol {

// Block types (the USB_TO_XXX command codes)
namespace block {
inline constexpr uint8_t gpio = 0x23;
inline constexpr uint8_t swd = 0x46;
inline constexpr uint8_t jtagRaw = 0x47;
inline constexpr uint8_t power = 0x60;
inline constexpr uint8_t delay = 0x61;
inline constexpr uint8_t info = 0x7E;
inline constexpr uint8_t all = 0x7F;
} // namespace block

// Sub-commands (top 5 bits of the first byte)
namespace subcommand {
inline constexpr uint8_t init = 0x00;
inline constexpr uint8_t fini = 0x08;
inline constexpr uint8_t config = 0x10;
inline constexpr uint8_t inOut = 0x28;
inline constexpr uint8_t in = 0x30;
inline constexpr uint8_t out = 0x38;

inline constexpr uint8_t commandMask = 0xF8;
inline constexpr uint8_t indexMask = 0x07;
} // namespace subcommand

// Status byte of a reply
enum class Status : uint8_t {
   Ok = 0,
   Failed = 1,
   Timeout = 2,
   InvalidIndex = 3,
   InvalidPara = 4,
   InvalidCmd = 5,
   CmdNotSupport = 6,
};

inline constexpr std::size_t blockHeaderSize = 3;

// Writes the reply of a USB_TO_ALL command into the reply buffer. A write
// that does not fit sets overflow(). The dispatcher then runs no further
// sub-command or block, and the command gets an empty reply.
class Reply {
   public:
   // The reply of one sub-command: its status byte and its data bytes.
   class Slot {
      public:
      Slot(uint8_t &status, art::span<uint8_t> data)
         : status_(&status)
         , data_(data) {
      }

      void setStatus(Status status) {
         *status_ = static_cast<uint8_t>(status);
      }

      // Exactly the data size that reserve() got.
      [[nodiscard]] art::span<uint8_t> data() const {
         return data_;
      }

      private:
      uint8_t *status_;
      art::span<uint8_t> data_;
   };

   explicit Reply(art::span<uint8_t> buffer)
      : buffer_(buffer) {
   }

   // Reserves the status (set to OK) and dataSize data bytes (set to 0).
   // Call it before the operation of the sub-command. No value when the
   // reply does not fit (overflow() is then set): do not run the operation.
   std::optional<Slot> reserve(std::size_t dataSize = 0);

   // Adds a status without data, for a sub-command that does not run. Same
   // as reserve(0) and setStatus().
   void add(Status status);

   [[nodiscard]] std::size_t size() const {
      return size_;
   }

   [[nodiscard]] bool overflow() const {
      return overflow_;
   }

   private:
   art::span<uint8_t> buffer_;
   std::size_t size_{ 0 };
   bool overflow_{ false };
};

// A block type that the USB_TO_ALL dispatcher runs.
class Block {
   public:
   [[nodiscard]] uint8_t type() const {
      return type_;
   }

   // True when the framing of the payload is correct. The dispatcher checks
   // all blocks of a command before it runs the first one.
   [[nodiscard]] virtual bool isWellFormed(art::span<const uint8_t> payload) const {
      static_cast<void>(payload);
      return true;
   }

   // Runs the payload of one block (isWellFormed() is true) and adds its
   // reply. Stops at the first overflow of reply.
   virtual void run(art::span<const uint8_t> payload, Reply &reply) = 0;

   // The host is gone (UsbToXxxHandler::hostGone()). A block releases the
   // target lines that keep the target in a state, for example reset.
   virtual void hostGone() {
   }

   protected:
   explicit Block(uint8_t type)
      : type_(type) {
   }
   ~Block() = default;

   private:
   uint8_t type_;
};

// The target voltage (Vref) that GET_TVCC replies.
class TargetVoltage {
   public:
   // The target voltage in mV, or 0 when it is not known (no sense, or a
   // failed measurement).
   [[nodiscard]] virtual uint16_t millivolts() = 0;

   protected:
   ~TargetVoltage() = default;
};

struct SubCommand {
   // Top 5 bits, for example subcommand::config
   uint8_t command;
   // Port index, 0-7
   uint8_t index;
   art::span<const uint8_t> payload;
};

// A block whose payload is a list of sub-commands.
class SubCommandBlock : public Block {
   public:
   [[nodiscard]] bool isWellFormed(art::span<const uint8_t> payload) const final;
   void run(art::span<const uint8_t> payload, Reply &reply) final;

   protected:
   explicit SubCommandBlock(uint8_t type)
      : Block(type) {
   }
   ~SubCommandBlock() = default;

   // Runs one sub-command and adds its status and data to reply. It calls
   // reply.reserve() before it changes the hardware, and does nothing when
   // reserve() has no value.
   virtual void runSubCommand(const SubCommand &command, Reply &reply) = 0;
};

// Reads a little-endian u16 or u32 at data[offset]. The caller checks the
// size.
constexpr uint16_t getU16(art::span<const uint8_t> data, std::size_t offset) {
   return static_cast<uint16_t>(data[offset] | data[offset + 1] << 8);
}

constexpr uint32_t getU32(art::span<const uint8_t> data, std::size_t offset) {
   return static_cast<uint32_t>(data[offset]) | static_cast<uint32_t>(data[offset + 1]) << 8 |
          static_cast<uint32_t>(data[offset + 2]) << 16 | static_cast<uint32_t>(data[offset + 3]) << 24;
}

} // namespace protocol
