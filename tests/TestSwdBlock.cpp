// SPDX-License-Identifier: GPL-3.0-or-later
// The USB_TO_SWD block on a fake SWD port, and the request and parity
// encoding.

#include "protocol/versaloon/SwdBlock.h"
#include "protocol/versaloon/UsbToAll.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace swd = protocol::swd;
namespace subcommand = protocol::subcommand;

class NoDelay final : public protocol::Delay {
   public:
   void microseconds(uint16_t) override {
   }
   void milliseconds(uint16_t) override {
   }
};

// Records the calls. sequenceIn() fills the data with 0xA5; transfer()
// replies ack, and readData for a read.
class FakeSwdPort final : public protocol::SwdPort {
   public:
   void init() override {
      calls.emplace_back("init");
   }
   void fini() override {
      calls.emplace_back("fini");
   }
   void configure(uint8_t turnaround, uint16_t retries, uint16_t delay) override {
      calls.push_back("config " + std::to_string(turnaround) + " " + std::to_string(retries) + " " + std::to_string(delay));
   }
   void sequenceOut(uint16_t bitCount, art::span<const uint8_t> data) override {
      calls.push_back("out " + std::to_string(bitCount));
      REQUIRE(data.size() == swd::byteCount(bitCount));
      lastOut.assign(data.begin(), data.end());
   }
   void sequenceIn(uint16_t bitCount, art::span<uint8_t> data) override {
      calls.push_back("in " + std::to_string(bitCount));
      REQUIRE(data.size() == swd::byteCount(bitCount));
      for(uint8_t &byte : data) {
         CHECK(byte == 0);
         byte = 0xA5;
      }
   }
   uint8_t transfer(uint8_t request, uint32_t &data) override {
      calls.push_back("transfer " + std::to_string(request) + " " + std::to_string(data));
      if((request & swd::readRequest) != 0 && ack == swd::ackOk) {
         data = readData;
      }
      return ack;
   }

   std::vector<std::string> calls;
   std::vector<uint8_t> lastOut;
   uint8_t ack{ swd::ackOk };
   uint32_t readData{ 0 };
};

std::vector<uint8_t> u16(uint16_t value) {
   return { static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8) };
}

// One sub-command: [command | index][length][payload]
std::vector<uint8_t> sub(uint8_t command, const std::vector<uint8_t> &payload = {}) {
   std::vector<uint8_t> bytes{ command };
   const auto length = u16(static_cast<uint16_t>(payload.size()));
   bytes.insert(bytes.end(), length.begin(), length.end());
   bytes.insert(bytes.end(), payload.begin(), payload.end());
   return bytes;
}

std::vector<uint8_t> config(uint8_t trn, uint16_t retry, uint16_t delay) {
   std::vector<uint8_t> payload{ trn };
   for(const uint16_t value : { retry, delay }) {
      const auto bytes = u16(value);
      payload.insert(payload.end(), bytes.begin(), bytes.end());
   }
   return sub(subcommand::config, payload);
}

std::vector<uint8_t> transact(uint8_t request, uint32_t data) {
   return sub(subcommand::inOut, { request, static_cast<uint8_t>(data), static_cast<uint8_t>(data >> 8),
                                   static_cast<uint8_t>(data >> 16), static_cast<uint8_t>(data >> 24) });
}

class Bench {
   public:
   explicit Bench(std::size_t replySize = 256)
      : replySize_(replySize)
      , block_(port)
      , dispatcher_(delay_) {
      REQUIRE(dispatcher_.add(block_));
   }

   // Runs one SWD block of sub-commands in a USB_TO_ALL command. Returns
   // the reply.
   std::vector<uint8_t> run(const std::vector<std::vector<uint8_t>> &subs) {
      std::vector<uint8_t> command{ protocol::block::all, 0, 0, protocol::block::swd, 0, 0 };
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

   [[nodiscard]] uint32_t errorCount() const {
      return block_.errorCount();
   }

   FakeSwdPort port;

   private:
   std::size_t replySize_;
   NoDelay delay_;
   protocol::SwdBlock block_;
   protocol::UsbToAll dispatcher_;
   std::array<uint8_t, 256> reply_{};
};

} // namespace

TEST_CASE("SWD: parity of a word") {
   CHECK_FALSE(swd::parity(0));
   CHECK(swd::parity(1));
   CHECK(swd::parity(0x80000000));
   CHECK_FALSE(swd::parity(0x80000001));
   CHECK_FALSE(swd::parity(0xFFFFFFFF));
   CHECK_FALSE(swd::parity(0x2BA01477));
   CHECK(swd::parity(0x2BA01476));
}

TEST_CASE("SWD: request encoding sets start, parity, stop and park bits") {
   // The requests of OpenOCD (vsllink usbtoswd.c): read DPIDR, write ABORT,
   // read CTRL/STAT.
   CHECK(swd::encodeRequest(0x04) == 0xA5);
   CHECK(swd::encodeRequest(0x00) == 0x81);
   CHECK(swd::encodeRequest(0x0C) == 0x8D);
   // Read AP register 0xC: APnDP, RnW, A2, A3: four 1 bits, even parity.
   CHECK(swd::encodeRequest(0x1E) == 0x9F);
   // An already encoded request does not change.
   for(unsigned request = 0; request < 0x20; request += 2) {
      CAPTURE(request);
      const uint8_t encoded = swd::encodeRequest(static_cast<uint8_t>(request));
      CHECK(swd::encodeRequest(encoded) == encoded);
      CHECK((encoded & 0x01) == 0x01);
      CHECK((encoded & 0x40) == 0);
      CHECK((encoded & 0x80) == 0x80);
   }
   // The stop bit is cleared and a wrong parity bit is fixed.
   CHECK(swd::encodeRequest(0xE5) == 0xA5);
   CHECK(swd::encodeRequest(0x85) == 0xA5);
}

TEST_CASE("SWD: INIT sets the default configuration, FINI releases") {
   Bench bench;
   CHECK(bench.run({ sub(subcommand::init), sub(subcommand::fini) }) == std::vector<uint8_t>{ 0, 0 });
   CHECK(bench.port.calls == std::vector<std::string>{ "init", "config 1 0 1", "fini" });
}

TEST_CASE("SWD: CONFIG maps trn to the cycles of one turnaround") {
   Bench bench;
   CHECK(bench.run({ config(2, 1087, 1), config(8, 0, 300) }) == std::vector<uint8_t>{ 0, 0 });
   CHECK(bench.port.calls == std::vector<std::string>{ "config 1 1087 1", "config 4 0 300" });
}

TEST_CASE("SWD: CONFIG with a bad trn or size is INVALID_PARA") {
   Bench bench;
   CHECK(bench.run({ config(0, 0, 0), config(1, 0, 0), config(3, 0, 0), config(10, 0, 0),
                     sub(subcommand::config, { 2, 0, 0 }) }) == std::vector<uint8_t>{ 4, 4, 4, 4, 4 });
   CHECK(bench.port.calls.empty());
}

TEST_CASE("SWD: OUT drives the sequence") {
   Bench bench;
   CHECK(bench.run({ sub(subcommand::out, { 10, 0, 0xFF, 0x03 }) }) == std::vector<uint8_t>{ 0 });
   CHECK(bench.port.calls == std::vector<std::string>{ "out 10" });
   CHECK(bench.port.lastOut == std::vector<uint8_t>{ 0xFF, 0x03 });
}

TEST_CASE("SWD: OUT with the wrong data size is INVALID_PARA") {
   Bench bench;
   CHECK(bench.run({ sub(subcommand::out, { 10, 0, 0xFF }), sub(subcommand::out, { 8 }) }) == std::vector<uint8_t>{ 4, 4 });
   CHECK(bench.port.calls.empty());
}

TEST_CASE("SWD: IN replies the sequence") {
   Bench bench;
   CHECK(bench.run({ sub(subcommand::in, u16(12)) }) == std::vector<uint8_t>{ 0, 0xA5, 0xA5 });
   CHECK(bench.port.calls == std::vector<std::string>{ "in 12" });
}

TEST_CASE("SWD: IN with the wrong payload size is INVALID_PARA with the data size") {
   Bench bench;
   CHECK(bench.run({ sub(subcommand::in, { 12, 0, 0 }) }) == std::vector<uint8_t>{ 4, 0, 0 });
   CHECK(bench.run({ sub(subcommand::in) }) == std::vector<uint8_t>{ 4 });
   CHECK(bench.port.calls.empty());
}

TEST_CASE("SWD: IN_OUT read replies the ack and the read data") {
   Bench bench;
   bench.port.readData = 0x2BA01477;
   CHECK(bench.run({ transact(0xA5, 0) }) == std::vector<uint8_t>{ 0, 0x01, 0x77, 0x14, 0xA0, 0x2B });
   CHECK(bench.port.calls == std::vector<std::string>{ "transfer 165 0" });
}

TEST_CASE("SWD: IN_OUT encodes the request and passes the write data") {
   Bench bench;
   // A request without start and park bits, as a host other than OpenOCD
   // can send it: write ABORT.
   CHECK(bench.run({ transact(0x00, 0x1E) }) == std::vector<uint8_t>{ 0, 0x01, 0x1E, 0, 0, 0 });
   CHECK(bench.port.calls == std::vector<std::string>{ "transfer 129 30" });
}

TEST_CASE("SWD: IN_OUT that is not OK is FAILED with the ack byte") {
   Bench bench;
   uint32_t errors = 0;
   for(const uint8_t ack :
       { static_cast<uint8_t>(swd::ackWait | swd::retryOutFlag), static_cast<uint8_t>(swd::ackFault | swd::faultFlag),
         static_cast<uint8_t>(0x07 | swd::ackErrorFlag), static_cast<uint8_t>(swd::ackOk | swd::parityErrorFlag) }) {
      CAPTURE(ack);
      bench.port.ack = ack;
      CHECK(bench.run({ transact(0x8D, 0x12345678) }) == std::vector<uint8_t>{ 1, ack, 0x78, 0x56, 0x34, 0x12 });
      // Each failed transfer counts as an error.
      CHECK(bench.errorCount() == ++errors);
   }
   bench.port.ack = swd::ackOk;
   bench.run({ transact(0x8D, 0x12345678) });
   CHECK(bench.errorCount() == errors);
}

TEST_CASE("SWD: a DP TARGETSEL write is OK for each ack and keeps the ack byte") {
   CHECK(swd::encodeRequest(0x18) == swd::targetSelRequest);
   Bench bench;
   for(const uint8_t ack : { static_cast<uint8_t>(0x07 | swd::ackErrorFlag), static_cast<uint8_t>(swd::ackFault | swd::faultFlag),
                             static_cast<uint8_t>(swd::ackWait | swd::retryOutFlag), swd::ackOk }) {
      CAPTURE(ack);
      bench.port.ack = ack;
      // OpenOCD sends the encoded request (swd_cmd()). Another host can
      // send it without start, parity and park bits.
      for(const uint8_t request : { swd::targetSelRequest, uint8_t{ 0x18 } }) {
         CAPTURE(request);
         CHECK(bench.run({ transact(request, 0x01002927) }) == std::vector<uint8_t>{ 0, ack, 0x27, 0x29, 0x00, 0x01 });
      }
   }
   CHECK(bench.errorCount() == 0);
   // A read of DP address 0xC (RDBUFF) is not TARGETSEL.
   bench.port.ack = static_cast<uint8_t>(0x07 | swd::ackErrorFlag);
   CHECK(bench.run({ transact(0x1C, 0) })[0] == 1);
   // A write of AP address 0xC is not TARGETSEL.
   CHECK(bench.run({ transact(0x1A, 0) })[0] == 1);
   CHECK(bench.errorCount() == 2);
}

TEST_CASE("SWD: IN_OUT with the wrong payload size is INVALID_PARA with the data size") {
   Bench bench;
   CHECK(bench.run({ sub(subcommand::inOut, { 0xA5, 0, 0, 0 }) }) == std::vector<uint8_t>{ 4, 0, 0, 0, 0, 0 });
   CHECK(bench.port.calls.empty());
}

TEST_CASE("SWD: a port index other than 0 is INVALID_INDEX") {
   Bench bench;
   auto transfer = transact(0xA5, 0);
   transfer[0] |= 1;
   CHECK(bench.run({ sub(subcommand::init | 1), transfer }) == std::vector<uint8_t>{ 3, 3, 0, 0, 0, 0, 0 });
   CHECK(bench.port.calls.empty());
}

TEST_CASE("SWD: an unknown sub-command is CMD_NOT_SUPPORT") {
   Bench bench;
   CHECK(bench.run({ sub(0x20) }) == std::vector<uint8_t>{ 6 });
}

TEST_CASE("SWD: a transfer whose reply does not fit does not run") {
   // Status + 5 bytes fit once.
   Bench bench(8);
   CHECK(bench.run({ transact(0xA5, 0), transact(0xA5, 0) }).empty());
   CHECK(bench.port.calls.size() == 1);
}
