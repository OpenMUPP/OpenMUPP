#pragma once

// The USB composite device of the dongle on the STM32F1 USB peripheral:
// the protocol interface and one CDC-ACM serial port. Target only.
//
// The build selects the personality (VSL_PROTOCOL in CMakeLists.txt):
//
//   versaloon (default)  the Versaloon vendor interface of OpenOCD
//                        (usb/UsbDescriptors.h, usb/VersaloonFunction.h)
//   cmsis-dap            the CMSIS-DAP v2 interface, VSL_PROTOCOL_CMSIS_DAP
//                        (usb/CmsisDapDescriptors.h, usb/CmsisDapFunction.h)
//
// Only one personality is in an image. The serial port is the same.
//
// The USB interrupt only queues the events. poll() runs the USB protocol in
// the main loop. The OUT endpoint of the protocol interface is double-buffered: the hardware
// receives the next packet of a command while the main loop handles the
// last one. pollSerial() runs only the serial port, during a long
// USB_TO_XXX command (usb/SerialEventQueue.h). The D+ pull-up of the board definition connects the
// device; a board without one has a fixed pull-up.

#include <cstdint>
#include <usb/CdcAcmFunction.h>
#include <usb/Device.h>
#include <usb/Stm32UsbDeviceController.h>

#include "usb/SerialEventQueue.h"
#if defined(VSL_PROTOCOL_CMSIS_DAP)
#include "usb/CmsisDapFunction.h"
#else
#include "usb/VersaloonFunction.h"
#endif

#if !defined(ART_STM32_HAS_USB_DEVICE_CONTROLLER)
#error "libart-stm32 has no USB device controller for this device"
#endif

namespace usb {

class UsbStack {
   public:
#if defined(VSL_PROTOCOL_CMSIS_DAP)
   // processor: runs the CMSIS-DAP commands (see usb/CmsisDapFunction.h).
   // It must outlive the stack.
   explicit UsbStack(cmsisdap::Processor &processor);
#else
   // bootloader: the bootloader entry of VERSALOON_FW_UPDATE, nullptr when
   // the firmware has no bootloader. usbToXxx: runs the USB_TO_XXX commands
   // (see protocol/CommandProcessor.h).
   UsbStack(boot::BootloaderEntry *bootloader, protocol::UsbToXxxHandler &usbToXxx);
#endif

   UsbStack(const UsbStack &) = delete;
   UsbStack &operator=(const UsbStack &) = delete;

   // Turns on the USB clock, sets the serial number from the chip UID and
   // connects to the bus. Needs the 72 MHz clock from the crystal. Call it
   // immediately before the main loop starts to call poll().
   void start();

   // Runs the USB protocol. Call it in each main loop iteration.
   void poll();

   // Delivers the serial port events at the front of the queue, and no
   // other events. In the CMSIS-DAP personality, also the packet events of
   // the CMSIS-DAP endpoints (usb/SerialEventQueue.h). Call it during a
   // command (see protocol/Background.h), not in the main loop.
   void pollSerial();

   // True when the host has configured the device.
   [[nodiscard]] bool isConfigured() const {
      return device_.isConfigured();
   }

   // The CDC-ACM serial port: the UART bridge, or the debug log in a
   // VSL_DEBUG_CDC build (see debug/DebugLog.h).
   art::usb::CdcAcmFunctionBase &serialPort() {
      return serialPort_;
   }

   private:
   // Queue sizes of the serial port
   static constexpr std::size_t serialRxSize = 256;
   static constexpr std::size_t serialTxSize = 256;

   static void setPullUp(void *context, bool enable);

   // VSL_DEBUG_CDC: the CDC port carries the log, so received data is
   // dropped.
   void discardSerialInput();

   hw::UsbDevice hardware_{ endpointCount };
   SerialEventQueue<16> events_;
   Stm32UsbDeviceController controller_;
   art::usb::Device device_;
#if defined(VSL_PROTOCOL_CMSIS_DAP)
   CmsisDapFunction protocol_;
#else
   VersaloonFunction protocol_;
#endif
   art::usb::CdcAcmFunction<serialRxSize, serialTxSize> serialPort_;
};

} // namespace usb
