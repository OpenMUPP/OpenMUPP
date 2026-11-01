#pragma once

// The USB composite device of the dongle on the STM32F1 USB peripheral:
// the protocol interface of one personality and, in most personalities,
// one CDC-ACM serial port. Target only.
//
// The build selects the personalities of the image (MUPP_PERSONALITIES,
// cmake/Personalities.cmake). Each one has a define, its descriptors and its USB
// function:
//
//   versaloon   the Versaloon vendor interface of OpenOCD,
//               MUPP_WITH_VERSALOON (usb/UsbDescriptors.h,
//               usb/VersaloonFunction.h)
//   cmsis-dap   the CMSIS-DAP v2 interface, MUPP_WITH_CMSIS_DAP
//               (usb/CmsisDapDescriptors.h, usb/CmsisDapFunction.h)
//   wch-link    the WCH-Link interface, MUPP_WITH_WCH_LINK
//               (usb/WchLinkDescriptors.h, usb/WchLinkFunction.h)
//
// The stack has the USB function of each personality in the image, so an
// image with one personality has the RAM and flash of only that one. The
// constructor selects the active personality. The device then has only the
// descriptors and the USB function of that personality. The functions of
// the other personalities are not used.
//
// The serial port is the same in each personality, except wch-link: its
// two bulk pairs leave no packet memory for a serial port. hasSerialPort()
// tells if the active personality has one. The serial port function is in
// the stack when a personality of the image has a serial port
// (MUPP_USB_SERIAL_PORT). An image with only wch-link has no serial port, and
// the UART bridge and the debug log are off.
//
// The USB interrupt only queues the events. poll() runs the USB protocol in
// the main loop. In the personalities with a serial port, the OUT endpoint
// of the protocol interface is double-buffered: the hardware receives the
// next packet of a command while the main loop handles the last one.
// pollSerial() runs only the serial port, during a long USB_TO_XXX command
// (usb/SerialEventQueue.h). The D+ pull-up of the board definition connects
// the device; a board without one has a fixed pull-up.

#include <cstdint>
#include <usb/CdcAcmFunction.h>
#include <usb/Device.h>
#include <usb/Stm32UsbDeviceController.h>

#include "personality/Personality.h"
#include "usb/SerialEventQueue.h"
#include "usb/SerialPortConfig.h"
#include "usb/SwitchRequest.h"
#if defined(MUPP_WITH_CMSIS_DAP)
#include "usb/CmsisDapFunction.h"
#endif
#if defined(MUPP_WITH_VERSALOON)
#include "usb/VersaloonFunction.h"
#endif
#if defined(MUPP_WITH_WCH_LINK)
#include "usb/WchLinkFunction.h"
#endif

#if !defined(ART_STM32_HAS_USB_DEVICE_CONTROLLER)
#error "libart-stm32 has no USB device controller for this device"
#endif

// The objects behind the USB functions (UsbStack::Functions). Each
// personality includes its own header.
namespace boot {
class BootloaderEntry;
}
namespace cmsisdap {
class Processor;
}
namespace personality {
class PersonalitySwitch;
}
namespace protocol {
class UsbToXxxHandler;
}
namespace wchlink {
class Processor;
}

namespace usb {

class UsbStack {
   public:
   // What the Versaloon personality needs
   struct VersaloonDeps {
      // Runs the USB_TO_XXX commands (see protocol/versaloon/CommandProcessor.h)
      protocol::UsbToXxxHandler *usbToXxx;
   };

   // The objects behind the USB functions. A pointer is nullptr when its
   // personality is not in the image, and not nullptr when it is. The
   // objects must outlive the stack.
   struct Functions {
      const VersaloonDeps *versaloon;
      // Runs the CMSIS-DAP commands (see usb/CmsisDapFunction.h)
      cmsisdap::Processor *dap;
      // Runs the WCH-Link requests (see usb/WchLinkFunction.h)
      wchlink::Processor *wch;
      // Takes SWITCH_PERSONALITY of the Versaloon protocol and the switch
      // request on EP0 (usb/SwitchRequest.h), nullptr in an image with one
      // personality. The CMSIS-DAP processor has its own pointer to it
      // (ID_DAP_Vendor0).
      personality::PersonalitySwitch *personalitySwitch;
      // Takes VERSALOON_FW_UPDATE and the bootloader code of
      // SWITCH_PERSONALITY and of the switch request on EP0, nullptr when
      // the firmware has no bootloader. The CMSIS-DAP processor has its own
      // pointer to it (ID_DAP_Vendor0).
      boot::BootloaderEntry *bootloader;
   };

   // active: the personality of this start. It must be in the image.
   UsbStack(personality::Personality active, const Functions &functions);

   UsbStack(const UsbStack &) = delete;
   UsbStack &operator=(const UsbStack &) = delete;

   // Turns on the USB clock, sets the serial number from the chip UID and
   // connects to the bus. Needs the 72 MHz clock from the crystal. Call it
   // immediately before the main loop starts to call poll().
   void start();

   // Runs the USB protocol. Call it in each main loop iteration.
   void poll();

   // Disconnects from the bus before a reset, so the host sees the device
   // go away. A board with a fixed D+ pull-up gets D+ driven low.
   void disconnect();

   // Delivers the serial port events at the front of the queue, and no
   // other events. In the CMSIS-DAP personality, also the packet events of
   // the CMSIS-DAP endpoints (usb/SerialEventQueue.h). Call it during a
   // command (see protocol/Background.h), not in the main loop. The
   // WCH-Link personality has no serial port: the call delivers no event.
   void pollSerial();

   // True when USB events wait for poll(). The main loop must not sleep
   // then.
   [[nodiscard]] bool hasPendingEvents() const {
      return !events_.empty();
   }

   // True when the host has configured the device.
   [[nodiscard]] bool isConfigured() const {
      return device_.isConfigured();
   }

   // Number of commands of the host so far, for the activity LED. It wraps
   // at 2^32: only the difference between two values counts.
   [[nodiscard]] uint32_t commandCount() const {
      switch(personality()) {
#if defined(MUPP_WITH_VERSALOON)
      case personality::Personality::Versaloon:
         return versaloon_.commandCount();
#endif
#if defined(MUPP_WITH_CMSIS_DAP)
      case personality::Personality::CmsisDap:
         return dap_.commandCount();
#endif
#if defined(MUPP_WITH_WCH_LINK)
      case personality::Personality::WchLink:
         return wch_.commandCount();
#endif
      default:
         return 0;
      }
   }

   // The personality of the descriptors and of interface 0. In an image
   // with one personality, it is a constant, so the compiler drops the
   // branches of the other ones.
   [[nodiscard]] personality::Personality personality() const {
      if constexpr(personality::all.size() > 1) {
         return active_;
      } else {
         return personality::all[0];
      }
   }

   // True when the active personality has the CDC-ACM serial port
   [[nodiscard]] bool hasSerialPort() const {
      return !personality::has(personality::Personality::WchLink) || personality() != personality::Personality::WchLink;
   }

#if defined(MUPP_USB_SERIAL_PORT)
   // The CDC-ACM serial port: the UART bridge, or the debug log in a
   // MUPP_DEBUG_CDC build (see debug/DebugLog.h). The device has it only
   // when hasSerialPort() is true.
   art::usb::CdcAcmFunctionBase &serialPort() {
      return serialPort_;
   }
#endif

   private:
#if defined(MUPP_USB_SERIAL_PORT)
   // Queue sizes of the serial port
   static constexpr std::size_t serialRxSize = 256;
   static constexpr std::size_t serialTxSize = 256;
#endif

   static void setPullUp(void *context, bool enable);

#if defined(MUPP_USB_SERIAL_PORT)
   // MUPP_DEBUG_CDC: the CDC port carries the log, so received data is
   // dropped.
   void discardSerialInput();
#endif

   hw::UsbDevice hardware_;
   SerialEventQueue<16> events_;
   Stm32UsbDeviceController controller_;
   personality::Personality active_;
   art::usb::Device device_;
#if defined(MUPP_WITH_VERSALOON)
   VersaloonFunction versaloon_;
#endif
#if defined(MUPP_WITH_CMSIS_DAP)
   CmsisDapFunction dap_;
#endif
#if defined(MUPP_WITH_WCH_LINK)
   WchLinkFunction wch_;
#endif
#if defined(MUPP_USB_SERIAL_PORT)
   art::usb::CdcAcmFunction<serialRxSize, serialTxSize> serialPort_;
#endif
   // The vendor requests to the device in each personality
   SwitchRequestHandler switchRequest_;
};

} // namespace usb
