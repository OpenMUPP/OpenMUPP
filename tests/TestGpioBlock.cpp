// SPDX-License-Identifier: GPL-3.0-or-later
// The USB_TO_GPIO block, and the map of its bits to the board pins.

#include "board/DebugGpio.h"
#include "board/boards/Mini.h"
#include "board/boards/OtherUse.h"
#include "board/boards/StLinkV2.h"
#include "protocol/versaloon/GpioBlock.h"
#include "protocol/versaloon/UsbToAll.h"

#include "VersaloonBench.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace gpio = protocol::gpio;
using board::Pin;
using board::Port;
using protocol::PinMode;

class NoDelay final : public protocol::Delay {
   public:
   void microseconds(uint16_t) override {
   }
   void milliseconds(uint16_t) override {
   }
};

// Pins with a mode and a level each. read() gives the output level of an
// output, and the input level (inputs) of an input.
class FakeGpioPins final : public protocol::GpioPins {
   public:
   struct State {
      PinMode mode{ PinMode::Input };
      bool level{ false };
   };

   explicit FakeGpioPins(uint16_t available = gpio::all & ~(gpio::usr1 | gpio::usr2))
      : available_(available) {
   }

   [[nodiscard]] uint16_t available() const override {
      return available_;
   }

   void configure(uint16_t bit, PinMode mode, bool level) override {
      REQUIRE((bit & available_) == bit);
      State &state = pins[indexOf(bit)];
      state.mode = mode;
      if(mode == PinMode::PushPull || mode == PinMode::OpenDrain) {
         state.level = level;
      }
      configured.push_back(bit);
   }

   void write(uint16_t mask, uint16_t value) override {
      REQUIRE((mask & available_) == mask);
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
         const bool output = state.mode == PinMode::PushPull || state.mode == PinMode::OpenDrain;
         if((mask & bit) && (output ? state.level : (inputs & bit) != 0)) {
            value = static_cast<uint16_t>(value | bit);
         }
      }
      return value;
   }

   static std::size_t indexOf(uint16_t bit) {
      std::size_t index = 0;
      while((bit >> index) != 1) {
         ++index;
      }
      return index;
   }

   const State &state(uint16_t bit) const {
      return pins[indexOf(bit)];
   }

   std::array<State, 9> pins{};
   std::vector<uint16_t> configured;
   // The levels that the target drives on the inputs
   uint16_t inputs{ 0 };

   private:
   uint16_t available_;
};

std::vector<uint8_t> u16(uint16_t value) {
   return { static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8) };
}

// One sub-command: [command | index][length][payload]
std::vector<uint8_t> sub(uint8_t command, const std::vector<uint8_t> &payload) {
   std::vector<uint8_t> bytes{ command };
   const auto length = u16(static_cast<uint16_t>(payload.size()));
   bytes.insert(bytes.end(), length.begin(), length.end());
   bytes.insert(bytes.end(), payload.begin(), payload.end());
   return bytes;
}

std::vector<uint8_t> config(uint16_t mask, uint16_t io, uint16_t pullEnable, uint16_t pull) {
   std::vector<uint8_t> payload;
   for(const uint16_t value : { mask, static_cast<uint16_t>(io & mask), pullEnable, pull }) {
      const auto bytes = u16(value);
      payload.insert(payload.end(), bytes.begin(), bytes.end());
   }
   return sub(protocol::subcommand::config, payload);
}

std::vector<uint8_t> out(uint16_t mask, uint16_t value) {
   auto payload = u16(mask);
   const auto bytes = u16(value);
   payload.insert(payload.end(), bytes.begin(), bytes.end());
   return sub(protocol::subcommand::out, payload);
}

std::vector<uint8_t> in(uint16_t mask) {
   return sub(protocol::subcommand::in, u16(mask));
}

class Bench {
   public:
   // replySize: the size of the reply buffer, for the overflow tests
   explicit Bench(uint16_t available = gpio::all & ~(gpio::usr1 | gpio::usr2), std::size_t replySize = 256)
      : pins(available)
      , replySize_(replySize)
      , block_(pins)
      , dispatcher_(delay_) {
      REQUIRE(dispatcher_.add(block_));
   }

   // Runs one GPIO block of sub-commands in a USB_TO_ALL command. Returns
   // the reply.
   std::vector<uint8_t> run(const std::vector<std::vector<uint8_t>> &subs) {
      std::vector<uint8_t> payload;
      for(const auto &bytes : subs) {
         payload.insert(payload.end(), bytes.begin(), bytes.end());
      }
      std::vector<uint8_t> command{ protocol::block::all, 0, 0, protocol::block::gpio };
      const auto blockLength = u16(static_cast<uint16_t>(payload.size() + 3));
      command.insert(command.end(), blockLength.begin(), blockLength.end());
      command.insert(command.end(), payload.begin(), payload.end());
      command[1] = static_cast<uint8_t>(command.size());
      command[2] = static_cast<uint8_t>(command.size() >> 8);
      const auto size = dispatcher_.process(art::span<const uint8_t>(command.data(), command.size()),
                                            art::span<uint8_t>(reply_.data(), replySize_));
      REQUIRE(size.has_value());
      return { reply_.begin(), reply_.begin() + static_cast<std::ptrdiff_t>(*size) };
   }

   protocol::UsbToAll &dispatcher() {
      return dispatcher_;
   }

   FakeGpioPins pins;

   private:
   std::size_t replySize_;
   NoDelay delay_;
   protocol::GpioBlock block_;
   protocol::UsbToAll dispatcher_;
   std::array<uint8_t, 256> reply_{};
};

} // namespace

TEST_CASE("GPIO bits map to the debug pins of the Mini") {
   const auto &pins = board::mini.debug;
   CHECK(board::gpioOutputPin(pins, gpio::srst) == Pin{ Port::B, 11 });
   CHECK(board::gpioOutputPin(pins, gpio::trst) == Pin{ Port::B, 10 });
   CHECK(board::gpioOutputPin(pins, gpio::tck) == Pin{ Port::B, 13 });
   CHECK(board::gpioOutputPin(pins, gpio::tdo) == Pin{ Port::B, 14 });
   CHECK(board::gpioOutputPin(pins, gpio::tdi) == Pin{ Port::B, 15 });
   CHECK(board::gpioOutputPin(pins, gpio::rtck) == Pin{ Port::A, 8 });
   CHECK(board::gpioOutputPin(pins, gpio::tms) == Pin{ Port::B, 4 });
   CHECK(board::gpioInputPin(pins, gpio::tms) == Pin{ Port::B, 6 });
   CHECK(board::gpioInputPin(pins, gpio::srst) == Pin{ Port::B, 11 });
   CHECK_FALSE(board::gpioOutputPin(pins, gpio::usr1).has_value());
   CHECK_FALSE(board::gpioOutputPin(pins, gpio::usr2).has_value());
   CHECK(board::gpioAvailable(pins) == 0x01F3);
   CHECK(board::gpioAvailable(board::otherUse.debug) == 0x01F3);
}

TEST_CASE("GPIO bits map to the debug pins of the ST-Link/V2") {
   const auto &pins = board::stLinkV2.debug;
   CHECK(board::gpioOutputPin(pins, gpio::srst) == Pin{ Port::B, 0 });
   CHECK(board::gpioOutputPin(pins, gpio::tms) == Pin{ Port::B, 14 });
   CHECK(board::gpioInputPin(pins, gpio::tms) == Pin{ Port::B, 14 });
   CHECK_FALSE(board::gpioOutputPin(pins, gpio::trst).has_value());
   CHECK_FALSE(board::gpioOutputPin(pins, gpio::rtck).has_value());
   // No TRST, USR1, USR2 and RTCK
   CHECK(board::gpioAvailable(pins) == 0x0171);
}

TEST_CASE("GPIO bits are the bits of OpenOCD") {
   CHECK(gpio::srst == 0x01);
   CHECK(gpio::trst == 0x02);
   CHECK(gpio::usr1 == 0x04);
   CHECK(gpio::usr2 == 0x08);
   CHECK(gpio::tck == 0x10);
   CHECK(gpio::tdo == 0x20);
   CHECK(gpio::tdi == 0x40);
   CHECK(gpio::rtck == 0x80);
   CHECK(gpio::tms == 0x100);
}

TEST_CASE("CONFIG sets the mode of each selected pin") {
   Bench bench;
   CHECK(bench.run({ config(gpio::srst | gpio::trst | gpio::tck | gpio::tdo | gpio::tms, gpio::srst | gpio::trst | gpio::tck,
                            gpio::srst | gpio::tdo | gpio::tms, gpio::srst | gpio::trst | gpio::tdo) }) ==
         std::vector<uint8_t>{ 0x00 });
   CHECK(bench.pins.state(gpio::srst).mode == PinMode::OpenDrain);
   CHECK(bench.pins.state(gpio::srst).level);
   CHECK(bench.pins.state(gpio::trst).mode == PinMode::PushPull);
   CHECK(bench.pins.state(gpio::trst).level);
   CHECK(bench.pins.state(gpio::tck).mode == PinMode::PushPull);
   CHECK_FALSE(bench.pins.state(gpio::tck).level);
   CHECK(bench.pins.state(gpio::tdo).mode == PinMode::InputPullUp);
   CHECK(bench.pins.state(gpio::tms).mode == PinMode::InputPullDown);
   // Unselected pins do not change.
   CHECK(bench.pins.configured.size() == 5);
}

TEST_CASE("OpenOCD: SRST released, SRST asserted, TRST in JTAG mode") {
   Bench bench;
   // vsllink_init(): gpio.init(0), gpio.config(0, SRST, 0, SRST, SRST)
   CHECK(bench.run({ sub(protocol::subcommand::init, {}), config(gpio::srst, 0, gpio::srst, gpio::srst) }) ==
         std::vector<uint8_t>{ 0x00, 0x00 });
   CHECK(bench.pins.state(gpio::srst).mode == PinMode::InputPullUp);

   // JTAG mode: gpio.config(0, SRST | TRST, TRST, SRST, SRST)
   CHECK(bench.run({ config(gpio::srst | gpio::trst, gpio::trst, gpio::srst, gpio::srst) }) == std::vector<uint8_t>{ 0x00 });
   CHECK(bench.pins.state(gpio::srst).mode == PinMode::InputPullUp);
   CHECK(bench.pins.state(gpio::trst).mode == PinMode::PushPull);
   CHECK_FALSE(bench.pins.state(gpio::trst).level);

   // vsllink_reset(0, 1): gpio.config(0, SRST, SRST, 0, 0), gpio.out(0, TRST, TRST)
   CHECK(bench.run({ config(gpio::srst, gpio::srst, 0, 0), out(gpio::trst, gpio::trst) }) == std::vector<uint8_t>{ 0x00, 0x00 });
   CHECK(bench.pins.state(gpio::srst).mode == PinMode::PushPull);
   CHECK_FALSE(bench.pins.state(gpio::srst).level);
   CHECK(bench.pins.state(gpio::trst).level);

   // vsllink_reset(1, 0): TRST asserted
   CHECK(bench.run({ config(gpio::srst, 0, gpio::srst, gpio::srst), out(gpio::trst, 0) }) == std::vector<uint8_t>{ 0x00, 0x00 });
   CHECK(bench.pins.state(gpio::srst).mode == PinMode::InputPullUp);
   CHECK_FALSE(bench.pins.state(gpio::trst).level);

   // vsllink_quit(): gpio.config(0, SRST | TRST, 0, 0, SRST | TRST), gpio.fini(0)
   CHECK(bench.run({ config(gpio::srst | gpio::trst, 0, 0, gpio::srst | gpio::trst), sub(protocol::subcommand::fini, {}) }) ==
         std::vector<uint8_t>{ 0x00, 0x00 });
   CHECK(bench.pins.state(gpio::srst).mode == PinMode::Input);
   CHECK(bench.pins.state(gpio::trst).mode == PinMode::Input);
}

TEST_CASE("OUT writes the selected pins only") {
   Bench bench;
   CHECK(bench.run({ config(gpio::tck | gpio::tdi, gpio::tck | gpio::tdi, 0, 0), out(gpio::tck, gpio::tck | gpio::tdi) }) ==
         std::vector<uint8_t>{ 0x00, 0x00 });
   CHECK(bench.pins.state(gpio::tck).level);
   CHECK_FALSE(bench.pins.state(gpio::tdi).level);
}

TEST_CASE("IN reads the selected pins") {
   Bench bench;
   bench.pins.inputs = gpio::tdo | gpio::rtck | gpio::srst;
   CHECK(bench.run({ config(gpio::tck, gpio::tck, 0, gpio::tck), in(gpio::tdo | gpio::tck | gpio::srst) }) ==
         std::vector<uint8_t>{ 0x00, 0x00, 0x31, 0x00 });
   CHECK(bench.run({ in(gpio::tms) }) == std::vector<uint8_t>{ 0x00, 0x00, 0x00 });
}

TEST_CASE("Pins that the board does not have are ignored and read as 0") {
   // The ST-Link/V2: no TRST, USR1, USR2 and RTCK
   Bench bench(board::gpioAvailable(board::stLinkV2.debug));
   bench.pins.inputs = gpio::all;
   // OpenOCD in SWD mode: gpio.config(0, TRST, 0, TRST, TRST)
   CHECK(bench.run({ config(gpio::trst, 0, gpio::trst, gpio::trst) }) == std::vector<uint8_t>{ 0x00 });
   CHECK(bench.run({ out(gpio::trst | gpio::usr1, gpio::trst) }) == std::vector<uint8_t>{ 0x00 });
   CHECK(bench.run({ in(gpio::trst | gpio::rtck | gpio::tdo) }) == std::vector<uint8_t>{ 0x00, 0x20, 0x00 });
   CHECK(bench.pins.configured.empty());
}

TEST_CASE("Wrong GPIO requests") {
   Bench bench;
   SUBCASE("A bit above TMS") {
      CHECK(bench.run({ config(0x0200, 0, 0, 0) }) == std::vector<uint8_t>{ 0x04 });
      CHECK(bench.run({ out(0x8000, 0) }) == std::vector<uint8_t>{ 0x04 });
      // IN keeps its data size.
      CHECK(bench.run({ in(0x0200) }) == std::vector<uint8_t>{ 0x04, 0x00, 0x00 });
   }
   SUBCASE("An output on an input-only pin") {
      CHECK(bench.run({ config(gpio::tdo, gpio::tdo, 0, 0) }) == std::vector<uint8_t>{ 0x04 });
      CHECK(bench.run({ config(gpio::rtck | gpio::srst, gpio::rtck, 0, 0) }) == std::vector<uint8_t>{ 0x04 });
   }
   SUBCASE("A wrong payload size") {
      CHECK(bench.run({ sub(protocol::subcommand::config, { 0x01, 0x00 }) }) == std::vector<uint8_t>{ 0x04 });
      CHECK(bench.run({ sub(protocol::subcommand::out, { 0x01, 0x00 }) }) == std::vector<uint8_t>{ 0x04 });
      CHECK(bench.run({ sub(protocol::subcommand::in, {}) }) == std::vector<uint8_t>{ 0x04, 0x00, 0x00 });
   }
   SUBCASE("A port index other than 0") {
      CHECK(bench.run({ sub(protocol::subcommand::init | 1, {}) }) == std::vector<uint8_t>{ 0x03 });
      CHECK(bench.run({ sub(protocol::subcommand::in | 2, u16(gpio::srst)) }) == std::vector<uint8_t>{ 0x03, 0x00, 0x00 });
   }
   SUBCASE("An unknown sub-command") {
      CHECK(bench.run({ sub(protocol::subcommand::inOut, {}) }) == std::vector<uint8_t>{ 0x06 });
   }
   // No pin changed.
   CHECK(bench.pins.configured.empty());
}

TEST_CASE("A GPIO sub-command whose reply does not fit changes no pin") {
   const uint16_t mini = board::gpioAvailable(board::mini.debug);

   SUBCASE("CONFIG that asserts SRST") {
      // Room for the first status only
      Bench bench(mini, 1);
      CHECK(bench.run({ config(gpio::trst, gpio::trst, 0, gpio::trst), config(gpio::srst, gpio::srst, 0, 0) }).empty());
      CHECK(bench.pins.configured == std::vector<uint16_t>{ gpio::trst });
      CHECK(bench.pins.state(gpio::srst).mode == PinMode::Input);
   }
   SUBCASE("OUT") {
      Bench bench(mini, 1);
      CHECK(bench.run({ config(gpio::tck, gpio::tck, 0, 0), out(gpio::tck, gpio::tck) }).empty());
      CHECK_FALSE(bench.pins.state(gpio::tck).level);
   }
   SUBCASE("IN needs 3 bytes") {
      Bench bench(mini, 2);
      CHECK(bench.run({ in(gpio::srst) }).empty());
      CHECK(bench.pins.configured.empty());
   }
}

TEST_CASE("CONFIG of TMS on the Mini: PB4 drives, PB6 reads") {
   using board::GpioPinSetting;
   using Settings = std::array<std::optional<GpioPinSetting>, 2>;
   const auto &pins = board::mini.debug;
   const Pin pb4{ Port::B, 4 };
   const Pin pb6{ Port::B, 6 };

   CHECK(board::gpioPinSettings(pins, gpio::tms, PinMode::PushPull) ==
         Settings{ GpioPinSetting{ pb4, PinMode::PushPull }, std::nullopt });
   CHECK(board::gpioPinSettings(pins, gpio::tms, PinMode::OpenDrain) ==
         Settings{ GpioPinSetting{ pb4, PinMode::OpenDrain }, std::nullopt });
   // An input: PB6 gets the mode, PB4 releases the line.
   CHECK(board::gpioPinSettings(pins, gpio::tms, PinMode::InputPullUp) ==
         Settings{ GpioPinSetting{ pb6, PinMode::InputPullUp }, GpioPinSetting{ pb4, PinMode::Input } });
   CHECK(board::gpioPinSettings(pins, gpio::tms, PinMode::Input) ==
         Settings{ GpioPinSetting{ pb6, PinMode::Input }, GpioPinSetting{ pb4, PinMode::Input } });
   // Other bits have one pin.
   CHECK(board::gpioPinSettings(pins, gpio::srst, PinMode::InputPullUp) ==
         Settings{ GpioPinSetting{ Pin{ Port::B, 11 }, PinMode::InputPullUp }, std::nullopt });
   CHECK(board::gpioPinSettings(pins, gpio::usr1, PinMode::PushPull) == Settings{});
}

TEST_CASE("CONFIG of TMS on the ST-Link/V2: one pin for both directions") {
   using board::GpioPinSetting;
   using Settings = std::array<std::optional<GpioPinSetting>, 2>;
   const auto &pins = board::stLinkV2.debug;
   const Pin pb14{ Port::B, 14 };

   CHECK(board::gpioPinSettings(pins, gpio::tms, PinMode::PushPull) ==
         Settings{ GpioPinSetting{ pb14, PinMode::PushPull }, std::nullopt });
   CHECK(board::gpioPinSettings(pins, gpio::tms, PinMode::InputPullDown) ==
         Settings{ GpioPinSetting{ pb14, PinMode::InputPullDown }, std::nullopt });
   CHECK(board::gpioPinSettings(pins, gpio::trst, PinMode::PushPull) == Settings{});
}

TEST_CASE("Host gone: the GPIO block releases SRST and TRST with pull-up") {
   Bench bench;
   CHECK(bench.run({ config(gpio::srst | gpio::trst | gpio::tck, gpio::srst | gpio::trst | gpio::tck, 0, 0) }) ==
         std::vector<uint8_t>{ 0 });
   REQUIRE(bench.pins.state(gpio::srst).mode == PinMode::PushPull);
   bench.pins.configured.clear();

   protocol::UsbToAll &dispatcher = bench.dispatcher();
   dispatcher.hostGone();
   CHECK(bench.pins.configured == std::vector<uint16_t>{ gpio::srst, gpio::trst });
   CHECK(bench.pins.state(gpio::srst).mode == PinMode::InputPullUp);
   CHECK(bench.pins.state(gpio::trst).mode == PinMode::InputPullUp);
   // Other pins stay.
   CHECK(bench.pins.state(gpio::tck).mode == PinMode::PushPull);
}

TEST_CASE("Host gone: a board without TRST releases only SRST") {
   Bench bench(gpio::all & ~(gpio::usr1 | gpio::usr2 | gpio::trst | gpio::rtck));
   bench.dispatcher().hostGone();
   CHECK(bench.pins.configured == std::vector<uint16_t>{ gpio::srst });
}

namespace {

// SRST asserted through the USB function: config(SRST, SRST, 0, 0), a
// push-pull output at the low level, as OpenOCD asserts it.
struct UsbGpioBench {
   UsbGpioBench()
      : dispatcher(delay)
      , block(pins)
      , usb(nullptr, &dispatcher) {
      REQUIRE(dispatcher.add(block));
      usb.enumerate();
      const auto sub = config(gpio::srst, gpio::srst, 0, 0);
      std::vector<uint8_t> command{
         protocol::block::all, 0, 0, protocol::block::gpio, static_cast<uint8_t>(sub.size() + 3), 0
      };
      command.insert(command.end(), sub.begin(), sub.end());
      command[1] = static_cast<uint8_t>(command.size());
      REQUIRE(usb.write(command));
      REQUIRE(usb.read() == std::vector<uint8_t>{ 0 });
      REQUIRE(pins.state(gpio::srst).mode == PinMode::PushPull);
      REQUIRE_FALSE(pins.state(gpio::srst).level);
   }

   NoDelay delay;
   protocol::UsbToAll dispatcher;
   FakeGpioPins pins;
   protocol::GpioBlock block;
   mupptest::Bench usb;
};

} // namespace

TEST_CASE("Host gone: a USB bus reset releases SRST") {
   UsbGpioBench bench;
   bench.usb.controller().busReset();
   CHECK(bench.pins.state(gpio::srst).mode == PinMode::InputPullUp);
}

TEST_CASE("Host gone: SET_CONFIGURATION 0 releases SRST") {
   UsbGpioBench bench;
   bench.usb.request(mupptest::setConfiguration, 0);
   CHECK(bench.pins.state(gpio::srst).mode == PinMode::InputPullUp);
}

TEST_CASE("Host gone: a USB suspend releases SRST") {
   UsbGpioBench bench;
   bench.usb.controller().busSuspend();
   CHECK(bench.pins.state(gpio::srst).mode == PinMode::InputPullUp);
}
