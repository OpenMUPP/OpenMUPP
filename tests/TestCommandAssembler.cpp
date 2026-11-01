// SPDX-License-Identifier: GPL-3.0-or-later
// The command assembler: joins OUT packets into Versaloon commands.

#include "protocol/versaloon/CommandAssembler.h"
#include "protocol/versaloon/Versaloon.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace {

using protocol::CommandAssembler;
using Result = CommandAssembler::Result;

constexpr std::size_t packetSize = 64;
constexpr std::size_t bufferSize = 256;

// A USB_TO_XXX command of total size length. The bytes after the header
// count up.
std::vector<uint8_t> usbToXxx(std::size_t length, uint16_t lengthField) {
   std::vector<uint8_t> command(length);
   command[0] = protocol::usbToXxxLast;
   for(std::size_t i = 1; i < length; ++i) {
      command[i] = static_cast<uint8_t>(i);
   }
   if(length >= 3) {
      command[1] = static_cast<uint8_t>(lengthField);
      command[2] = static_cast<uint8_t>(lengthField >> 8);
   }
   return command;
}

std::vector<uint8_t> usbToXxx(std::size_t length) {
   return usbToXxx(length, static_cast<uint16_t>(length));
}

class Bench {
   public:
   Bench()
      : assembler(art::span<uint8_t>(buffer_.data(), buffer_.size()), packetSize) {
   }

   // Sends data as one transfer, in packets of packetSize. Returns the
   // results of the packets.
   std::vector<Result> send(const std::vector<uint8_t> &data) {
      std::vector<Result> results;
      std::size_t offset = 0;
      do {
         const std::size_t size = std::min(packetSize, data.size() - offset);
         results.push_back(assembler.add(art::span<const uint8_t>(data.data() + offset, size)));
         offset += size;
      } while(offset < data.size());
      return results;
   }

   Result sendPacket(const std::vector<uint8_t> &packet) {
      return assembler.add(art::span<const uint8_t>(packet.data(), packet.size()));
   }

   std::vector<uint8_t> command() const {
      const auto command = assembler.command();
      return { command.begin(), command.end() };
   }

   CommandAssembler assembler;

   private:
   std::array<uint8_t, bufferSize> buffer_{};
};

} // namespace

TEST_CASE("A common command is one packet") {
   Bench bench;
   CHECK(bench.sendPacket({ protocol::getInfo }) == Result::Complete);
   CHECK(bench.command() == std::vector<uint8_t>{ protocol::getInfo });

   CHECK(bench.sendPacket({ protocol::fwUpdate, protocol::fwUpdateKey }) == Result::Complete);
   CHECK(bench.command() == std::vector<uint8_t>{ protocol::fwUpdate, protocol::fwUpdateKey });
}

TEST_CASE("An unused command byte is one packet") {
   Bench bench;
   CHECK(bench.sendPacket({ 0x10, 0x20, 0x00 }) == Result::Complete);
   CHECK(bench.sendPacket({ 0x80, 0xFF, 0xFF }) == Result::Complete);
   CHECK(bench.command() == std::vector<uint8_t>{ 0x80, 0xFF, 0xFF });
}

TEST_CASE("A ZLP between two commands is ignored") {
   Bench bench;
   CHECK(bench.sendPacket({}) == Result::Ignored);
   CHECK(bench.sendPacket({ protocol::getHardware }) == Result::Complete);
   CHECK(bench.sendPacket({}) == Result::Ignored);
}

TEST_CASE("A USB_TO_XXX command in one packet") {
   Bench bench;
   const auto command = usbToXxx(10);
   CHECK(bench.send(command) == std::vector<Result>{ Result::Complete });
   CHECK(bench.command() == command);
}

TEST_CASE("A USB_TO_XXX command in many packets") {
   Bench bench;
   for(const std::size_t length : { std::size_t{ 65 }, std::size_t{ 100 }, std::size_t{ 200 } }) {
      CAPTURE(length);
      const auto command = usbToXxx(length);
      const auto results = bench.send(command);
      REQUIRE(results.size() == (length + packetSize - 1) / packetSize);
      for(std::size_t i = 0; i + 1 < results.size(); ++i) {
         CHECK(results[i] == Result::Incomplete);
      }
      CHECK(results.back() == Result::Complete);
      CHECK(bench.command() == command);
   }
}

TEST_CASE("A USB_TO_XXX command that is a multiple of the packet size ends without a ZLP") {
   Bench bench;
   const auto command = usbToXxx(128);
   CHECK(bench.send(command) == std::vector<Result>{ Result::Incomplete, Result::Complete });
   CHECK(bench.command() == command);
   // The next packet is a new command.
   CHECK(bench.sendPacket({ protocol::getInfo }) == Result::Complete);
}

TEST_CASE("A USB_TO_XXX command that fills the buffer") {
   Bench bench;
   const auto command = usbToXxx(bufferSize);
   CHECK(bench.send(command).back() == Result::Complete);
   CHECK(bench.command() == command);
}

TEST_CASE("Length errors") {
   Bench bench;

   SUBCASE("The header is incomplete") {
      CHECK(bench.sendPacket({ protocol::usbToXxxLast, 0x03 }) == Result::Error);
   }
   SUBCASE("The length is smaller than the header") {
      CHECK(bench.send(usbToXxx(3, 2)) == std::vector<Result>{ Result::Error });
      CHECK(bench.send(usbToXxx(3, 0)) == std::vector<Result>{ Result::Error });
   }
   SUBCASE("The packet is longer than the length") {
      CHECK(bench.send(usbToXxx(10, 9)) == std::vector<Result>{ Result::Error });
   }
   SUBCASE("A later packet goes past the length") {
      CHECK(bench.send(usbToXxx(100, 90)) == std::vector<Result>{ Result::Incomplete, Result::Error });
   }
   SUBCASE("A short packet ends the transfer before the length") {
      CHECK(bench.send(usbToXxx(10, 11)) == std::vector<Result>{ Result::Error });
      CHECK(bench.send(usbToXxx(100, 101)) == std::vector<Result>{ Result::Incomplete, Result::Error });
   }
   SUBCASE("A ZLP ends the transfer before the length") {
      CHECK(bench.send(usbToXxx(64, 100)) == std::vector<Result>{ Result::Incomplete });
      CHECK(bench.sendPacket({}) == Result::Error);
   }
   SUBCASE("An error on a full packet drops the rest of the transfer") {
      // Packets of 64, 64, 64 and 8 bytes: the second goes past the length.
      CHECK(bench.send(usbToXxx(200, 80)) ==
            std::vector<Result>{ Result::Incomplete, Result::Incomplete, Result::Incomplete, Result::Error });
   }
   SUBCASE("A full packet with a length error drops the rest of the transfer") {
      CHECK(bench.send(usbToXxx(100, 2)) == std::vector<Result>{ Result::Incomplete, Result::Error });
   }
   SUBCASE("An error on a full packet, and a ZLP ends the transfer") {
      CHECK(bench.send(usbToXxx(128, 80)) == std::vector<Result>{ Result::Incomplete, Result::Incomplete });
      CHECK(bench.sendPacket({}) == Result::Error);
   }
   SUBCASE("A common command in a full packet drops the rest of the transfer") {
      std::vector<uint8_t> transfer(100, 0x00);
      transfer[0] = protocol::getInfo;
      CHECK(bench.send(transfer) == std::vector<Result>{ Result::Incomplete, Result::Error });
   }

   // The assembler is in sync again: the next packet is a new command.
   const auto command = usbToXxx(70);
   CHECK(bench.send(command) == std::vector<Result>{ Result::Incomplete, Result::Complete });
   CHECK(bench.command() == command);
}

TEST_CASE("Overflow: a command larger than the buffer is read to its end and dropped") {
   Bench bench;
   const auto tooLarge = usbToXxx(bufferSize + 1);
   const auto results = bench.send(tooLarge);
   REQUIRE(results.size() == 5);
   for(std::size_t i = 0; i < 4; ++i) {
      CHECK(results[i] == Result::Incomplete);
   }
   CHECK(results.back() == Result::Error);

   // The largest length field, in full packets
   const auto largest = usbToXxx(0xFFC0, 0xFFC0);
   const auto largestResults = bench.send(largest);
   CHECK(largestResults.back() == Result::Error);
   CHECK(std::count(largestResults.begin(), largestResults.end(), Result::Incomplete) == 0xFFC0 / 64 - 1);

   const auto command = usbToXxx(20);
   CHECK(bench.send(command) == std::vector<Result>{ Result::Complete });
   CHECK(bench.command() == command);
}

TEST_CASE("Overflow: a short packet ends the transfer before the length") {
   Bench bench;
   CHECK(bench.send(usbToXxx(100, bufferSize + 1)) == std::vector<Result>{ Result::Incomplete, Result::Error });
   CHECK(bench.sendPacket({ protocol::getInfo }) == Result::Complete);
}

TEST_CASE("reset() drops a partial command") {
   Bench bench;
   CHECK(bench.send(usbToXxx(64, 100)) == std::vector<Result>{ Result::Incomplete });
   bench.assembler.reset();
   CHECK(bench.sendPacket({ protocol::getInfo }) == Result::Complete);
   CHECK(bench.command() == std::vector<uint8_t>{ protocol::getInfo });
}
