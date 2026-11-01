// SPDX-License-Identifier: GPL-3.0-or-later
// Replays the USB traffic of OpenOCD sessions (tests/replay/*.trace) through
// the Versaloon function and the USB_TO_ALL dispatcher, on the fake USB
// controller.
//
// A trace has one transfer per line: "> hex" is an OUT transfer of the
// host, "< hex" the IN transfer that the device must reply. A "*" at the end
// of a reply matches the rest of the reply. Lines that start with "#" are
// comments.
//
// The GPIO block is the real block on fake pins. The JTAG_RAW block is the
// real block on the bit-bang engine, with fake pins. On the fake pins, the
// target drives all inputs low. The SWD block is the real block on the
// bit-bang engine, with the mock pins and mock target of the engine tests
// (MockSwdPins.h). The mock target replies OK and swdReadData to each
// read.

#include "jtag/JtagBitBang.h"
#include "protocol/versaloon/GpioBlock.h"
#include "protocol/versaloon/JtagRawBlock.h"
#include "protocol/versaloon/SwdBlock.h"
#include "protocol/versaloon/UsbToAll.h"
#include "protocol/versaloon/UsbToXxx.h"
#include "swd/SwdBitBang.h"

#include "MockSwdPins.h"
#include "VersaloonBench.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// The DPIDR of the SW-DP of a Cortex-M3 (STM32F1)
constexpr uint32_t swdReadData = 0x1BA01477;

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

// The pins of the Mini; the target drives all inputs low.
class LowPins final : public protocol::GpioPins {
   public:
   [[nodiscard]] uint16_t available() const override {
      return protocol::gpio::all & ~(protocol::gpio::usr1 | protocol::gpio::usr2);
   }
   void configure(uint16_t, protocol::PinMode, bool) override {
   }
   void write(uint16_t, uint16_t) override {
   }
   [[nodiscard]] uint16_t read(uint16_t) const override {
      return 0;
   }
};

// The JTAG pins of the Mini; the target drives TDO low, and RTCK follows
// TCK. now() counts one cycle for each call.
class LowJtagPins {
   public:
   static constexpr bool hasRtck = true;

   void init(bool tckLevel) {
      tck_ = tckLevel;
   }
   void fini() {
   }
   void begin() {
   }
   void tck(bool level) {
      tck_ = level;
   }
   void tms(bool) {
   }
   void tdi(bool) {
   }
   [[nodiscard]] bool tdo() const {
      return false;
   }
   [[nodiscard]] bool rtck() const {
      return tck_;
   }
   uint32_t now() {
      return time_++;
   }
   [[nodiscard]] uint32_t coreHz() const {
      return 72'000'000;
   }

   private:
   bool tck_{ false };
   uint32_t time_{ 0 };
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
   const std::string path = std::string(MUPP_REPLAY_DIR) + "/" + name;
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
   LowPins pins;
   protocol::GpioBlock gpio(pins);
   LowJtagPins jtagPins;
   jtag::BitBangPort<LowJtagPins> jtagPort(jtagPins);
   protocol::JtagRawBlock jtagRaw(jtagPort);
   mupptest::MockSwdPins swdPins;
   swdPins.readData = swdReadData;
   swd::BitBangPort<mupptest::MockSwdPins> swdPort(swdPins);
   protocol::SwdBlock swd(swdPort);
   REQUIRE(usbToAll.add(gpio));
   REQUIRE(usbToAll.add(jtagRaw));
   REQUIRE(usbToAll.add(swd));

   mupptest::Bench bench(nullptr, &usbToAll);
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
   CHECK(swdPins.contentions == 0);
}

} // namespace

TEST_CASE("Replay: OpenOCD session in JTAG mode") {
   replay("openocd-jtag.trace");
}

TEST_CASE("Replay: OpenOCD session in SWD mode") {
   replay("openocd-swd.trace");
}
