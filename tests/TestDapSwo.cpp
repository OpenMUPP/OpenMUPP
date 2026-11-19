// SPDX-License-Identifier: GPL-3.0-or-later
// The DAP_SWO_* commands of the CMSIS-DAP processor on a fake SWO receiver
// (protocol/cmsisdap/DapSwo.h), with the baud rates of the CH32V307 USART2.
// The last tests run the SWO trace endpoint of the CMSIS-DAP v2 function
// (usb/CmsisDapFunction.h) on the fake USB controller.

#include "protocol/cmsisdap/DapProcessor.h"
#include "swo/SwoRing.h"

#include "CmsisDapBench.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t pclk1 = 144'000'000;

// A receiver that the test fills, with the baud rates of the CH32V307
// USART2.
class FakeSwo final : public cmsisdap::DapSwo {
   public:
   [[nodiscard]] uint32_t capacity() const override {
      return 4096;
   }
   [[nodiscard]] bool hasMode(uint8_t mode) const override {
      return mode == cmsisdap::swoMode::uart || (manchester && mode == cmsisdap::swoMode::manchester);
   }
   // Manchester: up to 2 Mbit/s, the baud rate as requested
   [[nodiscard]] uint32_t realBaud(uint8_t mode, uint32_t baud) const override {
      if(!hasMode(mode)) {
         return 0;
      }
      if(mode == cmsisdap::swoMode::manchester) {
         return std::min<uint32_t>(baud, 2'000'000);
      }
      return swo::realBaud(pclk1, std::min(baud, pclk1 / 16));
   }
   uint32_t start(uint8_t mode, uint32_t baud) override {
      ++starts;
      startedMode = mode;
      const uint32_t real = realBaud(mode, baud);
      active_ = real != 0;
      startedBaud = real;
      data.clear();
      overrun = false;
      framingError = false;
      return real;
   }
   uint32_t setBaud(uint32_t baud) override {
      const uint32_t real = realBaud(startedMode, baud);
      if(!active_ || real == 0) {
         return 0;
      }
      startedBaud = real;
      return real;
   }
   void stop() override {
      active_ = false;
   }
   [[nodiscard]] bool active() const override {
      return active_;
   }
   [[nodiscard]] std::size_t read(art::span<uint8_t> out) override {
      const std::size_t count = std::min(out.size(), data.size());
      for(std::size_t i = 0; i < count; ++i) {
         out[i] = data.front();
         data.pop_front();
      }
      return count;
   }
   [[nodiscard]] Status status() override {
      return { active_, overrun, framingError, static_cast<uint32_t>(data.size()) };
   }
   void clearErrors() override {
      overrun = false;
      framingError = false;
   }

   std::deque<uint8_t> data;
   bool overrun = false;
   bool framingError = false;
   int starts = 0;
   uint8_t startedMode = 0;
   uint32_t startedBaud = 0;
   // The receiver has the Manchester mode
   bool manchester = false;

   private:
   bool active_ = false;
};

Bytes word(uint32_t value) {
   return { static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
            static_cast<uint8_t>(value >> 24) };
}

Bytes cat(std::initializer_list<Bytes> parts) {
   Bytes result;
   for(const Bytes &part : parts) {
      result.insert(result.end(), part.begin(), part.end());
   }
   return result;
}

// A JTAG port that does nothing: DAP_Connect to JTAG
class NullJtag final : public protocol::JtagPort {
   public:
   void init() override {
   }
   void fini() override {
   }
   [[nodiscard]] bool setSpeed(uint32_t) override {
      return true;
   }
   [[nodiscard]] bool shift(uint32_t, art::span<const uint8_t>, art::span<const uint8_t>, art::span<uint8_t>) override {
      return true;
   }
};

struct SwoBench {
   explicit SwoBench(bool withSwo = true, bool streaming = false)
      : info(mupptest::dapTestInfo())
      , dap(swd, gpio, platform, info, &jtag) {
      if(withSwo) {
         dap.setSwo(swo, streaming);
      }
   }

   Bytes run(const Bytes &request) {
      REQUIRE(dap.canSubmit());
      dap.submit(request);
      dap.run();
      REQUIRE(dap.hasResponse());
      const auto response = dap.response();
      Bytes result(response.begin(), response.end());
      dap.popResponse();
      return result;
   }

   // Transport DAP_SWO_Data, mode UART, 2 Mbaud, start
   void startCapture() {
      REQUIRE(run({ 0x17, 0x01 }) == Bytes{ 0x17, 0x00 });
      REQUIRE(run({ 0x18, 0x01 }) == Bytes{ 0x18, 0x00 });
      REQUIRE(run(cat({ { 0x19 }, word(2'000'000) })) == cat({ { 0x19 }, word(2'000'000) }));
      REQUIRE(run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0x00 });
      REQUIRE(swo.active());
   }

   void receive(std::size_t count) {
      for(std::size_t i = 0; i < count; ++i) {
         swo.data.push_back(static_cast<uint8_t>(i));
      }
   }

   NullJtag jtag;
   mupptest::FakeDapSwd swd;
   mupptest::FakeDapGpio gpio;
   mupptest::FakeDapPlatform platform;
   cmsisdap::DeviceInfo info;
   FakeSwo swo;
   cmsisdap::Processor dap;
};

} // namespace

TEST_CASE("Without a DapSwo the DAP_SWO_* commands are invalid, and DAP_Info reports no SWO") {
   SwoBench b(false);
   for(const uint8_t id : { 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1E }) {
      CHECK(b.run({ id, 0x00, 0x00, 0x00, 0x00 }) == Bytes{ 0xFF });
   }
   CHECK(b.run({ 0x00, 0xF0 }) == Bytes{ 0x00, 0x01, 0x13 });
   CHECK(b.run({ 0x00, 0xFD }) == Bytes{ 0x00, 0x00 });
}

TEST_CASE("DAP_Info reports SWO UART and the SWO buffer size") {
   SwoBench b;
   CHECK(b.run({ 0x00, 0xF0 }) == Bytes{ 0x00, 0x01, 0x17 });
   CHECK(b.run({ 0x00, 0xFD }) == cat({ { 0x00, 0x04 }, word(4096) }));
}

TEST_CASE("DAP_SWO_Transport accepts none and DAP_SWO_Data only") {
   SwoBench b;
   CHECK(b.run({ 0x17, 0x00 }) == Bytes{ 0x17, 0x00 });
   CHECK(b.run({ 0x17, 0x01 }) == Bytes{ 0x17, 0x00 });
   // The streaming endpoint is not there yet.
   CHECK(b.run({ 0x17, 0x02 }) == Bytes{ 0x17, 0xFF });
   CHECK(b.run({ 0x17, 0x03 }) == Bytes{ 0x17, 0xFF });
   CHECK(b.run({ 0x17 }) == Bytes{ 0xFF });
}

TEST_CASE("With a Manchester receiver, DAP_Info reports SWO Manchester and DAP_SWO_Mode accepts mode 2") {
   SwoBench b;
   b.swo.manchester = true;
   CHECK(b.run({ 0x00, 0xF0 }) == Bytes{ 0x00, 0x01, 0x1F });
   REQUIRE(b.run({ 0x17, 0x01 }) == Bytes{ 0x17, 0x00 });
   CHECK(b.run({ 0x18, 0x02 }) == Bytes{ 0x18, 0x00 });
   // The Manchester bit rate of the receiver, here up to 2 Mbit/s
   CHECK(b.run(cat({ { 0x19 }, word(3'000'000) })) == cat({ { 0x19 }, word(2'000'000) }));
   CHECK(b.run(cat({ { 0x19 }, word(1'000'000) })) == cat({ { 0x19 }, word(1'000'000) }));
   CHECK(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0x00 });
   CHECK(b.swo.startedMode == 2);
   CHECK(b.swo.startedBaud == 1'000'000);
   // A baud rate change keeps the mode
   CHECK(b.run(cat({ { 0x19 }, word(500'000) })) == cat({ { 0x19 }, word(500'000) }));
   CHECK(b.swo.startedBaud == 500'000);
   CHECK(b.swo.starts == 1);
   b.receive(2);
   CHECK(b.run({ 0x1C, 0x10, 0x00 }) == Bytes{ 0x1C, 0x01, 0x02, 0x00, 0x00, 0x01 });
}

TEST_CASE("DAP_Connect to JTAG stops the capture, and a start with the JTAG port gives an error") {
   SwoBench b;
   b.startCapture();
   CHECK(b.run({ 0x02, 0x02 }) == Bytes{ 0x02, 0x02 });
   CHECK_FALSE(b.swo.active());
   CHECK(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0xFF });
   CHECK(b.swo.starts == 1);
   // Back on SWD, the capture starts again with the stored settings.
   CHECK(b.run({ 0x02, 0x01 }) == Bytes{ 0x02, 0x01 });
   CHECK(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0x00 });
   CHECK(b.swo.active());
   CHECK(b.swo.startedBaud == 2'000'000);
}

TEST_CASE("DAP_SWO_Mode accepts off and UART only, and another mode sets mode off") {
   SwoBench b;
   CHECK(b.run({ 0x18, 0x00 }) == Bytes{ 0x18, 0x00 });
   CHECK(b.run({ 0x18, 0x01 }) == Bytes{ 0x18, 0x00 });
   CHECK(b.run(cat({ { 0x19 }, word(115'200) })) == cat({ { 0x19 }, word(115'200) }));
   CHECK(b.run({ 0x18, 0x02 }) == Bytes{ 0x18, 0xFF });
   // Mode off: no baud rate
   CHECK(b.run(cat({ { 0x19 }, word(115'200) })) == cat({ { 0x19 }, word(0) }));
   CHECK(b.run({ 0x18, 0x03 }) == Bytes{ 0x18, 0xFF });
}

TEST_CASE("DAP_SWO_Mode clears the baud rate") {
   SwoBench b;
   REQUIRE(b.run({ 0x18, 0x01 }) == Bytes{ 0x18, 0x00 });
   REQUIRE(b.run(cat({ { 0x19 }, word(115'200) })) == cat({ { 0x19 }, word(115'200) }));
   REQUIRE(b.run({ 0x18, 0x01 }) == Bytes{ 0x18, 0x00 });
   CHECK(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0xFF });
   CHECK(b.swo.starts == 0);
}

TEST_CASE("DAP_SWO_Baudrate replies the real baud rate of the USART divider") {
   SwoBench b;
   // Mode off: no baud rate
   CHECK(b.run(cat({ { 0x19 }, word(115'200) })) == cat({ { 0x19 }, word(0) }));
   REQUIRE(b.run({ 0x18, 0x01 }) == Bytes{ 0x18, 0x00 });
   CHECK(b.run(cat({ { 0x19 }, word(115'200) })) == cat({ { 0x19 }, word(115'200) }));
   CHECK(b.run(cat({ { 0x19 }, word(7'000'000) })) == cat({ { 0x19 }, word(6'857'143) }));
   // Above the maximum: the maximum
   CHECK(b.run(cat({ { 0x19 }, word(10'000'000) })) == cat({ { 0x19 }, word(9'000'000) }));
   CHECK(b.run(cat({ { 0x19 }, word(2197) })) == cat({ { 0x19 }, word(0) }));
   CHECK(b.run({ 0x19, 0x00, 0x00 }) == Bytes{ 0xFF });
   // The failed baud rate is stored: no start
   CHECK(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0xFF });
   CHECK(b.swo.starts == 0);
}

TEST_CASE("DAP_SWO_Control needs mode UART and a baud rate for a start") {
   SwoBench b;
   CHECK(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0xFF });
   REQUIRE(b.run({ 0x18, 0x01 }) == Bytes{ 0x18, 0x00 });
   CHECK(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0xFF });
   REQUIRE(b.run(cat({ { 0x19 }, word(4'500'000) })) == cat({ { 0x19 }, word(4'500'000) }));
   CHECK(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0x00 });
   CHECK(b.swo.active());
   CHECK(b.swo.startedBaud == 4'500'000);
   // A second start does not start again.
   CHECK(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0x00 });
   CHECK(b.swo.starts == 1);
   CHECK(b.run({ 0x1A, 0x00 }) == Bytes{ 0x1A, 0x00 });
   CHECK_FALSE(b.swo.active());
   CHECK(b.run({ 0x1A, 0x00 }) == Bytes{ 0x1A, 0x00 });
   CHECK(b.run({ 0x1A }) == Bytes{ 0xFF });
}

TEST_CASE("While the capture runs, DAP_SWO_Transport gives an error") {
   SwoBench b;
   b.startCapture();
   CHECK(b.run({ 0x17, 0x00 }) == Bytes{ 0x17, 0xFF });
   CHECK(b.swo.active());
}

TEST_CASE("While the capture runs, DAP_SWO_Baudrate changes the baud rate and the capture goes on") {
   SwoBench b;
   b.startCapture();
   b.receive(3);
   CHECK(b.run(cat({ { 0x19 }, word(115'200) })) == cat({ { 0x19 }, word(115'200) }));
   CHECK(b.swo.active());
   CHECK(b.swo.starts == 1);
   CHECK(b.swo.startedBaud == 115'200);
   CHECK(b.swo.data.size() == 3);
   // A baud rate out of range stops the capture.
   CHECK(b.run(cat({ { 0x19 }, word(100) })) == cat({ { 0x19 }, word(0) }));
   CHECK_FALSE(b.swo.active());
   CHECK(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0xFF });
}

TEST_CASE("While the capture runs, DAP_SWO_Mode stops the capture") {
   SwoBench b;
   b.startCapture();
   b.receive(3);
   CHECK(b.run({ 0x18, 0x01 }) == Bytes{ 0x18, 0x00 });
   CHECK_FALSE(b.swo.active());
   // The data stays readable.
   CHECK(b.run({ 0x1C, 0x10, 0x00 }) == Bytes{ 0x1C, 0x00, 0x03, 0x00, 0x00, 0x01, 0x02 });
}

TEST_CASE("DAP_SWO_Status replies the status and the count, and clears the error bits") {
   SwoBench b;
   CHECK(b.run({ 0x1B }) == cat({ { 0x1B, 0x00 }, word(0) }));
   b.startCapture();
   b.receive(5);
   b.swo.overrun = true;
   b.swo.framingError = true;
   CHECK(b.run({ 0x1B }) == cat({ { 0x1B, 0xC1 }, word(5) }));
   CHECK(b.run({ 0x1B }) == cat({ { 0x1B, 0x01 }, word(5) }));
}

TEST_CASE("DAP_SWO_ExtendedStatus replies the requested fields") {
   SwoBench b;
   b.startCapture();
   b.receive(3);
   b.swo.overrun = true;
   // No status field: the error bits stay.
   CHECK(b.run({ 0x1E, 0x00 }) == Bytes{ 0x1E });
   CHECK(b.run({ 0x1E, 0x02 }) == cat({ { 0x1E }, word(3) }));
   CHECK(b.run({ 0x1E, 0x01 }) == Bytes{ 0x1E, 0x01 | 0x80 });
   CHECK(b.run({ 0x1E, 0x01 }) == Bytes{ 0x1E, 0x01 });
   // Bit 2 (index and timestamp) is ignored.
   CHECK(b.run({ 0x1E, 0x07 }) == cat({ { 0x1E, 0x01 }, word(3) }));
   CHECK(b.run({ 0x1E }) == Bytes{ 0xFF });
}

TEST_CASE("DAP_SWO_Data gives the trace data") {
   SwoBench b;
   b.startCapture();
   b.receive(5);
   CHECK(b.run({ 0x1C, 0x03, 0x00 }) == Bytes{ 0x1C, 0x01, 0x03, 0x00, 0x00, 0x01, 0x02 });
   CHECK(b.run({ 0x1C, 0x10, 0x00 }) == Bytes{ 0x1C, 0x01, 0x02, 0x00, 0x03, 0x04 });
   CHECK(b.run({ 0x1C, 0x10, 0x00 }) == Bytes{ 0x1C, 0x01, 0x00, 0x00 });
   // The data stays readable after a stop.
   b.receive(1);
   REQUIRE(b.run({ 0x1A, 0x00 }) == Bytes{ 0x1A, 0x00 });
   CHECK(b.run({ 0x1C, 0x10, 0x00 }) == Bytes{ 0x1C, 0x00, 0x01, 0x00, 0x00 });
   CHECK(b.run({ 0x1C, 0x10 }) == Bytes{ 0xFF });
}

TEST_CASE("DAP_SWO_Data reads the status before the data, and clears the error bits") {
   SwoBench b;
   b.startCapture();
   b.receive(2);
   b.swo.framingError = true;
   CHECK(b.run({ 0x1C, 0x10, 0x00 }) == Bytes{ 0x1C, 0x41, 0x02, 0x00, 0x00, 0x01 });
   CHECK(b.run({ 0x1B }) == cat({ { 0x1B, 0x01 }, word(0) }));
   // With transport none too
   REQUIRE(b.run({ 0x1A, 0x00 }) == Bytes{ 0x1A, 0x00 });
   REQUIRE(b.run({ 0x17, 0x00 }) == Bytes{ 0x17, 0x00 });
   b.swo.overrun = true;
   CHECK(b.run({ 0x1C, 0x10, 0x00 }) == Bytes{ 0x1C, 0x80, 0x00, 0x00 });
   CHECK(b.run({ 0x1B }) == cat({ { 0x1B, 0x00 }, word(0) }));
}

TEST_CASE("DAP_SWO_Data gives at most the packet size - 4 bytes") {
   SwoBench b;
   b.startCapture();
   b.receive(1000);
   Bytes response = b.run({ 0x1C, 0xFF, 0xFF });
   REQUIRE(response.size() == 64);
   CHECK(response[2] == 60);
   CHECK(response[3] == 0);
   CHECK(response[63] == 59);

   b.dap.hostGone();
   b.dap.setPacketSize(512);
   b.startCapture();
   b.receive(1000);
   response = b.run({ 0x1C, 0xFF, 0xFF });
   REQUIRE(response.size() == 512);
   CHECK(response[2] == static_cast<uint8_t>(508));
   CHECK(response[3] == 508 >> 8);
}

TEST_CASE("DAP_SWO_Data with transport none gives no data") {
   SwoBench b;
   b.startCapture();
   REQUIRE(b.run({ 0x1A, 0x00 }) == Bytes{ 0x1A, 0x00 });
   REQUIRE(b.run({ 0x17, 0x00 }) == Bytes{ 0x17, 0x00 });
   b.receive(4);
   CHECK(b.run({ 0x1C, 0x10, 0x00 }) == Bytes{ 0x1C, 0x00, 0x00, 0x00 });
   CHECK(b.swo.data.size() == 4);
}

TEST_CASE("DAP_SWO_* commands in DAP_ExecuteCommands") {
   SwoBench b;
   b.startCapture();
   b.receive(2);
   CHECK(b.run({ 0x7F, 0x02, 0x1B, 0x1C, 0x08, 0x00 }) ==
         cat({ { 0x7F, 0x02, 0x1B, 0x01 }, word(2), { 0x1C, 0x01, 0x02, 0x00, 0x00, 0x01 } }));
}

TEST_CASE("hostGone stops the capture and resets the SWO settings, DAP_Disconnect does not") {
   SwoBench b;
   b.startCapture();
   CHECK(b.run({ 0x03 }) == Bytes{ 0x03, 0x00 });
   CHECK(b.swo.active());
   b.dap.hostGone();
   CHECK_FALSE(b.swo.active());
   // Mode off and baud rate 0: a start fails.
   CHECK(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0xFF });
   REQUIRE(b.run({ 0x18, 0x01 }) == Bytes{ 0x18, 0x00 });
   CHECK(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0xFF });
   // Transport none
   REQUIRE(b.run(cat({ { 0x19 }, word(2'000'000) })) == cat({ { 0x19 }, word(2'000'000) }));
   REQUIRE(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0x00 });
   b.receive(2);
   CHECK(b.run({ 0x1C, 0x10, 0x00 }) == Bytes{ 0x1C, 0x01, 0x00, 0x00 });
}

TEST_CASE("Without the SWO trace endpoint, DAP_SWO_Transport 2 gives an error") {
   SwoBench b;
   CHECK(b.run({ 0x17, 0x02 }) == Bytes{ 0x17, 0xFF });
   std::array<uint8_t, 8> data{};
   b.receive(4);
   CHECK(b.dap.readSwoStream(data) == 0);
}

TEST_CASE("With the SWO trace endpoint, DAP_Info reports SWO streaming and DAP_SWO_Transport 2 works") {
   SwoBench b(true, true);
   CHECK(b.run({ 0x00, 0xF0 }) == Bytes{ 0x00, 0x01, 0x57 });
   CHECK(b.run({ 0x17, 0x02 }) == Bytes{ 0x17, 0x00 });
   REQUIRE(b.run({ 0x18, 0x01 }) == Bytes{ 0x18, 0x00 });
   REQUIRE(b.run(cat({ { 0x19 }, word(2'000'000) })) == cat({ { 0x19 }, word(2'000'000) }));
   REQUIRE(b.run({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0x00 });
   b.receive(5);
   // DAP_SWO_Data gives no data with transport 2.
   CHECK(b.run({ 0x1C, 0x10, 0x00 }) == Bytes{ 0x1C, 0x01, 0x00, 0x00 });
   std::array<uint8_t, 3> data{};
   CHECK(b.dap.readSwoStream(data) == 3);
   CHECK(data == std::array<uint8_t, 3>{ 0x00, 0x01, 0x02 });
   // Transport 2 cannot change during the capture.
   CHECK(b.run({ 0x17, 0x01 }) == Bytes{ 0x17, 0xFF });
   REQUIRE(b.run({ 0x1A, 0x00 }) == Bytes{ 0x1A, 0x00 });
   REQUIRE(b.run({ 0x17, 0x01 }) == Bytes{ 0x17, 0x00 });
   CHECK(b.dap.readSwoStream(data) == 0);
}

namespace {

constexpr uint8_t swoIn = usb::dap::swoEndpoint;

// The CMSIS-DAP v2 function with the SWO trace endpoint, on the fake
// controller, with the fake receiver
struct StreamBench : mupptest::DapBenchT<true> {
   explicit StreamBench(bool highSpeed = false) {
      processor.setSwo(swo, true);
      controller().setHighSpeed(highSpeed);
      enumerate();
   }

   Bytes command(const Bytes &request) {
      REQUIRE(write(request));
      const auto response = read();
      REQUIRE(response.has_value());
      mainLoop();
      return *response;
   }

   // Transport 2, mode UART, 2 Mbaud, start
   void startStream() {
      REQUIRE(command({ 0x17, 0x02 }) == Bytes{ 0x17, 0x00 });
      REQUIRE(command({ 0x18, 0x01 }) == Bytes{ 0x18, 0x00 });
      REQUIRE(command(cat({ { 0x19 }, word(2'000'000) })) == cat({ { 0x19 }, word(2'000'000) }));
      REQUIRE(command({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0x00 });
   }

   void receive(std::size_t count, uint8_t first = 0) {
      for(std::size_t i = 0; i < count; ++i) {
         swo.data.push_back(static_cast<uint8_t>(first + i));
      }
   }

   // The host reads one packet of the trace endpoint, then the main loop runs.
   std::optional<Bytes> trace() {
      auto packet = controller().takeIn(swoIn);
      mainLoop();
      return packet;
   }

   FakeSwo swo;
};

Bytes sequence(std::size_t count, uint8_t first = 0) {
   Bytes result(count);
   for(std::size_t i = 0; i < count; ++i) {
      result[i] = static_cast<uint8_t>(first + i);
   }
   return result;
}

} // namespace

TEST_CASE("The SWO trace endpoint sends the trace data of transport 2") {
   for(const bool highSpeed : { false, true }) {
      CAPTURE(highSpeed);
      StreamBench b(highSpeed);
      const std::size_t packetSize = highSpeed ? 512 : 64;
      CHECK_FALSE(b.trace().has_value());
      b.startStream();
      b.receive(packetSize + 10);
      b.mainLoop();
      // One full packet, then the rest at once after the acknowledge
      CHECK(b.trace() == sequence(packetSize));
      CHECK(b.trace() == sequence(10, static_cast<uint8_t>(packetSize)));
      CHECK_FALSE(b.trace().has_value());
      // The data after a stop goes out too.
      REQUIRE(b.command({ 0x1A, 0x00 }) == Bytes{ 0x1A, 0x00 });
      b.receive(3, 0x40);
      b.mainLoop();
      CHECK(b.trace() == sequence(3, 0x40));
   }
}

TEST_CASE("The SWO trace endpoint sends nothing with transport DAP_SWO_Data") {
   StreamBench b;
   REQUIRE(b.command({ 0x17, 0x01 }) == Bytes{ 0x17, 0x00 });
   REQUIRE(b.command({ 0x18, 0x01 }) == Bytes{ 0x18, 0x00 });
   REQUIRE(b.command(cat({ { 0x19 }, word(2'000'000) })) == cat({ { 0x19 }, word(2'000'000) }));
   REQUIRE(b.command({ 0x1A, 0x01 }) == Bytes{ 0x1A, 0x00 });
   b.receive(4);
   b.mainLoop();
   CHECK_FALSE(b.trace().has_value());
   CHECK(b.command({ 0x1C, 0x10, 0x00 }) == Bytes{ 0x1C, 0x01, 0x04, 0x00, 0x00, 0x01, 0x02, 0x03 });
}

TEST_CASE("The SWO trace packet goes out again after a halt clear") {
   StreamBench b;
   b.startStream();
   b.receive(5);
   b.mainLoop();
   b.request(0x02, 3, 0, swoIn); // SET_FEATURE(ENDPOINT_HALT)
   CHECK_FALSE(b.trace().has_value());
   b.request(0x02, 1, 0, swoIn); // CLEAR_FEATURE(ENDPOINT_HALT)
   b.mainLoop();
   CHECK(b.trace() == sequence(5));
   CHECK_FALSE(b.trace().has_value());
}

TEST_CASE("A bus reset drops the SWO trace packet and stops the capture") {
   StreamBench b;
   b.startStream();
   b.receive(5);
   b.mainLoop();
   b.enumerate();
   CHECK_FALSE(b.swo.active());
   b.receive(3);
   b.mainLoop();
   // Transport none after hostGone()
   CHECK_FALSE(b.trace().has_value());
}
