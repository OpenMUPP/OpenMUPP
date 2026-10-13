// The DAP_UART_* side of the UART (uart/DapUartChannel.h) and the owner
// switch with the UART bridge, on the bench of the bridge tests: the real
// CDC-ACM function, a fake UART and small rings.

#include "uart/DapUartChannel.h"

#include "UartBench.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

namespace control = cmsisdap::uartControl;
namespace status = cmsisdap::uartStatus;
namespace configError = cmsisdap::uartConfigError;
namespace transport = cmsisdap::uartTransport;
using uart::DataBits;
using uart::FrameFormat;
using uart::fromDapControl;
using uart::Parity;
using uart::StopBits;
using vsltest::cdcPacketSize;
using vsltest::pattern;
using vsltest::ringCapacity;
using vsltest::ringSize;

using Channel = uart::DapUartChannel<vsltest::FakeUart, ringSize>;

constexpr uint32_t clock72 = 72'000'000;
constexpr FrameFormat format115200{ 115200, DataBits::Eight, Parity::None, StopBits::One };
constexpr FrameFormat format9600{ 9600, DataBits::Seven, Parity::Even, StopBits::Two };

// The control byte of DAP_UART_Configure
constexpr uint8_t config(uint8_t dataBits, uint8_t parity, uint8_t stopBits) {
   return static_cast<uint8_t>(dataBits | parity << 4 | stopBits << 6);
}

// 7 data bits, even parity, 2 stop bits
constexpr uint8_t config7E2 = config(7, 1, 1);

// The bridge bench with the channel, started as the firmware does
// (uart/SerialBridge.cpp).
struct Bench : vsltest::UartBench {
   explicit Bench(bool withBridge = true)
      : channel(uart, buffers) {
      channel.start(withBridge ? &bridge() : nullptr);
      enumerate();
   }

   // One iteration of the main loop
   void poll() {
      bridge().poll();
      channel.poll();
   }

   // The DAP commands get the UART, at 9600 7E2 with the receiver and the
   // transmitter enabled.
   void takeForDap() {
      REQUIRE(channel.setTransport(transport::dap));
      uint32_t baud = 9600;
      REQUIRE(channel.configure(config7E2, baud) == 0);
      REQUIRE(channel.control(control::rxEnable | control::txEnable));
      poll();
      REQUIRE(uart.formats.back() == format9600);
   }

   std::vector<uint8_t> read(std::size_t count) {
      std::vector<uint8_t> data(count);
      data.resize(channel.read(art::span<uint8_t>(data.data(), data.size())));
      return data;
   }

   std::size_t write(const std::vector<uint8_t> &data) {
      return channel.write(art::span<const uint8_t>(data.data(), data.size()));
   }

   Channel channel;
};

} // namespace

TEST_CASE("fromDapControl maps the formats of the UART") {
   CHECK(fromDapControl(config(0, 0, 0), 115200, clock72).format == format115200);
   CHECK(fromDapControl(config(8, 0, 0), 115200, clock72).format == format115200);
   CHECK(fromDapControl(config7E2, 9600, clock72).format == format9600);
   CHECK(fromDapControl(config(8, 2, 2), 9600, clock72).format ==
         FrameFormat{ 9600, DataBits::Eight, Parity::Odd, StopBits::OneAndHalf });
   const auto ok = fromDapControl(config(0, 0, 0), 115200, clock72);
   CHECK(ok.errors == 0);
   CHECK(ok.baudRate == 115200);
}

TEST_CASE("fromDapControl reports the real baud rate") {
   // 72 MHz / 313 = 230032 baud
   CHECK(fromDapControl(0, 230400, clock72).baudRate == 230032);
}

TEST_CASE("fromDapControl reports the error of each field") {
   for(const uint8_t dataBits : { 1, 5, 6, 9, 15 }) {
      const auto result = fromDapControl(config(dataBits, 1, 0), 9600, clock72);
      CHECK_FALSE(result.format);
      CHECK(result.errors == configError::dataBits);
      CHECK(result.baudRate == 0);
   }
   // 7 data bits need the parity bit.
   CHECK(fromDapControl(config(7, 0, 0), 9600, clock72).errors == configError::dataBits);
   CHECK(fromDapControl(config(8, 3, 0), 9600, clock72).errors == configError::parity);
   // Only the parity error when 7 data bits have a bad parity
   CHECK(fromDapControl(config(7, 3, 0), 9600, clock72).errors == configError::parity);
   // 0.5 stop bits
   CHECK(fromDapControl(config(8, 0, 3), 9600, clock72).errors == configError::stopBits);
   CHECK(fromDapControl(config(5, 3, 3), 9600, clock72).errors == configError::all);
}

TEST_CASE("fromDapControl: a baud rate that the UART cannot make gives no error bit and baud rate 0") {
   for(const uint32_t baud : { 0u, 1000u, 5'000'000u }) {
      const auto result = fromDapControl(0, baud, clock72);
      CHECK_FALSE(result.format);
      CHECK(result.errors == 0);
      CHECK(result.baudRate == 0);
   }
}

TEST_CASE("DapUartChannel: the bridge owns the UART at the start") {
   Bench bench;
   CHECK(bench.channel.transport() == transport::comPort);
   CHECK(bench.channel.hasComPort());
   CHECK(bench.bridge().isActive());
   CHECK(bench.channel.rxBufferSize() == ringCapacity);
   CHECK(bench.channel.txBufferSize() == ringCapacity);
}

TEST_CASE("DapUartChannel: without a bridge the UART has no owner and no COM port") {
   Bench bench(false);
   CHECK(bench.channel.transport() == transport::none);
   CHECK_FALSE(bench.channel.hasComPort());
   CHECK_FALSE(bench.channel.setTransport(transport::comPort));
   CHECK(bench.channel.transport() == transport::none);
   CHECK(bench.channel.setTransport(transport::dap));
   bench.channel.hostGone();
   CHECK(bench.channel.transport() == transport::none);
   // Nobody takes the received bytes.
   bench.buffers.received(0x41);
   bench.poll();
   CHECK(bench.buffers.rx.empty());
}

TEST_CASE("DapUartChannel: the DAP commands do nothing while the COM port owns the UART") {
   Bench bench;
   uint32_t baud = 9600;
   CHECK(bench.channel.configure(config7E2, baud) == configError::all);
   CHECK(baud == 0);
   CHECK_FALSE(bench.channel.control(control::rxEnable | control::txEnable));
   bench.buffers.received(0x41);
   CHECK(bench.channel.status() == 0);
   CHECK(bench.channel.rxCount() == 0);
   CHECK(bench.write({ 1, 2, 3 }) == 0);
   CHECK(bench.read(10).empty());

   // The byte goes to the host on the COM port.
   bench.poll();
   CHECK(bench.hostReceives() == std::vector<uint8_t>{ 0x41 });
}

TEST_CASE("DapUartChannel: an unknown transport is refused, and the owner stays") {
   Bench bench;
   CHECK_FALSE(bench.channel.setTransport(3));
   CHECK(bench.channel.transport() == transport::comPort);
   CHECK(bench.channel.setTransport(transport::comPort));
   CHECK(bench.bridge().isActive());
}

TEST_CASE("DapUartChannel: the switch to the DAP commands drops the data of the bridge") {
   Bench bench;
   const auto data = pattern(20);
   REQUIRE(bench.hostSends(data));
   bench.poll();
   REQUIRE(bench.uart.txInterruptOn);
   bench.buffers.received(0x41);

   REQUIRE(bench.channel.setTransport(transport::dap));
   CHECK_FALSE(bench.bridge().isActive());
   CHECK_FALSE(bench.uart.txInterruptOn);
   CHECK(bench.buffers.tx.empty());
   CHECK(bench.buffers.rx.empty());

   // The COM port gets no data, and its data does not reach the UART.
   bench.buffers.received(0x42);
   REQUIRE(bench.hostSends(data));
   bench.poll();
   CHECK(bench.hostReceives().empty());
   CHECK(bench.buffers.tx.empty());
   CHECK(bench.uart.sent.empty());
}

TEST_CASE("DapUartChannel: transmit and receive with the DAP commands") {
   Bench bench;
   bench.takeForDap();
   CHECK(bench.channel.status() == (status::rxEnabled | status::txEnabled));

   const auto data = pattern(10);
   CHECK(bench.write(data) == data.size());
   CHECK(bench.uart.txInterruptOn);
   CHECK(bench.channel.txCount() == data.size());
   bench.uart.sendAll(bench.buffers);
   CHECK(bench.uart.sentBytes() == data);
   CHECK(bench.uart.sent.back().second == format9600);
   CHECK(bench.channel.txCount() == 0);

   for(const uint8_t byte : { 0x10, 0x20, 0x30 }) {
      bench.buffers.received(byte);
   }
   bench.poll();
   CHECK(bench.channel.rxCount() == 3);
   CHECK(bench.read(2) == std::vector<uint8_t>{ 0x10, 0x20 });
   CHECK(bench.read(10) == std::vector<uint8_t>{ 0x30 });
   CHECK(bench.channel.rxCount() == 0);
   // Nothing went to the COM port.
   CHECK(bench.hostReceives().empty());
}

TEST_CASE("DapUartChannel: write() takes only what fits in the TX ring") {
   Bench bench;
   bench.takeForDap();
   const auto data = pattern(ringCapacity + 10);
   CHECK(bench.write(data) == ringCapacity);
   CHECK(bench.channel.txCount() == ringCapacity);
}

TEST_CASE("DapUartChannel: the UART must be configured before an enable") {
   Bench bench;
   REQUIRE(bench.channel.setTransport(transport::dap));
   CHECK_FALSE(bench.channel.control(control::rxEnable));
   CHECK_FALSE(bench.channel.control(control::txEnable));
   CHECK(bench.channel.control(control::rxFlush | control::txFlush));
   CHECK(bench.channel.status() == 0);
   CHECK(bench.write({ 1 }) == 0);

   uint32_t baud = 115200;
   CHECK(bench.channel.configure(0, baud) == 0);
   CHECK(baud == 115200);
   CHECK(bench.channel.control(control::rxEnable | control::txEnable));
}

TEST_CASE("DapUartChannel: a new format waits until the TX path is empty") {
   Bench bench;
   bench.takeForDap();
   REQUIRE(bench.write(pattern(5)) == 5);

   uint32_t baud = 115200;
   REQUIRE(bench.channel.configure(0, baud) == 0);
   // No new bytes before the change
   CHECK(bench.write({ 9 }) == 0);
   bench.poll();
   CHECK(bench.uart.formats.back() == format9600);

   bench.uart.sendAll(bench.buffers);
   bench.poll();
   CHECK(bench.uart.formats.back() == format9600);
   bench.uart.shifting = false;
   // Bytes received in the old format are dropped at the change.
   bench.buffers.received(0x77);
   bench.poll();
   CHECK(bench.uart.formats.back() == format115200);
   CHECK(bench.channel.rxCount() == 0);
   CHECK(bench.write({ 9 }) == 1);
}

TEST_CASE("DapUartChannel: a configuration that fails disables the receiver and the transmitter") {
   Bench bench;
   bench.takeForDap();
   REQUIRE(bench.write(pattern(5)) == 5);
   uint32_t baud = 9600;
   CHECK(bench.channel.configure(config(6, 0, 0), baud) == configError::dataBits);
   CHECK(baud == 0);
   CHECK(bench.channel.status() == 0);
   CHECK(bench.buffers.tx.empty());
   CHECK_FALSE(bench.channel.control(control::txEnable));
   // The UART keeps its format.
   bench.poll();
   CHECK(bench.uart.formats.back() == format9600);
}

TEST_CASE("DapUartChannel: the receiver discards the data while it is disabled") {
   Bench bench;
   bench.takeForDap();
   bench.buffers.received(0x01);
   CHECK(bench.channel.control(control::rxDisable | control::rxEnable));
   CHECK(bench.channel.status() == status::txEnabled);
   bench.buffers.received(0x02);
   bench.poll();
   CHECK(bench.buffers.rx.empty());
   CHECK(bench.read(10).empty());

   // The enable starts with an empty ring.
   bench.buffers.received(0x03);
   CHECK(bench.channel.control(control::rxEnable));
   bench.buffers.received(0x04);
   CHECK(bench.read(10) == std::vector<uint8_t>{ 0x04 });
}

TEST_CASE("DapUartChannel: flush and disable of the transmitter drop the bytes that are not sent") {
   Bench bench;
   bench.takeForDap();
   REQUIRE(bench.write(pattern(5)) == 5);
   CHECK(bench.channel.control(control::txFlush));
   CHECK_FALSE(bench.uart.txInterruptOn);
   CHECK(bench.channel.txCount() == 0);
   CHECK(bench.channel.status() == (status::rxEnabled | status::txEnabled));

   REQUIRE(bench.write(pattern(5)) == 5);
   CHECK(bench.channel.control(control::txDisable));
   CHECK(bench.channel.txCount() == 0);
   CHECK(bench.channel.status() == status::rxEnabled);
   CHECK(bench.write(pattern(5)) == 0);
   CHECK(bench.uart.sent.empty());
}

TEST_CASE("DapUartChannel: rxFlush empties the RX ring") {
   Bench bench;
   bench.takeForDap();
   bench.buffers.received(0x01);
   CHECK(bench.channel.control(control::rxFlush));
   CHECK(bench.channel.rxCount() == 0);
   CHECK(bench.channel.status() == (status::rxEnabled | status::txEnabled));
}

TEST_CASE("DapUartChannel: the status reports each error once") {
   Bench bench;
   // Errors before the switch are not reported.
   bench.buffers.overrun();
   bench.buffers.receiveError(true);
   bench.takeForDap();
   const uint8_t enabled = status::rxEnabled | status::txEnabled;
   CHECK(bench.channel.status() == enabled);

   bench.buffers.overrun();
   CHECK(bench.channel.status() == (enabled | status::rxDataLost));
   CHECK(bench.channel.status() == enabled);

   for(std::size_t i = 0; i <= ringCapacity; ++i) {
      bench.buffers.received(0);
   }
   CHECK(bench.channel.status() == (enabled | status::rxDataLost));

   bench.buffers.receiveError(true);
   CHECK(bench.channel.status() == (enabled | status::parityError));
   bench.buffers.receiveError();
   CHECK(bench.channel.status() == (enabled | status::framingError));
   bench.buffers.receiveError();
   bench.buffers.receiveError(true);
   CHECK(bench.channel.status() == (enabled | status::framingError | status::parityError));
   CHECK(bench.channel.status() == enabled);
}

TEST_CASE("DapUartChannel: the switch back to the COM port") {
   Bench bench;
   REQUIRE(bench.setLineCoding(19200, 8, 0, 0));
   bench.poll();
   const FrameFormat format19200{ 19200, DataBits::Eight, Parity::None, StopBits::One };
   REQUIRE(bench.uart.formats.back() == format19200);

   bench.takeForDap();
   REQUIRE(bench.write(pattern(5)) == 5);
   bench.buffers.received(0x41);
   bench.uart.shifting = true;

   REQUIRE(bench.channel.setTransport(transport::comPort));
   CHECK(bench.bridge().isActive());
   CHECK_FALSE(bench.uart.txInterruptOn);
   CHECK(bench.buffers.tx.empty());
   CHECK(bench.buffers.rx.empty());
   // The last byte of the DAP side still goes out in its format.
   bench.poll();
   CHECK(bench.uart.formats.back() == format9600);
   bench.uart.shifting = false;
   bench.poll();
   CHECK(bench.uart.formats.back() == format19200);

   // The bridge moves data again.
   bench.buffers.received(0x42);
   bench.poll();
   CHECK(bench.hostReceives() == std::vector<uint8_t>{ 0x42 });

   // The DAP side forgot its configuration.
   REQUIRE(bench.channel.setTransport(transport::dap));
   CHECK_FALSE(bench.channel.control(control::rxEnable));
}

TEST_CASE("DapUartChannel: hostGone gives the UART back to the COM port") {
   Bench bench;
   bench.takeForDap();
   bench.channel.hostGone();
   CHECK(bench.channel.transport() == transport::comPort);
   CHECK(bench.bridge().isActive());
}

TEST_CASE("DapUartChannel: the COM port data is lost while the DAP commands own the UART") {
   Bench bench;
   bench.takeForDap();
   const auto data = pattern(4 * cdcPacketSize);
   // The OUT endpoint does not hold the host.
   CHECK(bench.hostSendsUntilNak(data, 0) == data.size());
   bench.poll();
   CHECK(bench.hostSendsUntilNak(data, 0) == data.size());
   bench.poll();
   CHECK(bench.channel.txCount() == 0);
}
