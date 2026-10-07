// The USB_TO_JTAG_RAW block on a fake JTAG port.

#include "protocol/JtagRawBlock.h"
#include "protocol/UsbToAll.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using protocol::jtagRaw::byteCount;
namespace subcommand = protocol::subcommand;

class NoDelay final : public protocol::Delay {
   public:
   void microseconds(uint16_t) override {
   }
   void milliseconds(uint16_t) override {
   }
};

// Records the calls. shift() copies TDI to TDO, so a test sees which
// bytes the block passed.
class FakeJtagPort final : public protocol::JtagPort {
   public:
   void init() override {
      calls.emplace_back("init");
   }
   void fini() override {
      calls.emplace_back("fini");
   }
   bool setSpeed(uint32_t kHz) override {
      calls.push_back("speed " + std::to_string(kHz));
      return kHz != 0 || hasRtck;
   }
   bool shift(uint32_t bitCount, art::span<const uint8_t> tdi, art::span<const uint8_t> tms, art::span<uint8_t> tdo) override {
      calls.push_back("shift " + std::to_string(bitCount));
      REQUIRE(tdi.size() == byteCount(bitCount));
      REQUIRE(tms.size() == tdi.size());
      REQUIRE(tdo.size() == tdi.size());
      for(const uint8_t byte : tdo) {
         CHECK(byte == 0);
      }
      lastTms.assign(tms.begin(), tms.end());
      std::copy(tdi.begin(), tdi.end(), tdo.begin());
      return !rtckTimeout;
   }

   std::vector<std::string> calls;
   std::vector<uint8_t> lastTms;
   bool hasRtck{ true };
   bool rtckTimeout{ false };
};

std::vector<uint8_t> u32(uint32_t value) {
   return { static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
            static_cast<uint8_t>(value >> 24) };
}

// One sub-command: [command | index][length][payload]
std::vector<uint8_t> sub(uint8_t command, const std::vector<uint8_t> &payload = {}) {
   std::vector<uint8_t> bytes{ command, static_cast<uint8_t>(payload.size()), static_cast<uint8_t>(payload.size() >> 8) };
   bytes.insert(bytes.end(), payload.begin(), payload.end());
   return bytes;
}

std::vector<uint8_t> inOut(uint32_t bitCount, const std::vector<uint8_t> &tdi, const std::vector<uint8_t> &tms) {
   auto payload = u32(bitCount);
   payload.insert(payload.end(), tdi.begin(), tdi.end());
   payload.insert(payload.end(), tms.begin(), tms.end());
   return sub(subcommand::inOut, payload);
}

class Bench {
   public:
   explicit Bench(std::size_t replySize = 256)
      : replySize_(replySize)
      , block_(port)
      , dispatcher_(delay_) {
      REQUIRE(dispatcher_.add(block_));
   }

   // Runs one JTAG_RAW block of sub-commands in a USB_TO_ALL command.
   // Returns the reply.
   std::vector<uint8_t> run(const std::vector<std::vector<uint8_t>> &subs) {
      std::vector<uint8_t> command{ protocol::block::all, 0, 0, protocol::block::jtagRaw, 0, 0 };
      for(const auto &bytes : subs) {
         command.insert(command.end(), bytes.begin(), bytes.end());
      }
      const std::size_t blockLength = command.size() - 3;
      command[1] = static_cast<uint8_t>(command.size());
      command[2] = static_cast<uint8_t>(command.size() >> 8);
      command[4] = static_cast<uint8_t>(blockLength);
      command[5] = static_cast<uint8_t>(blockLength >> 8);
      const auto size = dispatcher_.process(art::span<const uint8_t>(command.data(), command.size()),
                                            art::span<uint8_t>(reply_.data(), replySize_));
      REQUIRE(size.has_value());
      return { reply_.begin(), reply_.begin() + static_cast<std::ptrdiff_t>(*size) };
   }

   FakeJtagPort port;

   private:
   std::size_t replySize_;
   NoDelay delay_;
   protocol::JtagRawBlock block_;
   protocol::UsbToAll dispatcher_;
   std::array<uint8_t, 1024> reply_{};
};

} // namespace

TEST_CASE("JTAG_RAW: byte count of a bit stream") {
   CHECK(byteCount(0) == 0);
   CHECK(byteCount(1) == 1);
   CHECK(byteCount(8) == 1);
   CHECK(byteCount(9) == 2);
   CHECK(byteCount(0xFFFFFFFF) == 0x20000000);
}

TEST_CASE("JTAG_RAW: INIT, CONFIG and FINI") {
   Bench bench;
   CHECK(bench.run({ sub(subcommand::init), sub(subcommand::config, u32(4000)), sub(subcommand::fini) }) ==
         std::vector<uint8_t>{ 0, 0, 0 });
   CHECK(bench.port.calls == std::vector<std::string>{ "init", "speed 1000", "speed 4000", "fini" });
}

TEST_CASE("JTAG_RAW: CONFIG with kHz 0 selects RTCK") {
   Bench bench;
   CHECK(bench.run({ sub(subcommand::config, u32(0)) }) == std::vector<uint8_t>{ 0 });
   bench.port.hasRtck = false;
   CHECK(bench.run({ sub(subcommand::config, u32(0)) }) == std::vector<uint8_t>{ 4 });
}

TEST_CASE("JTAG_RAW: CONFIG with a wrong payload size is INVALID_PARA") {
   Bench bench;
   CHECK(bench.run({ sub(subcommand::config, { 0xE8, 0x03 }) }) == std::vector<uint8_t>{ 4 });
   CHECK(bench.port.calls.empty());
}

TEST_CASE("JTAG_RAW: IN_OUT passes the TDI and TMS streams and replies TDO") {
   Bench bench;
   const auto reply = bench.run({ inOut(12, { 0x12, 0x0F }, { 0xA5, 0x03 }) });
   CHECK(reply == std::vector<uint8_t>{ 0, 0x12, 0x0F });
   CHECK(bench.port.calls == std::vector<std::string>{ "shift 12" });
   CHECK(bench.port.lastTms == std::vector<uint8_t>{ 0xA5, 0x03 });
}

TEST_CASE("JTAG_RAW: IN_OUT of 0 bits replies only the status") {
   Bench bench;
   CHECK(bench.run({ inOut(0, {}, {}) }) == std::vector<uint8_t>{ 0 });
}

TEST_CASE("JTAG_RAW: two IN_OUT in one block reply in order") {
   Bench bench;
   const auto reply = bench.run({ inOut(8, { 0x11 }, { 0 }), inOut(16, { 0x22, 0x33 }, { 0, 0 }) });
   CHECK(reply == std::vector<uint8_t>{ 0, 0x11, 0, 0x22, 0x33 });
}

TEST_CASE("JTAG_RAW: IN_OUT with streams of the wrong size is INVALID_PARA with the TDO size") {
   Bench bench;
   // 16 bits need 2 + 2 bytes, the payload has 1 + 1.
   CHECK(bench.run({ inOut(16, { 0x12 }, { 0x34 }) }) == std::vector<uint8_t>{ 4, 0, 0 });
   // No bit count
   CHECK(bench.run({ sub(subcommand::inOut, { 1, 0 }) }) == std::vector<uint8_t>{ 4 });
   CHECK(bench.port.calls.empty());
}

TEST_CASE("JTAG_RAW: an RTCK timeout is TIMEOUT with the TDO size") {
   Bench bench;
   bench.port.rtckTimeout = true;
   const auto reply = bench.run({ inOut(8, { 0x5A }, { 0 }), sub(subcommand::fini) });
   REQUIRE(reply.size() == 3);
   CHECK(reply[0] == 2);
   CHECK(reply[2] == 0);
}

TEST_CASE("JTAG_RAW: a port index other than 0 is INVALID_INDEX") {
   Bench bench;
   auto inOutIndex2 = inOut(8, { 0xFF }, { 0 });
   inOutIndex2[0] |= 2;
   CHECK(bench.run({ sub(subcommand::init | 1), inOutIndex2 }) == std::vector<uint8_t>{ 3, 3, 0 });
   CHECK(bench.port.calls.empty());
}

TEST_CASE("JTAG_RAW: an unknown sub-command is CMD_NOT_SUPPORT") {
   Bench bench;
   CHECK(bench.run({ sub(subcommand::out, { 1 }) }) == std::vector<uint8_t>{ 6 });
}

TEST_CASE("JTAG_RAW: an IN_OUT whose TDO does not fit shifts no bit") {
   // Status + 3 bytes fit; the IN_OUT of 4 bytes does not.
   Bench bench(4);
   CHECK(bench.run({ inOut(24, { 1, 2, 3 }, { 0, 0, 0 }), inOut(32, { 1, 2, 3, 4 }, { 0, 0, 0, 0 }) }).empty());
   CHECK(bench.port.calls == std::vector<std::string>{ "shift 24" });
}
