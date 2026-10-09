// The CMSIS-DAP command processor on the SWD bit-bang engine, with the
// mock SWD target of MockSwdPins.h, a fake GPIO and a fake platform.

#include "protocol/cmsisdap/BitBangDapSwd.h"
#include "protocol/cmsisdap/DapProcessor.h"

#include "MockSwdPins.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace {

namespace gpio = protocol::gpio;
namespace pswd = protocol::swd;
using Bytes = std::vector<uint8_t>;
using cmsisdap::BitBangDapSwd;
using cmsisdap::Processor;
using protocol::PinMode;
using vsltest::MockSwdPins;

// The DAP_Transfer request bytes
constexpr uint8_t readDpidr = 0x02;
constexpr uint8_t readCtrlStat = 0x06;
constexpr uint8_t writeSelect = 0x08;
constexpr uint8_t writeAbort = 0x00;
constexpr uint8_t writeTar = 0x05;
constexpr uint8_t readApDrw = 0x0F;
constexpr uint8_t readRdbuff = 0x0E;
constexpr uint8_t matchValue = 0x10;
constexpr uint8_t matchMask = 0x20;

// The request on the wire
constexpr uint8_t wire(uint8_t request) {
   return pswd::encodeRequest(static_cast<uint8_t>((request & 0x0F) << 1));
}

// A DP read is 8 + turnaround + 3 + 32 + 1 + turnaround edges
constexpr std::size_t dataTransferEdges = 8 + 1 + 3 + 33 + 1;

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

Bytes text(const std::string &value) {
   Bytes result{ static_cast<uint8_t>(value.size() + 1) };
   result.insert(result.end(), value.begin(), value.end());
   result.push_back(0);
   return result;
}

// Pins with a mode and a level each. An input reads the external level
// (inputs), and an open-drain output reads low when either side is low.
class FakeGpio final : public protocol::GpioPins {
   public:
   struct State {
      PinMode mode{ PinMode::Input };
      bool level{ false };
   };

   explicit FakeGpio(uint16_t available = gpio::all & ~(gpio::usr1 | gpio::usr2))
      : available_(available) {
   }

   [[nodiscard]] uint16_t available() const override {
      return available_;
   }

   void configure(uint16_t bit, PinMode mode, bool level) override {
      REQUIRE((bit & available_) == bit);
      pins[indexOf(bit)] = { mode, level };
      configured.emplace_back(bit, mode, level);
   }

   void write(uint16_t mask, uint16_t value) override {
      for(uint16_t bit = 1; bit <= gpio::tms; bit = static_cast<uint16_t>(bit << 1)) {
         if(mask & bit) {
            pins[indexOf(bit)].level = (value & bit) != 0;
         }
      }
   }

   [[nodiscard]] uint16_t read(uint16_t mask) const override {
      REQUIRE((mask & available_) == mask);
      uint16_t value = 0;
      for(uint16_t bit = 1; bit <= gpio::tms; bit = static_cast<uint16_t>(bit << 1)) {
         const State &state = pins[indexOf(bit)];
         const bool external = (inputs & bit) != 0;
         bool level = external;
         if(state.mode == PinMode::PushPull) {
            level = state.level;
         } else if(state.mode == PinMode::OpenDrain) {
            level = state.level && external;
         }
         if((mask & bit) && level) {
            value = static_cast<uint16_t>(value | bit);
         }
      }
      return value;
   }

   [[nodiscard]] const State &pin(uint16_t bit) const {
      return pins[indexOf(bit)];
   }

   // The external levels
   uint16_t inputs{ gpio::all };
   std::vector<std::tuple<uint16_t, PinMode, bool>> configured;

   private:
   static std::size_t indexOf(uint16_t bit) {
      std::size_t index = 0;
      while(bit > 1) {
         bit = static_cast<uint16_t>(bit >> 1);
         ++index;
      }
      return index;
   }

   uint16_t available_;
   std::array<State, 9> pins{};
};

// Each call of nowUs() is one microsecond.
class FakePlatform final : public cmsisdap::Platform {
   public:
   uint32_t nowUs() override {
      return time++;
   }
   void delayUs(uint32_t microseconds) override {
      delays.push_back(microseconds);
      time += microseconds;
   }
   void setLed(cmsisdap::Led led, bool on) override {
      leds.emplace_back(led, on);
   }

   uint32_t time{ 0xFFFFFF00u };
   std::vector<uint32_t> delays;
   std::vector<std::pair<cmsisdap::Led, bool>> leds;
};

cmsisdap::DeviceInfo deviceInfo() {
   cmsisdap::DeviceInfo info;
   info.vendor = "Vendor";
   info.product = "Product";
   info.serialNumber = "0123456789AB";
   info.firmwareVersion = "1.2.3";
   return info;
}

struct Bench {
   explicit Bench(uint8_t turnaround = 1, uint16_t gpioAvailable = gpio::all & ~(gpio::usr1 | gpio::usr2))
      : pins(turnaround)
      , port(pins)
      , swd(port)
      , gpio(gpioAvailable)
      , info(deviceInfo())
      , dap(swd, gpio, platform, info) {
      // The mock target skips the data phase after WAIT and FAULT, as the
      // default of DAP_SWD_Configure.
      pins.dataPhase = false;
   }

   Bytes run(const Bytes &request) {
      REQUIRE(dap.canSubmit());
      dap.submit(request);
      REQUIRE(dap.hasResponse());
      return pop();
   }

   Bytes pop() {
      const auto response = dap.response();
      Bytes result(response.begin(), response.end());
      dap.popResponse();
      return result;
   }

   void connect() {
      CHECK(run({ 0x02, 0x01 }) == Bytes{ 0x02, 0x01 });
   }

   // The requests that the target saw, as DAP request bytes
   [[nodiscard]] std::vector<uint8_t> seen() const {
      std::vector<uint8_t> result;
      for(const auto &transfer : pins.transfers) {
         for(unsigned request = 0; request < 16; ++request) {
            if(wire(static_cast<uint8_t>(request)) == transfer.request) {
               result.push_back(static_cast<uint8_t>(request));
            }
         }
      }
      return result;
   }

   void checkIdle() const {
      CHECK(pins.contentions == 0);
      CHECK(pins.undrivenSamples == 0);
      CHECK(pins.desyncs == 0);
   }

   MockSwdPins pins;
   swd::BitBangPort<MockSwdPins> port;
   BitBangDapSwd<swd::BitBangPort<MockSwdPins>> swd;
   FakeGpio gpio;
   FakePlatform platform;
   cmsisdap::DeviceInfo info;
   Processor dap;
};

using V = std::vector<uint8_t>;

} // namespace

TEST_CASE("DAP_Info: strings, capabilities and packet size") {
   Bench b;
   CHECK(b.run({ 0x00, 0x01 }) == cat({ { 0x00 }, text("Vendor") }));
   CHECK(b.run({ 0x00, 0x02 }) == cat({ { 0x00 }, text("Product") }));
   CHECK(b.run({ 0x00, 0x03 }) == cat({ { 0x00 }, text("0123456789AB") }));
   CHECK(b.run({ 0x00, 0x04 }) == cat({ { 0x00 }, text("2.1.0") }));
   CHECK(b.run({ 0x00, 0x09 }) == cat({ { 0x00 }, text("1.2.3") }));
   CHECK(b.run({ 0x00, 0xF0 }) == V{ 0x00, 0x01, 0x11 });
   CHECK(b.run({ 0x00, 0xFE }) == V{ 0x00, 0x01, 0x04 });
   CHECK(b.run({ 0x00, 0xFF }) == V{ 0x00, 0x02, 0x40, 0x00 });
}

TEST_CASE("DAP_Info: information that is not available has length 0") {
   Bench b;
   for(const uint8_t id : { 0x05, 0x06, 0x07, 0x08, 0xF1, 0xFB, 0xFC, 0xFD, 0x42 }) {
      CHECK(b.run({ 0x00, id }) == V{ 0x00, 0x00 });
   }
   b.info.vendor = nullptr;
   Processor empty(b.swd, b.gpio, b.platform, b.info);
   empty.submit(V{ 0x00, 0x01 });
   CHECK(Bytes(empty.response().begin(), empty.response().end()) == V{ 0x00, 0x00 });
}

TEST_CASE("DAP_Info: a long string is cut, and a second capability byte is sent") {
   Bench b;
   const std::string longText(100, 'x');
   b.info.product = longText.c_str();
   b.info.capabilities = 0x0191;
   Processor dap(b.swd, b.gpio, b.platform, b.info);
   dap.submit(V{ 0x00, 0x02 });
   CHECK(dap.response().size() == 64);
   CHECK(dap.response()[1] == 62);
   CHECK(dap.response()[63] == 0);
   dap.popResponse();
   dap.submit(V{ 0x00, 0xF0 });
   CHECK(Bytes(dap.response().begin(), dap.response().end()) == V{ 0x00, 0x02, 0x91, 0x01 });
}

TEST_CASE("DAP_HostStatus sets the LEDs") {
   Bench b;
   CHECK(b.run({ 0x01, 0x00, 0x01 }) == V{ 0x01, 0x00 });
   CHECK(b.run({ 0x01, 0x01, 0x01 }) == V{ 0x01, 0x00 });
   CHECK(b.run({ 0x01, 0x00, 0x00 }) == V{ 0x01, 0x00 });
   CHECK(b.platform.leds.size() == 3);
   CHECK(b.platform.leds[0] == std::pair{ cmsisdap::Led::Connected, true });
   CHECK(b.platform.leds[1] == std::pair{ cmsisdap::Led::Running, true });
   CHECK(b.platform.leds[2] == std::pair{ cmsisdap::Led::Connected, false });
   CHECK(b.run({ 0x01, 0x02, 0x01 }) == V{ 0x01, 0x01 });
   CHECK(b.platform.leds.size() == 3);
}

TEST_CASE("DAP_Connect and DAP_Disconnect") {
   Bench b;
   CHECK_FALSE(b.dap.connected());
   // JTAG is not available yet.
   CHECK(b.run({ 0x02, 0x02 }) == V{ 0x02, 0x00 });
   CHECK(b.pins.inits == 0);
   // The default port is SWD.
   CHECK(b.run({ 0x02, 0x00 }) == V{ 0x02, 0x01 });
   CHECK(b.dap.connected());
   CHECK(b.pins.inits == 1);
   // Connect again: the pins are released and driven again.
   CHECK(b.run({ 0x02, 0x01 }) == V{ 0x02, 0x01 });
   CHECK(b.pins.inits == 2);
   CHECK(b.pins.finis == 1);
   CHECK(b.run({ 0x03 }) == V{ 0x03, 0x00 });
   CHECK_FALSE(b.dap.connected());
   CHECK(b.pins.finis == 2);
   // Disconnect again does not touch the pins.
   CHECK(b.run({ 0x03 }) == V{ 0x03, 0x00 });
   CHECK(b.pins.finis == 2);
}

TEST_CASE("DAP_Delay waits the microseconds") {
   Bench b;
   CHECK(b.run({ 0x09, 0x10, 0x27 }) == V{ 0x09, 0x00 });
   CHECK(b.platform.delays == std::vector<uint32_t>{ 10000 });
}

TEST_CASE("DAP_ResetTarget pulses SRST") {
   Bench b;
   CHECK(b.run({ 0x0A }) == V{ 0x0A, 0x00, 0x01 });
   REQUIRE(b.gpio.configured.size() == 2);
   CHECK(b.gpio.configured[0] == std::tuple{ gpio::srst, PinMode::OpenDrain, false });
   CHECK(b.gpio.configured[1] == std::tuple{ gpio::srst, PinMode::OpenDrain, true });
   CHECK(b.platform.delays == std::vector<uint32_t>{ Processor::resetPulseUs });

   // A board without SRST: the host has to reset the target.
   Bench noSrst(1, gpio::all & ~(gpio::usr1 | gpio::usr2 | gpio::srst));
   CHECK(noSrst.run({ 0x0A }) == V{ 0x0A, 0x00, 0x00 });
   CHECK(noSrst.gpio.configured.empty());
}

TEST_CASE("DAP_SWJ_Pins sets and reads the pins") {
   Bench b;
   // Assert nRESET (bit 7), drive SWCLK high (bit 0), TDI low (bit 2)
   const Bytes reply = b.run(cat({ { 0x10, 0x01, 0x85 }, word(0) }));
   CHECK(b.gpio.pin(gpio::srst).mode == PinMode::OpenDrain);
   CHECK_FALSE(b.gpio.pin(gpio::srst).level);
   CHECK(b.gpio.pin(gpio::tck).mode == PinMode::PushPull);
   CHECK(b.gpio.pin(gpio::tck).level);
   CHECK_FALSE(b.gpio.pin(gpio::tdi).level);
   // The other pins stay as they are. nRESET reads low.
   CHECK(b.gpio.configured.size() == 3);
   REQUIRE(reply.size() == 2);
   CHECK(reply[0] == 0x10);
   CHECK((reply[1] & 0x85) == 0x01);

   // Release nTRST (bit 5): open-drain high. TDO (bit 3) reads the target.
   b.gpio.inputs = static_cast<uint16_t>(b.gpio.inputs & ~gpio::tdo);
   const Bytes released = b.run(cat({ { 0x10, 0x20, 0x20 }, word(0) }));
   CHECK((released[1] & 0x28) == 0x20);
   // TDO is an input: a select bit does not change it.
   b.gpio.configured.clear();
   b.run(cat({ { 0x10, 0x08, 0x08 }, word(0) }));
   CHECK(b.gpio.configured.empty());
}

TEST_CASE("DAP_SWJ_Pins waits for the pins") {
   Bench b;
   // The target holds nRESET low
   b.gpio.inputs = static_cast<uint16_t>(b.gpio.inputs & ~gpio::srst);
   const uint32_t start = b.platform.time;
   const Bytes held = b.run(cat({ { 0x10, 0x80, 0x80 }, word(100) }));
   CHECK((held[1] & 0x80) == 0);
   CHECK(b.platform.time - start >= 100);
   CHECK(b.platform.time - start < 200);

   // The level is reached: no wait
   b.gpio.inputs = gpio::all;
   const uint32_t again = b.platform.time;
   const Bytes released = b.run(cat({ { 0x10, 0x80, 0x80 }, word(100) }));
   CHECK((released[1] & 0x80) != 0);
   CHECK(b.platform.time - again < 10);

   // The wait is at most 3 s.
   b.gpio.inputs = static_cast<uint16_t>(b.gpio.inputs & ~gpio::srst);
   const uint32_t limit = b.platform.time;
   b.run(cat({ { 0x10, 0x80, 0x80 }, word(0xFFFFFFFFu) }));
   CHECK(b.platform.time - limit >= Processor::maxPinWaitUs);
   CHECK(b.platform.time - limit < Processor::maxPinWaitUs + 100);
}

TEST_CASE("DAP_SWJ_Clock sets the clock in Hz") {
   Bench b;
   CHECK(b.port.halfCycles() == swd::halfPeriodCyclesOfHz(72'000'000, Processor::defaultClockHz));
   CHECK(b.run(cat({ { 0x11 }, word(100'000) })) == V{ 0x11, 0x00 });
   CHECK(b.port.halfCycles() == 360);
   CHECK(b.port.isTimed());
   // The fastest rate is untimed.
   CHECK(b.run(cat({ { 0x11 }, word(10'000'000) })) == V{ 0x11, 0x00 });
   CHECK_FALSE(b.port.isTimed());
   // 0 Hz is an error, and the clock stays.
   CHECK(b.run(cat({ { 0x11 }, word(0) })) == V{ 0x11, 0x01 });
   CHECK_FALSE(b.port.isTimed());
}

TEST_CASE("The clock does not depend on the order of the configuration commands") {
   Bench b;
   b.run(cat({ { 0x11 }, word(1'000'000) }));
   const uint32_t half = b.port.halfCycles();
   CHECK(half == 36);
   CHECK(b.run({ 0x13, 0x02 }) == V{ 0x13, 0x00 });
   CHECK(b.run({ 0x04, 0x03, 0x05, 0x00, 0x02, 0x00 }) == V{ 0x04, 0x00 });
   CHECK(b.port.halfCycles() == half);
   CHECK(b.port.isTimed());
}

TEST_CASE("DAP_SWJ_Sequence") {
   Bench b;
   // Not connected
   CHECK(b.run({ 0x12, 0x08, 0xFF }) == V{ 0x12, 0x01 });
   CHECK(b.pins.risingEdges.empty());

   b.connect();
   // A line reset of 51 cycles
   CHECK(b.run({ 0x12, 51, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07 }) == V{ 0x12, 0x00 });
   CHECK(b.pins.risingEdges.size() == 51);
   CHECK(b.pins.lineResets >= 1);

   // The first bit is the LSB of the first byte.
   b.pins.risingEdges.clear();
   CHECK(b.run({ 0x12, 0x08, 0xA5 }) == V{ 0x12, 0x00 });
   REQUIRE(b.pins.risingEdges.size() == 8);
   for(unsigned i = 0; i < 8; ++i) {
      CHECK(b.pins.risingEdges[i].level == (((0xA5 >> i) & 1) != 0));
   }

   // A count of 0 is 256 bits.
   b.pins.risingEdges.clear();
   Bytes request{ 0x12, 0x00 };
   request.resize(34, 0x55);
   CHECK(b.run(request) == V{ 0x12, 0x00 });
   CHECK(b.pins.risingEdges.size() == 256);

   // Too little data
   CHECK(b.run({ 0x12, 16, 0xFF }) == V{ 0xFF });
}

TEST_CASE("DAP_SWD_Sequence") {
   Bench b;
   b.pins.decodes = false;
   // Not connected: error, and no data
   CHECK(b.run({ 0x1D, 0x01, 0x84 }) == V{ 0x1D, 0x01 });
   CHECK(b.pins.risingEdges.empty());

   b.connect();
   // Out 8 bits, in 4 bits, in 0 = 64 bits, out 64 bits
   const Bytes request = cat({ { 0x1D, 0x04, 0x08, 0xA5, 0x84, 0x80, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 } });
   const Bytes reply = b.run(request);
   CHECK(reply == V{ 0x1D, 0x00, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF });
   REQUIRE(b.pins.risingEdges.size() == 8 + 4 + 64 + 64);
   for(unsigned i = 0; i < 8; ++i) {
      CHECK(b.pins.risingEdges[i].level == (((0xA5 >> i) & 1) != 0));
   }
   CHECK_FALSE(b.pins.risingEdges[8].hostDrives);
   CHECK(b.pins.risingEdges[8 + 4 + 64].hostDrives);
   CHECK(b.pins.risingEdges[8 + 4 + 64].level == true);
   CHECK(b.pins.risingEdges[8 + 4 + 64 + 1].level == false);

   // Too little data: nothing runs, also not the first sequence.
   b.pins.risingEdges.clear();
   CHECK(b.run({ 0x1D, 0x02, 0x08, 0xFF, 0x10, 0xFF }) == V{ 0xFF });
   CHECK(b.pins.risingEdges.empty());
   // The captured data does not fit in a packet: 8 sequences of 64 bits.
   Bytes many{ 0x1D, 0x08 };
   many.insert(many.end(), 8, 0x80);
   CHECK(b.run(many) == V{ 0xFF });
   CHECK(b.pins.risingEdges.empty());
}

TEST_CASE("DAP_SWD_Configure sets the turnaround and the data phase") {
   Bench b(2);
   b.connect();
   CHECK(b.run({ 0x13, 0x01 }) == V{ 0x13, 0x00 });
   b.pins.readData = 0x1BA01477;
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(0x1BA01477) }));
   b.checkIdle();
   CHECK(b.pins.risingEdges.size() == dataTransferEdges + 2);

   // A FAULT without the data phase: one turnaround, and the next
   // transfer is in step.
   b.pins.acks = { pswd::ackFault };
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == V{ 0x05, 0x00, 0x04 });
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(0x1BA01477) }));
   b.checkIdle();
}

TEST_CASE("DAP_SWD_Configure: the data phase after WAIT and FAULT") {
   Bench b;
   b.connect();
   b.pins.dataPhase = true;
   CHECK(b.run({ 0x13, 0x04 }) == V{ 0x13, 0x00 });
   b.pins.acks = { pswd::ackFault };
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == V{ 0x05, 0x00, 0x04 });
   // 8 + 1 + 3 + 33 + 1 edges: the data phase ran.
   CHECK(b.pins.risingEdges.size() == dataTransferEdges);
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr })[1] == 0x01);
   b.checkIdle();
}

TEST_CASE("DAP_TransferConfigure: idle cycles come only after an OK transfer") {
   Bench b;
   b.connect();
   CHECK(b.run({ 0x04, 0x05, 0x01, 0x00, 0x00, 0x00 }) == V{ 0x04, 0x00 });
   // WAIT, then OK after one retry
   b.pins.acks = { pswd::ackWait, pswd::ackOk };
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr })[1] == 0x01);
   // WAIT: 8 + 1 + 3 + 1 edges, no idle cycles. OK: the data phase and 5
   // idle cycles.
   CHECK(b.pins.risingEdges.size() == 13 + dataTransferEdges + 5);
   // The retry sends no ABORT write.
   CHECK(b.pins.transfers.size() == 2);
   CHECK(b.pins.transfers[0].request == b.pins.transfers[1].request);
   b.checkIdle();
}

TEST_CASE("DAP_TransferConfigure: no idle cycles by default") {
   Bench b;
   b.connect();
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr })[1] == 0x01);
   CHECK(b.pins.risingEdges.size() == dataTransferEdges);
}

TEST_CASE("DAP_Transfer: DP read and write") {
   Bench b;
   b.connect();
   b.pins.readData = 0x1BA01477;
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(0x1BA01477) }));
   REQUIRE(b.pins.transfers.size() == 1);
   CHECK(b.pins.transfers[0].request == wire(readDpidr));

   // A write is followed by a read of RDBUFF that checks the write.
   b.pins.transfers.clear();
   CHECK(b.run(cat({ { 0x05, 0x00, 0x01, writeSelect }, word(0x12345678) })) == V{ 0x05, 0x01, 0x01 });
   REQUIRE(b.pins.transfers.size() == 2);
   CHECK(b.pins.transfers[0].request == wire(writeSelect));
   CHECK(b.pins.transfers[0].written == 0x12345678);
   CHECK(b.pins.transfers[0].writtenParity == pswd::parity(0x12345678));
   CHECK(b.pins.transfers[1].request == wire(readRdbuff));
   b.checkIdle();
}

TEST_CASE("DAP_Transfer: a posted AP read gives its data from RDBUFF") {
   Bench b;
   b.connect();
   b.pins.readQueue = { 0xDEAD0001, 0xCAFE0002 };
   CHECK(b.run({ 0x05, 0x00, 0x01, readApDrw }) == cat({ { 0x05, 0x01, 0x01 }, word(0xCAFE0002) }));
   CHECK(b.seen() == V{ readApDrw, readRdbuff });
   b.checkIdle();
}

TEST_CASE("DAP_Transfer: AP reads in a row continue the posted read") {
   Bench b;
   b.connect();
   b.pins.readQueue = { 1, 2, 3, 4 };
   CHECK(b.run({ 0x05, 0x00, 0x03, readApDrw, readApDrw, readApDrw }) == cat({ { 0x05, 0x03, 0x01 }, word(2), word(3), word(4) }));
   // The last data comes from RDBUFF.
   CHECK(b.seen() == V{ readApDrw, readApDrw, readApDrw, readRdbuff });
}

TEST_CASE("DAP_Transfer: another register ends the posted read") {
   Bench b;
   b.connect();
   b.pins.readQueue = { 1, 2, 3, 4, 5 };
   // AP read, DP read, AP read, write
   CHECK(b.run(cat({ { 0x05, 0x00, 0x04, readApDrw, readCtrlStat, readApDrw, writeTar }, word(0x20000000) })) ==
         cat({ { 0x05, 0x04, 0x01 }, word(2), word(3), word(5) }));
   // AP read, RDBUFF (2), DP read (3), AP read, RDBUFF (5) before the
   // write, then the check of the write
   CHECK(b.seen() == V{ readApDrw, readRdbuff, readCtrlStat, readApDrw, readRdbuff, writeTar, readRdbuff });
   CHECK(b.pins.transfers[5].written == 0x20000000);
   b.checkIdle();
}

TEST_CASE("DAP_Transfer: a read after a write does not check the write") {
   Bench b;
   b.connect();
   CHECK(b.run(cat({ { 0x05, 0x00, 0x02, writeSelect }, word(1), { readDpidr } }))[1] == 0x02);
   CHECK(b.seen() == V{ writeSelect, readDpidr });
}

TEST_CASE("DAP_Transfer: match value") {
   Bench b;
   b.connect();
   b.run({ 0x04, 0x00, 0x05, 0x00, 0x05, 0x00 });
   // Match mask 0xFF, then read DPIDR until the low byte is 0x42. The
   // match mask write has no transfer.
   b.pins.readQueue = { 0x100, 0x41, 0x142 };
   const Bytes reply = b.run(cat({ { 0x05, 0x00, 0x02, matchMask }, word(0xFF), { readDpidr | matchValue }, word(0x42) }));
   CHECK(reply == V{ 0x05, 0x02, 0x01 });
   CHECK(b.seen() == V{ readDpidr, readDpidr, readDpidr });
   b.checkIdle();
}

TEST_CASE("DAP_Transfer: match value timeout") {
   Bench b;
   b.connect();
   b.run({ 0x04, 0x00, 0x05, 0x00, 0x02, 0x00 });
   b.pins.readData = 0x41;
   // The retries: 1 read and 2 retries. The read that does not match is
   // not counted.
   const Bytes reply =
   b.run(cat({ { 0x05, 0x00, 0x03, matchMask }, word(0xFF), { readDpidr | matchValue }, word(0x42), { readDpidr } }));
   CHECK(reply == V{ 0x05, 0x01, 0x11 });
   CHECK(b.seen() == V{ readDpidr, readDpidr, readDpidr });
}

TEST_CASE("DAP_Transfer: match value of an AP register posts the read first") {
   Bench b;
   b.connect();
   b.run({ 0x04, 0x00, 0x05, 0x00, 0x05, 0x00 });
   b.pins.readQueue = { 0x99, 0x42 };
   const Bytes reply = b.run(cat({ { 0x05, 0x00, 0x02, matchMask }, word(0xFF), { readApDrw | matchValue }, word(0x42) }));
   CHECK(reply == V{ 0x05, 0x02, 0x01 });
   // The posted read returns no data. The next read gets 0x42 and matches.
   CHECK(b.seen() == V{ readApDrw, readApDrw });
}

TEST_CASE("DAP_Transfer: WAIT retries") {
   Bench b;
   b.connect();
   b.run({ 0x04, 0x00, 0x02, 0x00, 0x00, 0x00 });
   b.pins.acks = { pswd::ackWait, pswd::ackWait, pswd::ackOk };
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr })[1] == 0x01);
   CHECK(b.pins.transfers.size() == 3);
   // No ABORT write between the retries
   for(const auto &transfer : b.pins.transfers) {
      CHECK(transfer.request == wire(readDpidr));
   }

   // All retries WAIT: the transfer gives up.
   b.pins.transfers.clear();
   b.pins.acks = { pswd::ackWait, pswd::ackWait, pswd::ackWait, pswd::ackOk };
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == V{ 0x05, 0x00, 0x02 });
   CHECK(b.pins.transfers.size() == 3);
   b.pins.acks.clear();
   b.checkIdle();
}

TEST_CASE("DAP_Transfer: the count is the number of finished requests") {
   Bench b;
   b.connect();
   b.run({ 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 });
   b.pins.readData = 0x55;
   b.pins.acks = { pswd::ackOk, pswd::ackWait };
   CHECK(b.run({ 0x05, 0x00, 0x03, readDpidr, readDpidr, readDpidr }) == cat({ { 0x05, 0x01, 0x02 }, word(0x55) }));
}

TEST_CASE("DAP_Transfer: FAULT stops the list, and the rest of the request is skipped") {
   Bench b;
   b.connect();
   b.pins.acks = { pswd::ackFault };
   // The Transfer has a write after the FAULT. The Delay after it runs.
   const Bytes request =
   cat({ { 0x7F, 0x02, 0x05, 0x00, 0x03, readDpidr, writeSelect }, word(1), { readApDrw, 0x09, 0x07, 0x00 } });
   // The write data would be part of the next command if the skip was wrong
   CHECK(b.run(request) == V{ 0x7F, 0x02, 0x05, 0x00, 0x04, 0x09, 0x00 });
   CHECK(b.platform.delays == std::vector<uint32_t>{ 7 });
   CHECK(b.pins.transfers.size() == 1);
}

TEST_CASE("DAP_Transfer: the skip counts the data of writes and match reads") {
   Bench b;
   b.connect();
   b.pins.acks = { pswd::ackFault };
   const Bytes request = cat({ { 0x7F, 0x02, 0x05, 0x00, 0x04, readDpidr, readDpidr | matchValue },
                               word(5),
                               { writeSelect },
                               word(6),
                               { matchMask },
                               word(7),
                               { 0x09, 0x08, 0x00 } });
   CHECK(b.run(request) == V{ 0x7F, 0x02, 0x05, 0x00, 0x04, 0x09, 0x00 });
   CHECK(b.platform.delays == std::vector<uint32_t>{ 8 });
}

TEST_CASE("DAP_Transfer: parity error and protocol error") {
   Bench b;
   b.connect();
   b.pins.badParity = true;
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == V{ 0x05, 0x00, 0x08 });
   b.pins.badParity = false;
   // No ACK: the line reads 111
   b.pins.acks = { 7 };
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == V{ 0x05, 0x00, 0x07 });
   b.checkIdle();
   // The next transfer is in step.
   b.pins.readData = 9;
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(9) }));
   b.checkIdle();
}

TEST_CASE("DAP_Transfer: a protocol error on a write leaves SWDIO released") {
   Bench b;
   b.connect();
   b.pins.acks = { 7 };
   CHECK(b.run(cat({ { 0x05, 0x00, 0x01, writeSelect }, word(0xFFFFFFFF) })) == V{ 0x05, 0x00, 0x07 });
   CHECK(b.pins.contentions == 0);
}

TEST_CASE("DAP_Transfer: not connected") {
   Bench b;
   CHECK(b.run(cat({ { 0x7F, 0x02, 0x05, 0x00, 0x02, readDpidr, writeSelect }, word(1), { 0x09, 0x01, 0x00 } })) ==
         V{ 0x7F, 0x02, 0x05, 0x00, 0x00, 0x09, 0x00 });
   CHECK(b.pins.transfers.empty());
}

TEST_CASE("DAP_Transfer: a request that is too short is invalid") {
   Bench b;
   b.connect();
   CHECK(b.run({ 0x05, 0x00, 0x02, readDpidr }) == V{ 0xFF });
   CHECK(b.run({ 0x05, 0x00, 0x01, writeSelect, 0x01 }) == V{ 0xFF });
   CHECK(b.run({ 0x05, 0x00, 0x02, readDpidr, readDpidr | matchValue, 0x01 }) == V{ 0xFF });
   // The target saw nothing of the bad requests.
   CHECK(b.pins.transfers.empty());
}

TEST_CASE("DAP_Transfer: more reads than fit in the response are invalid") {
   Bench b;
   b.connect();
   Bytes request{ 0x05, 0x00, 16 };
   request.insert(request.end(), 16, readDpidr);
   CHECK(b.run(request) == V{ 0xFF });
   CHECK(b.pins.transfers.empty());
   request[2] = 15;
   request.pop_back();
   CHECK(b.run(request)[1] == 15);
}

TEST_CASE("DAP_Transfer: a failed RDBUFF read still skips the data of the request") {
   Bench b;
   b.connect();
   // AP read (posted), then a write. The RDBUFF read before the write gets
   // a FAULT. The write data must not run as commands.
   b.pins.acks = { pswd::ackOk, pswd::ackFault };
   const Bytes write = cat({ { 0x7F, 0x02, 0x05, 0x00, 0x02, readApDrw, writeSelect }, word(0x00000907), { 0x09, 0x03, 0x00 } });
   CHECK(b.run(write) == V{ 0x7F, 0x02, 0x05, 0x01, 0x04, 0x09, 0x00 });
   CHECK(b.platform.delays == std::vector<uint32_t>{ 3 });

   // The same before a match read
   b.platform.delays.clear();
   b.pins.acks = { pswd::ackOk, pswd::ackFault };
   const Bytes match =
   cat({ { 0x7F, 0x02, 0x05, 0x00, 0x02, readApDrw, readDpidr | matchValue }, word(0x00000907), { 0x09, 0x04, 0x00 } });
   CHECK(b.run(match) == V{ 0x7F, 0x02, 0x05, 0x01, 0x04, 0x09, 0x00 });
   CHECK(b.platform.delays == std::vector<uint32_t>{ 4 });
}

TEST_CASE("DAP_TransferBlock: reads") {
   Bench b;
   b.connect();
   b.pins.readQueue = { 0xA, 0xB, 0xC };
   CHECK(b.run({ 0x06, 0x00, 0x03, 0x00, readCtrlStat }) == cat({ { 0x06, 0x03, 0x00, 0x01 }, word(0xA), word(0xB), word(0xC) }));
   CHECK(b.seen() == V{ readCtrlStat, readCtrlStat, readCtrlStat });

   // AP: post the first read, and read the last data from RDBUFF.
   b.pins.transfers.clear();
   b.pins.readQueue = { 0x0, 0x1, 0x2 };
   CHECK(b.run({ 0x06, 0x00, 0x02, 0x00, readApDrw }) == cat({ { 0x06, 0x02, 0x00, 0x01 }, word(0x1), word(0x2) }));
   CHECK(b.seen() == V{ readApDrw, readApDrw, readRdbuff });
   b.checkIdle();
}

TEST_CASE("DAP_TransferBlock: writes") {
   Bench b;
   b.connect();
   CHECK(b.run(cat({ { 0x06, 0x00, 0x02, 0x00, writeTar }, word(0x11), word(0x22) })) == V{ 0x06, 0x02, 0x00, 0x01 });
   CHECK(b.seen() == V{ writeTar, writeTar, readRdbuff });
   CHECK(b.pins.transfers[0].written == 0x11);
   CHECK(b.pins.transfers[1].written == 0x22);
   b.checkIdle();
}

TEST_CASE("DAP_TransferBlock: an error stops the block, and the data is skipped") {
   Bench b;
   b.connect();
   b.pins.acks = { pswd::ackOk, pswd::ackFault };
   const Bytes request =
   cat({ { 0x7F, 0x02, 0x06, 0x00, 0x03, 0x00, writeTar }, word(1), word(2), word(3), { 0x09, 0x04, 0x00 } });
   CHECK(b.run(request) == V{ 0x7F, 0x02, 0x06, 0x01, 0x00, 0x04, 0x09, 0x00 });
   CHECK(b.platform.delays == std::vector<uint32_t>{ 4 });

   // A read that fails after one word
   b.pins.transfers.clear();
   b.run({ 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 });
   b.pins.acks = { pswd::ackOk, pswd::ackWait };
   b.pins.readData = 0x77;
   CHECK(b.run({ 0x06, 0x00, 0x03, 0x00, readCtrlStat }) == cat({ { 0x06, 0x01, 0x00, 0x02 }, word(0x77) }));
}

TEST_CASE("DAP_TransferBlock: empty, not connected and too large") {
   Bench b;
   CHECK(b.run({ 0x06, 0x00, 0x02, 0x00, readCtrlStat }) == V{ 0x06, 0x00, 0x00, 0x00 });
   CHECK(b.run(cat({ { 0x06, 0x00, 0x01, 0x00, writeTar }, word(1) })) == V{ 0x06, 0x00, 0x00, 0x00 });
   CHECK(b.pins.transfers.empty());
   b.connect();
   CHECK(b.run({ 0x06, 0x00, 0x00, 0x00, readCtrlStat }) == V{ 0x06, 0x00, 0x00, 0x00 });
   CHECK(b.pins.transfers.empty());
   // 20 words do not fit in a packet of 64 bytes. The target sees nothing.
   CHECK(b.run({ 0x06, 0x00, 20, 0x00, readCtrlStat }) == V{ 0xFF });
   CHECK(b.run({ 0x06, 0xFF, 0xFF, 0x00, readCtrlStat }) == V{ 0xFF });
   // A write block with too little data
   CHECK(b.run(cat({ { 0x06, 0x00, 0x02, 0x00, writeTar }, word(1) })) == V{ 0xFF });
   CHECK(b.pins.transfers.empty());
}

TEST_CASE("DAP_WriteABORT writes the ABORT register") {
   Bench b;
   // Not connected: error
   CHECK(b.run(cat({ { 0x08, 0x00 }, word(0x1E) })) == V{ 0x08, 0x01 });
   CHECK(b.pins.transfers.empty());
   b.connect();
   CHECK(b.run(cat({ { 0x08, 0x00 }, word(0x1E) })) == V{ 0x08, 0x00 });
   REQUIRE(b.pins.transfers.size() == 1);
   CHECK(b.pins.transfers[0].request == wire(writeAbort));
   CHECK(b.pins.transfers[0].written == 0x1E);
}

TEST_CASE("DAP_ExecuteCommands runs the commands in order") {
   Bench b;
   CHECK(b.run({ 0x7F, 0x02, 0x09, 0x01, 0x00, 0x01, 0x00, 0x01 }) == V{ 0x7F, 0x02, 0x09, 0x00, 0x01, 0x00 });
   // The commands do not nest.
   CHECK(b.run({ 0x7F, 0x01, 0x7F, 0x01, 0x09, 0x01, 0x00 }) == V{ 0x7F, 0x01, 0xFF });
   // A command that is cut stops the list. The count is the commands that ran.
   CHECK(b.run({ 0x7F, 0x03, 0x09, 0x01, 0x00, 0x09, 0x01 }) == V{ 0x7F, 0x01, 0x09, 0x00 });
   CHECK(b.run({ 0x7F, 0x03, 0x09, 0x01, 0x00 }) == V{ 0x7F, 0x01, 0x09, 0x00 });
   CHECK(b.run({ 0x7F }) == V{ 0xFF });
}

TEST_CASE("DAP_ExecuteCommands: a response that does not fit stops the list") {
   Bench b;
   b.connect();
   // Two info responses of 23 bytes fit in 64 bytes, the third does not.
   const std::string longText(20, 'x');
   b.info.vendor = longText.c_str();
   Processor dap(b.swd, b.gpio, b.platform, b.info);
   dap.submit(V{ 0x7F, 0x03, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01 });
   const auto response = dap.response();
   CHECK(response.size() == 2 + 2 * 23);
   CHECK(response[1] == 2);
}

TEST_CASE("DAP_QueueCommands runs the packets back to back") {
   Bench b;
   b.dap.submit(V{ 0x7E, 0x01, 0x09, 0x05, 0x00 });
   b.dap.submit(V{ 0x7E, 0x01, 0x09, 0x06, 0x00 });
   CHECK_FALSE(b.dap.hasResponse());
   CHECK(b.platform.delays.empty());
   // The next other command runs the queue and then itself.
   b.dap.submit(V{ 0x00, 0xFF });
   CHECK(b.platform.delays == std::vector<uint32_t>{ 5, 6 });
   // The queued responses have the ID of DAP_ExecuteCommands.
   CHECK(b.pop() == V{ 0x7F, 0x01, 0x09, 0x00 });
   CHECK(b.pop() == V{ 0x7F, 0x01, 0x09, 0x00 });
   CHECK(b.pop() == V{ 0x00, 0x02, 0x40, 0x00 });
   CHECK_FALSE(b.dap.hasResponse());
}

TEST_CASE("DAP_QueueCommands: a packet needs a free slot") {
   Bench b;
   for(unsigned i = 0; i < Processor::packetCount; ++i) {
      CHECK(b.dap.canSubmit());
      b.dap.submit(V{ 0x7E, 0x01, 0x09, 0x01, 0x00 });
   }
   CHECK_FALSE(b.dap.canSubmit());
   // A packet that is not accepted has no effect.
   b.dap.submit(V{ 0x09, 0x01, 0x00 });
   CHECK(b.platform.delays.empty());
   b.dap.hostGone();
   CHECK(b.dap.canSubmit());
   b.dap.submit(V{ 0x09, 0x01, 0x00 });
   CHECK(b.platform.delays == std::vector<uint32_t>{ 1 });
   CHECK(b.pop() == V{ 0x09, 0x00 });
   CHECK_FALSE(b.dap.hasResponse());
}

TEST_CASE("DAP_QueueCommands: the responses make room") {
   Bench b;
   for(unsigned i = 0; i < Processor::packetCount - 1; ++i) {
      b.dap.submit(V{ 0x7E, 0x00 });
   }
   b.dap.submit(V{ 0x09, 0x01, 0x00 });
   CHECK(b.dap.hasResponse());
   CHECK_FALSE(b.dap.canSubmit());
   CHECK(b.pop() == V{ 0x7F, 0x00 });
   CHECK(b.dap.canSubmit());
}

TEST_CASE("Unknown commands, DAP_TransferAbort and bad packets") {
   Bench b;
   CHECK(b.run({ 0x55 }) == V{ 0xFF });
   CHECK(b.run({ 0x80, 0x01 }) == V{ 0xFF });
   CHECK(b.run({ 0xFF }) == V{ 0xFF });
   // JTAG commands: not available yet
   CHECK(b.run({ 0x14, 0x00 }) == V{ 0xFF });
   CHECK(b.run({ 0x7F, 0x02, 0x55, 0x09, 0x01, 0x00 })[0] == 0x7F);

   // DAP_TransferAbort has no response.
   b.dap.submit(V{ 0x07 });
   CHECK_FALSE(b.dap.hasResponse());
   b.dap.submit(V{});
   CHECK_FALSE(b.dap.hasResponse());

   // More than a packet
   CHECK(b.run(Bytes(65, 0x09)) == V{ 0xFF });
   // A short packet
   CHECK(b.run({ 0x09, 0x01 }) == V{ 0xFF });
}

TEST_CASE("hostGone disconnects") {
   Bench b;
   b.connect();
   b.dap.hostGone();
   CHECK_FALSE(b.dap.connected());
   CHECK(b.pins.finis == 1);
   b.dap.hostGone();
   CHECK(b.pins.finis == 1);
}

TEST_CASE("hostGone drops the responses that were not sent") {
   Bench b;
   b.dap.submit(V{ 0x09, 0x01, 0x00 });
   REQUIRE(b.dap.hasResponse());
   b.dap.hostGone();
   CHECK_FALSE(b.dap.hasResponse());
   CHECK(b.dap.canSubmit());
}

TEST_CASE("Disconnect and hostGone release the pins of DAP_SWJ_Pins") {
   Bench b;
   // Hold nRESET and nTRST low, drive TDI high
   b.run(cat({ { 0x10, 0x04, 0xA4 }, word(0) }));
   CHECK_FALSE(b.gpio.pin(gpio::srst).level);
   CHECK(b.run({ 0x03 }) == V{ 0x03, 0x00 });
   CHECK(b.gpio.pin(gpio::srst).mode == PinMode::InputPullUp);
   CHECK(b.gpio.pin(gpio::trst).mode == PinMode::InputPullUp);
   CHECK(b.gpio.pin(gpio::tdi).mode == PinMode::Input);

   b.run(cat({ { 0x10, 0x00, 0x80 }, word(0) }));
   CHECK(b.gpio.pin(gpio::srst).mode == PinMode::OpenDrain);
   b.dap.hostGone();
   CHECK(b.gpio.pin(gpio::srst).mode == PinMode::InputPullUp);

   // A board without SRST and TRST
   Bench none(1, gpio::tck | gpio::tms | gpio::tdo);
   none.dap.hostGone();
   CHECK(none.gpio.configured.empty());
}

TEST_CASE("DAP_Connect sets the configuration of the engine again") {
   Bench b;
   b.run(cat({ { 0x11 }, word(100'000) }));
   b.run({ 0x04, 0x05, 0x01, 0x00, 0x00, 0x00 });
   // The Versaloon block uses the engine in between.
   swd::TransferRules versaloon;
   b.port.setRules(versaloon);
   b.port.configure(4, 7, 5);
   b.connect();
   CHECK(b.port.rules().idleCycles == 5);
   CHECK_FALSE(b.port.rules().abortOnWait);
   CHECK(b.port.halfCycles() == 360);
   b.pins.readData = 1;
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr })[1] == 0x01);
   b.checkIdle();
}
