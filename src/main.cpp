// Versaloon next generation firmware. os_startup_initialize_hardware()
// (src/initialize_hardware.cpp) sets up the clocks and the board pins
// before main() runs. The main loop wakes up on each 1 ms SysTick and on
// each USB interrupt. It runs the USB device, the UART bridge
// (uart/SerialBridge.h) or, in a VSL_DEBUG_CDC build, the debug log
// (debug/DebugLog.h), and drives the heartbeat on the
// state LED and the USB state on the USB LED. After VERSALOON_FW_UPDATE it
// enters versaloon-bootloader (boot/BootloaderEntry.h). The USB_TO_XXX
// commands of OpenOCD go to the USB_TO_ALL dispatcher (protocol/UsbToAll.h),
// with the GPIO block (protocol/GpioBlock.h) on the debug connector pins.
#include <cmsis_device.h>
#include <delaybusywait.h>
#include <utils/Timekeeping.h>

#include "Hardware.h"
#include "Heartbeat.h"
#include "Version.h"
#include "board/Board.h"
#include "board/BoardGpio.h"
#include "board/Leds.h"
#include "boot/Bootloader.h"
#include "boot/BootloaderEntry.h"
#include "debug/DebugLog.h"
#include "protocol/GpioBlock.h"
#include "protocol/UsbToAll.h"
#include "uart/SerialBridge.h"
#include "usb/UsbStack.h"

namespace {

// The DELAY block busy-waits: the main loop stops for the delay, up to
// 32.767 s. The host waits for the reply in this time.
class BusyWaitDelay final : public protocol::Delay {
   public:
   void microseconds(uint16_t us) override {
      delayuS(us);
   }
   void milliseconds(uint16_t ms) override {
      delaymS(ms);
   }
};

BusyWaitDelay busyWaitDelay;
protocol::UsbToAll usbToAll{ busyWaitDelay };
board::BoardGpio boardGpio;
protocol::GpioBlock gpioBlock{ boardGpio };
boot::BootloaderEntry bootloaderEntry;
usb::UsbStack usbStack{ boot::hasBootloader ? &bootloaderEntry : nullptr, usbToAll };

} // namespace

int main() {
   const bool clockFromCrystal = hardware::isClockFromCrystal();
   const Heartbeat heartbeat{ clockFromCrystal ? Heartbeat::normal : Heartbeat::clockFault };

   debug::startLog();
   // The board names are string literals, so name.data() ends with a NUL.
   LOG_INFO(debug::appLog, "versaloon-next-gen %s, board %s", versionString, board::current.name.data());
   if(!usbToAll.add(gpioBlock)) {
      LOG_ERROR(debug::appLog, "USB_TO_ALL: cannot add the GPIO block");
   }

   // USB needs the 48 MHz clock from the PLL. Without the crystal the
   // device stays disconnected, and the UART bridge is off.
   if(clockFromCrystal) {
      uart::startBridge(usbStack);
      usbStack.start();
   }

   for(;;) {
      // One SysTick is 1 ms (initialize_hardware.cpp)
      const auto nowMs = static_cast<uint64_t>(getUptimeTicks());
      if(bootloaderEntry.isDue(nowMs)) {
         boot::enterBootloader();
      }
      if(clockFromCrystal) {
         usbStack.poll();
         debug::pollLog(usbStack);
         uart::pollBridge();
      }
      board::setLed(board::Led::State, heartbeat.isOn(nowMs));
      board::setLed(board::Led::Usb, usbStack.isConfigured());
      __WFI();
   }
}
