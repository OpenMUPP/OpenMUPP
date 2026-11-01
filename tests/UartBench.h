// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The UART bridge with the real CDC-ACM function of libArT on the device
// with the descriptors of the firmware, for the UART tests. The fake
// controller of libArT is the host, a fake UART with a simulated interrupt
// is the UART.

#include "uart/UartBridge.h"
#include "usb/UsbDescriptors.h"
#include "usb/VersaloonFunction.h"

#include <FakeDeviceController.h>
#include <doctest/doctest.h>
#include <usb/Device.h>

#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace mupptest {

using art::usb::CdcAcmFunctionBase;
using art::usb::LineCoding;
using art::usb::test::FakeDeviceController;
using art::usb::test::setupPacket;
using uart::DataBits;
using uart::FrameFormat;
using uart::Parity;
using uart::StopBits;


// Rings of 63 bytes: small, so the tests fill them and wrap around.
inline constexpr std::size_t ringSize = 64;
inline constexpr std::size_t ringCapacity = ringSize - 1;
inline constexpr std::size_t cdcPacketSize = CdcAcmFunctionBase::kDataPacketSize;

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
   // Runs in stopTx(), for example to check the state of the rings then.
   std::function<void()> onStopTx;

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

   void stopTx() {
      txInterruptOn = false;
      if(onStopTx) {
         onStopTx();
      }
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

inline std::vector<uint8_t> pattern(std::size_t size, uint8_t first = 0) {
   std::vector<uint8_t> data(size);
   for(std::size_t i = 0; i < size; ++i) {
      data[i] = static_cast<uint8_t>(first + i);
   }
   return data;
}

class UartBench {
   static constexpr uint8_t standardDeviceOut = 0x00;
   static constexpr uint8_t classInterfaceOut = 0x21;
   static constexpr uint8_t setAddress = 5;
   static constexpr uint8_t setConfiguration = 9;

   public:
   UartBench()
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
         const std::size_t length = std::min(cdcPacketSize, data.size() - at);
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
      // No BOS: the Versaloon personality is a USB 2.00 device.
      .bos = {},
   };
   art::usb::Device device_;
   usb::VersaloonFunction versaloon_{ protocol::DeviceInfo{ .version = "v0.0.0", .board = "test", .hardwareId = 0 } };
   // The queue sizes of the firmware (usb/UsbStack.h)
   art::usb::CdcAcmFunction<256, 256> serialPort_;
   uart::UartBridge<FakeUart, ringSize> bridge_;
};

} // namespace mupptest
