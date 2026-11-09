// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb/UsbStack.h"

#include <array>
#include <delaybusywait.h>
#if !defined(__riscv)
#include <peripherals/Rcc.h>
#if !defined(STM32F1)
#include <peripherals/Crs.h>
#endif
#endif
#include <peripherals/Uid.h>

#include "Hardware.h"
#include "Version.h"
#include "board/Board.h"
#include "board/BoardPins.h"
#include "debug/DebugLog.h"
#include "usb/UsbDescriptors.h"
#if defined(MUPP_WITH_CMSIS_DAP)
#include "usb/CmsisDapDescriptors.h"
#endif
#if defined(MUPP_WITH_WCH_LINK)
#include "usb/WchLinkDescriptors.h"
#endif

namespace usb {

namespace {

using personality::Personality;

#if defined(MUPP_USB_SERIAL_PORT)
static_assert(art::usb::CdcAcmFunctionBase::kDataPacketSize == cdcDataPacketSize);
#endif
static_assert(hw::Uid::SizeBytes == uidBytes);
#if defined(MUPP_WITH_CMSIS_DAP)
static_assert(dap::packetMemoryUsed <= hw::UsbDevice::PacketMemorySize);
static_assert(switchRequestCode != dap::msOs20VendorCode);
// The two personalities share the endpoint numbers, so the OUT endpoint is
// double-buffered in each.
static_assert(dap::outEndpoint == versaloonOutEndpoint);
#if defined(__riscv)
// The USBHS packet pool at high speed: the double-buffered DAP OUT endpoint,
// the DAP IN endpoint and the CDC-ACM endpoints.
static_assert(3 * dap::highSpeedPacketSize + cdcNotificationPacketSize + 2 * dap::highSpeedPacketSize <= hw::UsbDevice::PacketMemorySize);
#endif
#endif
#if defined(MUPP_WITH_VERSALOON)
// The packet memory: the buffer descriptor table, EP0 (64 bytes each way),
// two buffers for the double-buffered Versaloon OUT endpoint, and one for
// each other endpoint.
static_assert(endpointCount * 8 + 2 * 64 + 2 * versaloonPacketSize + versaloonPacketSize + cdcNotificationPacketSize + 2 * cdcDataPacketSize <=
              hw::UsbDevice::PacketMemorySize);
#endif

// How long D+ stays low before a reset, so the host sees a disconnect
constexpr uint32_t disconnectMs = 20;

#if defined(MUPP_USB_SERIAL_PORT)
constexpr art::usb::CdcAcmEndpoints serialPortEndpoints{
   .notification = cdcNotificationEndpoint,
   .notificationMaxPacketSize = cdcNotificationPacketSize,
   .dataOut = cdcDataOutEndpoint,
   .dataIn = cdcDataInEndpoint,
};
#endif

SerialNumber serialNumber;

#if defined(MUPP_WITH_VERSALOON)
const protocol::DeviceInfo deviceInfo{
   .version = versionString,
   .board = board::current.name,
   .hardwareId = board::current.hardwareId,
};

const std::array<art::span<const uint8_t>, stringCount> versaloonStrings{
   desc::asSpan(languageDescriptor),   desc::asSpan(manufacturerDescriptor),
   desc::asSpan(productDescriptor),    serialNumber.bytes(),
   desc::asSpan(serialPortDescriptor),
};

const art::usb::Descriptors versaloonDescriptors{
   .device = desc::asSpan(deviceDescriptor),
   .configuration = desc::asSpan(configurationDescriptor),
   .strings = art::span<const art::span<const uint8_t>>(versaloonStrings.data(), versaloonStrings.size()),
   // No BOS: the Versaloon personality is a USB 2.00 device.
   .bos = {},
};
#endif

#if defined(MUPP_WITH_CMSIS_DAP)
const std::array<art::span<const uint8_t>, dap::stringCount> dapStrings{
   desc::asSpan(dap::languageDescriptor),   desc::asSpan(dap::manufacturerDescriptor),
   desc::asSpan(dap::productDescriptor),    serialNumber.bytes(),
   desc::asSpan(dap::serialPortDescriptor), desc::asSpan(dap::interfaceStringDescriptor),
};

const art::usb::Descriptors dapDescriptors{
   .device = desc::asSpan(dap::deviceDescriptor),
   .configuration = desc::asSpan(dap::configurationDescriptor),
   .strings = art::span<const art::span<const uint8_t>>(dapStrings.data(), dapStrings.size()),
   .bos = desc::asSpan(dap::bosDescriptor),
#if defined(__riscv)
   .highSpeedConfiguration = desc::asSpan(dap::highSpeedConfigurationDescriptor),
   .deviceQualifier = desc::asSpan(dap::deviceQualifierDescriptor),
#endif
};

// Answers the request of Windows for the MS OS 2.0 descriptor set
art::usb::msos20::DescriptorSetHandler msOs20Handler(dap::msOs20VendorCode, desc::asSpan(dap::msOs20DescriptorSet));
#endif

#if defined(MUPP_WITH_WCH_LINK)
const std::array<art::span<const uint8_t>, wch::stringCount> wchStrings{
   desc::asSpan(wch::languageDescriptor),
   desc::asSpan(wch::manufacturerDescriptor),
   desc::asSpan(wch::productDescriptor),
   serialNumber.bytes(),
};

const art::usb::Descriptors wchDescriptors{
   .device = desc::asSpan(wch::deviceDescriptor),
   .configuration = desc::asSpan(wch::configurationDescriptor),
   .strings = art::span<const art::span<const uint8_t>>(wchStrings.data(), wchStrings.size()),
   // No BOS: the WCH-Link personality is a USB 2.00 device.
   .bos = {},
};
#endif

// The descriptors of a personality of the image. The constructor of
// UsbStack takes only a personality of the image.
const art::usb::Descriptors &descriptorsOf([[maybe_unused]] Personality personality) {
#if defined(MUPP_WITH_WCH_LINK)
   if(personality == Personality::WchLink) {
      return wchDescriptors;
   }
#endif
#if defined(MUPP_WITH_CMSIS_DAP)
   if(personality == Personality::CmsisDap) {
      return dapDescriptors;
   }
#endif
#if defined(MUPP_WITH_VERSALOON)
   return versaloonDescriptors;
#elif defined(MUPP_WITH_CMSIS_DAP)
   return dapDescriptors;
#else
   return wchDescriptors;
#endif
}

// The handler of the other vendor requests to the device in a personality
// (SwitchRequestHandler)
art::usb::VendorRequestHandler *nextVendorHandlerOf([[maybe_unused]] Personality personality) {
#if defined(MUPP_WITH_CMSIS_DAP)
   if(personality == Personality::CmsisDap) {
      return &msOs20Handler;
   }
#endif
   return nullptr;
}

// The highest endpoint number of a personality, plus 1 (hw::UsbDevice)
uint8_t endpointCountOf([[maybe_unused]] Personality personality) {
#if defined(MUPP_WITH_WCH_LINK)
   if(personality == Personality::WchLink) {
      return wch::endpointCount;
   }
#endif
   return endpointCount;
}

#if defined(__riscv)
// High speed only for a personality with high-speed descriptors (512-byte
// bulk endpoints, USB 2.0 section 5.8.3). The others have 64-byte bulk
// endpoints, which are valid at full speed only.
hw::UsbDevice::Speed maxSpeedOf([[maybe_unused]] Personality personality) {
#if defined(MUPP_WITH_CMSIS_DAP)
   if(personality == Personality::CmsisDap) {
      return hw::UsbDevice::Speed::High;
   }
#endif
   return hw::UsbDevice::Speed::Full;
}
#endif

#if !defined(STM32F1) && !defined(__riscv)
// USB clock of the STM32H5: HSI48, trimmed by the CRS from the USB SOF
// packets. PA11 and PA12 are USB_DM and USB_DP on AF10. A failure leaves
// USB without a clock, and the host sees no device.
void startUsbClock() {
   if(!Rcc::enableHSI48()) {
      LOG_ERROR(debug::appLog, "USB: HSI48 does not start");
      return;
   }
   Rcc::setUSBClockSource(Rcc::USBClockSource::HSI48);
   Rcc::enableCRSClock();
   if(hw::Crs::configure({})) {
      hw::Crs::enable();
   }
   for(const uint8_t pin : { 11, 12 }) {
      const auto gpio = board::gpioPin({ board::Port::A, pin });
      gpio.setAlternateFunction(hw::GpioPin::AlternateFunction::AF10);
      gpio.configure(hw::GpioPin::Mode::Alternate, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::None,
                     hw::GpioPin::Speed::VeryHigh);
   }
   Rcc::enableUSBClock();
}
#endif

} // namespace

UsbStack::UsbStack(Personality active, const Functions &functions)
#if defined(__riscv)
   : hardware_(endpointCountOf(active), maxSpeedOf(active))
#else
   : hardware_(endpointCountOf(active))
#endif
   , controller_(hardware_, events_)
   , active_(active)
   , device_(controller_, descriptorsOf(active))
#if defined(MUPP_WITH_VERSALOON)
   , versaloon_(deviceInfo, functions.bootloader, functions.versaloon->usbToXxx, functions.personalitySwitch)
#endif
#if defined(MUPP_WITH_CMSIS_DAP)
   , dap_(*functions.dap)
#endif
#if defined(MUPP_WITH_WCH_LINK)
   , wch_(*functions.wch)
#endif
#if defined(MUPP_USB_SERIAL_PORT)
   , serialPort_(cdcControlInterface, serialPortEndpoints)
#endif
   , switchRequest_(functions.personalitySwitch, functions.bootloader, nextVendorHandlerOf(active)) {
#if defined(MUPP_USB_SERIAL_PORT)
   if(hasSerialPort()) {
      // Both bulk pairs of the WCH-Link personality are single-buffered
      // (usb/WchLinkDescriptors.h).
      controller_.setDoubleBufferedOut(versaloonOutEndpoint);
   }
#endif
   // Fails only for an interface range that does not fit the device: a
   // static configuration error, which the host tests catch.
   switch(personality()) {
#if defined(MUPP_WITH_VERSALOON)
   case Personality::Versaloon:
      static_cast<void>(device_.addFunction(versaloon_));
      break;
#endif
#if defined(MUPP_WITH_CMSIS_DAP)
   case Personality::CmsisDap:
      // DAP_TransferAbort during a command (usb/SerialEventQueue.h)
      events_.setCommandEndpoints(dap::outEndpoint, dap::inEndpoint);
      static_cast<void>(device_.addFunction(dap_));
      break;
#endif
#if defined(MUPP_WITH_WCH_LINK)
   case Personality::WchLink:
      static_cast<void>(device_.addFunction(wch_));
      break;
#endif
   default:
      break;
   }
#if defined(MUPP_USB_SERIAL_PORT)
   if(hasSerialPort()) {
      static_cast<void>(device_.addFunction(serialPort_));
   }
#endif
   // The switch request, and in the CMSIS-DAP personality the MS OS 2.0
   // request after it. Without a personality switch and a bootloader, the
   // handler stalls each switch request.
   device_.setVendorRequestHandler(&switchRequest_);
}

void UsbStack::start() {
   std::array<uint8_t, uidBytes> uid{};
   hw::Uid::read(uid);
   serialNumber.set(uid);

#if !defined(__riscv)
   if(board::current.usbPullUp) {
      hardware_.setPullupCallback(&UsbStack::setPullUp, nullptr);
   }
#endif

#if defined(__riscv)
   // The USBHS has an internal D+ pull-up. hw::UsbDevice::begin() starts the PHY clock from the crystal.
   constexpr IRQn_Type usbIrq = USBHS_IRQn;
#elif defined(STM32F1)
   if(!board::current.usbPullUp) {
      // Fixed D+ pull-up: a reset without disconnect() (debugger, watchdog)
      // leaves the host with the old device. D+ low shows a disconnect. The
      // USB clock is still off, so PA12 is a GPIO.
      const board::Signal dPlus{ { board::Port::A, 12 }, board::Active::High };
      board::configureOutput(dPlus, false);
      delaymS(disconnectMs);
      board::gpioPin(dPlus.pin).configure(hw::GpioPin::Mode::Input);
   }
   // USBPRE is set by the 72 MHz clock setup, before the clock is enabled.
   Rcc::enableUSBClock();
#else
   startUsbClock();
#endif
#if !defined(__riscv)
   constexpr auto usbIrq = hw::usbfs::Irq;
#endif
   // Before begin(), which enables the interrupt (Hardware.h)
   NVIC_SetPriority(usbIrq, hardware::usbIrqPriority);
   controller_.begin();
   device_.start();
}

void UsbStack::poll() {
   controller_.poll();
#if defined(MUPP_WITH_CMSIS_DAP)
   if(personality() == Personality::CmsisDap) {
      // Run one command, then deliver the USB events that came during it:
      // the InComplete event frees the IN endpoint for the next response,
      // and the next OUT packet is stored. The loop has no limit: while
      // the host keeps a packet stored, the rest of the main loop does not
      // run (LEDs, key gesture, debug log). This is as before E6, where
      // the commands ran inside the event delivery. The serial service
      // still runs during the commands (protocol/Background.h).
      while(dap_.poll()) {
         controller_.poll();
      }
   }
#endif
#if defined(MUPP_WITH_WCH_LINK)
   if(personality() == Personality::WchLink) {
      // As the CMSIS-DAP personality: run requests and data packets, and
      // deliver the events that came during them.
      while(wch_.poll()) {
         controller_.poll();
      }
   }
#endif
#if defined(MUPP_USB_SERIAL_PORT)
   // Without MUPP_DEBUG_CDC the UART bridge reads the serial port
   // (uart/SerialBridge.h).
   if constexpr(debug::cdcLog) {
      if(hasSerialPort()) {
         discardSerialInput();
      }
   }
#endif
}

void UsbStack::disconnect() {
#if defined(__riscv)
   NVIC_DisableIRQ(USBHS_IRQn);
   controller_.disconnect();
#else
   NVIC_DisableIRQ(hw::usbfs::Irq);
   if(const auto pullUp = board::current.usbPullUp) {
      board::write(*pullUp, false);
   } else {
#if !defined(STM32F1)
      // The host sees the disconnect when the internal D+ pull-up is off
      hw::usbfs::setInternalPullup(false);
#else
      // Stop the transceiver and reset the USB peripheral, then turn off
      // its clock. The GPIO then drives D+ (PA12) low.
      USB->CNTR = USB_CNTR_FRES | USB_CNTR_PDWN;
      RCC->APB1RSTR |= RCC_APB1RSTR_USBRST;
      RCC->APB1RSTR &= ~RCC_APB1RSTR_USBRST;
      RCC->APB1ENR &= ~RCC_APB1ENR_USBEN;
      board::configureOutput(board::Signal{ { board::Port::A, 12 }, board::Active::High }, false);
#endif
   }
#endif
   delaymS(disconnectMs);
}

void UsbStack::pollSerial() {
   events_.setSerialOnly(true);
   controller_.poll();
   events_.setSerialOnly(false);
#if defined(MUPP_USB_SERIAL_PORT)
   if constexpr(debug::cdcLog) {
      if(hasSerialPort()) {
         discardSerialInput();
      }
   }
#endif
}

void UsbStack::setPullUp(void * /*context*/, bool enable) {
   if(const auto pullUp = board::current.usbPullUp) {
      board::write(*pullUp, enable);
   }
}

#if defined(MUPP_USB_SERIAL_PORT)
void UsbStack::discardSerialInput() {
   std::array<uint8_t, cdcDataPacketSize> chunk{};
   while(serialPort_.read(art::span<uint8_t>(chunk.data(), chunk.size())) != 0) {
   }
}
#endif

} // namespace usb
