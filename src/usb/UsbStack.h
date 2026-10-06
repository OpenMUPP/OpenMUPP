#pragma once

// The USB composite device of the dongle on the STM32F1 USB peripheral:
// the Versaloon vendor interface and one CDC-ACM serial port (see
// usb/UsbDescriptors.h). Target only.
//
// The USB interrupt only queues the events. poll() runs the USB protocol in
// the main loop. The D+ pull-up of the board definition connects the
// device; a board without one has a fixed pull-up.

#include <cstdint>
#include <usb/CdcAcmFunction.h>
#include <usb/Device.h>
#include <usb/Stm32UsbDeviceController.h>
#include <utils/RingBuffer.h>

#include "usb/VersaloonFunction.h"

#if !defined(ART_STM32_HAS_USB_DEVICE_CONTROLLER)
#error "libart-stm32 has no USB device controller for this device"
#endif

namespace usb {

class UsbStack {
   public:
   UsbStack();

   UsbStack(const UsbStack &) = delete;
   UsbStack &operator=(const UsbStack &) = delete;

   // Turns on the USB clock, sets the serial number from the chip UID and
   // connects to the bus. Needs the 72 MHz clock from the crystal. Call it
   // immediately before the main loop starts to call poll().
   void start();

   // Runs the USB protocol. Call it in each main loop iteration.
   void poll();

   // True when the host has configured the device.
   [[nodiscard]] bool isConfigured() const {
      return device_.isConfigured();
   }

   // The CDC-ACM serial port, for the UART bridge and the logger.
   art::usb::CdcAcmFunctionBase &serialPort() {
      return serialPort_;
   }

   private:
   // Queue sizes of the serial port
   static constexpr std::size_t serialRxSize = 256;
   static constexpr std::size_t serialTxSize = 256;

   static void setPullUp(void *context, bool enable);

   // Placeholder until the UART bridge: sends the received data back.
   void loopBackSerialPort();

   hw::UsbDevice hardware_;
   RingBuffer<hw::usb::Event, 16> events_;
   Stm32UsbDeviceController controller_;
   art::usb::Device device_;
   VersaloonFunction versaloon_;
   art::usb::CdcAcmFunction<serialRxSize, serialTxSize> serialPort_;
};

} // namespace usb
