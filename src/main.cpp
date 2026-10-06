// Versaloon next generation firmware. os_startup_initialize_hardware()
// (src/initialize_hardware.cpp) sets up the clocks and the board pins
// before main() runs. The main loop wakes up on each 1 ms SysTick and on
// each USB interrupt. It runs the USB device, the UART bridge
// (uart/SerialBridge.h) or, in a VSL_DEBUG_CDC build, the debug log
// (debug/DebugLog.h), and drives the heartbeat on the
// state LED and the USB state on the USB LED.
#include <cmsis_device.h>
#include <utils/Timekeeping.h>

#include "Hardware.h"
#include "Heartbeat.h"
#include "Version.h"
#include "board/Board.h"
#include "board/Leds.h"
#include "debug/DebugLog.h"
#include "uart/SerialBridge.h"
#include "usb/UsbStack.h"

namespace {

usb::UsbStack usbStack;

} // namespace

int main() {
   const bool clockFromCrystal = hardware::isClockFromCrystal();
   const Heartbeat heartbeat{ clockFromCrystal ? Heartbeat::normal : Heartbeat::clockFault };

   debug::startLog();
   // The board names are string literals, so name.data() ends with a NUL.
   LOG_INFO(debug::appLog, "versaloon-next-gen %s, board %s", versionString, board::current.name.data());

   // USB needs the 48 MHz clock from the PLL. Without the crystal the
   // device stays disconnected, and the UART bridge is off.
   if(clockFromCrystal) {
      uart::startBridge(usbStack);
      usbStack.start();
   }

   for(;;) {
      if(clockFromCrystal) {
         usbStack.poll();
         debug::pollLog(usbStack);
         uart::pollBridge();
      }
      // One SysTick is 1 ms (initialize_hardware.cpp)
      board::setLed(board::Led::State, heartbeat.isOn(static_cast<uint64_t>(getUptimeTicks())));
      board::setLed(board::Led::Usb, usbStack.isConfigured());
      __WFI();
   }
}
