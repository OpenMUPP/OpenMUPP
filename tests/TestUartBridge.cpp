// The UART bridge with the real CDC-ACM function of libArT on the device
// with the descriptors of the firmware. The fake controller of libArT is
// the host, a fake UART with a simulated interrupt is the UART.
//
// The last tests run a long JTAG shift on the bit-bang engine with mock
// pins, with and without the serial service of the firmware
// (protocol/Background.h).

#include "jtag/JtagBitBang.h"

#include "MockBackground.h"
#include "MockJtagPins.h"
#include "UartBench.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using art::usb::CdcAcmFunctionBase;
using art::usb::LineCoding;
using uart::DataBits;
using uart::FrameFormat;
using uart::Parity;
using uart::StopBits;
using vsltest::cdcPacketSize;
using vsltest::pattern;
using vsltest::ringCapacity;
using vsltest::ringSize;
using Bench = vsltest::UartBench;

constexpr FrameFormat format115200{ 115200, DataBits::Eight, Parity::None, StopBits::One };
constexpr FrameFormat format9600{ 9600, DataBits::Seven, Parity::Even, StopBits::Two };

} // namespace

TEST_CASE("UartBridge sets the default format at start") {
   Bench bench;
   CHECK(bench.uart.formats == std::vector<FrameFormat>{ format115200 });
   CHECK(bench.bridge().format() == format115200);
   CHECK(bench.serialPort().lineCoding() == LineCoding{});
}

TEST_CASE("UartBridge sends the host data on the UART in order") {
   Bench bench;
   bench.enumerate();
   const auto data = pattern(40);

   REQUIRE(bench.hostSends(data));
   CHECK_FALSE(bench.uart.txInterruptOn);
   bench.bridge().poll();
   CHECK(bench.uart.txInterruptOn);
   bench.uart.sendAll(bench.buffers);
   CHECK(bench.uart.sentBytes() == data);
}

TEST_CASE("UartBridge holds the OUT endpoint while the TX ring is full and loses no byte") {
   Bench bench;
   bench.enumerate();
   const auto data = pattern(1000, 7);

   // The UART sends nothing: the TX ring fills, then the CDC RX queue
   // (256 bytes), then the OUT endpoint NAKs.
   std::size_t accepted = bench.hostSendsUntilNak(data, 0);
   bench.bridge().poll();
   accepted += bench.hostSendsUntilNak(data, accepted);
   bench.bridge().poll();
   accepted += bench.hostSendsUntilNak(data, accepted);
   CHECK(bench.buffers.tx.size() == ringCapacity);
   CHECK(bench.serialPort().readAvailable() > 256 - cdcPacketSize);
   CHECK(accepted == bench.buffers.tx.size() + bench.serialPort().readAvailable());
   CHECK_FALSE(bench.hostSends(pattern(cdcPacketSize)));

   // The UART sends; the host sends the rest as the endpoint accepts it.
   while(accepted < data.size()) {
      bench.uart.sendAll(bench.buffers);
      bench.uart.shifting = false;
      bench.bridge().poll();
      accepted += bench.hostSendsUntilNak(data, accepted);
   }
   for(int i = 0; i < 10; ++i) {
      bench.bridge().poll();
      bench.uart.sendAll(bench.buffers);
   }
   CHECK(bench.uart.sentBytes() == data);
}

TEST_CASE("UartBridge sends the received UART data to the host") {
   Bench bench;
   bench.enumerate();
   const auto data = pattern(50, 100);

   for(const uint8_t byte : data) {
      bench.buffers.received(byte);
   }
   bench.bridge().poll();
   CHECK(bench.hostReceives() == data);
   CHECK(bench.bridge().rxDropped() == 0);
}

TEST_CASE("UartBridge moves more UART data than the CDC TX queue holds") {
   Bench bench;
   bench.enumerate();
   const auto data = pattern(1000, 3);

   std::vector<uint8_t> received;
   std::size_t at = 0;
   while(at < data.size()) {
      // The interrupt fills the ring between two main loop passes.
      const std::size_t count = std::min(ringCapacity, data.size() - at);
      for(std::size_t i = 0; i < count; ++i) {
         bench.buffers.received(data[at + i]);
      }
      at += count;
      bench.bridge().poll();
      const auto packets = bench.hostReceives();
      received.insert(received.end(), packets.begin(), packets.end());
   }
   for(int i = 0; i < 10; ++i) {
      bench.bridge().poll();
      const auto packets = bench.hostReceives();
      received.insert(received.end(), packets.begin(), packets.end());
   }
   CHECK(received == data);
   CHECK(bench.bridge().rxDropped() == 0);
}

TEST_CASE("UartBridge drops new UART data when the RX ring is full and counts it") {
   Bench bench;
   bench.enumerate();
   const auto data = pattern(ringCapacity + 20);

   for(const uint8_t byte : data) {
      bench.buffers.received(byte);
   }
   CHECK(bench.bridge().rxDropped() == 20);
   bench.bridge().poll();
   CHECK(bench.hostReceives() == std::vector<uint8_t>(data.begin(), data.begin() + ringCapacity));
}

TEST_CASE("UartBridge counts the overruns and the receive errors of the UART") {
   Bench bench;
   bench.enumerate();
   CHECK(bench.bridge().rxOverruns() == 0);
   CHECK(bench.bridge().rxErrors() == 0);

   // As UartPort::handleInterrupt() reports them: a byte with an error
   // still goes to the host, an overrun adds no byte.
   bench.buffers.receiveError();
   bench.buffers.received(0x41);
   bench.buffers.overrun();
   bench.buffers.overrun();
   bench.buffers.received(0x42);
   bench.bridge().poll();
   CHECK(bench.hostReceives() == std::vector<uint8_t>{ 0x41, 0x42 });
   CHECK(bench.bridge().rxOverruns() == 2);
   CHECK(bench.bridge().rxErrors() == 1);
   CHECK(bench.bridge().rxDropped() == 0);
}

TEST_CASE("UartBridge discards UART data while the device is not configured") {
   Bench bench;
   bench.buffers.received(0x55);
   bench.buffers.received(0xAA);
   bench.bridge().poll();
   CHECK(bench.buffers.rx.empty());

   bench.enumerate();
   bench.bridge().poll();
   CHECK(bench.hostReceives().empty());
}

TEST_CASE("UartBridge applies a new line coding to the UART") {
   Bench bench;
   bench.enumerate();

   CHECK(bench.setLineCoding(9600, 7, 2, 2));
   CHECK(bench.serialPort().lineCoding().dwDTERate == 9600);
   bench.bridge().poll();
   CHECK(bench.uart.formats == std::vector<FrameFormat>{ format115200, format9600 });
   CHECK(bench.bridge().format() == format9600);
   CHECK_FALSE(bench.bridge().hasPendingFormat());
}

TEST_CASE("UartBridge sends the queued data in the old format before a line coding change") {
   Bench bench;
   bench.enumerate();
   const auto before = pattern(30, 0);
   const auto after = pattern(30, 100);

   REQUIRE(bench.hostSends(before));
   bench.bridge().poll();
   CHECK(bench.setLineCoding(9600, 7, 2, 2));
   REQUIRE(bench.hostSends(after));

   // TX ring not empty: the format waits, and the new data stays in the
   // CDC port.
   bench.bridge().poll();
   CHECK(bench.bridge().hasPendingFormat());
   CHECK(bench.buffers.tx.size() == before.size());
   CHECK(bench.serialPort().readAvailable() == after.size());

   // The last byte is still in the shift register.
   bench.uart.sendAll(bench.buffers);
   bench.bridge().poll();
   CHECK(bench.bridge().hasPendingFormat());
   CHECK(bench.uart.formats.size() == 1);

   bench.uart.shifting = false;
   bench.bridge().poll();
   CHECK_FALSE(bench.bridge().hasPendingFormat());
   bench.uart.sendAll(bench.buffers);

   REQUIRE(bench.uart.sent.size() == before.size() + after.size());
   for(std::size_t i = 0; i < before.size(); ++i) {
      CHECK(bench.uart.sent[i] == std::make_pair(before[i], format115200));
   }
   for(std::size_t i = 0; i < after.size(); ++i) {
      CHECK(bench.uart.sent[before.size() + i] == std::make_pair(after[i], format9600));
   }
}

TEST_CASE("UartBridge refuses a line coding that the UART cannot do") {
   Bench bench;
   bench.enumerate();

   // The request stalls, the old line coding and the UART format stay.
   CHECK_FALSE(bench.setLineCoding(115200, 7, 0, 0)); // 7N1
   CHECK_FALSE(bench.setLineCoding(115200, 5, 0, 0));
   CHECK_FALSE(bench.setLineCoding(115200, 8, 3, 0)); // mark parity
   CHECK_FALSE(bench.setLineCoding(300, 8, 0, 0));    // below the baud rate generator
   CHECK_FALSE(bench.setLineCoding(0, 8, 0, 0));
   bench.bridge().poll();
   CHECK(bench.serialPort().lineCoding() == LineCoding{});
   CHECK(bench.uart.formats == std::vector<FrameFormat>{ format115200 });
   CHECK_FALSE(bench.bridge().hasPendingFormat());

   // The port still works.
   REQUIRE(bench.hostSends(pattern(5)));
   bench.bridge().poll();
   bench.uart.sendAll(bench.buffers);
   CHECK(bench.uart.sentBytes() == pattern(5));
}

TEST_CASE("UartBridge checks the baud rate against the clock of the UART") {
   Bench bench;
   bench.uart.clock = 36'000'000;
   bench.enumerate();

   CHECK_FALSE(bench.setLineCoding(3'000'000, 8, 0, 0));
   CHECK(bench.setLineCoding(2'250'000, 8, 0, 0));
}

TEST_CASE("UartBridge does not touch the UART for the same line coding") {
   Bench bench;
   bench.enumerate();

   CHECK(bench.setLineCoding(115200, 8, 0, 0));
   bench.bridge().poll();
   CHECK(bench.uart.formats.size() == 1);

   // A new format, then the old one again before the TX path is empty: the
   // new format is not applied.
   REQUIRE(bench.hostSends(pattern(10)));
   bench.bridge().poll();
   CHECK(bench.setLineCoding(9600, 7, 2, 2));
   CHECK(bench.bridge().hasPendingFormat());
   CHECK(bench.setLineCoding(115200, 8, 0, 0));
   CHECK_FALSE(bench.bridge().hasPendingFormat());
   bench.uart.sendAll(bench.buffers);
   bench.uart.shifting = false;
   bench.bridge().poll();
   CHECK(bench.uart.formats.size() == 1);
}

TEST_CASE("UartBridge stores the DTR and RTS state of the host") {
   Bench bench;
   bench.enumerate();
   CHECK(bench.bridge().controlLines() == 0);

   bench.setControlLineState(CdcAcmFunctionBase::kControlLineDtr | CdcAcmFunctionBase::kControlLineRts);
   CHECK(bench.bridge().controlLines() == (CdcAcmFunctionBase::kControlLineDtr | CdcAcmFunctionBase::kControlLineRts));
   bench.setControlLineState(CdcAcmFunctionBase::kControlLineRts);
   CHECK(bench.bridge().controlLines() == CdcAcmFunctionBase::kControlLineRts);

   // A bus reset clears the state.
   bench.controller().busReset();
   CHECK(bench.bridge().controlLines() == 0);
}

TEST_CASE("UartBuffers count the parity errors and the other receive errors separately") {
   uart::UartBuffers<ringSize> buffers;
   buffers.receiveError();
   buffers.receiveError(true);
   buffers.receiveError(false);
   CHECK(buffers.rxErrors == 3);
   CHECK(buffers.rxParityErrors == 1);
   CHECK(buffers.rxFramingErrors == 2);
}

TEST_CASE("UartBuffers flushTx and flushRx empty the rings, also when the data wraps") {
   uart::UartBuffers<ringSize> buffers;
   for(int round = 0; round < 3; ++round) {
      for(std::size_t i = 0; i < ringCapacity; ++i) {
         buffers.received(static_cast<uint8_t>(i));
         CHECK(buffers.tx.try_push(static_cast<uint8_t>(i)));
      }
      buffers.flushRx();
      buffers.flushTx();
      CHECK(buffers.rx.empty());
      CHECK(buffers.tx.empty());
      // Move the start of the data, so the next round wraps.
      buffers.received(0);
      CHECK(buffers.tx.try_push(0));
      buffers.flushRx();
      buffers.flushTx();
   }
   CHECK(buffers.rxDropped == 0);
}

TEST_CASE("UartBridge moves no data while it is not the owner") {
   Bench bench;
   bench.enumerate();
   bench.bridge().setActive(false);
   CHECK_FALSE(bench.bridge().isActive());

   // The data of the host is lost, and the OUT endpoint does not hold the
   // host.
   const auto data = pattern(3 * cdcPacketSize);
   CHECK(bench.hostSendsUntilNak(data, 0) == data.size());
   bench.bridge().poll();
   CHECK(bench.hostSendsUntilNak(data, 0) == data.size());
   bench.bridge().poll();
   CHECK(bench.buffers.tx.empty());
   CHECK_FALSE(bench.uart.txInterruptOn);
   CHECK(bench.serialPort().readAvailable() == 0);

   // The received bytes stay in the ring for the owner.
   bench.buffers.received(0x41);
   bench.bridge().poll();
   CHECK(bench.hostReceives().empty());
   CHECK(bench.buffers.rx.size() == 1);
}

TEST_CASE("UartBridge sets its format again when it gets the UART back") {
   Bench bench;
   bench.enumerate();
   REQUIRE(bench.setLineCoding(9600, 7, 2, 2));
   bench.bridge().poll();
   REQUIRE(bench.uart.formats.back() == format9600);

   bench.bridge().setActive(false);
   // Another owner changes the format.
   bench.uart.formats.push_back(format115200);
   bench.uart.shifting = true;
   bench.bridge().setActive(true);
   CHECK(bench.bridge().isActive());

   // No data before the format is back: the last byte of the other owner
   // still goes out.
   const auto data = pattern(10);
   REQUIRE(bench.hostSends(data));
   bench.bridge().poll();
   CHECK(bench.uart.formats.back() == format115200);
   CHECK(bench.buffers.tx.empty());

   bench.uart.shifting = false;
   bench.bridge().poll();
   CHECK(bench.uart.formats.back() == format9600);
   bench.bridge().poll();
   bench.uart.sendAll(bench.buffers);
   CHECK(bench.uart.sentBytes() == data);
   CHECK(bench.uart.sent.back().second == format9600);
}

TEST_CASE("UartBridge takes a line coding while it is not the owner, and applies it when it gets the UART back") {
   Bench bench;
   bench.enumerate();
   bench.bridge().setActive(false);
   REQUIRE(bench.setLineCoding(9600, 7, 2, 2));
   bench.bridge().poll();
   CHECK(bench.uart.formats.back() == format115200);
   CHECK(bench.bridge().hasPendingFormat());

   bench.bridge().setActive(true);
   bench.bridge().poll();
   CHECK(bench.uart.formats.back() == format9600);
   CHECK(bench.bridge().format() == format9600);
   CHECK_FALSE(bench.bridge().hasPendingFormat());
}

namespace {

// The UART receives a byte each 6250 core cycles: 115200 baud at 72 MHz.
constexpr uint32_t rxByteCycles = 72'000'000 / 11'520;

// The target sends a byte pattern to the UART RX pin, by the time of the
// mock pins. receiveUntil() runs the RX interrupt for the bytes until now.
struct SerialTarget {
   explicit SerialTarget(uint32_t start)
      : next(start) {
   }

   uint32_t next;
   uint8_t value = 0;
   std::vector<uint8_t> sent;

   void receiveUntil(uint32_t now, uart::UartBuffers<ringSize> &buffers) {
      while(static_cast<int32_t>(now - next) >= 0) {
         buffers.received(value);
         sent.push_back(value);
         ++value;
         next += rxByteCycles;
      }
   }
};

using JtagPins = vsltest::MockJtagPins<false>;

// A shift of 512 bits at 10 kHz: 51 ms, 9 times the RX ring of the bench
// (63 bytes, 5.5 ms). Returns the bytes that the target sent.
std::vector<uint8_t> shiftWithSerialTraffic(Bench &bench, JtagPins &pins, protocol::Background *background, SerialTarget &target) {
   jtag::BitBangPort<JtagPins> port(pins, background);
   REQUIRE(port.setSpeed(10));
   std::vector<uint8_t> tdi(64, 0xA5);
   std::vector<uint8_t> tms(64, 0);
   std::vector<uint8_t> tdo(64, 0);
   REQUIRE(port.shift(512, art::span<const uint8_t>(tdi.data(), tdi.size()),
                      art::span<const uint8_t>(tms.data(), tms.size()), art::span<uint8_t>(tdo.data(), tdo.size())));
   // The main loop runs again after the command.
   target.receiveUntil(pins.time, bench.buffers);
   return target.sent;
}

} // namespace

TEST_CASE("UartBridge loses no UART data during a long JTAG shift with the serial service") {
   Bench bench;
   bench.enumerate();
   JtagPins pins;
   SerialTarget target(pins.time);
   std::vector<uint8_t> received;
   vsltest::MockBackground background([&pins] { return vsltest::MockBackground::Call{ pins.time, pins.tckLevel(), 0 }; });
   // The serial service: the RX interrupt has run up to now, the bridge
   // moves the data, and the host reads the IN packets.
   background.onService = [&] {
      target.receiveUntil(pins.time, bench.buffers);
      bench.bridge().poll();
      const auto data = bench.hostReceives();
      received.insert(received.end(), data.begin(), data.end());
   };

   const auto sent = shiftWithSerialTraffic(bench, pins, &background, target);
   bench.bridge().poll();
   const auto rest = bench.hostReceives();
   received.insert(received.end(), rest.begin(), rest.end());

   CHECK(sent.size() > 8 * ringCapacity);
   // A call each 1 ms plus one byte of 0.8 ms
   CHECK(background.calls.size() >= 28);
   CHECK(bench.bridge().rxDropped() == 0);
   CHECK(received == sent);
}

TEST_CASE("UartBridge loses UART data during a long JTAG shift without the serial service") {
   Bench bench;
   bench.enumerate();
   JtagPins pins;
   SerialTarget target(pins.time);

   const auto sent = shiftWithSerialTraffic(bench, pins, nullptr, target);
   bench.bridge().poll();
   const auto received = bench.hostReceives();

   CHECK(received.size() == ringCapacity);
   CHECK(bench.bridge().rxDropped() == sent.size() - ringCapacity);
}
