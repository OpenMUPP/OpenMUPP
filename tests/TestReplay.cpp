// Replays the USB traffic of OpenOCD sessions (tests/replay/*.trace) through
// the Versaloon function and the USB_TO_ALL dispatcher, on the fake USB
// controller.
//
// A trace has one transfer per line: "> hex" is an OUT transfer of the
// host, "< hex" the IN transfer that the device must reply. A "*" at the end
// of a reply matches the rest of the reply. Lines that start with "#" are
// comments.
//
// The GPIO, JTAG_RAW and SWD blocks are fakes here: they reply OK with the
// data size that OpenOCD expects, and zero data. The real blocks replace
// them in the tests of their own.

#include "protocol/UsbToAll.h"
#include "protocol/UsbToXxx.h"

#include "VersaloonBench.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using protocol::SubCommand;
namespace block = protocol::block;
namespace subcommand = protocol::subcommand;

class NoDelay final : public protocol::Delay {
   public:
   void microseconds(uint16_t us) override {
      totalUs += us;
   }
   void milliseconds(uint16_t ms) override {
      totalUs += 1000u * ms;
   }

   uint32_t totalUs{ 0 };
};

// Replies OK with the data size of the OpenOCD usbtoxxx encoder: GPIO IN 2
// bytes, JTAG_RAW IN_OUT (bits + 7) / 8 bytes, SWD IN_OUT 5 bytes and SWD
// IN (bits + 7) / 8 bytes. The other sub-commands have no data.
class OpenOcdFakeBlock final : public protocol::SubCommandBlock {
   public:
   explicit OpenOcdFakeBlock(uint8_t type)
      : SubCommandBlock(type) {
   }

   protected:
   void runSubCommand(const SubCommand &command, protocol::Reply &reply) override {
      static_cast<void>(reply.reserve(dataSize(command)));
   }

   private:
   [[nodiscard]] std::size_t dataSize(const SubCommand &command) const {
      switch(type()) {
      case block::gpio:
         return command.command == subcommand::in ? 2 : 0;
      case block::jtagRaw:
         if(command.command == subcommand::inOut) {
            REQUIRE(command.payload.size() >= 4);
            return (protocol::getU32(command.payload, 0) + 7) / 8;
         }
         return 0;
      case block::swd:
         if(command.command == subcommand::inOut) {
            return 5;
         }
         if(command.command == subcommand::in) {
            REQUIRE(command.payload.size() >= 2);
            return (protocol::getU16(command.payload, 0) + 7u) / 8u;
         }
         return 0;
      default:
         return 0;
      }
   }
};

struct Transfer {
   int line;
   std::vector<uint8_t> out;
   std::vector<uint8_t> in;
   // The reply only has to start with in.
   bool prefix;
};

std::vector<uint8_t> parseHex(std::istringstream &stream, bool *wildcard) {
   std::vector<uint8_t> bytes;
   std::string token;
   while(stream >> token) {
      if(token == "*") {
         *wildcard = true;
         break;
      }
      bytes.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
   }
   return bytes;
}

std::vector<Transfer> loadTrace(const std::string &name) {
   const std::string path = std::string(VSL_REPLAY_DIR) + "/" + name;
   std::ifstream file(path);
   REQUIRE_MESSAGE(file.good(), "cannot open ", path);
   std::vector<Transfer> transfers;
   std::string text;
   int lineNumber = 0;
   while(std::getline(file, text)) {
      ++lineNumber;
      std::istringstream stream(text);
      std::string marker;
      if(!(stream >> marker) || marker[0] == '#') {
         continue;
      }
      bool wildcard = false;
      if(marker == ">") {
         transfers.push_back({ lineNumber, parseHex(stream, &wildcard), {}, false });
      } else {
         REQUIRE_MESSAGE(marker == "<", path, ":", lineNumber, ": a line must start with >, < or #");
         REQUIRE_MESSAGE((!transfers.empty() && transfers.back().in.empty() && !transfers.back().prefix), path, ":",
                         lineNumber, ": a reply without a command");
         transfers.back().in = parseHex(stream, &wildcard);
         transfers.back().prefix = wildcard;
      }
   }
   return transfers;
}

void replay(const std::string &name) {
   NoDelay delay;
   protocol::UsbToAll usbToAll(delay);
   OpenOcdFakeBlock gpio(block::gpio);
   OpenOcdFakeBlock jtagRaw(block::jtagRaw);
   OpenOcdFakeBlock swd(block::swd);
   REQUIRE(usbToAll.add(gpio));
   REQUIRE(usbToAll.add(jtagRaw));
   REQUIRE(usbToAll.add(swd));

   vsltest::Bench bench(nullptr, &usbToAll);
   bench.enumerate();

   const auto transfers = loadTrace(name);
   REQUIRE_FALSE(transfers.empty());
   for(const Transfer &transfer : transfers) {
      CAPTURE(name);
      CAPTURE(transfer.line);
      REQUIRE(bench.write(transfer.out));
      const auto reply = bench.read();
      REQUIRE(reply.has_value());
      if(transfer.prefix) {
         REQUIRE(reply->size() >= transfer.in.size());
         CHECK(std::vector<uint8_t>(reply->begin(), reply->begin() + static_cast<std::ptrdiff_t>(transfer.in.size())) ==
               transfer.in);
      } else {
         CHECK(*reply == transfer.in);
      }
   }
   CHECK(delay.totalUs == 100000);
}

} // namespace

TEST_CASE("Replay: OpenOCD session in JTAG mode") {
   replay("openocd-jtag.trace");
}

TEST_CASE("Replay: OpenOCD session in SWD mode") {
   replay("openocd-swd.trace");
}
