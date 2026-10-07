#include "usb/UsbStack.h"

#include <array>
#include <peripherals/Rcc.h>
#include <peripherals/Uid.h>

#include "board/Board.h"
#include "board/BoardPins.h"
#include "debug/DebugLog.h"
#include "usb/UsbDescriptors.h"

namespace usb {

namespace {

static_assert(art::usb::CdcAcmFunctionBase::kDataPacketSize == cdcDataPacketSize);
static_assert(hw::Uid::SizeBytes == uidBytes);

constexpr art::usb::CdcAcmEndpoints serialPortEndpoints{
   .notification = cdcNotificationEndpoint,
   .notificationMaxPacketSize = cdcNotificationPacketSize,
   .dataOut = cdcDataOutEndpoint,
   .dataIn = cdcDataInEndpoint,
};

SerialNumber serialNumber;

const std::array<art::span<const uint8_t>, stringCount> strings{
   desc::asSpan(languageDescriptor),   desc::asSpan(manufacturerDescriptor),
   desc::asSpan(productDescriptor),    serialNumber.bytes(),
   desc::asSpan(serialPortDescriptor),
};

const art::usb::Descriptors descriptors{
   .device = desc::asSpan(deviceDescriptor),
   .configuration = desc::asSpan(configurationDescriptor),
   .strings = art::span<const art::span<const uint8_t>>(strings.data(), strings.size()),
};

} // namespace

UsbStack::UsbStack(boot::BootloaderEntry *bootloader)
   : controller_(hardware_, events_)
   , device_(controller_, descriptors)
   , versaloon_(bootloader)
   , serialPort_(cdcControlInterface, serialPortEndpoints) {
   // Fails only for an interface range that does not fit the device: a
   // static configuration error, which the host tests catch.
   static_cast<void>(device_.addFunction(versaloon_));
   static_cast<void>(device_.addFunction(serialPort_));
}

void UsbStack::start() {
   std::array<uint8_t, uidBytes> uid{};
   hw::Uid::read(uid);
   serialNumber.set(uid);

   if(board::current.usbPullUp) {
      hardware_.setPullupCallback(&UsbStack::setPullUp, nullptr);
   }

   // USBPRE is set by the 72 MHz clock setup, before the clock is enabled.
   Rcc::enableUSBClock();
   controller_.begin();
   device_.start();
}

void UsbStack::poll() {
   controller_.poll();
   // Without VSL_DEBUG_CDC the UART bridge reads the serial port
   // (uart/SerialBridge.h).
   if constexpr(debug::cdcLog) {
      discardSerialInput();
   }
}

void UsbStack::setPullUp(void * /*context*/, bool enable) {
   if(const auto pullUp = board::current.usbPullUp) {
      board::write(*pullUp, enable);
   }
}

void UsbStack::discardSerialInput() {
   std::array<uint8_t, cdcDataPacketSize> chunk{};
   while(serialPort_.read(art::span<uint8_t>(chunk.data(), chunk.size())) != 0) {
   }
}

} // namespace usb
