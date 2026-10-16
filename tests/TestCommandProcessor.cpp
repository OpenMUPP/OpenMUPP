// The command processor: the common Versaloon commands and the hand-over
// of the USB_TO_XXX commands.

#include "boot/BootloaderEntry.h"
#include "protocol/versaloon/CommandProcessor.h"
#include "protocol/versaloon/Versaloon.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using protocol::CommandProcessor;
using protocol::DeviceInfo;

constexpr DeviceInfo testInfo{ .version = "v1.2.3", .board = "mini", .hardwareId = 0x15 };

// Records the commands and replies with replySize bytes 0xA5.
class FakeUsbToXxx final : public protocol::UsbToXxxHandler {
   public:
   std::optional<std::size_t> process(art::span<const uint8_t> command, art::span<uint8_t> reply) override {
      commands.emplace_back(command.begin(), command.end());
      if(replySize) {
         std::fill_n(reply.begin(), *replySize, 0xA5);
      }
      return replySize;
   }

   uint16_t targetVoltage() override {
      return voltage;
   }

   std::vector<std::vector<uint8_t>> commands;
   std::optional<std::size_t> replySize;
   uint16_t voltage{ 0 };
};

class Bench {
   public:
   explicit Bench(boot::BootloaderEntry *bootloader = nullptr, protocol::UsbToXxxHandler *usbToXxx = nullptr)
      : processor(testInfo, bootloader, usbToXxx) {
   }

   // Runs the command. Returns the reply, or no value for no reply.
   std::optional<std::vector<uint8_t>> run(const std::vector<uint8_t> &command) {
      const auto size = processor.process(art::span<const uint8_t>(command.data(), command.size()),
                                          art::span<uint8_t>(reply_.data(), reply_.size()));
      if(!size) {
         return std::nullopt;
      }
      REQUIRE(*size <= reply_.size());
      return std::vector<uint8_t>(reply_.begin(), reply_.begin() + static_cast<std::ptrdiff_t>(*size));
   }

   CommandProcessor processor;

   private:
   std::array<uint8_t, protocol::bufferSize> reply_{};
};

} // namespace

TEST_CASE("GET_INFO replies the buffer size and the version text") {
   Bench bench;
   const auto reply = bench.run({ protocol::getInfo });
   REQUIRE(reply.has_value());
   REQUIRE(reply->size() >= 3);
   CHECK((*reply)[0] == (protocol::bufferSize & 0xFF));
   CHECK((*reply)[1] == (protocol::bufferSize >> 8));
   CHECK(reply->back() == 0);
   const std::string text(reply->begin() + 2, reply->end() - 1);
   CHECK(text == "versaloon-ng v1.2.3, board mini");
}

TEST_CASE("GET_INFO limits the version text") {
   const std::string longVersion(300, 'x');
   const DeviceInfo info{ .version = longVersion, .board = "mini", .hardwareId = 0x15 };
   CommandProcessor processor(info, nullptr, nullptr);
   std::array<uint8_t, protocol::bufferSize> reply{};
   const uint8_t command = protocol::getInfo;
   const auto size = processor.process(art::span<const uint8_t>(&command, 1), art::span<uint8_t>(reply.data(), reply.size()));
   REQUIRE(size.has_value());
   CHECK(*size == 2 + CommandProcessor::maxInfoText + 1);
   // The host reads into 256 bytes and adds a NUL after the reply.
   CHECK(*size < 256);
   CHECK(reply[*size - 1] == 0);
   CHECK(reply[*size - 2] == 'x');
}

TEST_CASE("GET_TVCC replies 0 mV without a handler") {
   Bench bench;
   CHECK(bench.run({ protocol::getTvcc }) == std::vector<uint8_t>{ 0x00, 0x00 });
}

TEST_CASE("GET_TVCC replies the target voltage of the handler") {
   FakeUsbToXxx handler;
   Bench bench(nullptr, &handler);
   handler.voltage = 3291;
   CHECK(bench.run({ protocol::getTvcc }) == std::vector<uint8_t>{ 0xDB, 0x0C });
   handler.voltage = 0;
   CHECK(bench.run({ protocol::getTvcc }) == std::vector<uint8_t>{ 0x00, 0x00 });
}

TEST_CASE("GET_HARDWARE replies the hardware ID") {
   Bench bench;
   CHECK(bench.run({ protocol::getHardware }) == std::vector<uint8_t>{ 0x15 });
}

TEST_CASE("FW_UPDATE with the key replies and requests the bootloader") {
   boot::BootloaderEntry bootloader;
   Bench bench(&bootloader);
   CHECK(bench.run({ protocol::fwUpdate, protocol::fwUpdateKey }) == std::vector<uint8_t>{ protocol::fwUpdateAccepted });
   CHECK(bootloader.isRequested());
}

TEST_CASE("FW_UPDATE without the key is refused") {
   boot::BootloaderEntry bootloader;
   Bench bench(&bootloader);
   CHECK(bench.run({ protocol::fwUpdate, 0x55 }) == std::vector<uint8_t>{ protocol::fwUpdateRefused });
   CHECK(bench.run({ protocol::fwUpdate }) == std::vector<uint8_t>{ protocol::fwUpdateRefused });
   CHECK_FALSE(bootloader.isRequested());
}

TEST_CASE("FW_UPDATE is refused without a bootloader") {
   Bench bench;
   CHECK(bench.run({ protocol::fwUpdate, protocol::fwUpdateKey }) == std::vector<uint8_t>{ protocol::fwUpdateRefused });
}

TEST_CASE("Unknown commands have no reply") {
   Bench bench;
   for(const uint8_t command : { 0x03, 0x0D, 0x10, 0x1F, 0x80, 0xFF }) {
      CAPTURE(int(command));
      CHECK_FALSE(bench.run({ command }).has_value());
   }
   CHECK_FALSE(bench.run({}).has_value());
}

TEST_CASE("USB_TO_XXX commands have no reply without a handler") {
   Bench bench;
   CHECK_FALSE(bench.run({ protocol::usbToXxxLast, 0x03, 0x00 }).has_value());
}

TEST_CASE("USB_TO_XXX commands go to the handler") {
   FakeUsbToXxx handler;
   Bench bench(nullptr, &handler);

   const std::vector<uint8_t> command{ protocol::usbToXxxLast, 0x05, 0x00, 0x11, 0x22 };
   handler.replySize = 3;
   CHECK(bench.run(command) == std::vector<uint8_t>{ 0xA5, 0xA5, 0xA5 });
   handler.replySize = std::nullopt;
   CHECK_FALSE(bench.run({ protocol::usbToXxxFirst, 0x03, 0x00 }).has_value());

   REQUIRE(handler.commands.size() == 2);
   CHECK(handler.commands[0] == command);
   CHECK(handler.commands[1] == std::vector<uint8_t>{ protocol::usbToXxxFirst, 0x03, 0x00 });

   // The common commands do not go to the handler.
   CHECK(bench.run({ protocol::getHardware }) == std::vector<uint8_t>{ 0x15 });
   CHECK(handler.commands.size() == 2);
}
