// The USB_TO_ALL dispatcher: block framing, INFO, DELAY and the sub-command
// blocks.

#include "protocol/versaloon/UsbToAll.h"
#include "protocol/versaloon/UsbToXxx.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using protocol::Status;
using protocol::SubCommand;
using protocol::UsbToAll;
namespace block = protocol::block;
namespace subcommand = protocol::subcommand;

class FakeDelay final : public protocol::Delay {
   public:
   void microseconds(uint16_t us) override {
      calls.push_back("us " + std::to_string(us));
   }
   void milliseconds(uint16_t ms) override {
      calls.push_back("ms " + std::to_string(ms));
   }

   std::vector<std::string> calls;
};

// Records the sub-commands that run. Each reply is OK with dataSize bytes 0xD0,
// 0xD1, ...; a sub-command with index 7 fails with INVALID_INDEX and still
// has its data size.
class FakeBlock final : public protocol::SubCommandBlock {
   public:
   explicit FakeBlock(uint8_t type, std::size_t dataSize = 0)
      : SubCommandBlock(type)
      , dataSize_(dataSize) {
   }

   struct Recorded {
      uint8_t command;
      uint8_t index;
      std::vector<uint8_t> payload;
   };

   std::vector<Recorded> recorded;

   protected:
   void runSubCommand(const SubCommand &command, protocol::Reply &reply) override {
      auto slot = reply.reserve(dataSize_);
      if(!slot) {
         return;
      }
      recorded.push_back({ command.command, command.index, { command.payload.begin(), command.payload.end() } });
      REQUIRE(slot->data().size() == dataSize_);
      for(std::size_t i = 0; i < dataSize_; ++i) {
         slot->data()[i] = static_cast<uint8_t>(0xD0 + i);
      }
      if(command.index == 7) {
         slot->setStatus(Status::InvalidIndex);
      }
   }

   private:
   std::size_t dataSize_;
};

// Builds a USB_TO_ALL command from blocks. Each block is its type and its
// payload; the lengths are filled in.
std::vector<uint8_t> usbToAll(const std::vector<std::pair<uint8_t, std::vector<uint8_t>>> &blocks) {
   std::vector<uint8_t> command{ block::all, 0, 0 };
   for(const auto &[type, payload] : blocks) {
      const std::size_t length = payload.size() + 3;
      command.push_back(type);
      command.push_back(static_cast<uint8_t>(length));
      command.push_back(static_cast<uint8_t>(length >> 8));
      command.insert(command.end(), payload.begin(), payload.end());
   }
   command[1] = static_cast<uint8_t>(command.size());
   command[2] = static_cast<uint8_t>(command.size() >> 8);
   return command;
}

class Bench {
   public:
   Bench()
      : dispatcher(delay) {
   }

   std::optional<std::vector<uint8_t>> run(const std::vector<uint8_t> &command) {
      const auto size = dispatcher.process(art::span<const uint8_t>(command.data(), command.size()),
                                           art::span<uint8_t>(reply_.data(), reply_.size()));
      if(!size) {
         return std::nullopt;
      }
      return std::vector<uint8_t>(reply_.begin(), reply_.begin() + static_cast<std::ptrdiff_t>(*size));
   }

   // A smaller reply buffer, for the overflow test
   void setReplySize(std::size_t size) {
      replySize_ = size;
   }

   std::optional<std::vector<uint8_t>> runLimited(const std::vector<uint8_t> &command) {
      const auto size = dispatcher.process(art::span<const uint8_t>(command.data(), command.size()),
                                           art::span<uint8_t>(reply_.data(), replySize_));
      if(!size) {
         return std::nullopt;
      }
      return std::vector<uint8_t>(reply_.begin(), reply_.begin() + static_cast<std::ptrdiff_t>(*size));
   }

   FakeDelay delay;
   UsbToAll dispatcher;

   private:
   std::array<uint8_t, 4096> reply_{};
   std::size_t replySize_{ 4096 };
};

} // namespace

TEST_CASE("INFO replies OK and the capability bitmap") {
   Bench bench;
   FakeBlock gpio(block::gpio);
   FakeBlock swd(block::swd);
   FakeBlock jtag(block::jtagRaw);
   REQUIRE(bench.dispatcher.add(gpio));
   REQUIRE(bench.dispatcher.add(swd));
   REQUIRE(bench.dispatcher.add(jtag));

   // The command of OpenOCD usbtoxxx_init()
   const std::vector<uint8_t> command{ 0x7F, 0x06, 0x00, 0x7E, 0x03, 0x00 };
   // GPIO 0x23: bit 3. SWD 0x46, JTAG_RAW 0x47: bits 38, 39. DELAY 0x61:
   // bit 65. INFO 0x7E, ALL 0x7F: bits 94, 95.
   CHECK(bench.run(command) == std::vector<uint8_t>{ 0x00, 0x08, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xC0 });
}

TEST_CASE("INFO without block types") {
   Bench bench;
   const auto bitmap = bench.dispatcher.capabilities();
   CHECK(bitmap == std::array<uint8_t, 12>{ 0, 0, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0xC0 });
}

TEST_CASE("DELAY in us and in ms") {
   Bench bench;
   CHECK(bench.run(usbToAll({ { block::delay, { 0x64, 0x80 } }, { block::delay, { 0x0A, 0x00 } } })) ==
         std::vector<uint8_t>{ 0x00, 0x00 });
   CHECK(bench.delay.calls == std::vector<std::string>{ "ms 100", "us 10" });
}

TEST_CASE("DELAY with a wrong payload size replies INVALID_PARA") {
   Bench bench;
   CHECK(bench.run(usbToAll({ { block::delay, { 0x01 } } })) == std::vector<uint8_t>{ 0x04 });
   CHECK(bench.delay.calls.empty());
}

TEST_CASE("Sub-commands go to their block, with the command, the index and the payload") {
   Bench bench;
   FakeBlock gpio(block::gpio, 2);
   REQUIRE(bench.dispatcher.add(gpio));

   // OpenOCD: gpio.init(0), gpio.config(0, SRST, 0, SRST, SRST), gpio.in(0, 0x0003)
   const auto command = usbToAll(
   { { block::gpio,
       { 0x00, 0x00, 0x00, 0x10, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x31, 0x02, 0x00, 0x03, 0x00 } } });
   CHECK(bench.run(command) == std::vector<uint8_t>{ 0x00, 0xD0, 0xD1, 0x00, 0xD0, 0xD1, 0x00, 0xD0, 0xD1 });

   REQUIRE(gpio.recorded.size() == 3);
   CHECK(gpio.recorded[0].command == subcommand::init);
   CHECK(gpio.recorded[0].index == 0);
   CHECK(gpio.recorded[0].payload.empty());
   CHECK(gpio.recorded[1].command == subcommand::config);
   CHECK(gpio.recorded[1].payload == std::vector<uint8_t>{ 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00 });
   CHECK(gpio.recorded[2].command == subcommand::in);
   CHECK(gpio.recorded[2].index == 1);
   CHECK(gpio.recorded[2].payload == std::vector<uint8_t>{ 0x03, 0x00 });
}

TEST_CASE("A failed sub-command keeps its data size") {
   Bench bench;
   FakeBlock gpio(block::gpio, 2);
   REQUIRE(bench.dispatcher.add(gpio));
   CHECK(bench.run(usbToAll({ { block::gpio, { 0x37, 0x00, 0x00 } } })) == std::vector<uint8_t>{ 0x03, 0xD0, 0xD1 });
}

TEST_CASE("The replies of many blocks are joined in order") {
   Bench bench;
   FakeBlock gpio(block::gpio);
   FakeBlock jtag(block::jtagRaw, 1);
   REQUIRE(bench.dispatcher.add(gpio));
   REQUIRE(bench.dispatcher.add(jtag));

   const auto command = usbToAll({
   { block::jtagRaw, { 0x00, 0x00, 0x00 } },
   { block::delay, { 0x05, 0x00 } },
   { block::gpio, { 0x00, 0x00, 0x00, 0x08, 0x00, 0x00 } },
   { block::jtagRaw, { 0x28, 0x01, 0x00, 0xAA } },
   });
   CHECK(bench.run(command) == std::vector<uint8_t>{ 0x00, 0xD0, 0x00, 0x00, 0x00, 0x00, 0xD0 });
   CHECK(gpio.recorded.size() == 2);
   CHECK(jtag.recorded.size() == 2);
   CHECK(bench.delay.calls == std::vector<std::string>{ "us 5" });
}

TEST_CASE("A block type without a block replies CMD_NOT_SUPPORT and the next block runs") {
   Bench bench;
   CHECK(bench.run(usbToAll({ { block::swd, { 0x00, 0x00, 0x00 } }, { block::delay, { 0x01, 0x00 } } })) ==
         std::vector<uint8_t>{ 0x06, 0x00 });
}

TEST_CASE("A top-level command other than USB_TO_ALL is one block") {
   Bench bench;
   FakeBlock gpio(block::gpio);
   REQUIRE(bench.dispatcher.add(gpio));
   CHECK(bench.run({ block::gpio, 0x06, 0x00, 0x00, 0x00, 0x00 }) == std::vector<uint8_t>{ 0x00 });
   CHECK(bench.run({ block::delay, 0x05, 0x00, 0x02, 0x00 }) == std::vector<uint8_t>{ 0x00 });
   CHECK(bench.delay.calls == std::vector<std::string>{ "us 2" });
}

TEST_CASE("An empty USB_TO_ALL has an empty reply") {
   Bench bench;
   CHECK(bench.run({ 0x7F, 0x03, 0x00 }) == std::vector<uint8_t>{});
}

TEST_CASE("Malformed commands get an empty reply") {
   Bench bench;
   FakeBlock gpio(block::gpio);
   REQUIRE(bench.dispatcher.add(gpio));

   SUBCASE("The block header is incomplete") {
      CHECK(bench.run({ 0x7F, 0x05, 0x00, 0x7E, 0x03 }) == std::vector<uint8_t>{});
   }
   SUBCASE("The block length is smaller than its header") {
      CHECK(bench.run({ 0x7F, 0x06, 0x00, 0x7E, 0x02, 0x00 }) == std::vector<uint8_t>{});
   }
   SUBCASE("The block length goes past the command") {
      CHECK(bench.run({ 0x7F, 0x06, 0x00, 0x7E, 0x04, 0x00 }) == std::vector<uint8_t>{});
   }
   SUBCASE("The sub-command header is incomplete") {
      CHECK(bench.run(usbToAll({ { block::gpio, { 0x00, 0x00 } } })) == std::vector<uint8_t>{});
   }
   SUBCASE("The sub-command length goes past the block") {
      CHECK(bench.run(usbToAll({ { block::gpio, { 0x38, 0x04, 0x00, 0x01, 0x00 } } })) == std::vector<uint8_t>{});
   }
   SUBCASE("A good block before a malformed one does not run") {
      CHECK(bench.run(usbToAll({ { block::delay, { 0x01, 0x00 } }, { block::gpio, { 0x00 } } })) == std::vector<uint8_t>{});
   }
   SUBCASE("A good sub-command before a malformed one does not run") {
      CHECK(bench.run(usbToAll({ { block::delay, { 0x01, 0x00 } }, { block::gpio, { 0x00, 0x00, 0x00, 0x38, 0x04, 0x00, 0x01 } } })) ==
            std::vector<uint8_t>{});
   }
   SUBCASE("A malformed block after a good one does not run the good one") {
      CHECK(bench.run({ 0x7F, 0x0D, 0x00, 0x23, 0x06, 0x00, 0x00, 0x00, 0x00, 0x61, 0x02, 0x00, 0x00 }) == std::vector<uint8_t>{});
   }
   // Nothing ran.
   CHECK(bench.delay.calls.empty());
   CHECK(gpio.recorded.empty());
}

TEST_CASE("An overflow stops the command at the sub-command that does not fit") {
   Bench bench;
   FakeBlock jtag(block::jtagRaw, 100);
   FakeBlock gpio(block::gpio);
   REQUIRE(bench.dispatcher.add(jtag));
   REQUIRE(bench.dispatcher.add(gpio));
   bench.setReplySize(150);

   SUBCASE("A later sub-command of the same block") {
      CHECK(bench.runLimited(usbToAll({ { block::jtagRaw, { 0x28, 0x00, 0x00, 0x28, 0x01, 0x00, 0xAA, 0x28, 0x00, 0x00 } },
                                        { block::delay, { 0x01, 0x00 } } })) == std::vector<uint8_t>{});
      // The first sub-command ran, the second did not fit.
      CHECK(jtag.recorded.size() == 1);
   }
   SUBCASE("A later block") {
      CHECK(bench.runLimited(usbToAll({ { block::jtagRaw, { 0x28, 0x00, 0x00 } },
                                        { block::jtagRaw, { 0x28, 0x00, 0x00 } },
                                        { block::gpio, { 0x00, 0x00, 0x00 } } })) == std::vector<uint8_t>{});
      CHECK(jtag.recorded.size() == 1);
   }
   SUBCASE("A DELAY that does not fit does not wait") {
      FakeBlock big(0x30, 149);
      REQUIRE(bench.dispatcher.add(big));
      CHECK(bench.runLimited(usbToAll({ { 0x30, { 0x00, 0x00, 0x00 } }, { block::delay, { 0x01, 0x00 } } })) ==
            std::vector<uint8_t>{});
      CHECK(big.recorded.size() == 1);
   }
   CHECK(bench.delay.calls.empty());
   CHECK(gpio.recorded.empty());
}

TEST_CASE("A reply that does not fit the reply buffer is empty") {
   Bench bench;
   FakeBlock jtag(block::jtagRaw, 100);
   REQUIRE(bench.dispatcher.add(jtag));
   bench.setReplySize(150);
   CHECK(bench.runLimited(usbToAll({ { block::jtagRaw, { 0x28, 0x00, 0x00 } } }))->size() == 101);
   CHECK(bench.runLimited(usbToAll({ { block::jtagRaw, { 0x28, 0x00, 0x00, 0x28, 0x00, 0x00 } } })) == std::vector<uint8_t>{});
}

TEST_CASE("add() refuses a full table, a duplicate type and the types of the dispatcher") {
   Bench bench;
   FakeBlock gpio(block::gpio);
   FakeBlock gpio2(block::gpio);
   CHECK(bench.dispatcher.add(gpio));
   CHECK_FALSE(bench.dispatcher.add(gpio2));

   for(const uint8_t type : { block::delay, block::info, block::all, uint8_t{ 0x1F }, uint8_t{ 0x80 } }) {
      CAPTURE(int(type));
      FakeBlock wrong(type);
      CHECK_FALSE(bench.dispatcher.add(wrong));
   }

   std::vector<std::unique_ptr<FakeBlock>> blocks;
   for(uint8_t type = 0x30; blocks.size() < UsbToAll::maxBlocks - 1; ++type) {
      blocks.push_back(std::make_unique<FakeBlock>(type));
      REQUIRE(bench.dispatcher.add(*blocks.back()));
   }
   FakeBlock oneTooMany(0x50);
   CHECK_FALSE(bench.dispatcher.add(oneTooMany));
}

TEST_CASE("targetVoltage() is 0 without a source, else the voltage of the source") {
   class FixedVoltage final : public protocol::TargetVoltage {
      public:
      uint16_t millivolts() override {
         return 3300;
      }
   };

   Bench bench;
   CHECK(bench.dispatcher.targetVoltage() == 0);
   FixedVoltage source;
   bench.dispatcher.setTargetVoltage(&source);
   CHECK(bench.dispatcher.targetVoltage() == 3300);
   bench.dispatcher.setTargetVoltage(nullptr);
   CHECK(bench.dispatcher.targetVoltage() == 0);
}
