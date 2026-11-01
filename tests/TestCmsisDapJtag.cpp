// SPDX-License-Identifier: GPL-3.0-or-later
// The CMSIS-DAP command processor on JTAG: the JTAG bit-bang engine and the
// JTAG fast path on the mock JTAG pins, with the JTAG chain and JTAG-DP of
// MockJtagDp.h.

#include "jtag/JtagBitBang.h"
#include "jtag/JtagFastPath.h"
#include "protocol/cmsisdap/DapProcessor.h"

#include "MockJtagDp.h"
#include "MockJtagPins.h"
#include "MockJtagSpi.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
using V = std::vector<uint8_t>;
using cmsisdap::Processor;
using mupptest::MockJtagChain;
using Access = MockJtagChain::Access;
using State = MockJtagChain::State;
using Pins = mupptest::MockJtagPins<>;

// The DAP_Transfer request bytes
constexpr uint8_t readDpidr = 0x02;
constexpr uint8_t writeCtrlStat = 0x04;
constexpr uint8_t readCtrlStat = 0x06;
constexpr uint8_t writeSelect = 0x08;
constexpr uint8_t writeTar = 0x05;
constexpr uint8_t writeDrw = 0x0D;
constexpr uint8_t readDrw = 0x0F;
constexpr uint8_t matchValue = 0x10;
constexpr uint8_t matchMask = 0x20;

constexpr uint32_t dpIdcode = 0x3BA00477;
constexpr uint32_t bscIdcode = 0x06414041;
constexpr uint32_t otherIdcode = 0x12345679;
constexpr uint32_t firstIdcode = 0x0A0B0C0D;

// The chain of an STM32F1: the Cortex-M3 JTAG-DP next to TDO, then the
// boundary scan TAP. Its IDCODE instruction is 0b00001, not 0x0E.
constexpr uint32_t bscIdcodeIr = 0x01;

std::vector<MockJtagChain::Device> stm32Chain() {
   return { { 4, dpIdcode, true }, { 5, bscIdcode, false, bscIdcodeIr } };
}

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

// An SWD engine that only counts: the JTAG tests do not use it.
class CountingSwd final : public cmsisdap::DapSwd {
   public:
   void init() override {
      ++inits;
   }
   void fini() override {
      ++finis;
   }
   void setClockHz(uint32_t) override {
   }
   void apply(const cmsisdap::SwdSettings &) override {
   }
   void sequenceOut(uint16_t, art::span<const uint8_t>) override {
      ++sequences;
   }
   void sequenceIn(uint16_t, art::span<uint8_t>) override {
      ++sequences;
   }
   uint8_t transfer(uint8_t, uint32_t &) override {
      ++transfers;
      return cmsisdap::dap::ackOk;
   }
   void setAbort(const cmsisdap::TransferAbort &) override {
   }

   int inits{ 0 };
   int finis{ 0 };
   int sequences{ 0 };
   int transfers{ 0 };
};

class NoGpio final : public protocol::GpioPins {
   public:
   [[nodiscard]] uint16_t available() const override {
      return 0;
   }
   void configure(uint16_t, protocol::PinMode, bool) override {
   }
   void write(uint16_t, uint16_t) override {
   }
   [[nodiscard]] uint16_t read(uint16_t) const override {
      return 0;
   }
};

class NoPlatform final : public cmsisdap::Platform {
   public:
   uint32_t nowUs() override {
      return 0;
   }
   void delayUs(uint32_t) override {
   }
   void setLed(cmsisdap::Led, bool) override {
   }
};

cmsisdap::DeviceInfo deviceInfo() {
   cmsisdap::DeviceInfo info;
   info.vendor = "Vendor";
   info.product = "Product";
   return info;
}

// A processor on a JTAG port of type Port, with the mock chain.
template <typename Port>
struct BasicBench {
   explicit BasicBench(std::vector<MockJtagChain::Device> devices = stm32Chain())
      : chain(std::move(devices))
      , pins(chain)
      , port(pins)
      , info(deviceInfo())
      , dap(swd, gpio, platform, info, &port) {
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

   // DAP_Connect to JTAG, and Test-Logic-Reset then Run-Test/Idle with
   // DAP_SWJ_Sequence, as the host tools do.
   void connect() {
      REQUIRE(run({ 0x02, 0x02 }) == V{ 0x02, 0x02 });
      REQUIRE(run({ 0x12, 0x08, 0x3F }) == V{ 0x12, 0x00 });
      REQUIRE(chain.state == State::Idle);
   }

   void configure(const Bytes &irLengths) {
      Bytes request{ 0x15, static_cast<uint8_t>(irLengths.size()) };
      request.insert(request.end(), irLengths.begin(), irLengths.end());
      REQUIRE(run(request) == V{ 0x15, 0x00 });
   }

   MockJtagChain chain;
   Pins pins;
   Port port;
   CountingSwd swd;
   NoGpio gpio;
   NoPlatform platform;
   cmsisdap::DeviceInfo info;
   Processor dap;
};

using Bench = BasicBench<jtag::BitBangPort<Pins>>;

// The bit-bang engine with a hook after each shift: the background service
// of the engine runs during a shift (protocol/Background.h), and the host
// can send a packet then.
class HookedPort final : public protocol::JtagPort {
   public:
   explicit HookedPort(Pins &pins)
      : port_(pins) {
   }

   void init() override {
      port_.init();
   }
   void fini() override {
      port_.fini();
   }
   [[nodiscard]] bool setSpeed(uint32_t kHz) override {
      return port_.setSpeed(kHz);
   }
   [[nodiscard]] bool
   shift(uint32_t bitCount, art::span<const uint8_t> tdi, art::span<const uint8_t> tms, art::span<uint8_t> tdo) override {
      const bool ok = port_.shift(bitCount, tdi, tms, tdo);
      if(onShift) {
         onShift();
      }
      return ok;
   }

   std::function<void()> onShift;

   private:
   jtag::BitBangPort<Pins> port_;
};

// The bit-bang engine, with a check of the JtagPort contract at each
// shift: the TDI and TMS bits above bitCount are 0, and tdo is 0 on entry.
class CheckedPort final : public protocol::JtagPort {
   public:
   explicit CheckedPort(Pins &pins)
      : port_(pins) {
   }

   void init() override {
      port_.init();
   }
   void fini() override {
      port_.fini();
   }
   [[nodiscard]] bool setSpeed(uint32_t kHz) override {
      return port_.setSpeed(kHz);
   }
   [[nodiscard]] bool
   shift(uint32_t bitCount, art::span<const uint8_t> tdi, art::span<const uint8_t> tms, art::span<uint8_t> tdo) override {
      ++shifts;
      const auto unused = static_cast<uint8_t>(bitCount % 8 == 0 ? 0 : 0xFFu << (bitCount % 8));
      if((tdi.back() & unused) != 0 || (tms.back() & unused) != 0 ||
         std::any_of(tdo.begin(), tdo.end(), [](uint8_t byte) { return byte != 0; })) {
         ++violations;
      }
      return port_.shift(bitCount, tdi, tms, tdo);
   }

   int shifts{ 0 };
   int violations{ 0 };

   private:
   jtag::BitBangPort<Pins> port_;
};

// An STM32F1 chain, connected and configured
struct Stm32Bench : Bench {
   Stm32Bench() {
      connect();
      configure({ 4, 5 });
   }
};

} // namespace

TEST_CASE("DAP JTAG: DAP_Info reports JTAG with a JTAG port") {
   Bench b;
   CHECK(b.run({ 0x00, 0xF0 }) == V{ 0x00, 0x01, 0x13 });
}

TEST_CASE("DAP JTAG: DAP_Connect and DAP_Disconnect") {
   Bench b;
   CHECK(b.run({ 0x02, 0x02 }) == V{ 0x02, 0x02 });
   CHECK(b.dap.connected());
   CHECK(b.pins.inits == 1);
   CHECK(b.swd.inits == 0);
   // The clock after a reset of the debug unit: 1 MHz
   CHECK(b.port.timing().halfCycles == jtag::halfPeriodCycles(72'000'000, 1000));

   // SWD releases the JTAG pins first, and JTAG the SWD pins.
   CHECK(b.run({ 0x02, 0x01 }) == V{ 0x02, 0x01 });
   CHECK(b.pins.finis == 1);
   CHECK(b.swd.inits == 1);
   CHECK(b.run({ 0x02, 0x02 }) == V{ 0x02, 0x02 });
   CHECK(b.swd.finis == 1);
   CHECK(b.pins.inits == 2);

   CHECK(b.run({ 0x03 }) == V{ 0x03, 0x00 });
   CHECK_FALSE(b.dap.connected());
   CHECK(b.pins.finis == 2);

   CHECK(b.run({ 0x02, 0x02 }) == V{ 0x02, 0x02 });
   b.dap.hostGone();
   CHECK_FALSE(b.dap.connected());
   CHECK(b.pins.finis == 3);
}

TEST_CASE("DAP JTAG: DAP_SWJ_Clock sets TCK in kHz") {
   Bench b;
   // Before the connection: DAP_Connect sets it.
   CHECK(b.run({ 0x11, 0x20, 0xA1, 0x07, 0x00 }) == V{ 0x11, 0x00 });
   CHECK(b.run({ 0x02, 0x02 }) == V{ 0x02, 0x02 });
   CHECK(b.port.timing().halfCycles == jtag::halfPeriodCycles(72'000'000, 500));
   // 1999 Hz is 1 kHz, rounded down. Below 1 kHz is 1 kHz.
   CHECK(b.run(cat({ { 0x11 }, word(1999) })) == V{ 0x11, 0x00 });
   CHECK(b.port.timing().halfCycles == jtag::halfPeriodCycles(72'000'000, 1));
   CHECK(b.run(cat({ { 0x11 }, word(100) })) == V{ 0x11, 0x00 });
   CHECK(b.port.timing() == jtag::Timing{ jtag::Clocking::Timed, jtag::halfPeriodCycles(72'000'000, 1), 0 });
   CHECK(b.run(cat({ { 0x11 }, word(4'000'000) })) == V{ 0x11, 0x00 });
   CHECK(b.port.timing().halfCycles == jtag::halfPeriodCycles(72'000'000, 4000));
}

TEST_CASE("DAP JTAG: DAP_JTAG_Configure checks the chain") {
   Bench b;
   CHECK(b.run({ 0x15, 0x02, 0x04, 0x05 }) == V{ 0x15, 0x00 });
   CHECK(b.run({ 0x15, 0x08, 4, 4, 4, 4, 4, 4, 4, 32 }) == V{ 0x15, 0x00 });
   CHECK(b.run({ 0x15, 0x00 }) == V{ 0x15, 0x00 });
   // More than 8 devices, an IR length of 0 or more than 32
   CHECK(b.run({ 0x15, 0x09, 4, 4, 4, 4, 4, 4, 4, 4, 4 }) == V{ 0x15, 0xFF });
   CHECK(b.run({ 0x15, 0x02, 0x04, 0x00 }) == V{ 0x15, 0xFF });
   CHECK(b.run({ 0x15, 0x01, 33 }) == V{ 0x15, 0xFF });
   // Too short
   CHECK(b.run({ 0x15, 0x02, 0x04 }) == V{ 0xFF });

   // A bad chain has no device.
   b.connect();
   CHECK(b.run({ 0x16, 0x00 }) == V{ 0x16, 0xFF });
}

TEST_CASE("DAP JTAG: DAP_JTAG_Configure without a JTAG port replies an error") {
   CountingSwd swd;
   NoGpio gpio;
   NoPlatform platform;
   Processor dap(swd, gpio, platform, deviceInfo());
   dap.submit(V{ 0x15, 0x01, 0x04 });
   dap.run();
   const auto response = dap.response();
   CHECK(Bytes(response.begin(), response.end()) == V{ 0x15, 0xFF });
}

TEST_CASE("DAP JTAG: DAP_JTAG_IDCODE reads each device of the chain") {
   Stm32Bench b;
   CHECK(b.run({ 0x16, 0x00 }) == cat({ { 0x16, 0x00 }, word(dpIdcode) }));
   // The boundary scan TAP was in BYPASS.
   CHECK(b.chain.ir(1) == 0x1F);
   // DAP_JTAG_IDCODE uses the instruction of the Arm JTAG-DP, as the
   // reference implementation. The boundary scan TAP then is in BYPASS: a
   // 0 bit, then the ones of TDI.
   CHECK(b.run({ 0x16, 0x01 }) == cat({ { 0x16, 0x00 }, word(0xFFFFFFFE) }));
   CHECK(b.chain.ir(0) == 0x0F);
   CHECK(b.chain.state == State::Idle);
   // No device 2
   CHECK(b.run({ 0x16, 0x02 }) == V{ 0x16, 0xFF });
   CHECK(b.run({ 0x16 }) == V{ 0xFF });
}

TEST_CASE("DAP JTAG: DAP_JTAG_IDCODE needs a JTAG connection") {
   Bench b;
   b.configure({ 4, 5 });
   CHECK(b.run({ 0x16, 0x00 }) == V{ 0x16, 0xFF });
   CHECK(b.run({ 0x02, 0x01 }) == V{ 0x02, 0x01 });
   CHECK(b.run({ 0x16, 0x00 }) == V{ 0x16, 0xFF });
   CHECK(b.pins.rising.empty());
}

TEST_CASE("DAP JTAG: DAP_JTAG_Sequence moves the TAP and reads TDO") {
   Bench b;
   CHECK(b.run({ 0x02, 0x02 }) == V{ 0x02, 0x02 });
   // Test-Logic-Reset (IR is IDCODE), Run-Test/Idle, Select-DR-Scan,
   // Capture-DR, Shift-DR. Then 64 bits of TDO (cycle count 0), Exit1-DR,
   // Update-DR. The next request goes to Run-Test/Idle.
   const Bytes request{ 0x14, 0x06,
                        // 5 cycles, TMS high
                        0x45, 0x00,
                        // 1 cycle, TMS low
                        0x01, 0x00,
                        // 1 cycle, TMS high
                        0x41, 0x00,
                        // 2 cycles, TMS low
                        0x02, 0x00,
                        // 64 cycles, TMS low, read TDO
                        0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                        // 2 cycles, TMS high
                        0x42, 0x00 };
   CHECK(b.run(request) == cat({ { 0x14, 0x00 }, word(dpIdcode), word(bscIdcode) }));
   CHECK(b.chain.state == State::UpdateDr);
   CHECK(b.run({ 0x14, 0x01, 0x01, 0x00 }) == V{ 0x14, 0x00 });
   CHECK(b.chain.state == State::Idle);
   // Too short
   CHECK(b.run({ 0x14, 0x02, 0x01, 0x00, 0x08 }) == V{ 0xFF });
}

TEST_CASE("DAP JTAG: DAP_JTAG_Sequence needs a JTAG connection") {
   Bench b;
   CHECK(b.run({ 0x14, 0x01, 0x88, 0x00 }) == V{ 0x14, 0xFF });
   CHECK(b.pins.rising.empty());
}

TEST_CASE("DAP JTAG: DAP_SWJ_Sequence drives TMS, and DAP_SWD_Sequence fails") {
   Bench b;
   CHECK(b.run({ 0x02, 0x02 }) == V{ 0x02, 0x02 });
   // Test-Logic-Reset, then Run-Test/Idle
   CHECK(b.run({ 0x12, 0x06, 0x1F }) == V{ 0x12, 0x00 });
   CHECK(b.chain.state == State::Idle);
   CHECK(b.pins.rising.size() == 6);
   for(const auto &edge : b.pins.rising) {
      CHECK(edge.tdi);
   }
   CHECK(b.run({ 0x1D, 0x01, 0x08, 0x00 }) == V{ 0x1D, 0xFF });
   CHECK(b.swd.sequences == 0);
}

TEST_CASE("DAP JTAG: DAP_Transfer reads and writes DP and AP registers") {
   Stm32Bench b;
   const Bytes request = cat({ { 0x05, 0x00, 0x0A },
                               { writeCtrlStat },
                               word(0x50000000),
                               { readCtrlStat },
                               { writeSelect },
                               word(0),
                               { writeTar },
                               word(0x20000000),
                               { writeDrw },
                               word(0x11111111),
                               { writeDrw },
                               word(0x22222222),
                               { writeTar },
                               word(0x20000000),
                               { readDrw },
                               { readDrw },
                               { readDpidr } });
   CHECK(b.run(request) ==
         cat({ { 0x05, 0x0A, 0x01 }, word(0xF0000000), word(0x11111111), word(0x22222222), word(b.chain.dpidr) }));
   CHECK(b.chain.memory[0x20000000] == 0x11111111);
   CHECK(b.chain.memory[0x20000004] == 0x22222222);
   // The DP posts each read: a DP read needs a read of RDBUFF, and the AP
   // reads are posted as on SWD.
   const std::vector<Access> expected{
      { false, false, 0x4, 0x50000000 },
      { false, true, 0x4, 0 },
      { false, true, 0xC, 0 },
      { false, false, 0x8, 0 },
      { true, false, 0x4, 0x20000000 },
      { true, false, 0xC, 0x11111111 },
      { true, false, 0xC, 0x22222222 },
      { true, false, 0x4, 0x20000000 },
      { true, true, 0xC, 0 },
      { true, true, 0xC, 0 },
      { false, true, 0xC, 0 },
      { false, true, 0x0, 0 },
      { false, true, 0xC, 0 },
   };
   CHECK(b.chain.accesses == expected);
   // DPACC, APACC, DPACC
   CHECK(b.chain.irScans == 3);
   CHECK(b.chain.state == State::Idle);

   // The IR stays DPACC for the next command.
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(b.chain.dpidr) }));
   CHECK(b.chain.irScans == 3);
}

TEST_CASE("DAP JTAG: a write ends with a read of RDBUFF") {
   Stm32Bench b;
   CHECK(b.run(cat({ { 0x05, 0x00, 0x01, writeSelect }, word(0x01000000) })) == V{ 0x05, 0x01, 0x01 });
   CHECK(b.chain.select == 0x01000000);
   const std::vector<Access> expected{ { false, false, 0x8, 0x01000000 }, { false, true, 0xC, 0 } };
   CHECK(b.chain.accesses == expected);
}

TEST_CASE("DAP JTAG: transfers with a device in the middle of the chain") {
   Bench b({ { 5, firstIdcode, false }, { 4, dpIdcode, true }, { 6, otherIdcode, false } });
   b.connect();
   b.configure({ 5, 4, 6 });
   CHECK(b.run({ 0x16, 0x00 }) == cat({ { 0x16, 0x00 }, word(firstIdcode) }));
   CHECK(b.run({ 0x16, 0x01 }) == cat({ { 0x16, 0x00 }, word(dpIdcode) }));
   CHECK(b.run({ 0x16, 0x02 }) == cat({ { 0x16, 0x00 }, word(otherIdcode) }));

   const Bytes request = cat({ { 0x05, 0x01, 0x03, writeCtrlStat }, word(0x50000000), { readCtrlStat, readDpidr } });
   CHECK(b.run(request) == cat({ { 0x05, 0x03, 0x01 }, word(0xF0000000), word(b.chain.dpidr) }));
   // The other devices are in BYPASS.
   CHECK(b.chain.ir(0) == 0x1F);
   CHECK(b.chain.ir(1) == 0x0A);
   CHECK(b.chain.ir(2) == 0x3F);

   // A device that is not in the chain gives no transfer.
   b.chain.accesses.clear();
   CHECK(b.run({ 0x05, 0x03, 0x01, readDpidr }) == V{ 0x05, 0x00, 0x00 });
   CHECK(b.run({ 0x06, 0x03, 0x01, 0x00, readDpidr }) == V{ 0x06, 0x00, 0x00, 0x00 });
   CHECK(b.chain.accesses.empty());
}

TEST_CASE("DAP JTAG: WAIT retries, and the retry limit") {
   Stm32Bench b;
   b.chain.apWaits = 3;
   CHECK(b.run(cat({ { 0x05, 0x00, 0x03, writeTar }, word(0x20000000), { writeDrw }, word(0xCAFEF00D), { readDpidr } })) ==
         cat({ { 0x05, 0x03, 0x01 }, word(b.chain.dpidr) }));
   CHECK(b.chain.waits == 6);
   CHECK(b.chain.memory[0x20000000] == 0xCAFEF00D);
   CHECK(b.dap.errorCount() == 0);

   // One retry: the AP read after the AP write gets WAIT.
   CHECK(b.run({ 0x04, 0x00, 0x01, 0x00, 0x00, 0x00 }) == V{ 0x04, 0x00 });
   CHECK(b.run(cat({ { 0x05, 0x00, 0x02, writeTar }, word(0x20000000), { readDrw } })) == V{ 0x05, 0x01, 0x02 });
   CHECK(b.dap.errorCount() == 1);
}

TEST_CASE("DAP JTAG: an invalid acknowledge is an error") {
   Stm32Bench b;
   b.chain.tdoStuckHigh = true;
   // The acknowledge 0b111 is no acknowledge.
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == V{ 0x05, 0x00, 0x07 });
   CHECK(b.dap.errorCount() == 1);
   CHECK(b.run({ 0x06, 0x00, 0x02, 0x00, readDrw }) == V{ 0x06, 0x00, 0x00, 0x07 });
   CHECK(b.dap.errorCount() == 2);
}

TEST_CASE("DAP JTAG: value match") {
   Stm32Bench b;
   CHECK(b.run(cat({ { 0x05, 0x00, 0x01, writeCtrlStat }, word(0x50000000) })) == V{ 0x05, 0x01, 0x01 });
   const Bytes match =
   cat({ { 0x05, 0x00, 0x02, matchMask }, word(0xF0000000), { readCtrlStat | matchValue }, word(0xF0000000) });
   CHECK(b.run(match) == V{ 0x05, 0x02, 0x01 });
   const Bytes mismatch =
   cat({ { 0x05, 0x00, 0x02, matchMask }, word(0xF0000000), { readCtrlStat | matchValue }, word(0x50000000) });
   CHECK(b.run(mismatch) == V{ 0x05, 0x01, 0x11 });
}

TEST_CASE("DAP JTAG: DAP_TransferBlock writes and reads a block") {
   Stm32Bench b;
   CHECK(b.run(cat({ { 0x05, 0x00, 0x01, writeTar }, word(0x20000100) })) == V{ 0x05, 0x01, 0x01 });
   const Bytes write = cat({ { 0x06, 0x00, 0x04, 0x00, writeDrw }, word(1), word(2), word(3), word(4) });
   CHECK(b.run(write) == V{ 0x06, 0x04, 0x00, 0x01 });
   CHECK(b.chain.memory[0x2000010C] == 4);

   CHECK(b.run(cat({ { 0x05, 0x00, 0x01, writeTar }, word(0x20000100) })) == V{ 0x05, 0x01, 0x01 });
   CHECK(b.run({ 0x06, 0x00, 0x04, 0x00, readDrw }) == cat({ { 0x06, 0x04, 0x00, 0x01 }, word(1), word(2), word(3), word(4) }));
   // A DP block read
   CHECK(b.run({ 0x06, 0x00, 0x02, 0x00, readDpidr }) ==
         cat({ { 0x06, 0x02, 0x00, 0x01 }, word(b.chain.dpidr), word(b.chain.dpidr) }));
}

TEST_CASE("DAP JTAG: WAIT in a block, and the retry limit") {
   Stm32Bench b;
   b.chain.apWaits = 2;
   CHECK(b.run(cat({ { 0x05, 0x00, 0x01, writeTar }, word(0x20000200) })) == V{ 0x05, 0x01, 0x01 });
   const Bytes write = cat({ { 0x06, 0x00, 0x03, 0x00, writeDrw }, word(7), word(8), word(9) });
   CHECK(b.run(write) == V{ 0x06, 0x03, 0x00, 0x01 });
   CHECK(b.run(cat({ { 0x05, 0x00, 0x01, writeTar }, word(0x20000200) })) == V{ 0x05, 0x01, 0x01 });
   CHECK(b.run({ 0x06, 0x00, 0x03, 0x00, readDrw }) == cat({ { 0x06, 0x03, 0x00, 0x01 }, word(7), word(8), word(9) }));
   CHECK(b.chain.waits > 0);

   // No retry: the second write of the block gets WAIT.
   CHECK(b.run({ 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 }) == V{ 0x04, 0x00 });
   b.chain.apWaits = 1;
   CHECK(b.run(write) == V{ 0x06, 0x01, 0x00, 0x02 });
}

TEST_CASE("DAP JTAG: a DP write after a posted AP read") {
   Stm32Bench b;
   b.chain.memory[0] = 0x5EED0001;
   b.chain.accesses.clear();
   CHECK(b.run(cat({ { 0x05, 0x00, 0x02, readDrw, writeSelect }, word(0x000000F0) })) ==
         cat({ { 0x05, 0x02, 0x01 }, word(0x5EED0001) }));
   CHECK(b.chain.select == 0xF0);
   // The posted data from RDBUFF, then the write and its check
   const std::vector<Access> expected{
      { true, true, 0xC, 0 }, { false, true, 0xC, 0 }, { false, false, 0x8, 0xF0 }, { false, true, 0xC, 0 }
   };
   CHECK(b.chain.accesses == expected);
}

TEST_CASE("DAP JTAG: DAP_TransferAbort stops an AP block read") {
   BasicBench<HookedPort> b;
   b.connect();
   b.configure({ 4, 5 });
   for(uint32_t i = 0; i < 4; ++i) {
      b.chain.memory[0x20000000 + 4 * i] = 0xA0 + i;
   }
   CHECK(b.run(cat({ { 0x05, 0x00, 0x01, writeTar }, word(0x20000000) })) == V{ 0x05, 0x01, 0x01 });
   b.chain.accesses.clear();
   bool sent = false;
   b.port.onShift = [&] {
      const auto apReads = std::count_if(b.chain.accesses.begin(), b.chain.accesses.end(),
                                         [](const Access &access) { return access.ap && access.read; });
      if(apReads == 2 && !sent) {
         sent = true;
         b.dap.submit(V{ 0x07 });
      }
   };
   // The posted read and one more AP read, then the abort: the data of the
   // posted read comes from RDBUFF.
   CHECK(b.run({ 0x06, 0x00, 0x04, 0x00, readDrw }) == cat({ { 0x06, 0x02, 0x00, 0x01 }, word(0xA0), word(0xA1) }));
   CHECK(sent);
   const std::vector<Access> expected{ { true, true, 0xC, 0 }, { true, true, 0xC, 0 }, { false, true, 0xC, 0 } };
   CHECK(b.chain.accesses == expected);
   // The next packet runs normally.
   b.port.onShift = nullptr;
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(b.chain.dpidr) }));
}

TEST_CASE("DAP JTAG: DAP_WriteABORT scans ABORT") {
   Stm32Bench b;
   CHECK(b.run(cat({ { 0x08, 0x00 }, word(0x1E) })) == V{ 0x08, 0x00 });
   CHECK(b.chain.aborts == std::vector<uint32_t>{ 0x1E });
   CHECK(b.chain.ir(0) == 0x08);
   CHECK(b.chain.accesses.empty());
   // No device 2
   CHECK(b.run(cat({ { 0x08, 0x02 }, word(0x1E) })) == V{ 0x08, 0xFF });
   CHECK(b.chain.aborts.size() == 1);
   // The next transfer writes the IR again.
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(b.chain.dpidr) }));
}

TEST_CASE("DAP JTAG: a sequence makes the next transfer write the IR") {
   Stm32Bench b;
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(b.chain.dpidr) }));
   CHECK(b.chain.irScans == 1);
   // Test-Logic-Reset loads IDCODE.
   CHECK(b.run({ 0x12, 0x08, 0x3F }) == V{ 0x12, 0x00 });
   CHECK(b.chain.ir(0) == mupptest::tapIr::idcode);
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(b.chain.dpidr) }));
   CHECK(b.chain.irScans == 2);
   CHECK(b.run({ 0x14, 0x01, 0x01, 0x00 }) == V{ 0x14, 0x00 });
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(b.chain.dpidr) }));
   CHECK(b.chain.irScans == 3);
   CHECK(b.run({ 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 }) == V{ 0x10, 0x00 });
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(b.chain.dpidr) }));
   CHECK(b.chain.irScans == 4);
}

TEST_CASE("DAP JTAG: each scan clears its unused bits and TDO bytes") {
   BasicBench<CheckedPort> b;
   b.connect();
   b.configure({ 4, 5 });
   // DR scans, then an IR scan in the same buffers
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(b.chain.dpidr) }));
   CHECK(b.run({ 0x16, 0x00 }) == cat({ { 0x16, 0x00 }, word(dpIdcode) }));
   CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(b.chain.dpidr) }));
   // A sequence of 64 bits with TDO capture, then sequences of 3 bits with
   // TMS and of 13 bits, all with TDI bytes of 0xFF
   CHECK(b.run(cat({ { 0x14, 0x01, 0x80 }, Bytes(8, 0xFF) })).size() == 10);
   CHECK(b.run({ 0x14, 0x02, 0x43, 0xFF, 0x0D, 0xFF, 0xFF }) == V{ 0x14, 0x00 });
   CHECK(b.port.shifts > 5);
   CHECK(b.port.violations == 0);
}

TEST_CASE("DAP JTAG: the idle cycles of DAP_TransferConfigure follow each DR scan") {
   const auto idleClocks = [](uint8_t idleCycles) {
      Stm32Bench b;
      CHECK(b.run({ 0x04, idleCycles, 0x50, 0x00, 0x00, 0x00 }) == V{ 0x04, 0x00 });
      const uint32_t before = b.chain.idleClocks;
      const uint32_t scans = b.chain.drScans;
      CHECK(b.run({ 0x05, 0x00, 0x01, readDpidr }) == cat({ { 0x05, 0x01, 0x01 }, word(b.chain.dpidr) }));
      // An IR scan, then the DP read and the read of RDBUFF
      CHECK(b.chain.drScans - scans == 2);
      return b.chain.idleClocks - before;
   };
   CHECK(idleClocks(5) - idleClocks(0) == 10);
}

TEST_CASE("DAP JTAG: the fast path does the same transfers") {
   using Spi = mupptest::MockJtagSpi<Pins>;
   BasicBench<jtag::FastPort<Pins, Spi>> b;
   // 4.5 MHz: SPI divider 8
   CHECK(b.run(cat({ { 0x11 }, word(4'500'000) })) == V{ 0x11, 0x00 });
   b.connect();
   CHECK(b.port.spiShift() == 3);
   b.configure({ 4, 5 });
   CHECK(b.run({ 0x16, 0x00 }) == cat({ { 0x16, 0x00 }, word(dpIdcode) }));
   const Bytes request =
   cat({ { 0x05, 0x00, 0x04, writeCtrlStat }, word(0x50000000), { readCtrlStat, writeTar }, word(0x20000000), { readDrw } });
   b.chain.memory[0x20000000] = 0x600DF00D;
   CHECK(b.run(request) == cat({ { 0x05, 0x04, 0x01 }, word(0xF0000000), word(0x600DF00D) }));
   CHECK_FALSE(b.port.spi().transfers.empty());
   CHECK_FALSE(b.port.spi().tckLowAtTransfer);
}
