#pragma once

// Joins the OUT packets of one bulk transfer into one Versaloon command
// (protocol/versaloon/Versaloon.h). Hardware-independent: the host unit tests check it.
//
// A common command, or an unused command byte, is one packet. A USB_TO_XXX
// command has its total size in the header. The assembler collects packets
// until it has this size. The command is wrong (Error) when:
//
// - the header is incomplete, or its length is smaller than the header,
// - a packet goes past the length,
// - a short packet (smaller than the packet size, a ZLP included) ends the
//   transfer before the length,
// - a common command, or an unused command byte, is in a full packet (the
//   transfer goes on).
//
// The assembler gives Error only at the end of the transfer of the host, so
// that the next transfer starts as a new command again. After an error on a
// short packet (a ZLP included), Error comes at once. After an error on a
// full packet, the assembler drops the packets until a short packet ends
// the transfer, and then gives Error. Note: if that transfer ends with a
// full packet and no ZLP, the next transfer is dropped too.
//
// A command larger than the buffer is an overflow. The assembler then reads
// and drops its packets until the length, and gives Error at the end.
//
// After Complete or Error, the next packet starts a new command. A ZLP
// between two commands is ignored.

#include <cstddef>
#include <cstdint>
#include <utils/compat/span.h>

namespace protocol {

class CommandAssembler {
   public:
   enum class Result : uint8_t {
      // The command needs more packets.
      Incomplete,
      // command() has the command.
      Complete,
      // The command is wrong or too large. It is dropped.
      Error,
      // A ZLP between two commands. Nothing changes.
      Ignored,
   };

   // buffer: receives the command. packetSize: the maximum packet size of
   // the OUT endpoint.
   CommandAssembler(art::span<uint8_t> buffer, std::size_t packetSize);

   // Adds the next OUT packet.
   Result add(art::span<const uint8_t> packet);

   // The command after Complete. Valid until the next add() or reset().
   [[nodiscard]] art::span<const uint8_t> command() const {
      return art::span<const uint8_t>(buffer_.data(), received_);
   }

   // Drops a partial command, for example when the host deconfigures the
   // device.
   void reset();

   private:
   enum class State : uint8_t {
      Idle,
      // A USB_TO_XXX command that fits the buffer
      Collecting,
      // A USB_TO_XXX command larger than the buffer: drop it until its length
      Overflow,
      // After an error: drop the packets until a short packet
      Draining,
   };

   Result start(art::span<const uint8_t> packet);
   Result proceed(art::span<const uint8_t> packet);
   Result drain(art::span<const uint8_t> packet);
   // An error on packet: Error if the packet ends the transfer, else drop
   // the rest of the transfer first.
   Result failTransfer(art::span<const uint8_t> packet);

   art::span<uint8_t> buffer_;
   std::size_t packetSize_;
   State state_{ State::Idle };
   // Bytes of the command so far, and its total size
   std::size_t received_{ 0 };
   std::size_t expected_{ 0 };
};

} // namespace protocol
