// SPDX-License-Identifier: GPL-3.0-or-later

#include "debug/LogSink.h"

#include <doctest/doctest.h>
#include <logging/Logger.h>
#include <string>
#include <string_view>

namespace {

// The CDC-ACM port as LogSink::drainTo() sees it.
struct FakePort {
   bool terminalOpen = true;
   std::size_t room = 1000;
   std::string sent;

   [[nodiscard]] bool isTerminalOpen() const {
      return terminalOpen;
   }

   [[nodiscard]] std::size_t writeAvailable() const {
      return room;
   }

   std::size_t write(art::span<const uint8_t> data) {
      const std::size_t count = data.size() < room ? data.size() : room;
      sent.append(reinterpret_cast<const char *>(data.data()), count);
      room -= count;
      return count;
   }
};

template <std::size_t Size>
void writeText(debug::LogSink<Size> &sink, std::string_view text) {
   const auto result = sink.write(art::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
   REQUIRE(result);
   CHECK(*result == text.size());
}

} // namespace

TEST_CASE("LogSink sends the buffered log when a terminal has the port open") {
   debug::LogSink<64> sink;
   FakePort port;

   writeText(sink, "hello\r\n");
   CHECK(sink.pending() == 7);

   sink.drainTo(port);
   CHECK(port.sent == "hello\r\n");
   CHECK(sink.pending() == 0);
}

TEST_CASE("LogSink keeps the log while no terminal has the port open") {
   debug::LogSink<64> sink;
   FakePort port;
   port.terminalOpen = false;

   writeText(sink, "boot\r\n");
   sink.drainTo(port);
   CHECK(port.sent.empty());
   CHECK(sink.pending() == 6);

   port.terminalOpen = true;
   sink.drainTo(port);
   CHECK(port.sent == "boot\r\n");
}

TEST_CASE("LogSink sends only what the port takes and keeps the rest") {
   debug::LogSink<64> sink;
   FakePort port;
   port.room = 3;

   writeText(sink, "abcdef");
   sink.drainTo(port);
   CHECK(port.sent == "abc");
   CHECK(sink.pending() == 3);

   port.room = 10;
   sink.drainTo(port);
   CHECK(port.sent == "abcdef");
   CHECK(sink.pending() == 0);
}

TEST_CASE("LogSink sends the data across the end of the ring buffer") {
   debug::LogSink<64> sink;
   FakePort port;

   // Move the read position near the end of the ring buffer
   writeText(sink, std::string(60, 'x'));
   sink.drainTo(port);
   port.sent.clear();

   writeText(sink, "0123456789");
   sink.drainTo(port);
   CHECK(port.sent == "0123456789");
}

TEST_CASE("LogSink drops the newest bytes when the ring buffer is full") {
   debug::LogSink<64> sink;
   static_assert(debug::LogSink<64>::capacity() == 63);

   writeText(sink, std::string(60, 'a'));
   // Only 3 bytes fit: the old bytes stay, the new bytes are dropped
   writeText(sink, "bcdefgh");
   CHECK(sink.pending() == 63);
   CHECK(sink.droppedBytes() == 4);

   // No room for the marker: the next write is dropped too
   writeText(sink, "ij");
   CHECK(sink.droppedBytes() == 6);
}

TEST_CASE("LogSink writes a marker with the dropped byte count when it has room again") {
   debug::LogSink<64> sink;
   FakePort port;

   writeText(sink, std::string(63, 'a'));
   writeText(sink, "lost");
   CHECK(sink.droppedBytes() == 4);

   sink.drainTo(port);
   writeText(sink, "next\r\n");
   CHECK(sink.droppedBytes() == 0);

   port.sent.clear();
   sink.drainTo(port);
   CHECK(port.sent == "\r\n[log: 4 bytes dropped]\r\nnext\r\n");
}

TEST_CASE("The libArT logger writes complete lines into the LogSink") {
   debug::LogSink<128> sink;
   FakePort port;
   logging::Logger logger{ "test" };
   auto &manager = LogManager::getInstance();
   manager.set_sink(&sink);

   LOG_INFO(logger, "value %u", 42u);
   manager.set_sink(nullptr);

   sink.drainTo(port);
   CHECK(port.sent == "[test][INFO] value 42\r\n");
}
