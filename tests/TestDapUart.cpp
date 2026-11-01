// The DAP_UART_* side of the UART (uart/DapUartChannel.h) and the owner
// switch with the UART bridge, on the bench of the bridge tests: the real
// CDC-ACM function, a fake UART and small rings. The last tests run the
// DAP_UART_* commands of the CMSIS-DAP processor on this channel.

#include "protocol/cmsisdap/DapProcessor.h"
#include "uart/DapUartChannel.h"

#include "CmsisDapBench.h"
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
using mupptest::cdcPacketSize;
using mupptest::pattern;
using mupptest::ringCapacity;
using mupptest::ringSize;

using Channel = uart::DapUartChannel<mupptest::FakeUart, ringSize>;

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
struct Bench : mupptest::UartBench {
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

using Bytes = std::vector<uint8_t>;

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

// The processor with the channel of a Bench
struct DapBench : Bench {
   explicit DapBench(bool withBridge = true, bool withUart = true)
      : Bench(withBridge)
      , info(mupptest::dapTestInfo())
      , dap(swd, gpio, platform, info, nullptr, nullptr, withUart ? &channel : nullptr) {
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

   // DAP_UART_Transport 2, DAP_UART_Configure 9600 7E2, DAP_UART_Control
   // RX and TX enable
   void takeForDap() {
      REQUIRE(run({ 0x1F, 0x02 }) == Bytes{ 0x1F, 0x00 });
      REQUIRE(run(cat({ { 0x20, config7E2 }, word(9600) })) == cat({ { 0x20, 0x00 }, word(9600) }));
      REQUIRE(run({ 0x22, control::rxEnable | control::txEnable }) == Bytes{ 0x22, 0x00 });
      poll();
   }

   mupptest::FakeDapSwd swd;
   mupptest::FakeDapGpio gpio;
   mupptest::FakeDapPlatform platform;
   cmsisdap::DeviceInfo info;
   cmsisdap::Processor dap;
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

TEST_CASE("DapUartChannel: errors while the receiver is disabled are not reported") {
   Bench bench;
   REQUIRE(bench.channel.setTransport(transport::dap));
   // The target talks before the configuration.
   bench.buffers.receiveError();
   bench.buffers.receiveError(true);
   bench.buffers.overrun();
   uint32_t baud = 9600;
   REQUIRE(bench.channel.configure(config7E2, baud) == 0);
   REQUIRE(bench.channel.control(control::rxEnable | control::txEnable));
   bench.poll();
   const uint8_t enabled = status::rxEnabled | status::txEnabled;
   CHECK(bench.channel.status() == enabled);

   // The same after a disable and an enable of the receiver
   REQUIRE(bench.channel.control(control::rxDisable));
   bench.buffers.receiveError();
   bench.buffers.overrun();
   REQUIRE(bench.channel.control(control::rxEnable));
   CHECK(bench.channel.status() == enabled);
}

TEST_CASE("DapUartChannel: errors before a format change are not reported") {
   Bench bench;
   bench.takeForDap();
   uint32_t baud = 115200;
   REQUIRE(bench.channel.configure(0, baud) == 0);
   bench.buffers.receiveError();
   bench.poll();
   const uint8_t enabled = status::rxEnabled | status::txEnabled;
   CHECK(bench.channel.status() == enabled);
}

TEST_CASE("DapUartChannel: no received bytes while a new format waits") {
   Bench bench;
   bench.takeForDap();
   REQUIRE(bench.write(pattern(5)) == 5);
   bench.buffers.received(0x41);
   uint32_t baud = 115200;
   REQUIRE(bench.channel.configure(0, baud) == 0);
   bench.buffers.received(0x42);
   bench.poll();
   CHECK(bench.channel.rxCount() == 0);
   CHECK(bench.read(10).empty());

   bench.uart.sendAll(bench.buffers);
   bench.uart.shifting = false;
   bench.poll();
   REQUIRE(bench.uart.formats.back() == format115200);
   CHECK(bench.channel.rxCount() == 0);
   bench.buffers.received(0x43);
   CHECK(bench.read(10) == std::vector<uint8_t>{ 0x43 });
}

TEST_CASE("DapUartChannel: a switch stops the TX interrupt before it drops the TX ring") {
   Bench bench;
   REQUIRE(bench.hostSends(pattern(20)));
   bench.poll();
   REQUIRE(bench.uart.txInterruptOn);
   std::size_t txAtStop = 0;
   bench.uart.onStopTx = [&bench, &txAtStop] { txAtStop = bench.buffers.tx.size(); };
   REQUIRE(bench.channel.setTransport(transport::dap));
   CHECK(txAtStop == 20);
   CHECK(bench.buffers.tx.empty());

   // The same for the TX flush and disable of the DAP side
   bench.takeForDap();
   for(const uint8_t bits : { control::txFlush, control::txDisable }) {
      REQUIRE(bench.channel.control(control::txEnable));
      REQUIRE(bench.write(pattern(7)) == 7);
      txAtStop = 0;
      REQUIRE(bench.channel.control(bits));
      CHECK(txAtStop == 7);
      CHECK(bench.buffers.tx.empty());
   }
}

TEST_CASE("DapUartChannel: transport 0 gives the UART to nobody") {
   Bench bench;
   bench.buffers.received(0x41);
   REQUIRE(bench.channel.setTransport(transport::none));
   CHECK(bench.channel.transport() == transport::none);
   CHECK_FALSE(bench.bridge().isActive());
   CHECK(bench.buffers.rx.empty());

   // Nobody gets data.
   bench.buffers.received(0x42);
   REQUIRE(bench.hostSends(pattern(10)));
   bench.poll();
   CHECK(bench.buffers.rx.empty());
   CHECK(bench.buffers.tx.empty());
   CHECK(bench.hostReceives().empty());
   uint32_t baud = 9600;
   CHECK(bench.channel.configure(config7E2, baud) == configError::all);
   CHECK(bench.channel.status() == 0);

   // To the DAP commands, back to nobody, then to the COM port
   bench.takeForDap();
   REQUIRE(bench.channel.setTransport(transport::none));
   CHECK(bench.channel.status() == 0);
   REQUIRE(bench.channel.setTransport(transport::comPort));
   CHECK(bench.bridge().isActive());
   bench.poll();
   CHECK(bench.uart.formats.back() == format115200);
   bench.buffers.received(0x43);
   bench.poll();
   CHECK(bench.hostReceives() == std::vector<uint8_t>{ 0x43 });
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

TEST_CASE("DAP_Info reports the UART, the USB COM port and the buffer sizes") {
   DapBench b;
   CHECK(b.run({ 0x00, 0xF0 }) == Bytes{ 0x00, 0x02, 0x91, 0x01 });
   CHECK(b.run({ 0x00, 0xFB }) == cat({ { 0x00, 0x04 }, word(ringCapacity) }));
   CHECK(b.run({ 0x00, 0xFC }) == cat({ { 0x00, 0x04 }, word(ringCapacity) }));
}

TEST_CASE("DAP_Info: no USB COM port without a bridge") {
   DapBench b(false);
   CHECK(b.run({ 0x00, 0xF0 }) == Bytes{ 0x00, 0x01, 0x91 });
}

TEST_CASE("Without a DapUart the DAP_UART_* commands are invalid, and DAP_Info reports no UART") {
   DapBench b(true, false);
   for(const uint8_t id : { 0x1F, 0x20, 0x21, 0x22, 0x23 }) {
      CHECK(b.run({ id, 0x00, 0x00, 0x00, 0x00, 0x00 }) == Bytes{ 0xFF });
   }
   CHECK(b.run({ 0x00, 0xF0 }) == Bytes{ 0x00, 0x01, 0x11 });
   CHECK(b.run({ 0x00, 0xFB }) == Bytes{ 0x00, 0x00 });
   CHECK(b.run({ 0x00, 0xFC }) == Bytes{ 0x00, 0x00 });
}

TEST_CASE("DAP_UART_Transport switches the owner") {
   DapBench b;
   CHECK(b.run({ 0x1F, 0x02 }) == Bytes{ 0x1F, 0x00 });
   CHECK(b.channel.transport() == transport::dap);
   CHECK(b.run({ 0x1F, 0x03 }) == Bytes{ 0x1F, 0xFF });
   CHECK(b.run({ 0x1F, 0x00 }) == Bytes{ 0x1F, 0x00 });
   CHECK(b.channel.transport() == transport::none);
   CHECK(b.run({ 0x1F, 0x01 }) == Bytes{ 0x1F, 0x00 });
   CHECK(b.bridge().isActive());
}

TEST_CASE("DAP_UART_Configure replies the errors and the real baud rate") {
   DapBench b;
   // The COM port owns the UART.
   CHECK(b.run(cat({ { 0x20, 0x00 }, word(115200) })) == cat({ { 0x20, 0x07 }, word(0) }));
   REQUIRE(b.run({ 0x1F, 0x02 }) == Bytes{ 0x1F, 0x00 });
   CHECK(b.run(cat({ { 0x20, 0x00 }, word(115200) })) == cat({ { 0x20, 0x00 }, word(115200) }));
   CHECK(b.run(cat({ { 0x20, 0x00 }, word(230400) })) == cat({ { 0x20, 0x00 }, word(230032) }));
   CHECK(b.run(cat({ { 0x20, config(8, 3, 3) }, word(9600) })) == cat({ { 0x20, 0x06 }, word(0) }));
   CHECK(b.run(cat({ { 0x20, 0x00 }, word(100) })) == cat({ { 0x20, 0x00 }, word(0) }));
   // Too short
   CHECK(b.run({ 0x20, 0x00, 0x00, 0xC2, 0x01 }) == Bytes{ 0xFF });
}

TEST_CASE("DAP_UART_Control replies DAP_ERROR for an enable before the configuration") {
   DapBench b;
   CHECK(b.run({ 0x22, control::rxEnable }) == Bytes{ 0x22, 0xFF });
   REQUIRE(b.run({ 0x1F, 0x02 }) == Bytes{ 0x1F, 0x00 });
   CHECK(b.run({ 0x22, control::rxEnable }) == Bytes{ 0x22, 0xFF });
   CHECK(b.run({ 0x22, control::rxFlush }) == Bytes{ 0x22, 0x00 });
   CHECK(b.run({ 0x22 }) == Bytes{ 0xFF });
}

TEST_CASE("DAP_UART_Status replies the status and the counts") {
   DapBench b;
   CHECK(b.run({ 0x23 }) == cat({ { 0x23, 0x00 }, word(0), word(0) }));
   b.takeForDap();
   b.buffers.received(0x41);
   b.buffers.received(0x42);
   REQUIRE(b.write(pattern(3)) == 3);
   b.buffers.receiveError(true);
   CHECK(b.run({ 0x23 }) == cat({ { 0x23, 0x19 }, word(2), word(3) }));
   CHECK(b.run({ 0x23 }) == cat({ { 0x23, 0x11 }, word(2), word(3) }));
}

TEST_CASE("DAP_UART_Transfer sends and receives") {
   DapBench b;
   b.takeForDap();
   for(const uint8_t byte : { 0x10, 0x20, 0x30 }) {
      b.buffers.received(byte);
   }
   // Rx Count 2, Tx Count 4
   CHECK(b.run({ 0x21, 0x02, 0x00, 0x04, 0x00, 0xA0, 0xA1, 0xA2, 0xA3 }) == Bytes{ 0x21, 0x11, 0x04, 0x00, 0x02, 0x00, 0x10, 0x20 });
   b.uart.sendAll(b.buffers);
   CHECK(b.uart.sentBytes() == Bytes{ 0xA0, 0xA1, 0xA2, 0xA3 });
   // Receive only
   CHECK(b.run({ 0x21, 0x10, 0x00, 0x00, 0x00 }) == Bytes{ 0x21, 0x11, 0x00, 0x00, 0x01, 0x00, 0x30 });
   CHECK(b.run({ 0x21, 0x10, 0x00, 0x00, 0x00 }) == Bytes{ 0x21, 0x11, 0x00, 0x00, 0x00, 0x00 });
}

TEST_CASE("DAP_UART_Transfer replies the bytes that fit in the TX ring and in the response") {
   DapBench b;
   b.takeForDap();
   for(std::size_t i = 0; i < ringCapacity; ++i) {
      b.buffers.received(static_cast<uint8_t>(i));
   }
   // 64 - 6 bytes of received data fit in the response.
   const Bytes response = b.run({ 0x21, 0xFF, 0xFF, 0x00, 0x00 });
   REQUIRE(response.size() == 64);
   CHECK(response[4] == 58);
   CHECK(response[5] == 0);
   CHECK(response[6] == 0);
   CHECK(response[63] == 57);

   // The TX ring takes ringCapacity - 50 bytes more.
   REQUIRE(b.write(pattern(50)) == 50);
   const Bytes data = pattern(40);
   CHECK(b.run(cat({ { 0x21, 0x00, 0x00, 40, 0x00 }, data })) == Bytes{ 0x21, 0x11, ringCapacity - 50, 0x00, 0x00, 0x00 });
}

TEST_CASE("DAP_UART_Transfer: a Tx Count that is more than the request has is invalid") {
   DapBench b;
   b.takeForDap();
   CHECK(b.run({ 0x21, 0x00, 0x00, 0x04, 0x00, 0xA0, 0xA1 }) == Bytes{ 0xFF });
   CHECK(b.run({ 0x21, 0x00, 0x00 }) == Bytes{ 0xFF });
   CHECK(b.channel.txCount() == 0);
}

TEST_CASE("DAP_UART_Transfer while the COM port owns the UART moves no data") {
   DapBench b;
   b.buffers.received(0x41);
   CHECK(b.run({ 0x21, 0x10, 0x00, 0x02, 0x00, 0xA0, 0xA1 }) == Bytes{ 0x21, 0x00, 0x00, 0x00, 0x00, 0x00 });
   CHECK(b.buffers.tx.empty());
}

TEST_CASE("DAP_UART_* commands in DAP_ExecuteCommands") {
   DapBench b;
   b.takeForDap();
   b.buffers.received(0x55);
   // Transfer of 1 byte each way, then the status
   CHECK(b.run({ 0x7F, 0x02, 0x21, 0x01, 0x00, 0x01, 0x00, 0xAA, 0x23 }) ==
         cat({ { 0x7F, 0x02, 0x21, 0x11, 0x01, 0x00, 0x01, 0x00, 0x55, 0x23, 0x11 }, word(0), word(1) }));
}

TEST_CASE("hostGone gives the UART back to the COM port") {
   DapBench b;
   b.takeForDap();
   b.dap.hostGone();
   CHECK(b.channel.transport() == transport::comPort);
   CHECK(b.bridge().isActive());
}

TEST_CASE("DAP_UART_Configure and DAP_UART_Transfer in one packet give no bytes of the old format") {
   DapBench b;
   b.takeForDap();
   REQUIRE(b.write(pattern(3)) == 3);
   b.buffers.received(0x55);
   // Configure 115200 8N1, then a transfer of 1 byte each way
   CHECK(b.run(cat({ { 0x7F, 0x02, 0x20, 0x00 }, word(115200), { 0x21, 0x10, 0x00, 0x01, 0x00, 0xAA } })) ==
         cat({ { 0x7F, 0x02, 0x20, 0x00 }, word(115200), { 0x21, 0x11, 0x00, 0x00, 0x00, 0x00 } }));
}
