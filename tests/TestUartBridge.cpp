// The UART bridge with the real CDC-ACM function of libArT on the device
// with the descriptors of the firmware. The fake controller of libArT is
// the host, a fake UART with a simulated interrupt is the UART.

#include "uart/UartBridge.h"
#include "usb/UsbDescriptors.h"
#include "usb/VersaloonFunction.h"

#include <FakeDeviceController.h>
#include <doctest/doctest.h>
#include <usb/Device.h>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using art::usb::CdcAcmFunctionBase;
using art::usb::LineCoding;
using art::usb::test::FakeDeviceController;
using art::usb::test::setupPacket;
using uart::DataBits;
using uart::FrameFormat;
using uart::Parity;
using uart::StopBits;

constexpr uint8_t standardDeviceOut = 0x00;
constexpr uint8_t classInterfaceOut = 0x21;
constexpr uint8_t setAddress = 5;
constexpr uint8_t setConfiguration = 9;

// Rings of 63 bytes: small, so the tests fill them and wrap around.
constexpr std::size_t ringSize = 64;
constexpr std::size_t ringCapacity = ringSize - 1;
constexpr std::size_t packetSize = CdcAcmFunctionBase::kDataPacketSize;

// The UART: setFormat() and startTx() as UartPort does them. sendAll() is
// the TX interrupt.
struct FakeUart {
   uint32_t clock = 72'000'000;
   std::vector<FrameFormat> formats;
   bool txInterruptOn = false;
   // The last byte is still in the shift register.
   bool shifting = false;
   // The bytes on the TX pin, each with the format it went out in.
   std::vector<std::pair<uint8_t, FrameFormat>> sent;

   [[nodiscard]] uint32_t clockFrequency() const {
      return clock;
   }

   void setFormat(const FrameFormat &format) {
      CHECK(isTxIdle());
      formats.push_back(format);
   }

   [[nodiscard]] bool isTxIdle() const {
      return !txInterruptOn && !shifting;
   }

   void startTx() {
      txInterruptOn = true;
   }

   // Runs the TX interrupt until the ring is empty. The last byte stays in
   // the shift register.
   void sendAll(uart::UartBuffers<ringSize> &buffers) {
      while(txInterruptOn) {
         uint8_t byte = 0;
         if(buffers.nextToSend(byte)) {
            sent.emplace_back(byte, formats.back());
            shifting = true;
         } else {
            txInterruptOn = false;
         }
      }
   }

   [[nodiscard]] std::vector<uint8_t> sentBytes() const {
      std::vector<uint8_t> bytes;
      for(const auto &entry : sent) {
         bytes.push_back(entry.first);
      }
      return bytes;
   }
};

std::vector<uint8_t> pattern(std::size_t size, uint8_t first = 0) {
   std::vector<uint8_t> data(size);
   for(std::size_t i = 0; i < size; ++i) {
      data[i] = static_cast<uint8_t>(first + i);
   }
   return data;
}

class Bench {
   public:
   Bench()
      : device_(controller_, descriptors_)
      , serialPort_(usb::cdcControlInterface, endpoints)
      , bridge_(serialPort_, uart, buffers) {
      REQUIRE(device_.addFunction(versaloon_));
      REQUIRE(device_.addFunction(serialPort_));
      bridge_.start();
      device_.start();
   }

   // Bus reset, SET_ADDRESS 5 and SET_CONFIGURATION 1.
   void enumerate() {
      controller_.busReset();
      request(standardDeviceOut, setAddress, 5, 0);
      request(standardDeviceOut, setConfiguration, 1, 0);
      REQUIRE(serialPort_.isConfigured());
   }

   // Sends one OUT packet to the serial port. False when the endpoint NAKs.
   bool hostSends(const std::vector<uint8_t> &packet) {
      return controller_.sendOut(usb::cdcDataOutEndpoint, art::span<const uint8_t>(packet.data(), packet.size()));
   }

   // Sends data in packets until the endpoint NAKs. Returns the bytes sent.
   std::size_t hostSendsUntilNak(const std::vector<uint8_t> &data, std::size_t from) {
      std::size_t at = from;
      while(at < data.size()) {
         const std::size_t length = std::min(packetSize, data.size() - at);
         if(!hostSends(std::vector<uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(at),
                                            data.begin() + static_cast<std::ptrdiff_t>(at + length)))) {
            break;
         }
         at += length;
      }
      return at - from;
   }

   // Reads all IN packets of the serial port.
   std::vector<uint8_t> hostReceives() {
      std::vector<uint8_t> data;
      while(auto packet = controller_.takeIn(usb::cdcDataInEndpoint)) {
         data.insert(data.end(), packet->begin(), packet->end());
      }
      return data;
   }

   // SET_LINE_CODING. Returns false when the device stalls it.
   bool setLineCoding(uint32_t baud, uint8_t dataBits, uint8_t parity, uint8_t stopBits) {
      const std::array<uint8_t, LineCoding::kSize> data{
         static_cast<uint8_t>(baud),
         static_cast<uint8_t>(baud >> 8),
         static_cast<uint8_t>(baud >> 16),
         static_cast<uint8_t>(baud >> 24),
         stopBits,
         parity,
         dataBits,
      };
      bool ok = false;
      const auto result = controller_.controlOut(setupPacket(classInterfaceOut, CdcAcmFunctionBase::kSetLineCoding, 0,
                                                             usb::cdcControlInterface, LineCoding::kSize),
                                                 art::span<const uint8_t>(data.data(), data.size()), &ok);
      CHECK(ok);
      return !result.stalled;
   }

   void setControlLineState(uint16_t state) {
      request(classInterfaceOut, CdcAcmFunctionBase::kSetControlLineState, state, usb::cdcControlInterface);
   }

   FakeDeviceController &controller() {
      return controller_;
   }

   CdcAcmFunctionBase &serialPort() {
      return serialPort_;
   }

   uart::UartBridge<FakeUart, ringSize> &bridge() {
      return bridge_;
   }

   FakeUart uart;
   uart::UartBuffers<ringSize> buffers;

   private:
   static constexpr art::usb::CdcAcmEndpoints endpoints{
      .notification = usb::cdcNotificationEndpoint,
      .notificationMaxPacketSize = usb::cdcNotificationPacketSize,
      .dataOut = usb::cdcDataOutEndpoint,
      .dataIn = usb::cdcDataInEndpoint,
   };

   void request(uint8_t requestType, uint8_t request, uint16_t value, uint16_t index) {
      bool ok = false;
      const auto result = controller_.controlOut(setupPacket(requestType, request, value, index, 0), {}, &ok);
      CHECK(ok);
      CHECK_FALSE(result.stalled);
   }

   FakeDeviceController controller_;
   usb::SerialNumber serial_;
   const std::array<art::span<const uint8_t>, usb::stringCount> strings_{
      usb::desc::asSpan(usb::languageDescriptor),   usb::desc::asSpan(usb::manufacturerDescriptor),
      usb::desc::asSpan(usb::productDescriptor),    serial_.bytes(),
      usb::desc::asSpan(usb::serialPortDescriptor),
   };
   const art::usb::Descriptors descriptors_{
      .device = usb::desc::asSpan(usb::deviceDescriptor),
      .configuration = usb::desc::asSpan(usb::configurationDescriptor),
      .strings = art::span<const art::span<const uint8_t>>(strings_.data(), strings_.size()),
   };
   art::usb::Device device_;
   usb::VersaloonFunction versaloon_{ protocol::DeviceInfo{ .version = "v0.0.0", .board = "test", .hardwareId = 0 } };
   // The queue sizes of the firmware (usb/UsbStack.h)
   art::usb::CdcAcmFunction<256, 256> serialPort_;
   uart::UartBridge<FakeUart, ringSize> bridge_;
};

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
   CHECK(bench.serialPort().readAvailable() > 256 - packetSize);
   CHECK(accepted == bench.buffers.tx.size() + bench.serialPort().readAvailable());
   CHECK_FALSE(bench.hostSends(pattern(packetSize)));

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
