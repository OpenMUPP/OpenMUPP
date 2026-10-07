#pragma once

// The USB composite device of the dongle on the STM32F1 USB peripheral:
// the Versaloon vendor interface and one CDC-ACM serial port (see
// usb/UsbDescriptors.h). Target only.
//
// The USB interrupt only queues the events. poll() runs the USB protocol in
// the main loop. pollSerial() runs only the serial port, during a long
// USB_TO_XXX command (usb/SerialEventQueue.h). The D+ pull-up of the board definition connects the
// device; a board without one has a fixed pull-up.

#include <cstdint>
#include <usb/CdcAcmFunction.h>
#include <usb/Device.h>
#include <usb/Stm32UsbDeviceController.h>

#include "usb/SerialEventQueue.h"
#include "usb/VersaloonFunction.h"

#if !defined(ART_STM32_HAS_USB_DEVICE_CONTROLLER)
#error "libart-stm32 has no USB device controller for this device"
#endif

namespace usb {

class UsbStack {
   public:
   // bootloader: the bootloader entry of VERSALOON_FW_UPDATE, nullptr when
   // the firmware has no bootloader. usbToXxx: runs the USB_TO_XXX commands
   // (see protocol/CommandProcessor.h).
   UsbStack(boot::BootloaderEntry *bootloader, protocol::UsbToXxxHandler &usbToXxx);

   UsbStack(const UsbStack &) = delete;
   UsbStack &operator=(const UsbStack &) = delete;

   // Turns on the USB clock, sets the serial number from the chip UID and
   // connects to the bus. Needs the 72 MHz clock from the crystal. Call it
   // immediately before the main loop starts to call poll().
   void start();

   // Runs the USB protocol. Call it in each main loop iteration.
   void poll();

   // Delivers the serial port events at the front of the queue, and no
   // other events. Call it during a USB_TO_XXX command (see
   // protocol/Background.h), not in the main loop.
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

   hw::UsbDevice hardware_;
   SerialEventQueue<16> events_;
   Stm32UsbDeviceController controller_;
   art::usb::Device device_;
   VersaloonFunction versaloon_;
   art::usb::CdcAcmFunction<serialRxSize, serialTxSize> serialPort_;
};

} // namespace usb
