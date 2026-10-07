// The USB_TO_POWER block, on a fake power switch and voltage sense.

#include "protocol/PowerBlock.h"
#include "protocol/UsbToAll.h"

#include "VersaloonBench.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace block = protocol::block;
using protocol::PowerBlock;

// The rail of the target. With the switch off, the rail is at externalMv
// (the supply of the target). With the switch on, it is at 3300 mV, or at
// loadedMv when the target pulls it down.
class FakePower final : public protocol::PowerHardware {
   public:
   [[nodiscard]] bool hasSwitch() const override {
      return hasSwitchPin;
   }

   void setSwitch(bool on) override {
      REQUIRE(hasSwitchPin);
      isOn = on;
      switches.push_back(on);
   }

   [[nodiscard]] bool hasSense() const override {
      return hasSensePin;
   }

   [[nodiscard]] std::optional<uint16_t> senseMillivolts() override {
      REQUIRE(hasSensePin);
      ++senses;
      if(failSense) {
         return std::nullopt;
      }
      return isOn ? loadedMv.value_or(3300) : externalMv;
   }

   bool hasSwitchPin{ true };
   bool hasSensePin{ true };
   bool isOn{ false };
   bool failSense{ false };
   uint16_t externalMv{ 0 };
   std::optional<uint16_t> loadedMv;
   std::vector<bool> switches;
   unsigned senses{ 0 };
};

// Records the delays and the state of the switch at each delay.
class FakeDelay final : public protocol::Delay {
   public:
   explicit FakeDelay(const FakePower &power)
      : power_(power) {
   }

   void microseconds(uint16_t) override {
   }

   void milliseconds(uint16_t ms) override {
      delays.push_back({ ms, power_.isOn });
   }

   struct Delayed {
      uint16_t ms;
      bool switchOn;
   };
   std::vector<Delayed> delays;

   private:
   const FakePower &power_;
};

using Bytes = std::vector<uint8_t>;

// A POWER block with one sub-command.
Bytes power(uint8_t command, const Bytes &payload = {}, uint8_t index = 0) {
   Bytes block{ block::power,
                0,
                0,
                static_cast<uint8_t>(command | index),
                static_cast<uint8_t>(payload.size()),
                static_cast<uint8_t>(payload.size() >> 8) };
   block.insert(block.end(), payload.begin(), payload.end());
   block[1] = static_cast<uint8_t>(block.size());
   block[2] = static_cast<uint8_t>(block.size() >> 8);
   return block;
}

Bytes out(uint16_t mv) {
   return power(protocol::subcommand::out, { static_cast<uint8_t>(mv), static_cast<uint8_t>(mv >> 8) });
}

Bytes in() {
   return power(protocol::subcommand::in, { 0x00, 0x00 });
}

class Bench {
   public:
   Bench()
      : delay(hardware)
      , block(hardware, delay)
      , dispatcher(delay) {
      REQUIRE(dispatcher.add(block));
      dispatcher.setTargetVoltage(&block);
   }

   // Runs a POWER block as a top-level command.
   Bytes run(const Bytes &command) {
      std::array<uint8_t, protocol::bufferSize> reply{};
      const auto size = dispatcher.process(art::span<const uint8_t>(command.data(), command.size()),
                                           art::span<uint8_t>(reply.data(), reply.size()));
      REQUIRE(size.has_value());
      return Bytes(reply.begin(), reply.begin() + static_cast<std::ptrdiff_t>(*size));
   }

   FakePower hardware;
   FakeDelay delay;
   PowerBlock block;
   protocol::UsbToAll dispatcher;
};

constexpr uint8_t ok = 0;
constexpr uint8_t failed = 1;
constexpr uint8_t invalidIndex = 3;
constexpr uint8_t invalidPara = 4;
constexpr uint8_t notSupported = 6;

} // namespace

TEST_CASE("INIT, CONFIG and FINI reply OK and change nothing") {
   Bench bench;
   for(const uint8_t command : { protocol::subcommand::init, protocol::subcommand::config, protocol::subcommand::fini }) {
      CAPTURE(int(command));
      CHECK(bench.run(power(command)) == Bytes{ ok });
   }
   CHECK(bench.hardware.switches.empty());
   CHECK(bench.hardware.senses == 0);
}

TEST_CASE("A port index other than 0 replies INVALID_INDEX and changes nothing") {
   Bench bench;
   CHECK(bench.run(power(protocol::subcommand::out, { 0xE4, 0x0C }, 1)) == Bytes{ invalidIndex });
   CHECK(bench.run(power(protocol::subcommand::in, { 0, 0 }, 2)) == Bytes{ invalidIndex, 0x00, 0x00 });
   CHECK(bench.run(power(protocol::subcommand::init, {}, 7)) == Bytes{ invalidIndex });
   CHECK(bench.hardware.switches.empty());
}

TEST_CASE("OUT 3300 switches the supply on, OUT 0 switches it off") {
   Bench bench;
   CHECK(bench.run(out(3300)) == Bytes{ ok });
   CHECK(bench.hardware.isOn);
   CHECK(bench.block.isOn());
   // The block waits for the rail before it checks it.
   REQUIRE(bench.delay.delays.size() == 1);
   CHECK(bench.delay.delays[0].ms == PowerBlock::settleMs);
   CHECK(bench.delay.delays[0].switchOn);

   CHECK(bench.run(out(0)) == Bytes{ ok });
   CHECK_FALSE(bench.hardware.isOn);
   CHECK_FALSE(bench.block.isOn());
}

TEST_CASE("OUT in the state that the supply has already changes nothing") {
   Bench bench;
   CHECK(bench.run(out(0)) == Bytes{ ok });
   CHECK(bench.hardware.switches.empty());
   CHECK(bench.run(out(3300)) == Bytes{ ok });
   CHECK(bench.run(out(3300)) == Bytes{ ok });
   CHECK(bench.hardware.switches == std::vector<bool>{ true });
}

TEST_CASE("OUT accepts only 0 and 3300") {
   Bench bench;
   for(const uint16_t mv : { 1, 1800, 3299, 3301, 5000, 0xFFFF }) {
      CAPTURE(mv);
      CHECK(bench.run(out(mv)) == Bytes{ invalidPara });
   }
   CHECK(bench.run(power(protocol::subcommand::out)) == Bytes{ invalidPara });
   CHECK(bench.run(power(protocol::subcommand::out, { 0xE4 })) == Bytes{ invalidPara });
   CHECK(bench.run(power(protocol::subcommand::out, { 0xE4, 0x0C, 0x00 })) == Bytes{ invalidPara });
   CHECK(bench.hardware.switches.empty());
}

TEST_CASE("OUT 3300 refuses a target with its own supply") {
   Bench bench;
   // The threshold itself is not a supply.
   bench.hardware.externalMv = PowerBlock::externalSupplyMillivolts + 1;
   CHECK(bench.run(out(3300)) == Bytes{ failed });
   CHECK(bench.hardware.switches.empty());
   CHECK_FALSE(bench.block.isOn());

   bench.hardware.externalMv = 3300;
   CHECK(bench.run(out(3300)) == Bytes{ failed });
   CHECK(bench.hardware.switches.empty());

   bench.hardware.externalMv = PowerBlock::externalSupplyMillivolts;
   CHECK(bench.run(out(3300)) == Bytes{ ok });
   CHECK(bench.hardware.isOn);
}

TEST_CASE("OUT 3300 refuses when the voltage cannot be measured") {
   Bench bench;
   bench.hardware.failSense = true;
   CHECK(bench.run(out(3300)) == Bytes{ failed });
   CHECK(bench.hardware.switches.empty());
}

TEST_CASE("OUT 3300 switches off again when the rail does not come up") {
   Bench bench;
   bench.hardware.loadedMv = 200;
   CHECK(bench.run(out(3300)) == Bytes{ failed });
   CHECK(bench.hardware.switches == std::vector<bool>{ true, false });
   CHECK_FALSE(bench.hardware.isOn);
   CHECK_FALSE(bench.block.isOn());
}

TEST_CASE("OUT 0 switches off with a sense that fails") {
   Bench bench;
   REQUIRE(bench.run(out(3300)) == Bytes{ ok });
   bench.hardware.failSense = true;
   CHECK(bench.run(out(0)) == Bytes{ ok });
   CHECK_FALSE(bench.hardware.isOn);
}

TEST_CASE("IN replies the voltage of the sense, little-endian") {
   Bench bench;
   bench.hardware.externalMv = 0x1234;
   CHECK(bench.run(in()) == Bytes{ ok, 0x34, 0x12 });
   bench.hardware.externalMv = 0;
   CHECK(bench.run(in()) == Bytes{ ok, 0x00, 0x00 });
   REQUIRE(bench.run(out(3300)) == Bytes{ ok });
   CHECK(bench.run(in()) == Bytes{ ok, 0xE4, 0x0C });
}

TEST_CASE("IN ignores the payload, as the original firmware does") {
   Bench bench;
   bench.hardware.externalMv = 100;
   CHECK(bench.run(power(protocol::subcommand::in)) == Bytes{ ok, 100, 0x00 });
   CHECK(bench.run(power(protocol::subcommand::in, { 1, 2, 3, 4 })) == Bytes{ ok, 100, 0x00 });
}

TEST_CASE("IN replies FAILED with its data size when the measurement fails") {
   Bench bench;
   bench.hardware.failSense = true;
   CHECK(bench.run(in()) == Bytes{ failed, 0x00, 0x00 });
}

TEST_CASE("A sub-command that is not in the block replies CMD_NOT_SUPPORT") {
   Bench bench;
   CHECK(bench.run(power(protocol::subcommand::inOut, { 0, 0 })) == Bytes{ notSupported });
}

TEST_CASE("Without reply space, a sub-command changes no hardware") {
   Bench bench;
   // Two OUT sub-commands in one block. The reply buffer holds one status.
   Bytes two{ block::power, 0,   0, protocol::subcommand::out, 2, 0, 0xE4, 0x0C, protocol::subcommand::out, 2, 0,
              0x00,         0x00 };
   two[1] = static_cast<uint8_t>(two.size());
   std::array<uint8_t, 1> small{};
   CHECK(bench.dispatcher.process(art::span<const uint8_t>(two.data(), two.size()),
                                  art::span<uint8_t>(small.data(), small.size())) == 0);
   // The first sub-command has run, the second has not.
   CHECK(bench.hardware.switches == std::vector<bool>{ true });

   // No space for the first one: nothing runs.
   Bench other;
   CHECK(other.dispatcher.process(art::span<const uint8_t>(two.data(), two.size()), art::span<uint8_t>(small.data(), 0)) == 0);
   CHECK(other.hardware.switches.empty());
   CHECK(other.hardware.senses == 0);
}

TEST_CASE("The host going away keeps the supply on") {
   Bench bench;
   REQUIRE(bench.run(out(3300)) == Bytes{ ok });
   bench.dispatcher.hostGone();
   CHECK(bench.hardware.isOn);
}

TEST_CASE("poll() switches the supply off after the voltage stays low") {
   Bench bench;
   REQUIRE(bench.run(out(3300)) == Bytes{ ok });
   // The target pulls the rail down, for example with a short circuit.
   bench.hardware.loadedMv = 100;
   uint64_t now = 1000;
   for(unsigned i = 1; i < PowerBlock::lowChecksToOff; ++i) {
      bench.block.poll(now);
      now += PowerBlock::pollIntervalMs;
      CHECK(bench.block.isOn());
   }
   bench.block.poll(now);
   CHECK_FALSE(bench.block.isOn());
   CHECK_FALSE(bench.hardware.isOn);
   // The sense now shows the target supply again, which is 0.
   CHECK(bench.hardware.switches == std::vector<bool>{ true, false });
}

TEST_CASE("poll() keeps the supply on when the voltage recovers") {
   Bench bench;
   REQUIRE(bench.run(out(3300)) == Bytes{ ok });
   uint64_t now = 1000;
   for(unsigned round = 0; round < 5; ++round) {
      bench.hardware.loadedMv = 100;
      for(unsigned i = 1; i < PowerBlock::lowChecksToOff; ++i) {
         bench.block.poll(now);
         now += PowerBlock::pollIntervalMs;
      }
      // A dip of the rail, for example when the target starts
      bench.hardware.loadedMv = std::nullopt;
      bench.block.poll(now);
      now += PowerBlock::pollIntervalMs;
   }
   CHECK(bench.block.isOn());
}

TEST_CASE("poll() checks only each poll interval, and only while the supply is on") {
   Bench bench;
   bench.block.poll(5000);
   CHECK(bench.hardware.senses == 0);

   REQUIRE(bench.run(out(3300)) == Bytes{ ok });
   const unsigned afterOut = bench.hardware.senses;
   bench.block.poll(10'000);
   CHECK(bench.hardware.senses == afterOut + 1);
   bench.block.poll(10'000 + PowerBlock::pollIntervalMs - 1);
   CHECK(bench.hardware.senses == afterOut + 1);
   bench.block.poll(10'000 + PowerBlock::pollIntervalMs);
   CHECK(bench.hardware.senses == afterOut + 2);
}

TEST_CASE("poll() ignores a failed measurement") {
   Bench bench;
   REQUIRE(bench.run(out(3300)) == Bytes{ ok });
   bench.hardware.failSense = true;
   uint64_t now = 1000;
   for(unsigned i = 0; i < 3 * PowerBlock::lowChecksToOff; ++i) {
      bench.block.poll(now);
      now += PowerBlock::pollIntervalMs;
   }
   CHECK(bench.block.isOn());
}

TEST_CASE("A low check before OUT 3300 does not count for the new supply") {
   Bench bench;
   REQUIRE(bench.run(out(3300)) == Bytes{ ok });
   bench.hardware.loadedMv = 100;
   uint64_t now = 1000;
   for(unsigned i = 1; i < PowerBlock::lowChecksToOff; ++i) {
      bench.block.poll(now);
      now += PowerBlock::pollIntervalMs;
   }
   REQUIRE(bench.run(out(0)) == Bytes{ ok });
   bench.hardware.loadedMv = std::nullopt;
   REQUIRE(bench.run(out(3300)) == Bytes{ ok });
   bench.hardware.loadedMv = 100;
   bench.block.poll(now);
   CHECK(bench.block.isOn());
}

TEST_CASE("GET_TVCC replies the voltage of the sense") {
   Bench bench;
   bench.hardware.externalMv = 500;
   CHECK(bench.dispatcher.targetVoltage() == 500);
   REQUIRE(bench.run(out(3300)) == Bytes{ ok });
   CHECK(bench.dispatcher.targetVoltage() == 3300);
   bench.hardware.failSense = true;
   CHECK(bench.dispatcher.targetVoltage() == 0);
}

TEST_CASE("A board without a power switch: OUT 0 is OK, OUT 3300 is not supported") {
   Bench bench;
   bench.hardware.hasSwitchPin = false;
   CHECK(bench.run(out(0)) == Bytes{ ok });
   CHECK(bench.run(out(3300)) == Bytes{ notSupported });
   CHECK(bench.run(out(1234)) == Bytes{ invalidPara });
   CHECK_FALSE(bench.block.isOn());
   // The sense still works.
   bench.hardware.externalMv = 3300;
   CHECK(bench.run(in()) == Bytes{ ok, 0xE4, 0x0C });
}

TEST_CASE("A board without a voltage sense switches without the checks and never by itself") {
   Bench bench;
   bench.hardware.hasSensePin = false;
   CHECK(bench.run(in()) == Bytes{ notSupported, 0x00, 0x00 });
   CHECK(bench.dispatcher.targetVoltage() == 0);

   CHECK(bench.run(out(3300)) == Bytes{ ok });
   CHECK(bench.hardware.isOn);
   CHECK(bench.delay.delays.empty());
   bench.block.poll(1000);
   bench.block.poll(2000);
   CHECK(bench.hardware.isOn);
   CHECK(bench.run(out(0)) == Bytes{ ok });
   CHECK_FALSE(bench.hardware.isOn);
}

TEST_CASE("A board with neither a switch nor a sense") {
   Bench bench;
   bench.hardware.hasSwitchPin = false;
   bench.hardware.hasSensePin = false;
   CHECK(bench.run(out(0)) == Bytes{ ok });
   CHECK(bench.run(out(3300)) == Bytes{ notSupported });
   CHECK(bench.run(in()) == Bytes{ notSupported, 0x00, 0x00 });
   CHECK(bench.dispatcher.targetVoltage() == 0);
   bench.block.poll(1000);
}

TEST_CASE("The POWER block is in the capability bitmap") {
   Bench bench;
   const auto bitmap = bench.dispatcher.capabilities();
   const unsigned bit = block::power - protocol::usbToXxxFirst;
   CHECK((bitmap[bit / 8] & (1u << (bit % 8))) != 0);
}
