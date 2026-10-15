#include "usb/UsbStack.h"

#include <array>
#include <delaybusywait.h>
#include <peripherals/Rcc.h>
#include <peripherals/Uid.h>

#include "Hardware.h"
#include "Version.h"
#include "board/Board.h"
#include "board/BoardPins.h"
#include "debug/DebugLog.h"
#include "usb/UsbDescriptors.h"
#if defined(VSL_WITH_CMSIS_DAP)
#include "usb/CmsisDapDescriptors.h"
#endif
#if defined(VSL_WITH_WCH_LINK)
#include "usb/WchLinkDescriptors.h"
#endif

namespace usb {

namespace {

using personality::Personality;

#if !defined(VSL_WITH_WCH_LINK)
static_assert(art::usb::CdcAcmFunctionBase::kDataPacketSize == cdcDataPacketSize);
#endif
static_assert(hw::Uid::SizeBytes == uidBytes);
#if defined(VSL_WITH_CMSIS_DAP)
static_assert(dap::packetMemoryUsed <= hw::UsbDevice::PacketMemorySize);
// The two personalities share the endpoint numbers, so the OUT endpoint is
// double-buffered in each.
static_assert(dap::outEndpoint == versaloonOutEndpoint);
#endif
#if defined(VSL_WITH_VERSALOON)
// The packet memory: the buffer descriptor table, EP0 (64 bytes each way),
// two buffers for the double-buffered Versaloon OUT endpoint, and one for
// each other endpoint.
static_assert(endpointCount * 8 + 2 * 64 + 2 * versaloonPacketSize + versaloonPacketSize + cdcNotificationPacketSize + 2 * cdcDataPacketSize <=
              hw::UsbDevice::PacketMemorySize);
#endif

// How long D+ stays low before a reset, so the host sees a disconnect
constexpr uint32_t disconnectMs = 20;

#if !defined(VSL_WITH_WCH_LINK)
constexpr art::usb::CdcAcmEndpoints serialPortEndpoints{
   .notification = cdcNotificationEndpoint,
   .notificationMaxPacketSize = cdcNotificationPacketSize,
   .dataOut = cdcDataOutEndpoint,
   .dataIn = cdcDataInEndpoint,
};
#endif

SerialNumber serialNumber;

#if defined(VSL_WITH_VERSALOON)
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

#if defined(VSL_WITH_CMSIS_DAP)
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
};

// Answers the request of Windows for the MS OS 2.0 descriptor set
art::usb::msos20::DescriptorSetHandler msOs20Handler(dap::msOs20VendorCode, desc::asSpan(dap::msOs20DescriptorSet));
#endif

#if defined(VSL_WITH_WCH_LINK)
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

#if defined(VSL_WITH_VERSALOON) && defined(VSL_WITH_CMSIS_DAP)
const art::usb::Descriptors &descriptorsOf(Personality personality) {
   return personality == Personality::CmsisDap ? dapDescriptors : versaloonDescriptors;
}
#endif

} // namespace

#if defined(VSL_WITH_VERSALOON) && defined(VSL_WITH_CMSIS_DAP)
UsbStack::UsbStack(Personality active,
                   boot::BootloaderEntry *bootloader,
                   protocol::UsbToXxxHandler &usbToXxx,
                   personality::PersonalitySwitch &personalitySwitch,
                   cmsisdap::Processor &processor)
   : controller_(hardware_, events_)
   , active_(active)
   , device_(controller_, descriptorsOf(active))
   , versaloon_(deviceInfo, bootloader, &usbToXxx, &personalitySwitch)
   , dap_(processor)
#elif defined(VSL_WITH_WCH_LINK)
UsbStack::UsbStack(wchlink::Processor &processor)
   : controller_(hardware_, events_)
   , active_(Personality::WchLink)
   , device_(controller_, wchDescriptors)
   , wch_(processor)
#elif defined(VSL_WITH_CMSIS_DAP)
UsbStack::UsbStack(cmsisdap::Processor &processor)
   : controller_(hardware_, events_)
   , active_(Personality::CmsisDap)
   , device_(controller_, dapDescriptors)
   , dap_(processor)
#else
UsbStack::UsbStack(boot::BootloaderEntry *bootloader, protocol::UsbToXxxHandler &usbToXxx)
   : controller_(hardware_, events_)
   , active_(Personality::Versaloon)
   , device_(controller_, versaloonDescriptors)
   , versaloon_(deviceInfo, bootloader, &usbToXxx)
#endif
#if !defined(VSL_WITH_WCH_LINK)
   , serialPort_(cdcControlInterface, serialPortEndpoints)
#endif
{
#if !defined(VSL_WITH_WCH_LINK)
   // Both bulk pairs of the WCH-Link personality are single-buffered
   // (usb/WchLinkDescriptors.h).
   controller_.setDoubleBufferedOut(versaloonOutEndpoint);
#endif
   // Fails only for an interface range that does not fit the device: a
   // static configuration error, which the host tests catch.
#if defined(VSL_WITH_CMSIS_DAP)
   if(active_ == Personality::CmsisDap) {
      // DAP_TransferAbort during a command (usb/SerialEventQueue.h)
      events_.setCommandEndpoints(dap::outEndpoint, dap::inEndpoint);
      device_.setVendorRequestHandler(&msOs20Handler);
      static_cast<void>(device_.addFunction(dap_));
   }
#endif
#if defined(VSL_WITH_VERSALOON)
   if(active_ == Personality::Versaloon) {
      static_cast<void>(device_.addFunction(versaloon_));
   }
#endif
#if defined(VSL_WITH_WCH_LINK)
   static_cast<void>(device_.addFunction(wch_));
#else
   static_cast<void>(device_.addFunction(serialPort_));
#endif
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
   // Before begin(), which enables the interrupt (Hardware.h)
   NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, hardware::usbIrqPriority);
   controller_.begin();
   device_.start();
}

void UsbStack::poll() {
   controller_.poll();
#if defined(VSL_WITH_CMSIS_DAP)
   if(active_ == Personality::CmsisDap) {
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
#if defined(VSL_WITH_WCH_LINK)
   // As the CMSIS-DAP personality: run requests and data packets, and
   // deliver the events that came during them.
   while(wch_.poll()) {
      controller_.poll();
   }
#else
   // Without VSL_DEBUG_CDC the UART bridge reads the serial port
   // (uart/SerialBridge.h).
   if constexpr(debug::cdcLog) {
      discardSerialInput();
   }
#endif
}

void UsbStack::disconnect() {
   NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
   if(const auto pullUp = board::current.usbPullUp) {
      board::write(*pullUp, false);
   } else {
      // Stop the transceiver and reset the USB peripheral, then turn off
      // its clock. The GPIO then drives D+ (PA12) low.
      USB->CNTR = USB_CNTR_FRES | USB_CNTR_PDWN;
      RCC->APB1RSTR |= RCC_APB1RSTR_USBRST;
      RCC->APB1RSTR &= ~RCC_APB1RSTR_USBRST;
      RCC->APB1ENR &= ~RCC_APB1ENR_USBEN;
      board::configureOutput(board::Signal{ { board::Port::A, 12 }, board::Active::High }, false);
   }
   delaymS(disconnectMs);
}

void UsbStack::pollSerial() {
   events_.setSerialOnly(true);
   controller_.poll();
   events_.setSerialOnly(false);
#if !defined(VSL_WITH_WCH_LINK)
   if constexpr(debug::cdcLog) {
      discardSerialInput();
   }
#endif
}

void UsbStack::setPullUp(void * /*context*/, bool enable) {
   if(const auto pullUp = board::current.usbPullUp) {
      board::write(*pullUp, enable);
   }
}

#if !defined(VSL_WITH_WCH_LINK)
void UsbStack::discardSerialInput() {
   std::array<uint8_t, cdcDataPacketSize> chunk{};
   while(serialPort_.read(art::span<uint8_t>(chunk.data(), chunk.size())) != 0) {
   }
}
#endif

} // namespace usb
